// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One MCP session (session.hpp): JSON-RPC messages, the lifecycle, the
// tool and resource registries, and the line buffer a transport fills.
#include "oa/app/mcp/session.hpp"

#include "schema.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oa::app::mcp {
namespace {

namespace json = oa::formats::json;

/// How many calls may be held at once. Another is refused.
constexpr std::size_t kMaxHeld = 8;

/// The revisions this server speaks, newest first.
constexpr std::string_view kProtocols[] = {
    protocol_2025_06_18, protocol_2025_03_26, protocol_2024_11_05
};

/// Tells whether a message is only spaces and tabs.
///
/// @param message the message
/// @return true when there is nothing to read
bool blank_message(std::string_view message) {
    for (const char byte : message)
        if (byte != ' ' && byte != '\t' && byte != '\r')
            return false;
    return true;
}

/// Writes one JSON-RPC error object, with no newline.
///
/// @param id_json the request id, as JSON
/// @param code the error code
/// @param message why it failed
/// @return the object
std::string error_line(std::string_view id_json, int64_t code, std::string_view message) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("jsonrpc");
    writer.string("2.0");
    writer.key("id");
    writer.raw(id_json.empty() ? "null" : id_json);
    writer.key("error");
    writer.begin_object();
    writer.key("code");
    writer.integer(code);
    writer.key("message");
    writer.string(message);
    writer.end_object();
    writer.end_object();
    return writer.text();
}

/// Writes one JSON-RPC result, with no newline.
///
/// @param id_json the request id, as JSON
/// @param result the result value, as JSON
/// @return the object
std::string result_line(std::string_view id_json, std::string_view result) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("jsonrpc");
    writer.string("2.0");
    writer.key("id");
    writer.raw(id_json);
    writer.key("result");
    writer.raw(result);
    writer.end_object();
    return writer.text();
}

/// One request's id, or the fact that the message is a notification.
struct RequestId {
    bool notification{}; ///< the message has no id
    bool invalid{};      ///< the id is not a string, a number or null
    std::string json{"null"};
};

/// Reads a message's id.
///
/// @param message the request object
/// @return the id
RequestId read_id(const json::Json& message) {
    RequestId id;
    const json::Json* member = message.find("id");
    if (member == nullptr) {
        id.notification = true;
        return id;
    }
    if (member->type() == json::JsonType::null)
        return id;
    if (member->type() == json::JsonType::number && member->number_text() != nullptr) {
        id.json = *member->number_text();
        return id;
    }
    if (member->type() == json::JsonType::string && member->string() != nullptr) {
        json::JsonWriter writer;
        writer.string(*member->string());
        id.json = writer.text();
        return id;
    }
    id.invalid = true;
    return id;
}

/// Returns a value's JSON text for an id or a progress token.
///
/// @param value a string or a number
/// @return the JSON, or empty when the value is neither
std::string json_atom(const json::Json& value) {
    if (value.type() == json::JsonType::number && value.number_text() != nullptr)
        return *value.number_text();
    if (value.type() == json::JsonType::string && value.string() != nullptr) {
        json::JsonWriter writer;
        writer.string(*value.string());
        return writer.text();
    }
    return {};
}

/// Returns the revision initialize agrees, newest when the client's is unknown.
///
/// @param params initialize's params; null when they were omitted
/// @return the revision
std::string_view agreed_version(const json::Json* params) {
    const json::Json* named = params != nullptr ? params->find("protocolVersion") : nullptr;
    if (named != nullptr && named->string() != nullptr) {
        const std::string_view offered = *named->string();
        for (const std::string_view known : kProtocols)
            if (offered == known)
                return known;
    }
    return protocol_2025_06_18;
}

/// Returns an empty JSON object, parsed once.
///
/// @return the object
const json::Json& empty_object() {
    static const json::Json object = [] {
        json::JsonError failure;
        std::optional<json::Json> parsed = json::parse_json("{}", failure);
        if (!parsed)
            throw std::runtime_error("mcp: {} is not JSON");
        return std::move(*parsed);
    }();
    return object;
}

/// Tells whether a tool name is one this server accepts.
///
/// @param name the name
/// @return true when it is dotted letters, digits, '_' or '-' of 1 to 128
bool tool_name_ok(std::string_view name) {
    if (name.empty() || name.size() > 128)
        return false;
    for (const char byte : name) {
        const bool letter = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
        const bool digit = byte >= '0' && byte <= '9';
        if (!letter && !digit && byte != '_' && byte != '.' && byte != '-')
            return false;
    }
    return true;
}

/// Tells whether a schema text is one line.
///
/// @param schema the text
/// @return true when it holds no newline
bool one_line(std::string_view schema) {
    return schema.find('\n') == std::string_view::npos &&
           schema.find('\r') == std::string_view::npos;
}

/// Writes a progress notification.
///
/// @param token_json the progress token, as JSON
/// @param reply the held answer
/// @return the notification, with no newline
std::string progress_line(std::string_view token_json, const ToolReply& reply) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("jsonrpc");
    writer.string("2.0");
    writer.key("method");
    writer.string("notifications/progress");
    writer.key("params");
    writer.begin_object();
    writer.key("progressToken");
    writer.raw(token_json);
    writer.key("progress");
    writer.integer(reply.progress);
    writer.key("total");
    writer.integer(reply.total);
    if (!reply.progress_message.empty()) {
        writer.key("message");
        writer.string(reply.progress_message);
    }
    writer.end_object();
    writer.end_object();
    return writer.text();
}

/// Encodes bytes as base64, with padding and no line breaks.
///
/// @param bytes the bytes
/// @return the text
std::string base64_of(const std::vector<uint8_t>& bytes) {
    static constexpr char kDigits[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t at = 0;
    while (at + 3 <= bytes.size()) {
        const uint32_t block = (static_cast<uint32_t>(bytes[at]) << 16) |
                               (static_cast<uint32_t>(bytes[at + 1]) << 8) |
                               static_cast<uint32_t>(bytes[at + 2]);
        encoded.push_back(kDigits[(block >> 18) & 63U]);
        encoded.push_back(kDigits[(block >> 12) & 63U]);
        encoded.push_back(kDigits[(block >> 6) & 63U]);
        encoded.push_back(kDigits[block & 63U]);
        at += 3;
    }
    if (at < bytes.size()) {
        uint32_t block = static_cast<uint32_t>(bytes[at]) << 16;
        if (at + 1 < bytes.size())
            block |= static_cast<uint32_t>(bytes[at + 1]) << 8;
        encoded.push_back(kDigits[(block >> 18) & 63U]);
        encoded.push_back(kDigits[(block >> 12) & 63U]);
        if (at + 1 < bytes.size())
            encoded.push_back(kDigits[(block >> 6) & 63U]);
        else
            encoded.push_back('=');
        encoded.push_back('=');
    }
    return encoded;
}

/// Writes a tool result. A PNG is an image content block after the text.
///
/// @param reply the tool's answer
/// @return the result value
std::string tool_result(const ToolReply& reply) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("content");
    writer.begin_array();
    writer.begin_object();
    writer.key("type");
    writer.string("text");
    writer.key("text");
    writer.string(reply.text);
    writer.end_object();
    if (!reply.png.empty()) {
        writer.begin_object();
        writer.key("type");
        writer.string("image");
        writer.key("data");
        writer.string(base64_of(reply.png));
        writer.key("mimeType");
        writer.string("image/png");
        writer.end_object();
    }
    writer.end_array();
    writer.key("isError");
    writer.boolean(reply.failed);
    writer.end_object();
    return writer.text();
}

} // namespace

std::string parse_error_line(std::string_view message) {
    return error_line("null", parse_error, message);
}

Session::Session(SessionConfig config) : config_(std::move(config)) {
}

void Session::add_tool(Tool tool) {
    if (!tool_name_ok(tool.name))
        throw std::runtime_error("mcp: a tool name must be 1 to 128 of A-Z a-z 0-9 . _ -");
    if (tool.call == nullptr)
        throw std::runtime_error("mcp: a tool needs a call");
    if (!one_line(tool.input_schema))
        throw std::runtime_error("mcp: a tool schema must be one line");
    json::JsonError failure;
    std::optional<json::Json> parsed = json::parse_json(tool.input_schema, failure);
    if (!parsed || parsed->type() != json::JsonType::object)
        throw std::runtime_error("mcp: a tool schema must be one JSON object");
    for (const ToolEntry& entry : tools_)
        if (entry.tool.name == tool.name)
            throw std::runtime_error("mcp: a tool is already named " + tool.name);
    tools_.push_back(ToolEntry{std::move(tool), std::move(*parsed)});
}

void Session::add_resource(Resource resource) {
    if (resource.uri.empty())
        throw std::runtime_error("mcp: a resource needs a uri");
    if (resource.read == nullptr)
        throw std::runtime_error("mcp: a resource needs a read");
    for (const Resource& existing : resources_)
        if (existing.uri == resource.uri)
            throw std::runtime_error("mcp: a resource is already named " + resource.uri);
    resources_.push_back(std::move(resource));
}

std::vector<std::string> Session::take_queued() {
    std::vector<std::string> taken;
    taken.swap(queued_);
    return taken;
}

std::vector<std::string> Session::receive(std::string_view message) {
    if (blank_message(message))
        return {};
    json::JsonError failure;
    std::optional<json::Json> parsed = json::parse_json(message, failure);
    if (!parsed) {
        queued_.push_back(parse_error_line("not JSON"));
        return take_queued();
    }
    if (parsed->type() == json::JsonType::array) {
        queued_.push_back(error_line("null", invalid_request, "JSON-RPC batches are not accepted"));
        return take_queued();
    }
    if (parsed->type() != json::JsonType::object) {
        queued_.push_back(error_line("null", invalid_request, "not a JSON-RPC 2.0 request"));
        return take_queued();
    }
    std::string answer = answer_request(*parsed);
    if (!answer.empty())
        queued_.push_back(std::move(answer));
    return take_queued();
}

std::string Session::answer_request(const json::Json& message) {
    const RequestId id = read_id(message);
    if (id.invalid)
        return error_line("null", invalid_request, "not a JSON-RPC 2.0 request");
    const json::Json* version = message.find("jsonrpc");
    const json::Json* method = message.find("method");
    const bool version_ok =
        version != nullptr && version->string() != nullptr && *version->string() == "2.0";
    const bool method_ok = method != nullptr && method->string() != nullptr;
    if (!version_ok || !method_ok) {
        if (id.notification)
            return {};
        return error_line(id.json, invalid_request, "not a JSON-RPC 2.0 request");
    }
    const std::string& method_name = *method->string();
    const json::Json* params = message.find("params");
    if (params != nullptr && params->type() != json::JsonType::object) {
        if (id.notification)
            return {};
        return error_line(id.json, invalid_params, "params must be an object");
    }
    if (id.notification) {
        if (method_name == "notifications/initialized") {
            if (initialize_answered_)
                initialized_ = true;
            return {};
        }
        if (method_name == "notifications/cancelled")
            cancel(params);
        return {};
    }
    if (!initialized_ && method_name != "initialize" && method_name != "ping")
        return error_line(
            id.json, invalid_request, "notifications/initialized has not been received"
        );
    if (method_name == "initialize")
        return initialize(id.json, params);
    if (method_name == "ping")
        return result_line(id.json, "{}");
    if (method_name == "tools/list")
        return list_tools(id.json, params);
    if (method_name == "tools/call")
        return call_tool(id.json, params);
    if (method_name == "resources/list")
        return list_resources(id.json);
    if (method_name == "resources/read")
        return read_resource(id.json, params);
    if (method_name == "resources/subscribe")
        return subscribe_resource(id.json, params, true);
    if (method_name == "resources/unsubscribe")
        return subscribe_resource(id.json, params, false);
    return error_line(id.json, method_not_found, "unknown method " + method_name);
}

std::string Session::initialize(const std::string& id_json, const json::Json* params) {
    if (initialize_answered_)
        return error_line(id_json, invalid_request, "initialize has already been answered");
    negotiated_ = std::string(agreed_version(params));
    initialize_answered_ = true;
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("protocolVersion");
    writer.string(negotiated_);
    writer.key("capabilities");
    writer.begin_object();
    writer.key("tools");
    writer.begin_object();
    writer.key("listChanged");
    writer.boolean(false);
    writer.end_object();
    writer.key("resources");
    writer.begin_object();
    writer.key("subscribe");
    writer.boolean(true);
    writer.key("listChanged");
    writer.boolean(false);
    writer.end_object();
    writer.end_object();
    writer.key("serverInfo");
    writer.begin_object();
    writer.key("name");
    writer.string(config_.server_name);
    writer.key("version");
    writer.string(config_.server_version);
    writer.end_object();
    writer.key("instructions");
    writer.string(config_.instructions);
    writer.end_object();
    return result_line(id_json, writer.text());
}

bool Session::offered(const Tool& tool) const noexcept {
    return tool.reads || !config_.observe;
}

const Session::ToolEntry* Session::find_tool(std::string_view name) const noexcept {
    for (const ToolEntry& entry : tools_)
        if (entry.tool.name == name)
            return &entry;
    return nullptr;
}

const Resource* Session::find_resource(std::string_view uri) const noexcept {
    for (const Resource& resource : resources_)
        if (resource.uri == uri)
            return &resource;
    return nullptr;
}

std::string Session::list_tools(const std::string& id_json, const json::Json* params) {
    if (params != nullptr) {
        const json::Json* cursor = params->find("cursor");
        if (cursor != nullptr && cursor->string() != nullptr && !cursor->string()->empty())
            return error_line(id_json, invalid_params, "unknown cursor");
    }
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("tools");
    writer.begin_array();
    for (const ToolEntry& entry : tools_) {
        if (!offered(entry.tool))
            continue;
        writer.begin_object();
        writer.key("name");
        writer.string(entry.tool.name);
        writer.key("description");
        writer.string(entry.tool.description);
        writer.key("inputSchema");
        writer.raw(entry.tool.input_schema);
        writer.key("annotations");
        writer.begin_object();
        writer.key("readOnlyHint");
        writer.boolean(entry.tool.reads);
        writer.key("destructiveHint");
        writer.boolean(entry.tool.destructive);
        writer.key("idempotentHint");
        writer.boolean(entry.tool.idempotent);
        writer.end_object();
        writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    return result_line(id_json, writer.text());
}

void Session::remember_held(HeldCall held, const ToolReply& reply, std::vector<std::string>& out) {
    const bool tell =
        held.has_token && (!held.progress_sent || held.last_progress != reply.progress);
    if (tell) {
        held.progress_sent = true;
        held.last_progress = reply.progress;
        out.push_back(progress_line(held.token_json, reply));
    }
    held_.push_back(std::move(held));
}

void Session::cancel(const json::Json* params) {
    if (params == nullptr)
        return;
    const json::Json* request_id = params->find("requestId");
    if (request_id == nullptr)
        return;
    const std::string id_json = json_atom(*request_id);
    if (id_json.empty() && request_id->type() != json::JsonType::null)
        return;
    const std::string match = request_id->type() == json::JsonType::null ? "null" : id_json;
    std::vector<HeldCall> kept;
    kept.reserve(held_.size());
    for (HeldCall& held : held_) {
        if (held.id_json != match) {
            kept.push_back(std::move(held));
            continue;
        }
        if (held.tool < tools_.size()) {
            const Tool& tool = tools_[held.tool].tool;
            if (tool.cancel != nullptr)
                tool.cancel(tool.context);
        }
    }
    held_ = std::move(kept);
}

std::string Session::call_tool(const std::string& id_json, const json::Json* params) {
    const json::Json* name = params != nullptr ? params->find("name") : nullptr;
    if (name == nullptr || name->string() == nullptr || name->string()->empty())
        return error_line(id_json, invalid_params, "tools/call needs a name");
    const std::string& tool_name = *name->string();
    const ToolEntry* entry = find_tool(tool_name);
    if (entry == nullptr || !offered(entry->tool))
        return error_line(id_json, invalid_params, "unknown tool " + tool_name);
    const json::Json* given = params->find("arguments");
    const json::Json* arguments = &empty_object();
    if (given != nullptr) {
        if (given->type() != json::JsonType::object)
            return error_line(
                id_json,
                invalid_params,
                "arguments do not match the tool's schema: arguments must be an object"
            );
        arguments = given;
    }
    const std::string why = schema_rejection(entry->schema, *arguments);
    if (!why.empty())
        return error_line(
            id_json, invalid_params, "arguments do not match the tool's schema: " + why
        );
    if (config_.on_call != nullptr)
        config_.on_call(config_.call_context, entry->tool.name, entry->tool.reads);
    ToolReply reply;
    try {
        const ToolCall call{arguments, 0};
        reply = entry->tool.call(entry->tool.context, call);
    } catch (const std::exception& failure) {
        return error_line(id_json, internal_error, failure.what());
    } catch (...) {
        return error_line(id_json, internal_error, "the tool failed");
    }
    if (!reply.held)
        return result_line(id_json, tool_result(reply));
    if (held_.size() >= kMaxHeld)
        return error_line(id_json, internal_error, "too many held calls");
    HeldCall held;
    held.id_json = id_json;
    held.tool = static_cast<std::size_t>(entry - tools_.data());
    held.arguments = *arguments;
    held.poll = 0;
    if (params != nullptr) {
        const json::Json* meta = params->find("_meta");
        if (meta != nullptr && meta->type() == json::JsonType::object) {
            const json::Json* token = meta->find("progressToken");
            if (token != nullptr) {
                const std::string token_json = json_atom(*token);
                if (!token_json.empty()) {
                    held.has_token = true;
                    held.token_json = token_json;
                }
            }
        }
    }
    remember_held(std::move(held), reply, queued_);
    return {};
}

std::string Session::list_resources(const std::string& id_json) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("resources");
    writer.begin_array();
    for (const Resource& resource : resources_) {
        writer.begin_object();
        writer.key("uri");
        writer.string(resource.uri);
        writer.key("name");
        writer.string(resource.name);
        writer.key("description");
        writer.string(resource.description);
        writer.key("mimeType");
        writer.string(resource.mime_type);
        writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    return result_line(id_json, writer.text());
}

std::string Session::read_resource(const std::string& id_json, const json::Json* params) {
    const json::Json* uri = params != nullptr ? params->find("uri") : nullptr;
    if (uri == nullptr || uri->string() == nullptr || uri->string()->empty())
        return error_line(id_json, invalid_params, "resources/read needs a uri");
    const Resource* resource = find_resource(*uri->string());
    if (resource == nullptr)
        return error_line(id_json, invalid_params, "unknown resource " + *uri->string());
    std::string body;
    try {
        body = resource->read(resource->context);
    } catch (const std::exception& failure) {
        return error_line(id_json, internal_error, failure.what());
    } catch (...) {
        return error_line(id_json, internal_error, "the resource could not be read");
    }
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("contents");
    writer.begin_array();
    writer.begin_object();
    writer.key("uri");
    writer.string(resource->uri);
    writer.key("mimeType");
    writer.string(resource->mime_type);
    writer.key("text");
    writer.string(body);
    writer.end_object();
    writer.end_array();
    writer.end_object();
    return result_line(id_json, writer.text());
}

std::string
Session::subscribe_resource(const std::string& id_json, const json::Json* params, bool subscribe) {
    const json::Json* uri = params != nullptr ? params->find("uri") : nullptr;
    if (uri == nullptr || uri->string() == nullptr || uri->string()->empty())
        return error_line(
            id_json,
            invalid_params,
            subscribe ? "resources/subscribe needs a uri" : "resources/unsubscribe needs a uri"
        );
    const std::string& name = *uri->string();
    if (find_resource(name) == nullptr)
        return error_line(id_json, invalid_params, "unknown resource " + name);
    if (subscribe) {
        bool found = false;
        for (const std::string& existing : subscribed_)
            if (existing == name)
                found = true;
        if (!found)
            subscribed_.push_back(name);
    } else {
        std::vector<std::string> kept;
        for (const std::string& existing : subscribed_)
            if (existing != name)
                kept.push_back(existing);
        subscribed_ = std::move(kept);
    }
    return result_line(id_json, "{}");
}

void Session::enqueue(std::string notification) {
    if (!notification.empty())
        queued_.push_back(std::move(notification));
}

void Session::note_updated(std::string_view uri) {
    bool watching = false;
    for (const std::string& existing : subscribed_)
        if (existing == uri)
            watching = true;
    if (!watching)
        return;
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("jsonrpc");
    writer.string("2.0");
    writer.key("method");
    writer.string("notifications/resources/updated");
    writer.key("params");
    writer.begin_object();
    writer.key("uri");
    writer.string(uri);
    writer.end_object();
    writer.end_object();
    queued_.push_back(writer.text());
}

std::vector<std::string> Session::pump() {
    std::vector<std::string> out = take_queued();
    std::vector<HeldCall> still;
    still.reserve(held_.size());
    for (HeldCall& held : held_) {
        if (held.tool >= tools_.size() || tools_[held.tool].tool.call == nullptr) {
            out.push_back(error_line(held.id_json, internal_error, "the tool failed"));
            continue;
        }
        const Tool& tool = tools_[held.tool].tool;
        held.poll += 1;
        ToolReply reply;
        try {
            const ToolCall call{&held.arguments, held.poll};
            reply = tool.call(tool.context, call);
        } catch (const std::exception& failure) {
            out.push_back(error_line(held.id_json, internal_error, failure.what()));
            continue;
        } catch (...) {
            out.push_back(error_line(held.id_json, internal_error, "the tool failed"));
            continue;
        }
        if (!reply.held) {
            out.push_back(result_line(held.id_json, tool_result(reply)));
            continue;
        }
        if (held.has_token && (!held.progress_sent || held.last_progress != reply.progress)) {
            held.progress_sent = true;
            held.last_progress = reply.progress;
            out.push_back(progress_line(held.token_json, reply));
        }
        still.push_back(std::move(held));
    }
    held_ = std::move(still);
    const std::vector<std::string> more = take_queued();
    out.insert(out.end(), more.begin(), more.end());
    return out;
}

LineBuffer::LineBuffer(std::size_t max_bytes) : max_bytes_(max_bytes) {
}

void LineBuffer::append(std::string_view bytes) {
    pending_.append(bytes);
}

bool LineBuffer::take(FramedLine& line) {
    for (;;) {
        if (skipping_) {
            const std::size_t newline = pending_.find('\n');
            if (newline == std::string::npos) {
                pending_.clear();
                return false;
            }
            pending_.erase(0, newline + 1);
            skipping_ = false;
            continue;
        }
        const std::size_t newline = pending_.find('\n');
        if (newline == std::string::npos) {
            if (pending_.size() > max_bytes_) {
                pending_.clear();
                skipping_ = true;
                line.too_long = true;
                line.text.clear();
                return true;
            }
            return false;
        }
        if (newline > max_bytes_) {
            pending_.erase(0, newline + 1);
            line.too_long = true;
            line.text.clear();
            return true;
        }
        std::string text = pending_.substr(0, newline);
        pending_.erase(0, newline + 1);
        if (!text.empty() && text.back() == '\r')
            text.pop_back();
        line.too_long = false;
        line.text = std::move(text);
        return true;
    }
}

} // namespace oa::app::mcp

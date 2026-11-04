// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One MCP session: JSON-RPC 2.0 messages, the lifecycle, and the registries
// of tools and resources. A transport feeds one message at a time and writes
// what comes back. Nothing here reads a file or a socket, or waits: a call
// that is not finished is held, and pump advances held calls.
#pragma once

#include "oa/formats/json.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::mcp {

/// The protocol revisions this server speaks, newest first.
inline constexpr std::string_view protocol_2025_06_18 = "2025-06-18";
inline constexpr std::string_view protocol_2025_03_26 = "2025-03-26";
inline constexpr std::string_view protocol_2024_11_05 = "2024-11-05";

/// The most bytes of one message, newline not counted. A longer line is refused.
inline constexpr std::size_t max_message_bytes = 1U << 20;

/// JSON-RPC error codes.
inline constexpr int64_t parse_error = -32700;
inline constexpr int64_t invalid_request = -32600;
inline constexpr int64_t method_not_found = -32601;
inline constexpr int64_t invalid_params = -32602;
inline constexpr int64_t internal_error = -32603;

/// A JSON-RPC parse error with a null id: one object and no newline.
///
/// @param message why the message was refused
/// @return the error object
[[nodiscard]] std::string parse_error_line(std::string_view message);

/// One call of a tool, as tools/call gives it.
struct ToolCall {
    /// The arguments object. Never null: a call with no arguments has an empty object.
    const oa::formats::json::Json* arguments{};
    /// How many times this call has been polled while held. 0 on the first call.
    int64_t poll{};
};

/// What a tool answers. A held answer is polled again from pump until it finishes.
struct ToolReply {
    bool held{};                  ///< the call is not finished
    bool failed{};                ///< the tool ran and failed; text is the failure
    std::string text;             ///< the text content of the result
    int64_t progress{};           ///< how far a held call has got
    int64_t total{1};             ///< what progress counts toward
    std::string progress_message; ///< a short note sent with a progress notification
    /// A PNG sent as an image content block after the text. Empty sends none.
    std::vector<uint8_t> png;
};

/// One tool a client may list and call.
struct Tool {
    std::string name;         ///< dotted name, such as game.status
    std::string description;  ///< what the tool does, for tools/list
    std::string input_schema; ///< a JSON Schema object, one line
    bool reads{};             ///< a tool that only reads; an act tool changes the game
    bool destructive{};       ///< the tool ends or discards something
    bool idempotent{true};    ///< calling it again has the same effect
    void* context{};          ///< passed back to call
    /// Runs the tool. A held reply is polled from pump with a rising poll.
    ///
    /// @param context Tool::context
    /// @param call the arguments and the poll count
    /// @return the answer, held or finished
    ToolReply (*call)(void* context, const ToolCall& call){};
    /// Drops a held call of this tool. Null when a dropped call needs nothing dropped.
    ///
    /// @param context Tool::context
    void (*cancel)(void* context){};
};

/// One resource a client may list, read and subscribe to.
struct Resource {
    std::string uri;         ///< the resource's name, such as oa://game/status
    std::string name;        ///< a short name for resources/list
    std::string description; ///< what the resource holds
    std::string mime_type;   ///< the contents' media type
    void* context{};         ///< passed back to read
    /// Reads the resource's text.
    ///
    /// @param context Resource::context
    /// @return the text
    std::string (*read)(void* context){};
};

/// How a session is named and which tools it offers.
struct SessionConfig {
    std::string server_name{"open-annihilation"}; ///< serverInfo.name
    std::string server_version{"0"};              ///< serverInfo.version
    std::string instructions;                     ///< the initialize result's instructions
    bool observe{};                               ///< list and call only tools that read
    void* call_context{};                         ///< passed back to on_call
    /// Told when a tool call has been accepted, before the tool runs.
    ///
    /// @param context SessionConfig::call_context
    /// @param name the tool's name
    /// @param reads true when the tool only reads
    void (*on_call)(void* context, std::string_view name, bool reads){};
};

/// The registries and the lifecycle of one client.
class Session {
  public:

    /// Starts a session that has not been initialized.
    ///
    /// @param config the server's name, and whether only tools that read are offered
    explicit Session(SessionConfig config);

    /// Adds a tool. A schema that is not one JSON object, or a repeated name, throws
    /// std::runtime_error.
    ///
    /// @param tool the tool; its call is kept
    void add_tool(Tool tool);

    /// Adds a resource. An empty uri or a repeated one throws std::runtime_error.
    ///
    /// @param resource the resource; its read is kept
    void add_resource(Resource resource);

    /// Answers one JSON-RPC message.
    ///
    /// A notification answers nothing. A call that holds answers nothing yet
    /// and may answer a progress notification. Each string is one JSON object
    /// with no newline.
    ///
    /// @param message one JSON value, newline already removed
    /// @return the messages to write, in order
    [[nodiscard]] std::vector<std::string> receive(std::string_view message);

    /// Polls held calls and collects resource notifications.
    ///
    /// @return the messages to write, in order
    [[nodiscard]] std::vector<std::string> pump();

    /// Queues notifications/resources/updated when a client has subscribed to the uri.
    ///
    /// @param uri the resource that changed
    void note_updated(std::string_view uri);

    /// Queues one notification for the next pump to write.
    ///
    /// @param notification one JSON-RPC notification, with no newline
    void enqueue(std::string notification);

    /// Tells whether the client has finished the lifecycle.
    ///
    /// @return true after initialize was answered and notifications/initialized arrived
    [[nodiscard]] bool initialized() const noexcept { return initialized_; }

    /// Returns the revision initialize agreed, or empty before that.
    ///
    /// @return the revision
    [[nodiscard]] std::string_view negotiated_version() const noexcept { return negotiated_; }

  private:

    struct ToolEntry {
        Tool tool;
        oa::formats::json::Json schema;
    };

    struct HeldCall {
        std::string id_json;
        std::size_t tool{};
        oa::formats::json::Json arguments;
        bool has_token{};
        std::string token_json;
        bool progress_sent{};
        int64_t last_progress{};
        int64_t poll{};
    };

    /// Returns the notifications queued since the last take.
    ///
    /// @return the messages, in order
    [[nodiscard]] std::vector<std::string> take_queued();

    /// Answers one request object.
    ///
    /// A notification answers nothing. A request other than initialize or
    /// ping, before the lifecycle is finished, is an error.
    ///
    /// @param message the request object
    /// @return the answer, or empty for a notification or a held call
    [[nodiscard]] std::string answer_request(const oa::formats::json::Json& message);

    /// Answers initialize.
    ///
    /// @param id_json the request id, as JSON
    /// @param params the params object; null when they were omitted
    /// @return the result, or an error when initialize was already answered
    [[nodiscard]] std::string
    initialize(const std::string& id_json, const oa::formats::json::Json* params);

    /// Answers tools/list.
    ///
    /// @param id_json the request id, as JSON
    /// @param params the params object; null when they were omitted
    /// @return the tool list, or an error for an unknown cursor
    [[nodiscard]] std::string
    list_tools(const std::string& id_json, const oa::formats::json::Json* params);

    /// Answers tools/call.
    ///
    /// @param id_json the request id, as JSON
    /// @param params the params object; null when they were omitted
    /// @return the result, an error, or empty when the call is held
    [[nodiscard]] std::string
    call_tool(const std::string& id_json, const oa::formats::json::Json* params);

    /// Answers resources/list.
    ///
    /// @param id_json the request id, as JSON
    /// @return the resource list
    [[nodiscard]] std::string list_resources(const std::string& id_json);

    /// Answers resources/read.
    ///
    /// @param id_json the request id, as JSON
    /// @param params the params object; null when they were omitted
    /// @return the resource's text, or an error
    [[nodiscard]] std::string
    read_resource(const std::string& id_json, const oa::formats::json::Json* params);

    /// Answers resources/subscribe or resources/unsubscribe.
    ///
    /// @param id_json the request id, as JSON
    /// @param params the params object; null when they were omitted
    /// @param subscribe true to subscribe; false drops the subscription
    /// @return an empty result, or an error
    [[nodiscard]] std::string subscribe_resource(
        const std::string& id_json, const oa::formats::json::Json* params, bool subscribe
    );

    /// Finds a registered tool by name.
    ///
    /// @param name the tool's name
    /// @return the tool; null when none has that name
    [[nodiscard]] const ToolEntry* find_tool(std::string_view name) const noexcept;

    /// Finds a registered resource by uri.
    ///
    /// @param uri the resource's uri
    /// @return the resource; null when none has that uri
    [[nodiscard]] const Resource* find_resource(std::string_view uri) const noexcept;

    /// Tells whether this client may list and call a tool.
    ///
    /// A session opened with --mcp=observe offers only tools that read.
    ///
    /// @param tool the tool
    /// @return true when the tool is offered
    [[nodiscard]] bool offered(const Tool& tool) const noexcept;

    /// Keeps a held call, and queues a progress notification when the
    /// progress changed or this is the first time the call is held.
    ///
    /// @param held the call
    /// @param reply the tool's held answer
    /// @param[out] out gains the progress notification
    void remember_held(HeldCall held, const ToolReply& reply, std::vector<std::string>& out);

    /// Drops a held call that notifications/cancelled names.
    ///
    /// @param params the notification's params; null when they were omitted
    void cancel(const oa::formats::json::Json* params);

    SessionConfig config_;
    std::vector<ToolEntry> tools_;
    std::vector<Resource> resources_;
    std::vector<std::string> subscribed_;
    std::vector<HeldCall> held_;
    std::vector<std::string> queued_;
    std::string negotiated_;
    bool initialize_answered_{};
    bool initialized_{};
};

/// One line a byte stream has completed, or a line that was too long to keep.
struct FramedLine {
    bool too_long{};  ///< the line passed max_message_bytes and was discarded
    std::string text; ///< the line, without its newline; empty when too_long
};

/// Collects bytes into lines. A line is ended by a newline. A carriage return
/// before the newline is removed. Bytes past the limit are discarded until
/// the next newline, and that line is reported too long.
class LineBuffer {
  public:

    /// Starts an empty buffer.
    ///
    /// @param max_bytes the most bytes kept of one line, newline not counted
    explicit LineBuffer(std::size_t max_bytes = max_message_bytes);

    /// Appends bytes. Lines are taken with take.
    ///
    /// @param bytes the bytes just read
    void append(std::string_view bytes);

    /// Takes the next completed line.
    ///
    /// @param[out] line the line; unchanged when none is ready
    /// @return true when a line was taken
    [[nodiscard]] bool take(FramedLine& line);

  private:

    std::string pending_;
    std::size_t max_bytes_;
    bool skipping_{}; ///< discarding until the next newline
};

} // namespace oa::app::mcp

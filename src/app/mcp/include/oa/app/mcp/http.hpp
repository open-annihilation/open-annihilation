// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// MCP's Streamable HTTP transport (http.cpp): POST carries a request, and
// an answer that is held, or a notification, is a server-sent event. The
// listener is this machine's loopback address only. One client is served
// at a time. The address, the bearer token and the process id are written
// beside the preferences, readable only by the user that owns the file.
#pragma once

#include "oa/app/mcp/session.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app::mcp {

/// The address --mcp-http serves when it is given none: 127.0.0.1, at a
/// port the system chooses.
inline constexpr std::string_view default_listen_address = "127.0.0.1:0";

/// The file the address, token and process id are written to, in the folder
/// of the preferences file.
inline constexpr std::string_view token_file_name = "mcp.json";

/// The most bytes read from the client each time the endpoint is served.
/// The automation endpoint's read budget.
inline constexpr std::size_t read_budget_bytes = std::size_t{1} << 20;

/// The most time spent taking a request each time the endpoint is served,
/// in microseconds. The automation endpoint's request budget.
inline constexpr uint64_t request_budget_us = 1000;

/// How long a connection may take to send its request, in milliseconds.
/// The automation endpoint's limit for a first frame.
inline constexpr uint64_t request_timeout_ms = 10000;

/// The most bytes of headers one request may send.
inline constexpr std::size_t max_header_bytes = 64U * 1024U;

/// The most bytes of a request body. One JSON-RPC message is at most
/// max_message_bytes.
inline constexpr std::size_t max_body_bytes = max_message_bytes;

/// The loopback address a listener is bound to.
enum class HttpFamily : uint8_t {
    ipv4, ///< 127.0.0.1
    ipv6, ///< ::1
};

/// A loopback address and port --mcp-http listens on.
struct HttpListen {
    HttpFamily family{HttpFamily::ipv4};
    uint16_t port{}; ///< 0 lets the system choose one
};

/// Reads a listen address: 127.0.0.1:PORT or [::1]:PORT, PORT from 0 to 65535.
///
/// @param text the address
/// @param[out] listen the address read; unchanged when the text is refused
/// @return empty when the address was read; otherwise why it was refused
[[nodiscard]] std::string read_listen_address(std::string_view text, HttpListen& listen);

/// Writes a loopback address as the option spells it.
///
/// @param family the address
/// @param port the port
/// @return 127.0.0.1:PORT or [::1]:PORT
[[nodiscard]] std::string address_text(HttpFamily family, uint16_t port);

/// The URL a client posts to.
///
/// @param family the address
/// @param port the port
/// @return http://127.0.0.1:PORT/mcp or http://[::1]:PORT/mcp
[[nodiscard]] std::string endpoint_url(HttpFamily family, uint16_t port);

/// The JSON object written to the token file, and a newline.
///
/// @param url the URL a client posts to
/// @param token the bearer token
/// @param pid the process id
/// @return the object
[[nodiscard]] std::string
token_file_text(std::string_view url, std::string_view token, int64_t pid);

/// Writes a file readable only by its owner, by writing a file beside it
/// and renaming it into place.
///
/// @param file the file
/// @param text what it holds
void write_token_file(const std::filesystem::path& file, std::string_view text);

/// Tells whether an Origin header is this machine, or absent.
///
/// An empty origin is allowed. A browser's origin must be 127.0.0.1,
/// localhost or [::1]. Any other origin is refused.
///
/// @param origin the header's value; empty when the request had none
/// @return true when the request may proceed
[[nodiscard]] bool origin_allowed(std::string_view origin) noexcept;

/// Tells whether an Authorization header carries the run's bearer token.
///
/// @param authorization the header's value
/// @param token the run's token
/// @return true when the scheme is Bearer and the token matches
[[nodiscard]] bool bearer_matches(std::string_view authorization, std::string_view token) noexcept;

/// One server-sent event carrying a JSON-RPC message.
///
/// @param id the event's id, counted from 1 for the connection
/// @param json the message, one JSON value
/// @return the event, including its trailing blank line
[[nodiscard]] std::string sse_event(uint64_t id, std::string_view json);

/// One HTTP response with a body and a content length.
///
/// @param status the status code
/// @param reason the status text
/// @param content_type the body's media type; empty sends no Content-Type
/// @param body the body
/// @param extra header lines, each ending in CR LF; empty for none
/// @return the response, headers and body
[[nodiscard]] std::string response_bytes(
    int status,
    std::string_view reason,
    std::string_view content_type,
    std::string_view body,
    std::string_view extra
);

/// One HTTP request, read without waiting. append takes the bytes that
/// have arrived. complete is true once the headers and the body named by
/// Content-Length have arrived. A request with no Content-Length has an
/// empty body.
class HttpRequest {
  public:

    /// An empty request.
    HttpRequest() = default;

    /// Adds bytes that have arrived.
    ///
    /// Bytes after a finished or failed request are ignored. A body larger
    /// than max_body_bytes, or headers larger than max_header_bytes, fails
    /// the request as too large.
    ///
    /// @param bytes the bytes just read
    void append(std::string_view bytes);

    /// Tells whether the headers and the body have arrived.
    ///
    /// @return true once they have, and the request has not failed
    [[nodiscard]] bool complete() const noexcept { return finished_ && !failed_; }

    /// Tells whether the request was refused.
    ///
    /// @return true when the bytes are not a request this server reads
    [[nodiscard]] bool failed() const noexcept { return failed_; }

    /// Tells whether the request was refused for its size.
    ///
    /// @return true when the headers or the body are past the limit
    [[nodiscard]] bool too_large() const noexcept { return too_large_; }

    /// Tells whether bytes followed the body.
    ///
    /// @return true when the same read continued past Content-Length
    [[nodiscard]] bool trailing() const noexcept { return trailing_; }

    /// The request's method, as it was written.
    ///
    /// @return the method; empty until the request line has arrived
    [[nodiscard]] std::string_view method() const noexcept { return method_; }

    /// The request's target.
    ///
    /// @return the target; empty until the request line has arrived
    [[nodiscard]] std::string_view target() const noexcept { return target_; }

    /// The body.
    ///
    /// The view is into this request and is invalidated by append.
    ///
    /// @return the body; empty until the request is complete, and when it has none
    [[nodiscard]] std::string_view body() const noexcept;

    /// A header's value.
    ///
    /// The name is matched without regard to case. The view is into this
    /// request and is invalidated by append.
    ///
    /// @param name the header's name
    /// @return the value; empty when the request has no such header
    [[nodiscard]] std::string_view header(std::string_view name) const noexcept;

    /// Why a failed request was refused.
    ///
    /// @return the reason; empty when the request has not failed
    [[nodiscard]] std::string_view failure() const noexcept { return failure_; }

  private:

    /// Reads the request line and the headers.
    ///
    /// @param head the bytes before the blank line
    /// @return false when they are not a request
    [[nodiscard]] bool parse_head(std::string_view head);

    /// Marks the request failed.
    ///
    /// @param reason why
    /// @param large true when the request passed a size limit
    void fail(std::string_view reason, bool large = false);

    std::string buffer_;
    std::size_t header_end_{};
    std::size_t body_length_{};
    bool headers_ready_{};
    bool finished_{};
    bool failed_{};
    bool too_large_{};
    bool trailing_{};
    std::string method_;
    std::string target_;
    std::string failure_;
    std::vector<std::pair<std::string, std::string>> headers_;
};

/// The HTTP endpoint of one process: a loopback listener, one client, and
/// the token file. A pump never waits. Nothing is opened until open.
class HttpServer {
  public:

    /// An endpoint that is not listening and has opened no socket.
    HttpServer();

    /// Closes the listener and the client.
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    /// Starts listening and writes the token file.
    ///
    /// The file is created only after the listener is bound, and the
    /// listener is closed again when the file cannot be written. Throws
    /// std::runtime_error when no token can be made, the address cannot be
    /// served or the file cannot be written.
    ///
    /// @param family the loopback address
    /// @param port the port; 0 lets the system choose one
    /// @param file where to write the address, the token and the process id
    void open(HttpFamily family, uint16_t port, const std::filesystem::path& file);

    /// Tells whether the endpoint listens.
    ///
    /// @return true once open has bound a socket
    [[nodiscard]] bool listening() const noexcept;

    /// The port the listener is bound to.
    ///
    /// @return the port; 0 when the endpoint is not listening
    [[nodiscard]] uint16_t port() const noexcept;

    /// The URL written to the token file.
    ///
    /// The view is invalidated by the next open and by destruction.
    ///
    /// @return the URL; empty when the endpoint is not listening
    [[nodiscard]] std::string_view url() const noexcept;

    /// Tells whether a client connection is open.
    ///
    /// @return true while the one client has not been closed
    [[nodiscard]] bool client_connected() const noexcept;

    /// Tells whether a response is still being written.
    ///
    /// A held call keeps this true until its answer has been written. A
    /// stream of server events that is only listening does not.
    ///
    /// @return true when bytes, or a held answer, are still owed
    [[nodiscard]] bool response_pending() const noexcept;

    /// Takes a client's request to end the session.
    ///
    /// The session the next pump is given should be a new one. The flag is
    /// cleared either way.
    ///
    /// @return true when a DELETE was answered since the last take
    [[nodiscard]] bool take_session_reset() noexcept;

    /// Accepts, reads, answers and writes, without waiting.
    ///
    /// At most read_budget_bytes are read, and at most one request is
    /// answered, and only when that fits in request_budget_us. A second
    /// connection is answered that another client is connected, and closed,
    /// before the open stream is served, so a stream that finishes in this
    /// pump still refuses it. A connection that sends no request within
    /// request_timeout_ms is closed.
    ///
    /// @param[in,out] session the session that answers
    void pump(Session& session);

    /// Polls held calls and writes their answers. Does not read or accept.
    ///
    /// Used once a frame has been presented, so a picture of that frame can
    /// finish in the same frame that drew it. A call that is still held
    /// stays open. One that finishes is written on the stream that asked
    /// for it.
    ///
    /// @param[in,out] session the session that answers
    void deliver(Session& session);

    /// The endpoint's state. It is defined with the server, and a caller
    /// cannot see its members.
    struct State;

  private:

    std::unique_ptr<State> state_;
};

} // namespace oa::app::mcp

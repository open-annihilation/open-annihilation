// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// MCP's Streamable HTTP transport (http.hpp). The listener is opened only
// by open, which a run calls when --mcp-http was given. POST /mcp carries
// one JSON-RPC message. A notification is answered 202. A request whose
// answer is ready is application/json. A held answer, and any notification
// that travels with one, is a server-sent event on that POST, and a GET
// /mcp stays open for server events. A second connection while a client is
// connected is refused. The bearer token is the one written to the token file.
#include "oa/app/mcp/http.hpp"

#include "token.hpp"

#include "oa/formats/json.hpp"
#include "oa/netgame/stream_socket.hpp"
#include "oa/platform/files.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace oa::app::mcp {
namespace {

namespace fs = std::filesystem;
namespace json = oa::formats::json;
namespace sock = oa::netgame::sock;

constexpr std::size_t kReadChunk = 64U * 1024U;
/// A client that has not read is not given more, and is dropped, once this
/// much of an answer is still unsent.
constexpr std::size_t kOutCap = std::size_t{1} << 20;
/// One answer may be a picture. A full frame's stored PNG, written as
/// base64, fits in 16 MiB, and is queued while less than kOutCap is unsent.
constexpr std::size_t kAnswerCap = std::size_t{16} << 20;
/// A client with kOutCap or more unsent whose bytes have not moved for this
/// long is not reading, and is dropped. One pump without progress is not
/// enough: a picture larger than the socket buffer waits for the client to
/// drain it between frames.
constexpr uint64_t kStallMs = 10000;
constexpr int kAcceptsPerPump = 4;

/// Returns the steady clock in milliseconds.
///
/// @return the clock
uint64_t steady_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()
    )
                                     .count());
}

/// Returns the steady clock in microseconds.
///
/// @return the clock
uint64_t steady_us() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()
    )
                                     .count());
}

/// Returns this process's id.
///
/// @return the id
int64_t process_id() {
#ifdef _WIN32
    return static_cast<int64_t>(GetCurrentProcessId());
#else
    return static_cast<int64_t>(::getpid());
#endif
}

/// Writes a letter in lower case, when it is an ASCII capital.
///
/// @param letter the letter
/// @return the lower case letter, or the byte unchanged
char lower_ascii(char letter) noexcept {
    if (letter >= 'A' && letter <= 'Z')
        return static_cast<char>(letter - 'A' + 'a');
    return letter;
}

/// Trims spaces and tabs from both ends.
///
/// @param text the text
/// @return the trimmed text
std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

/// Tells whether a JSON-RPC message is an answer, not a notification.
///
/// @param message one JSON value
/// @return true when it carries an id and no method
bool final_response(std::string_view message) {
    json::JsonError failure;
    const std::optional<json::Json> value = json::parse_json(message, failure);
    if (!value || value->type() != json::JsonType::object)
        return true;
    if (value->find("method") != nullptr)
        return false;
    return value->find("id") != nullptr;
}

/// Tells whether a JSON-RPC message is a successful result.
///
/// @param message one JSON value
/// @return true when it carries a result
bool successful_result(std::string_view message) {
    json::JsonError failure;
    const std::optional<json::Json> value = json::parse_json(message, failure);
    return value && value->type() == json::JsonType::object && value->find("result") != nullptr;
}

/// Reads a request's method.
///
/// @param body the JSON-RPC message
/// @return the method; empty when the body is not a request object
std::string method_name(std::string_view body) {
    json::JsonError failure;
    const std::optional<json::Json> value = json::parse_json(body, failure);
    if (!value || value->type() != json::JsonType::object)
        return {};
    const json::Json* method = value->find("method");
    if (method == nullptr || method->string() == nullptr)
        return {};
    return *method->string();
}

/// Tells whether a request object has an id.
///
/// @param body the JSON-RPC message
/// @return true when it has an id
bool has_request_id(std::string_view body) {
    json::JsonError failure;
    const std::optional<json::Json> value = json::parse_json(body, failure);
    if (!value || value->type() != json::JsonType::object)
        return false;
    return value->find("id") != nullptr;
}

/// Tells whether a header list contains a media type.
///
/// @param header the Accept header
/// @param media the media type
/// @return true when the header lists it
bool accepts(std::string_view header, std::string_view media) noexcept {
    return header.find(media) != std::string_view::npos;
}

} // namespace

std::string read_listen_address(std::string_view text, HttpListen& listen) {
    constexpr std::string_view kIpv4 = "127.0.0.1";
    constexpr std::string_view kIpv6 = "[::1]";
    constexpr std::size_t kMaxPortDigits = 5;
    constexpr uint32_t kMaxPort = 65535;
    const std::size_t colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0)
        return "--mcp-http expects 127.0.0.1:PORT or [::1]:PORT";
    const std::string_view host = text.substr(0, colon);
    const std::string_view port_text = text.substr(colon + 1);
    HttpFamily family = HttpFamily::ipv4;
    if (host == kIpv4)
        family = HttpFamily::ipv4;
    else if (host == kIpv6)
        family = HttpFamily::ipv6;
    else
        return "mcp: only a loopback address can be served";
    uint32_t port = 0;
    const auto [end, problem] =
        std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (port_text.empty() || port_text.size() > kMaxPortDigits || problem != std::errc{} ||
        end != port_text.data() + port_text.size() || port > kMaxPort)
        return "--mcp-http expects a port from 0 through 65535";
    listen = {family, static_cast<uint16_t>(port)};
    return {};
}

std::string address_text(HttpFamily family, uint16_t port) {
    std::string text(family == HttpFamily::ipv6 ? "[::1]" : "127.0.0.1");
    text += ':';
    text += std::to_string(port);
    return text;
}

std::string endpoint_url(HttpFamily family, uint16_t port) {
    std::string url = "http://";
    url += address_text(family, port);
    url += "/mcp";
    return url;
}

std::string token_file_text(std::string_view url, std::string_view token, int64_t pid) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("address");
    writer.string(url);
    writer.key("token");
    writer.string(token);
    writer.key("pid");
    writer.integer(pid);
    writer.end_object();
    return writer.text() + "\n";
}

void write_token_file(const fs::path& file, std::string_view text) {
    std::error_code problem;
    if (file.has_parent_path())
        fs::create_directories(file.parent_path(), problem);
    fs::path partial = file;
    partial += "." + std::to_string(process_id()) + ".partial";
    bool written = false;
#ifdef _WIN32
    {
        std::ofstream out(partial, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        written = static_cast<bool>(out.flush());
    }
#else
    constexpr mode_t kOwnerReadWrite = S_IRUSR | S_IWUSR;
    const int out =
        ::open(partial.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, kOwnerReadWrite);
    if (out >= 0) {
        std::size_t done = 0;
        while (done < text.size()) {
            const ssize_t put = ::write(out, text.data() + done, text.size() - done);
            if (put <= 0)
                break;
            done += static_cast<std::size_t>(put);
        }
        written = done == text.size() && ::fchmod(out, kOwnerReadWrite) == 0;
        written = ::close(out) == 0 && written;
    }
#endif
    if (written) {
        fs::rename(partial, file, problem);
        written = !problem;
    }
    if (!written) {
        fs::remove(partial, problem);
        throw std::runtime_error("mcp: cannot write " + oa::platform::path_to_utf8(file));
    }
}

bool origin_allowed(std::string_view origin) noexcept {
    if (origin.empty())
        return true;
    const std::size_t scheme = origin.find("://");
    std::string_view host = origin;
    if (scheme != std::string_view::npos)
        host = origin.substr(scheme + 3);
    const std::size_t slash = host.find('/');
    if (slash != std::string_view::npos)
        host = host.substr(0, slash);
    if (host.starts_with('[')) {
        const std::size_t end = host.find(']');
        if (end == std::string_view::npos)
            return false;
        return host.substr(0, end + 1) == "[::1]";
    }
    const std::size_t colon = host.rfind(':');
    if (colon != std::string_view::npos)
        host = host.substr(0, colon);
    std::string lowered(host);
    for (char& letter : lowered)
        letter = lower_ascii(letter);
    return lowered == "127.0.0.1" || lowered == "localhost";
}

bool bearer_matches(std::string_view authorization, std::string_view token) noexcept {
    constexpr std::string_view kScheme = "bearer";
    if (authorization.size() < kScheme.size() + 1)
        return false;
    for (std::size_t index = 0; index < kScheme.size(); ++index) {
        if (lower_ascii(authorization[index]) != kScheme[index])
            return false;
    }
    if (authorization[kScheme.size()] != ' ')
        return false;
    return oa::app::automation::token_matches(authorization.substr(kScheme.size() + 1), token);
}

std::string sse_event(uint64_t id, std::string_view message) {
    std::string out = "id: ";
    out += std::to_string(id);
    out += "\r\nevent: message\r\n";
    std::size_t at = 0;
    while (at <= message.size()) {
        const std::size_t newline = message.find('\n', at);
        const std::size_t end = newline == std::string_view::npos ? message.size() : newline;
        std::string_view line = message.substr(at, end - at);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        out += "data: ";
        out += line;
        out += "\r\n";
        if (newline == std::string_view::npos)
            break;
        at = newline + 1;
    }
    out += "\r\n";
    return out;
}

std::string response_bytes(
    int status,
    std::string_view reason,
    std::string_view content_type,
    std::string_view body,
    std::string_view extra
) {
    std::string out = "HTTP/1.1 ";
    out += std::to_string(status);
    out += ' ';
    out += reason;
    out += "\r\n";
    if (!content_type.empty()) {
        out += "Content-Type: ";
        out += content_type;
        out += "\r\n";
    }
    out += "Content-Length: ";
    out += std::to_string(body.size());
    out += "\r\nConnection: close\r\n";
    out += extra;
    out += "\r\n";
    out += body;
    return out;
}

void HttpRequest::fail(std::string_view reason, bool large) {
    failed_ = true;
    too_large_ = large;
    failure_ = std::string(reason);
    finished_ = false;
}

bool HttpRequest::parse_head(std::string_view head) {
    const std::size_t line_end = head.find("\r\n");
    const std::string_view request_line =
        line_end == std::string_view::npos ? head : head.substr(0, line_end);
    const std::size_t first = request_line.find(' ');
    const std::size_t second = first == std::string_view::npos ? std::string_view::npos
                                                               : request_line.find(' ', first + 1);
    if (first == std::string_view::npos || second == std::string_view::npos ||
        request_line.find(' ', second + 1) != std::string_view::npos) {
        fail("bad request");
        return false;
    }
    method_ = std::string(request_line.substr(0, first));
    target_ = std::string(request_line.substr(first + 1, second - first - 1));
    const std::string_view version = request_line.substr(second + 1);
    if (method_.empty() || target_.empty() || (version != "HTTP/1.1" && version != "HTTP/1.0")) {
        fail("bad request");
        return false;
    }
    std::string_view rest =
        line_end == std::string_view::npos ? std::string_view{} : head.substr(line_end + 2);
    bool length_seen = false;
    while (!rest.empty()) {
        if (headers_.size() >= 64) {
            fail("bad request");
            return false;
        }
        const std::size_t next = rest.find("\r\n");
        const std::string_view line = next == std::string_view::npos ? rest : rest.substr(0, next);
        rest = next == std::string_view::npos ? std::string_view{} : rest.substr(next + 2);
        if (line.empty() || line.front() == ' ' || line.front() == '\t') {
            fail("bad request");
            return false;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0) {
            fail("bad request");
            return false;
        }
        std::string name(line.substr(0, colon));
        for (char& letter : name)
            letter = lower_ascii(letter);
        if (name == "content-length") {
            if (length_seen) {
                fail("bad request");
                return false;
            }
            length_seen = true;
        }
        headers_.emplace_back(std::move(name), std::string(trim(line.substr(colon + 1))));
    }
    return true;
}

void HttpRequest::append(std::string_view bytes) {
    if (failed_ || finished_ || bytes.empty())
        return;
    if (bytes.find('\0') != std::string_view::npos) {
        fail("bad request");
        return;
    }
    if (buffer_.size() + bytes.size() > max_header_bytes + max_body_bytes) {
        fail("the message is too large", true);
        return;
    }
    buffer_.append(bytes);
    if (!headers_ready_) {
        if (buffer_.size() > max_header_bytes && buffer_.find("\r\n\r\n") == std::string::npos) {
            fail("the message is too large", true);
            buffer_.clear();
            return;
        }
        const std::size_t end = buffer_.find("\r\n\r\n");
        if (end == std::string::npos)
            return;
        if (!parse_head(std::string_view(buffer_).substr(0, end)))
            return;
        header_end_ = end + 4;
        headers_ready_ = true;
        const std::string_view encoding = header("transfer-encoding");
        if (!encoding.empty()) {
            fail("chunked bodies are not accepted");
            return;
        }
        const std::string_view length = header("content-length");
        if (!length.empty()) {
            uint64_t parsed = 0;
            const auto [stop, problem] =
                std::from_chars(length.data(), length.data() + length.size(), parsed);
            if (problem != std::errc{} || stop != length.data() + length.size()) {
                fail("bad request");
                return;
            }
            if (parsed > max_body_bytes) {
                fail("the message is too large", true);
                buffer_.resize(header_end_);
                return;
            }
            body_length_ = static_cast<std::size_t>(parsed);
        }
    }
    if (buffer_.size() < header_end_)
        return;
    const std::size_t have = buffer_.size() - header_end_;
    if (have >= body_length_) {
        finished_ = true;
        trailing_ = have > body_length_;
    }
}

std::string_view HttpRequest::body() const noexcept {
    if (!finished_ || header_end_ > buffer_.size())
        return {};
    return std::string_view(buffer_).substr(header_end_, body_length_);
}

std::string_view HttpRequest::header(std::string_view name) const noexcept {
    std::string lowered(name);
    for (char& letter : lowered)
        letter = lower_ascii(letter);
    for (const auto& [key, value] : headers_) {
        if (key == lowered)
            return value;
    }
    return {};
}

struct HttpServer::State {
    /// One accepted connection and the bytes still to write to it.
    struct Conn {
        intptr_t fd{sock::invalid_socket};
        std::string outgoing;
        std::size_t sent{};
        HttpRequest request;
        bool answered{};
        bool sse{};
        bool close_when_flushed{};
        uint64_t opened_ms{};
        /// The steady clock when its queued bytes were queued or last moved.
        uint64_t moved_ms{};
        bool waiting{};
        /// The refusal's write side has been shut down, so the next pump closes it.
        bool half_closed{};

        Conn() = default;
        Conn(const Conn&) = delete;
        Conn& operator=(const Conn&) = delete;

        ~Conn() { sock::stream_close(&fd); }
    };

    intptr_t listener{sock::invalid_socket};
    uint16_t port{};
    std::string token;
    std::string session_id;
    std::string url;
    bool session_started{};
    bool reset_session{};
    uint64_t event_id{};
    std::unique_ptr<Conn> client;
    std::vector<std::unique_ptr<Conn>> refused;

    /// Closes the listener. Connections close with their own destructors.
    ~State() { sock::stream_close(&listener); }
};

namespace {

using Conn = HttpServer::State::Conn;

/// Forgets queued bytes and closes the connection once they have been written.
///
/// Nothing is left queued, so the next pump drops the connection.
///
/// @param conn the connection
void drop_queued(Conn& conn) {
    conn.close_when_flushed = true;
    conn.waiting = false;
    conn.outgoing.clear();
    conn.sent = 0;
}

/// Queues bytes for a connection.
///
/// A client that already has kOutCap unsent is not given more. One picture
/// may be larger than that, and is queued while less than kOutCap is unsent,
/// up to kAnswerCap.
///
/// @param conn the connection
/// @param bytes the bytes
/// @return false when they would pass the cap, and then nothing was queued
bool enqueue(Conn& conn, std::string_view bytes) {
    const std::size_t unsent = conn.outgoing.size() - conn.sent;
    if (unsent >= kOutCap)
        return false;
    if (bytes.size() > kAnswerCap - unsent)
        return false;
    if (unsent == 0)
        conn.moved_ms = steady_ms();
    if (conn.sent > 0 && conn.sent == conn.outgoing.size()) {
        conn.outgoing.clear();
        conn.sent = 0;
    }
    conn.outgoing.append(bytes);
    return true;
}

/// Reports whether a client is not reading: kOutCap or more is unsent and
/// none of it has moved for kStallMs.
///
/// @param conn the connection
/// @return true when the client should be dropped
bool stalled(const Conn& conn) {
    const std::size_t unsent = conn.outgoing.size() - conn.sent;
    return unsent >= kOutCap && steady_ms() - conn.moved_ms > kStallMs;
}

/// Writes queued bytes without waiting.
///
/// A client that is not reading (stalled) is dropped. A picture larger than
/// the socket buffer stays queued while the client keeps draining it.
///
/// @param conn the connection
/// @return false when the connection failed
bool flush(Conn& conn) {
    std::size_t wrote_total = 0;
    while (conn.sent < conn.outgoing.size()) {
        const std::string_view rest(
            conn.outgoing.data() + conn.sent, conn.outgoing.size() - conn.sent
        );
        const std::ptrdiff_t wrote =
            sock::stream_write(conn.fd, reinterpret_cast<const uint8_t*>(rest.data()), rest.size());
        if (wrote == 0)
            break;
        if (wrote < 0)
            return false;
        const auto advanced = static_cast<std::size_t>(wrote);
        conn.sent += advanced;
        wrote_total += advanced;
    }
    if (wrote_total > 0)
        conn.moved_ms = steady_ms();
    if (conn.sent >= conn.outgoing.size()) {
        conn.outgoing.clear();
        conn.sent = 0;
    } else if (stalled(conn)) {
        drop_queued(conn);
    }
    return true;
}

/// The headers that name the session, once initialize has been answered.
///
/// @param state the endpoint
/// @return the header lines, or empty before the session starts
std::string session_headers(const HttpServer::State& state) {
    if (!state.session_started || state.session_id.empty())
        return {};
    return "Mcp-Session-Id: " + state.session_id + "\r\n";
}

/// Queues a complete response and closes the connection once it is written.
///
/// @param conn the connection
/// @param status the status code
/// @param reason the status text
/// @param content_type the body's media type
/// @param body the body
/// @param extra header lines
void reply(
    Conn& conn,
    int status,
    std::string_view reason,
    std::string_view content_type,
    std::string_view body,
    std::string_view extra
) {
    conn.sse = false;
    conn.waiting = false;
    conn.close_when_flushed = true;
    conn.answered = true;
    (void)enqueue(conn, response_bytes(status, reason, content_type, body, extra));
}

/// Queues the status used when a request is refused before it is a message.
///
/// @param conn the connection
/// @param status the status code
/// @param reason the status text
/// @param body the body
/// @param extra header lines
void reply_text(
    Conn& conn, int status, std::string_view reason, std::string_view body, std::string_view extra
) {
    reply(conn, status, reason, "text/plain", body, extra);
}

/// Starts a server-sent event stream.
///
/// @param conn the connection
/// @param extra header lines
void begin_sse(Conn& conn, std::string_view extra) {
    conn.sse = true;
    conn.answered = true;
    conn.close_when_flushed = false;
    std::string headers = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n";
    headers += "Cache-Control: no-cache\r\nConnection: close\r\n";
    headers += extra;
    headers += "\r\n";
    (void)enqueue(conn, headers);
}

/// Writes one JSON-RPC message to the client, as JSON or as an event.
///
/// @param state the endpoint
/// @param conn the connection
/// @param message the message
/// @param as_event true to send an event on an open stream
void send_message(HttpServer::State& state, Conn& conn, std::string_view message, bool as_event) {
    if (!as_event) {
        reply(conn, 200, "OK", "application/json", message, session_headers(state));
        return;
    }
    if (!conn.sse)
        begin_sse(conn, session_headers(state));
    state.event_id += 1;
    if (!enqueue(conn, sse_event(state.event_id, message))) {
        // A client that is not reading is dropped. A notification that comes
        // while a picture is still being written is discarded. A final answer
        // that does not fit is not queued; bytes already accepted are still
        // written, and then the connection closes.
        const std::size_t unsent = conn.outgoing.size() - conn.sent;
        if (stalled(conn))
            drop_queued(conn);
        else if (unsent >= kOutCap && !final_response(message))
            return;
        else {
            conn.close_when_flushed = true;
            conn.waiting = false;
        }
        return;
    }
    if (final_response(message)) {
        conn.waiting = false;
        conn.close_when_flushed = true;
    }
}

/// Answers one complete request.
///
/// @param state the endpoint
/// @param conn the connection
/// @param session the session
void dispatch(HttpServer::State& state, Conn& conn, Session& session) {
    conn.answered = true;
    const HttpRequest& request = conn.request;
    if (request.too_large()) {
        reply_text(conn, 413, "Content Too Large", "the message is too large", {});
        return;
    }
    if (request.failed() || !request.complete()) {
        const std::string_view why = request.failure().empty() ? "bad request" : request.failure();
        reply_text(conn, 400, "Bad Request", why, {});
        return;
    }
    if (request.target() != "/mcp") {
        reply_text(conn, 404, "Not Found", "not found", {});
        return;
    }
    if (!origin_allowed(request.header("origin"))) {
        reply_text(conn, 403, "Forbidden", "origin is not allowed", {});
        return;
    }
    if (!bearer_matches(request.header("authorization"), state.token)) {
        reply_text(conn, 401, "Unauthorized", "unauthorized", "WWW-Authenticate: Bearer\r\n");
        return;
    }
    const std::string_view method = request.method();
    if (state.session_started && method != "DELETE") {
        const std::string_view given = request.header("mcp-session-id");
        if (given.empty()) {
            reply_text(conn, 400, "Bad Request", "Mcp-Session-Id is required", {});
            return;
        }
        if (given != state.session_id) {
            reply_text(conn, 404, "Not Found", "unknown session", {});
            return;
        }
    }
    if (method == "DELETE") {
        const std::string extra = session_headers(state);
        state.reset_session = true;
        state.session_started = false;
        reply(conn, 200, "OK", "application/json", "", extra);
        return;
    }
    if (method == "GET") {
        if (!accepts(request.header("accept"), "text/event-stream")) {
            reply_text(conn, 406, "Not Acceptable", "not acceptable", {});
            return;
        }
        begin_sse(conn, session_headers(state));
        return;
    }
    if (method != "POST") {
        reply_text(
            conn, 405, "Method Not Allowed", "method not allowed", "Allow: GET, POST, DELETE\r\n"
        );
        return;
    }
    if (request.header("content-length").empty()) {
        reply_text(conn, 400, "Bad Request", "Content-Length is required", {});
        return;
    }
    if (!accepts(request.header("content-type"), "application/json")) {
        reply_text(conn, 400, "Bad Request", "Content-Type must be application/json", {});
        return;
    }
    if (!accepts(request.header("accept"), "application/json") ||
        !accepts(request.header("accept"), "text/event-stream")) {
        reply_text(conn, 406, "Not Acceptable", "not acceptable", {});
        return;
    }
    const std::string_view body = request.body();
    const bool notification = !has_request_id(body);
    const std::string rpc_method = method_name(body);
    const std::vector<std::string> messages = session.receive(body);
    if (notification && messages.empty()) {
        reply(conn, 202, "Accepted", "", "", session_headers(state));
        return;
    }
    bool held = !notification;
    for (const std::string& message : messages) {
        if (final_response(message))
            held = false;
    }
    const bool stream = held || messages.size() != 1;
    if (!stream && !messages.empty()) {
        if (rpc_method == "initialize" && successful_result(messages[0]))
            state.session_started = true;
        send_message(state, conn, messages[0], false);
        return;
    }
    if (rpc_method == "initialize") {
        for (const std::string& message : messages) {
            if (successful_result(message))
                state.session_started = true;
        }
    }
    begin_sse(conn, session_headers(state));
    conn.waiting = held;
    for (const std::string& message : messages)
        send_message(state, conn, message, true);
    if (!held)
        conn.close_when_flushed = true;
}

/// Reads the client and answers one request, inside the budgets.
///
/// @param state the endpoint
/// @param session the session
/// @param started the steady clock at the start of this serve, in microseconds
void serve_client(HttpServer::State& state, Session& session, uint64_t started) {
    Conn& conn = *state.client;
    if (!conn.answered && !conn.sse && steady_ms() - conn.opened_ms > request_timeout_ms) {
        reply_text(conn, 408, "Request Timeout", "timed out", {});
    }
    std::size_t taken = 0;
    while (!conn.answered && taken < read_budget_bytes && !conn.request.complete() &&
           !conn.request.failed()) {
        uint8_t chunk[kReadChunk];
        const std::size_t room = std::min(kReadChunk, read_budget_bytes - taken);
        const std::ptrdiff_t got = sock::stream_read(conn.fd, chunk, room);
        if (got == 0)
            break;
        if (got == sock::stream_ended || got == sock::stream_failed || got < 0) {
            // A finished request can still be answered. Anything else is dropped.
            if (got == sock::stream_ended && conn.request.complete() && !conn.answered)
                break;
            conn.close_when_flushed = true;
            conn.waiting = false;
            conn.outgoing.clear();
            conn.sent = 0;
            conn.answered = true;
            return;
        }
        conn.request.append(
            std::string_view(reinterpret_cast<const char*>(chunk), static_cast<std::size_t>(got))
        );
        taken += static_cast<std::size_t>(got);
    }
    if (!conn.answered && (conn.request.complete() || conn.request.failed()) &&
        steady_us() - started < request_budget_us)
        dispatch(state, conn, session);
    if (conn.sse || conn.waiting) {
        const std::vector<std::string> pumped = session.pump();
        for (const std::string& message : pumped)
            send_message(state, conn, message, true);
    } else if (conn.answered) {
        (void)session.pump();
    }
    if (!flush(conn)) {
        conn.close_when_flushed = true;
        conn.waiting = false;
        conn.outgoing.clear();
        conn.sent = 0;
    }
}

/// Polls held calls onto the open stream. Does not read the client.
///
/// @param state the endpoint
/// @param session the session
void deliver_client(HttpServer::State& state, Session& session) {
    if (!state.client) {
        (void)session.pump();
        return;
    }
    Conn& conn = *state.client;
    if (conn.sse || conn.waiting) {
        const std::vector<std::string> pumped = session.pump();
        for (const std::string& message : pumped)
            send_message(state, conn, message, true);
    } else {
        (void)session.pump();
    }
    if (!flush(conn)) {
        conn.close_when_flushed = true;
        conn.waiting = false;
        conn.outgoing.clear();
        conn.sent = 0;
    }
}

/// Accepts connections waiting at the listener.
///
/// The first becomes the client. Any other is answered that a client is
/// already connected, and closed once that answer is written.
///
/// @param state the endpoint
void accept_waiting(HttpServer::State& state) {
    if (state.reset_session)
        return;
    for (int accepted = 0; accepted < kAcceptsPerPump; ++accepted) {
        const intptr_t fd = sock::stream_accept(state.listener);
        if (fd == sock::invalid_socket)
            break;
        if (!state.client) {
            state.client = std::make_unique<Conn>();
            state.client->fd = fd;
            state.client->opened_ms = steady_ms();
            continue;
        }
        auto refused = std::make_unique<Conn>();
        refused->fd = fd;
        refused->opened_ms = steady_ms();
        refused->close_when_flushed = true;
        refused->answered = true;
        (void)enqueue(
            *refused,
            response_bytes(409, "Conflict", "text/plain", "another client is connected", {})
        );
        state.refused.push_back(std::move(refused));
    }
}

/// Writes the refusals. One whose answer has been written is shut down and
/// kept until the next pump, so the close cannot discard the bytes.
///
/// @param state the endpoint
void flush_refused(HttpServer::State& state) {
    std::vector<std::unique_ptr<Conn>> kept;
    for (auto& refused : state.refused) {
        if (!refused)
            continue;
        // Shut down on an earlier pump: the peer has had a pump to read it.
        if (refused->half_closed)
            continue;
        if (!flush(*refused))
            continue;
        if (!refused->outgoing.empty()) {
            kept.push_back(std::move(refused));
            continue;
        }
        sock::stream_finish(refused->fd);
        refused->half_closed = true;
        kept.push_back(std::move(refused));
    }
    state.refused = std::move(kept);
}

} // namespace

HttpServer::HttpServer() : state_(std::make_unique<State>()) {
}

HttpServer::~HttpServer() = default;

void HttpServer::open(HttpFamily family, uint16_t port, const fs::path& file) {
    if (listening())
        return;
    const std::optional<std::string> token = oa::app::automation::make_token();
    const std::optional<std::string> session_id = oa::app::automation::make_token();
    if (!token || !session_id)
        throw std::runtime_error(
            "mcp: the system's random number generator cannot be read for the token"
        );
    char reason[160]{};
    uint16_t bound = 0;
    const auto loopback = family == HttpFamily::ipv6 ? sock::Loopback::ipv6 : sock::Loopback::ipv4;
    intptr_t listener = sock::stream_listen(loopback, port, bound, reason, sizeof reason);
    if (listener == sock::invalid_socket)
        throw std::runtime_error("mcp: cannot serve " + address_text(family, port) + ": " + reason);
    const std::string url = endpoint_url(family, bound);
    try {
        write_token_file(file, token_file_text(url, *token, process_id()));
    } catch (...) {
        sock::stream_close(&listener);
        throw;
    }
    state_->listener = listener;
    state_->port = bound;
    state_->token = *token;
    state_->session_id = *session_id;
    state_->url = url;
}

bool HttpServer::listening() const noexcept {
    return state_ && state_->listener != sock::invalid_socket;
}

uint16_t HttpServer::port() const noexcept {
    return state_ ? state_->port : 0;
}

std::string_view HttpServer::url() const noexcept {
    return state_ ? std::string_view(state_->url) : std::string_view{};
}

bool HttpServer::client_connected() const noexcept {
    return state_ && state_->client != nullptr;
}

bool HttpServer::response_pending() const noexcept {
    if (!state_ || !state_->client)
        return false;
    const State::Conn& conn = *state_->client;
    return conn.waiting || !conn.outgoing.empty();
}

bool HttpServer::take_session_reset() noexcept {
    if (!state_)
        return false;
    const bool reset = state_->reset_session;
    state_->reset_session = false;
    return reset;
}

void HttpServer::pump(Session& session) {
    if (!state_ || state_->listener == sock::invalid_socket)
        return;
    const uint64_t started = steady_us();
    // A second connection is refused while the open one is still the client.
    // Serving first would let a stream that finishes in this pump free the
    // slot, and the waiting connection would be taken and left unread.
    accept_waiting(*state_);
    if (state_->client) {
        serve_client(*state_, session, started);
        if (state_->client->close_when_flushed && state_->client->outgoing.empty() &&
            !state_->client->waiting)
            state_->client.reset();
    } else {
        (void)session.pump();
    }
    // A connection that arrived after the open one was dropped can be the
    // next client. One that was already waiting was refused above.
    accept_waiting(*state_);
    flush_refused(*state_);
}

void HttpServer::deliver(Session& session) {
    if (!state_ || state_->listener == sock::invalid_socket)
        return;
    deliver_client(*state_, session);
    if (state_->client && state_->client->close_when_flushed && state_->client->outgoing.empty() &&
        !state_->client->waiting)
        state_->client.reset();
    flush_refused(*state_);
}

} // namespace oa::app::mcp

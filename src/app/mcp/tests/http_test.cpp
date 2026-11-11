// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Streamable HTTP with no game: framing, the bearer token and its file,
// and a second connection refused while one client is connected.
#include "oa/app/mcp/http.hpp"
#include "oa/formats/json.hpp"
#include "oa/netgame/stream_socket.hpp"
#include "oa/test/check.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;
namespace json = oa::formats::json;
namespace mcp = oa::app::mcp;
namespace sock = oa::netgame::sock;

constexpr uint64_t kStepMs = 2000;

/// Returns the steady clock in milliseconds.
///
/// @return the clock
uint64_t steady_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
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

/// A connection closed when the test leaves it. A move clears the source.
struct Client {
    intptr_t fd{sock::invalid_socket};
    std::string got;

    Client() = default;

    explicit Client(intptr_t opened) : fd(opened) {}

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    Client(Client&& other) noexcept : fd(other.fd), got(std::move(other.got)) {
        other.fd = sock::invalid_socket;
    }

    Client& operator=(Client&& other) noexcept {
        if (this != &other) {
            sock::stream_close(&fd);
            fd = other.fd;
            other.fd = sock::invalid_socket;
            got = std::move(other.got);
        }
        return *this;
    }

    ~Client() { sock::stream_close(&fd); }
};

/// A scratch folder named with this ticket and this process, removed at the end.
struct Scratch {
    fs::path dir;

    Scratch() {
        dir = fs::temp_directory_path() / ("M03-mcp-" + std::to_string(process_id()));
        fs::remove_all(dir);
        fs::create_directories(dir);
    }

    ~Scratch() {
        std::error_code problem;
        fs::remove_all(dir, problem);
    }
};

/// Builds 127.0.0.1 and a port.
///
/// @param port the port, in host order
/// @return the address
sock::Address loopback(uint16_t port) {
    sock::Address address{};
    address.ip[0] = 127;
    address.ip[1] = 0;
    address.ip[2] = 0;
    address.ip[3] = 1;
    address.port = port;
    return address;
}

/// Opens a connection to the endpoint.
///
/// @param port the endpoint's port
/// @return the connection; invalid when it did not open
Client connect_to(uint16_t port) {
    char reason[160]{};
    Client client(sock::stream_connect(loopback(port), reason, sizeof reason));
    OA_CHECK(client.fd != sock::invalid_socket);
    if (client.fd == sock::invalid_socket)
        return client;
    OA_CHECK(
        sock::stream_wait_open(client.fd, static_cast<uint32_t>(kStepMs)) == sock::StreamOpen::open
    );
    return client;
}

/// Writes every byte, without spinning past the step limit.
///
/// @param client the connection
/// @param bytes the bytes
/// @return false when they were not all written in time
bool write_all(Client& client, std::string_view bytes) {
    std::size_t sent = 0;
    const uint64_t start = steady_ms();
    while (sent < bytes.size()) {
        if (steady_ms() - start > kStepMs)
            return false;
        const std::ptrdiff_t wrote = sock::stream_write(
            client.fd, reinterpret_cast<const uint8_t*>(bytes.data() + sent), bytes.size() - sent
        );
        if (wrote < 0)
            return false;
        if (wrote == 0) {
            sock::StreamWaitEntry wait{};
            wait.stream = client.fd;
            wait.write = true;
            (void)sock::stream_wait(&wait, 1, 20);
            continue;
        }
        sent += static_cast<std::size_t>(wrote);
    }
    return true;
}

/// Reads what has arrived.
///
/// @param[in,out] client keeps the bytes
void take_bytes(Client& client) {
    uint8_t chunk[4096];
    for (;;) {
        const std::ptrdiff_t got = sock::stream_read(client.fd, chunk, sizeof chunk);
        if (got <= 0)
            return;
        client.got.append(reinterpret_cast<const char*>(chunk), static_cast<std::size_t>(got));
    }
}

/// Pumps until the predicate holds or the step limit passes.
///
/// @param server the endpoint
/// @param session the session
/// @param client the connection being read
/// @param ready tells whether enough has arrived
/// @return true when the predicate held
template <typename Ready>
bool pump_until(mcp::HttpServer& server, mcp::Session& session, Client& client, Ready ready) {
    const uint64_t start = steady_ms();
    while (steady_ms() - start <= kStepMs) {
        server.pump(session);
        take_bytes(client);
        if (ready())
            return true;
        sock::StreamWaitEntry wait{};
        wait.stream = client.fd;
        wait.read = true;
        (void)sock::stream_wait(&wait, 1, 20);
    }
    take_bytes(client);
    return ready();
}

/// A header's value. The name is matched without regard to case.
///
/// @param response the bytes read so far
/// @param name the header
/// @return the value; empty when the headers have not arrived or omit it
std::string header_of(std::string_view response, std::string_view name) {
    const std::size_t end = response.find("\r\n\r\n");
    if (end == std::string_view::npos)
        return {};
    std::string_view head = response.substr(0, end);
    const std::size_t line = head.find("\r\n");
    if (line == std::string_view::npos)
        return {};
    head.remove_prefix(line + 2);
    while (!head.empty()) {
        const std::size_t next = head.find("\r\n");
        std::string_view one = next == std::string_view::npos ? head : head.substr(0, next);
        head = next == std::string_view::npos ? std::string_view{} : head.substr(next + 2);
        const std::size_t colon = one.find(':');
        if (colon == std::string_view::npos || colon != name.size())
            continue;
        bool same = true;
        for (std::size_t index = 0; index < name.size(); ++index) {
            char letter = one[index];
            if (letter >= 'A' && letter <= 'Z')
                letter = static_cast<char>(letter - 'A' + 'a');
            if (letter != name[index])
                same = false;
        }
        if (!same)
            continue;
        std::string_view value = one.substr(colon + 1);
        while (!value.empty() && value.front() == ' ')
            value.remove_prefix(1);
        return std::string(value);
    }
    return {};
}

/// Tells whether a response with a content length has all arrived.
///
/// @param response the bytes read so far
/// @return true when the headers and that many body bytes are present
bool body_ready(std::string_view response) {
    const std::size_t end = response.find("\r\n\r\n");
    if (end == std::string_view::npos)
        return false;
    const std::string length_text = header_of(response, "content-length");
    if (length_text.empty())
        return false;
    std::size_t length = 0;
    const auto [stop, problem] =
        std::from_chars(length_text.data(), length_text.data() + length_text.size(), length);
    if (problem != std::errc{} || stop != length_text.data() + length_text.size())
        return false;
    return response.size() >= end + 4 + length;
}

/// The body of a complete response.
///
/// @param response the response
/// @return the body; empty when it has not all arrived
std::string body_of(std::string_view response) {
    if (!body_ready(response))
        return {};
    const std::size_t end = response.find("\r\n\r\n");
    const std::string length_text = header_of(response, "content-length");
    std::size_t length = 0;
    const auto [stop, problem] =
        std::from_chars(length_text.data(), length_text.data() + length_text.size(), length);
    if (problem != std::errc{} || stop != length_text.data() + length_text.size())
        return {};
    return std::string(response.substr(end + 4, length));
}

/// The status code of a response.
///
/// @param response the response
/// @return the code; 0 when the status line has not arrived
int status_of(std::string_view response) {
    const std::size_t line = response.find("\r\n");
    const std::string_view status_line =
        line == std::string_view::npos ? response : response.substr(0, line);
    const std::size_t first = status_line.find(' ');
    if (first == std::string_view::npos)
        return 0;
    const std::size_t second = status_line.find(' ', first + 1);
    const std::string_view code = second == std::string_view::npos
                                      ? status_line.substr(first + 1)
                                      : status_line.substr(first + 1, second - first - 1);
    int status = 0;
    const auto [stop, problem] = std::from_chars(code.data(), code.data() + code.size(), status);
    if (problem != std::errc{} || stop != code.data() + code.size())
        return 0;
    return status;
}

/// One POST of a JSON-RPC message.
///
/// @param token the bearer token
/// @param session_id the session id; empty sends none
/// @param message the JSON-RPC message
/// @return the request
std::string post(std::string_view token, std::string_view session_id, std::string_view message) {
    std::string out = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    out += "Content-Type: application/json\r\n";
    out += "Accept: application/json, text/event-stream\r\n";
    out += "Authorization: Bearer ";
    out += token;
    out += "\r\n";
    if (!session_id.empty()) {
        out += "Mcp-Session-Id: ";
        out += session_id;
        out += "\r\n";
    }
    out += "Content-Length: ";
    out += std::to_string(message.size());
    out += "\r\n\r\n";
    out += message;
    return out;
}

/// Sends one request and pumps until its body has arrived.
///
/// @param server the endpoint
/// @param session the session
/// @param token the bearer token
/// @param session_id the session id; empty sends none
/// @param message the JSON-RPC message
/// @return the connection, holding the response
Client exchange(
    mcp::HttpServer& server,
    mcp::Session& session,
    std::string_view token,
    std::string_view session_id,
    std::string_view message
) {
    Client client = connect_to(server.port());
    if (client.fd == sock::invalid_socket)
        return client;
    OA_CHECK(write_all(client, post(token, session_id, message)));
    OA_CHECK(pump_until(server, session, client, [&client] { return body_ready(client.got); }));
    return client;
}

/// Holds until the test releases the one-client slot.
///
/// The call stays held through every pump, so a slow second connection is
/// still refused. The test sets the bool only after that refusal.
///
/// @param context a bool the test owns; true once the second client has been answered
/// @return held, then the text finished
mcp::ToolReply hold_tool(void* context, const mcp::ToolCall& /*call*/) {
    const auto* released = static_cast<const bool*>(context);
    if (released == nullptr || !*released) {
        mcp::ToolReply reply;
        reply.held = true;
        return reply;
    }
    mcp::ToolReply reply;
    reply.text = "finished";
    return reply;
}

/// A request split across reads, a trailing byte, a large body and a bad line.
void check_framing() {
    mcp::HttpRequest request;
    request.append("POST /mcp HTTP/1.1\r\nHost: loopback\r\n");
    OA_CHECK(!request.complete());
    request.append("Content-Length: 2\r\nContent-Type: application/json\r\n\r\n");
    OA_CHECK(!request.complete());
    request.append("hiEXTRA");
    OA_CHECK(request.complete());
    OA_CHECK(request.trailing());
    OA_CHECK(request.body() == "hi");
    OA_CHECK(request.method() == "POST");
    OA_CHECK(request.target() == "/mcp");
    OA_CHECK(request.header("content-type") == "application/json");
    OA_CHECK(request.header("Content-Type") == "application/json");

    mcp::HttpRequest bad;
    bad.append("NOT A REQUEST\r\n\r\n");
    OA_CHECK(bad.failed());
    OA_CHECK(bad.failure() == "bad request");

    mcp::HttpRequest huge;
    huge.append(
        "POST /mcp HTTP/1.1\r\nContent-Length: " + std::to_string(mcp::max_body_bytes + 1) +
        "\r\n\r\n"
    );
    OA_CHECK(huge.too_large());
    OA_CHECK(huge.failure() == "the message is too large");

    mcp::HttpRequest chunked;
    chunked.append("POST /mcp HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
    OA_CHECK(chunked.failed());
    OA_CHECK(chunked.failure() == "chunked bodies are not accepted");

    mcp::HttpRequest doubled;
    doubled.append("POST /mcp HTTP/1.1\r\nContent-Length: 0\r\nContent-Length: 0\r\n\r\n");
    OA_CHECK(doubled.failed());

    const std::string event = mcp::sse_event(1, "{\"id\":1}");
    OA_CHECK(event.find("id: 1\r\n") != std::string::npos);
    OA_CHECK(event.find("event: message\r\n") != std::string::npos);
    OA_CHECK(event.find("data: {\"id\":1}\r\n") != std::string::npos);

    const std::string response = mcp::response_bytes(202, "Accepted", "", "", {});
    OA_CHECK(response.find("HTTP/1.1 202 Accepted\r\n") == 0);
    OA_CHECK(response.find("Content-Length: 0\r\n") != std::string::npos);
    OA_CHECK(response.find("Connection: close\r\n") != std::string::npos);
}

/// The bearer comparison and the origins this machine allows.
void check_token_and_origin() {
    OA_CHECK(mcp::bearer_matches("Bearer secret", "secret"));
    OA_CHECK(mcp::bearer_matches("bearer secret", "secret"));
    OA_CHECK(!mcp::bearer_matches("Bearer  secret", "secret"));
    OA_CHECK(!mcp::bearer_matches("Basic secret", "secret"));
    OA_CHECK(!mcp::bearer_matches("Bearer", "secret"));
    OA_CHECK(!mcp::bearer_matches("", "secret"));

    OA_CHECK(mcp::origin_allowed(""));
    OA_CHECK(mcp::origin_allowed("http://127.0.0.1:9"));
    OA_CHECK(mcp::origin_allowed("http://localhost"));
    OA_CHECK(mcp::origin_allowed("http://LocalHost:80"));
    OA_CHECK(mcp::origin_allowed("http://[::1]:9"));
    OA_CHECK(!mcp::origin_allowed("null"));
    OA_CHECK(!mcp::origin_allowed("http://example.com"));
    OA_CHECK(!mcp::origin_allowed("http://10.0.0.1"));
}

/// The token file is one object, mode 0600 where the system has modes, and
/// no partial file is left beside it.
void check_token_file(const Scratch& scratch) {
    const fs::path file = scratch.dir / "written.json";
    const std::string text = mcp::token_file_text("http://127.0.0.1:9/mcp", "secret", 7);
    mcp::write_token_file(file, text);
    json::JsonError failure;
    const std::optional<json::Json> value = json::parse_json(text, failure);
    OA_CHECK(value.has_value());
    if (!value)
        return;
    const json::Json* address = value->find("address");
    const json::Json* token = value->find("token");
    const json::Json* pid = value->find("pid");
    OA_CHECK(
        address != nullptr && address->string() && *address->string() == "http://127.0.0.1:9/mcp"
    );
    OA_CHECK(token != nullptr && token->string() && *token->string() == "secret");
    OA_CHECK(pid != nullptr && pid->integer() && *pid->integer() == 7);
#ifndef _WIN32
    struct stat info{};
    OA_CHECK(::stat(file.c_str(), &info) == 0);
    OA_CHECK((info.st_mode & 0777) == 0600);
#endif
    int files = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(scratch.dir)) {
        (void)entry;
        files += 1;
    }
    OA_CHECK(files == 1);
}

/// An endpoint that was not opened listens on nothing.
void check_idle() {
    mcp::HttpServer server;
    OA_CHECK(!server.listening());
    OA_CHECK(server.port() == 0);
    OA_CHECK(server.url().empty());
    OA_CHECK(!server.client_connected());
    OA_CHECK(!server.response_pending());
    OA_CHECK(!server.take_session_reset());
    mcp::SessionConfig config;
    mcp::Session session(std::move(config));
    server.pump(session);
    OA_CHECK(!server.listening());
}

/// initialize, a wrong token, a held call that refuses a second client,
/// and a later client accepted once the first has gone.
void check_server(const Scratch& scratch) {
    bool released = false;
    mcp::SessionConfig config;
    config.server_name = "open-annihilation";
    config.server_version = "0";
    mcp::Session session(std::move(config));
    mcp::Tool tool;
    tool.name = "hold";
    tool.description = "holds";
    tool.input_schema = "{\"type\":\"object\",\"additionalProperties\":false}";
    tool.reads = true;
    tool.context = &released;
    tool.call = hold_tool;
    session.add_tool(std::move(tool));

    mcp::HttpServer server;
    const fs::path file = scratch.dir / std::string(mcp::token_file_name);
    server.open(mcp::HttpFamily::ipv4, 0, file);
    OA_CHECK(server.listening());
    OA_CHECK(server.port() != 0);
    OA_CHECK(server.url().find("/mcp") != std::string_view::npos);

    std::string recorded;
    {
        std::ifstream in(file);
        recorded.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    json::JsonError failure;
    const std::optional<json::Json> saved = json::parse_json(recorded, failure);
    OA_CHECK(saved.has_value());
    if (!saved)
        return;
    const json::Json* token_value = saved->find("token");
    OA_CHECK(token_value != nullptr && token_value->string() != nullptr);
    if (token_value == nullptr || token_value->string() == nullptr)
        return;
    const std::string token = *token_value->string();

    {
        Client refused = exchange(
            server,
            session,
            "nope",
            {},
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
            "\"protocolVersion\":\"2025-06-18\"}}"
        );
        OA_CHECK(status_of(refused.got) == 401);
        OA_CHECK(header_of(refused.got, "www-authenticate") == "Bearer");
    }
    OA_CHECK(!server.client_connected());

    std::string session_id;
    {
        Client opened = exchange(
            server,
            session,
            token,
            {},
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
            "\"protocolVersion\":\"2025-06-18\"}}"
        );
        OA_CHECK(status_of(opened.got) == 200);
        session_id = header_of(opened.got, "mcp-session-id");
        OA_CHECK(!session_id.empty());
        OA_CHECK(session_id != token);
        const std::string body = body_of(opened.got);
        OA_CHECK(body.find("\"result\"") != std::string::npos);
        OA_CHECK(body.find("2025-06-18") != std::string::npos);
    }

    {
        Client noted = exchange(
            server,
            session,
            token,
            session_id,
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}"
        );
        OA_CHECK(status_of(noted.got) == 202);
    }
    OA_CHECK(session.initialized());

    // A tool call that breaks its schema is a JSON-RPC error in HTTP 200.
    // The 2025-06-18 transport answers a request as one JSON object; invalid
    // arguments are -32602, not an HTTP error.
    {
        Client refused_schema = exchange(
            server,
            session,
            token,
            session_id,
            "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{"
            "\"name\":\"hold\",\"arguments\":{\"repeat\":40}}}"
        );
        OA_CHECK(status_of(refused_schema.got) == 200);
        const std::string schema_body = body_of(refused_schema.got);
        OA_CHECK(schema_body.find("\"code\":-32602") != std::string::npos);
        OA_CHECK(schema_body.find("arguments do not match the tool's schema") != std::string::npos);
    }

    Client held = connect_to(server.port());
    OA_CHECK(write_all(
        held,
        post(
            token,
            session_id,
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/"
            "call\",\"params\":{\"name\":\"hold\"}}"
        )
    ));
    OA_CHECK(pump_until(server, session, held, [&held] {
        return held.got.find("text/event-stream") != std::string::npos;
    }));
    OA_CHECK(server.client_connected());

    Client second = connect_to(server.port());
    OA_CHECK(pump_until(server, session, second, [&second] {
        return second.got.find("another client is connected") != std::string::npos;
    }));
    OA_CHECK(status_of(second.got) == 409);
    OA_CHECK(second.got.find("another client is connected") != std::string::npos);
    released = true;

    OA_CHECK(pump_until(server, session, held, [&held] {
        return held.got.find("finished") != std::string::npos;
    }));
    const uint64_t start = steady_ms();
    while (server.client_connected() && steady_ms() - start <= kStepMs)
        server.pump(session);
    OA_CHECK(!server.client_connected());

    {
        Client again = exchange(
            server, session, token, session_id, "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\"}"
        );
        OA_CHECK(status_of(again.got) == 200);
        OA_CHECK(body_of(again.got).find("\"result\"") != std::string::npos);
    }
    {
        Client unknown = exchange(
            server,
            session,
            token,
            "not-the-session",
            "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"ping\"}"
        );
        OA_CHECK(status_of(unknown.got) == 404);
        OA_CHECK(body_of(unknown.got).find("unknown session") != std::string::npos);
    }
}

/// Holds until the caller says the frame has been presented.
///
/// @param context a bool; true once the presented stage has begun
/// @return held, then the text presented
mcp::ToolReply present_tool(void* context, const mcp::ToolCall& /*call*/) {
    const auto* later = static_cast<const bool*>(context);
    if (later == nullptr || !*later) {
        mcp::ToolReply reply;
        reply.held = true;
        return reply;
    }
    mcp::ToolReply reply;
    reply.text = "presented";
    return reply;
}

/// A call that stays held through pump is answered by deliver.
///
/// @param scratch the folder the token file is written in
void check_presented(const Scratch& scratch) {
    bool presented = false;
    mcp::SessionConfig config;
    config.server_name = "open-annihilation";
    config.server_version = "0";
    mcp::Session session(std::move(config));
    mcp::Tool tool;
    tool.name = "present";
    tool.description = "holds until the frame is presented";
    tool.input_schema = "{\"type\":\"object\",\"additionalProperties\":false}";
    tool.reads = true;
    tool.context = &presented;
    tool.call = present_tool;
    session.add_tool(std::move(tool));

    mcp::HttpServer server;
    const fs::path file = scratch.dir / "present.json";
    server.open(mcp::HttpFamily::ipv4, 0, file);
    OA_CHECK(server.listening());

    std::string recorded;
    {
        std::ifstream in(file);
        recorded.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    json::JsonError failure;
    const std::optional<json::Json> saved = json::parse_json(recorded, failure);
    OA_CHECK(saved.has_value());
    if (!saved)
        return;
    const json::Json* token_value = saved->find("token");
    OA_CHECK(token_value != nullptr && token_value->string() != nullptr);
    if (token_value == nullptr || token_value->string() == nullptr)
        return;
    const std::string token = *token_value->string();

    std::string session_id;
    {
        Client opened = exchange(
            server,
            session,
            token,
            {},
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
            "\"protocolVersion\":\"2025-06-18\"}}"
        );
        session_id = header_of(opened.got, "mcp-session-id");
        OA_CHECK(!session_id.empty());
    }
    {
        Client noted = exchange(
            server,
            session,
            token,
            session_id,
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}"
        );
        OA_CHECK(status_of(noted.got) == 202);
    }

    Client held = connect_to(server.port());
    OA_CHECK(write_all(
        held,
        post(
            token,
            session_id,
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/"
            "call\",\"params\":{\"name\":\"present\"}}"
        )
    ));
    OA_CHECK(pump_until(server, session, held, [&held] {
        return held.got.find("text/event-stream") != std::string::npos;
    }));
    server.pump(session);
    take_bytes(held);
    OA_CHECK(held.got.find("presented") == std::string::npos);
    OA_CHECK(server.client_connected());

    presented = true;
    const uint64_t start = steady_ms();
    while (held.got.find("presented") == std::string::npos && steady_ms() - start <= kStepMs) {
        server.deliver(session);
        take_bytes(held);
        if (held.got.find("presented") != std::string::npos)
            break;
        sock::StreamWaitEntry wait{};
        wait.stream = held.fd;
        wait.read = true;
        (void)sock::stream_wait(&wait, 1, 20);
    }
    OA_CHECK(held.got.find("presented") != std::string::npos);
}

/// A picture large enough that its server-sent event is past 1 MiB.
///
/// The bytes start with a PNG signature, so the image block's base64 starts
/// with the signature's text.
struct PictureHold {
    bool ready{};
    std::vector<uint8_t> png;
};

/// Holds until the caller says the picture is ready, then returns it.
///
/// @param context a PictureHold
/// @return held, then the picture
mcp::ToolReply picture_tool(void* context, const mcp::ToolCall& /*call*/) {
    auto* held = static_cast<PictureHold*>(context);
    if (held == nullptr || !held->ready) {
        mcp::ToolReply reply;
        reply.held = true;
        return reply;
    }
    mcp::ToolReply reply;
    reply.text = "{\"width\":1,\"height\":1,\"source\":\"presented\"}";
    reply.png = held->png;
    return reply;
}

/// A held picture larger than 1 MiB is written on the POST that asked.
///
/// @param scratch the folder the token file is written in
void check_screenshot(const Scratch& scratch) {
    PictureHold picture;
    // 800 KiB of PNG bytes is about 1.04 MiB once base64 and the event
    // framing are added, which is past the old 1 MiB send cap.
    picture.png.assign(800U * 1024U, 0);
    const uint8_t signature[] = {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a};
    for (std::size_t index = 0; index < sizeof signature; ++index)
        picture.png[index] = signature[index];

    mcp::SessionConfig config;
    config.server_name = "open-annihilation";
    config.server_version = "0";
    mcp::Session session(std::move(config));
    mcp::Tool tool;
    tool.name = "capture.screenshot";
    tool.description = "holds until the picture is ready";
    tool.input_schema = "{\"type\":\"object\",\"additionalProperties\":false}";
    tool.reads = true;
    tool.context = &picture;
    tool.call = picture_tool;
    session.add_tool(std::move(tool));

    mcp::HttpServer server;
    const fs::path file = scratch.dir / "picture.json";
    server.open(mcp::HttpFamily::ipv4, 0, file);
    OA_CHECK(server.listening());

    std::string recorded;
    {
        std::ifstream in(file);
        recorded.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    json::JsonError failure;
    const std::optional<json::Json> saved = json::parse_json(recorded, failure);
    OA_CHECK(saved.has_value());
    if (!saved)
        return;
    const json::Json* token_value = saved->find("token");
    OA_CHECK(token_value != nullptr && token_value->string() != nullptr);
    if (token_value == nullptr || token_value->string() == nullptr)
        return;
    const std::string token = *token_value->string();

    std::string session_id;
    {
        Client opened = exchange(
            server,
            session,
            token,
            {},
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{"
            "\"protocolVersion\":\"2025-06-18\"}}"
        );
        session_id = header_of(opened.got, "mcp-session-id");
        OA_CHECK(!session_id.empty());
    }
    {
        Client noted = exchange(
            server,
            session,
            token,
            session_id,
            "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}"
        );
        OA_CHECK(status_of(noted.got) == 202);
    }

    Client held = connect_to(server.port());
    OA_CHECK(write_all(
        held,
        post(
            token,
            session_id,
            "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/"
            "call\",\"params\":{\"name\":\"capture.screenshot\"}}"
        )
    ));
    OA_CHECK(pump_until(server, session, held, [&held] {
        return held.got.find("text/event-stream") != std::string::npos;
    }));
    OA_CHECK(status_of(held.got) == 200);
    OA_CHECK(header_of(held.got, "content-type") == "text/event-stream");
    OA_CHECK(held.got.find("iVBORw0KGgo") == std::string::npos);
    OA_CHECK(server.client_connected());

    picture.ready = true;
    constexpr uint64_t kPictureMs = 10000;
    const uint64_t start = steady_ms();
    // The signature is near the start of the event. Keep reading until the
    // event itself is past 1 MiB, which is what the old send cap dropped, and
    // its type has arrived: on a busy machine the type can come after both.
    while (steady_ms() - start <= kPictureMs) {
        server.deliver(session);
        take_bytes(held);
        if (held.got.find("iVBORw0KGgo") != std::string::npos &&
            held.got.size() > (std::size_t{1} << 20) &&
            held.got.find("image/png") != std::string::npos)
            break;
        sock::StreamWaitEntry wait{};
        wait.stream = held.fd;
        wait.read = true;
        (void)sock::stream_wait(&wait, 1, 20);
    }
    OA_CHECK(status_of(held.got) == 200);
    OA_CHECK(header_of(held.got, "content-type") == "text/event-stream");
    OA_CHECK(held.got.find("image/png") != std::string::npos);
    OA_CHECK(held.got.find("iVBORw0KGgo") != std::string::npos);
    OA_CHECK(held.got.size() > (std::size_t{1} << 20));
}

} // namespace

int main() {
    check_framing();
    check_token_and_origin();
    const Scratch scratch;
    check_token_file(scratch);
    check_idle();
    check_server(scratch);
    check_presented(scratch);
    check_screenshot(scratch);
    return oa::test::check_exit_status();
}

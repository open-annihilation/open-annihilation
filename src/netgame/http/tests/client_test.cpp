// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The HTTP client against the in-process fixture. Every address is
// 127.0.0.1, or a name a test resolver maps to it.

#include "oa/base/threads.hpp"
#include "oa/netgame/http/client.hpp"
#include "oa/netgame/http_fixture/server.hpp"
#include "oa/netgame/stream_socket.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#if defined(OA_HTTP_TLS_OPENSSL)
#include <dlfcn.h>
#endif

namespace {

using oa::netgame::http::BodySink;
using oa::netgame::http::Client;
using oa::netgame::http::ClientOptions;
using oa::netgame::http::Failure;
using oa::netgame::http::Header;
using oa::netgame::http::Method;
using oa::netgame::http::Progress;
using oa::netgame::http::Request;
using oa::netgame::http::Response;
using oa::netgame::http_fixture::LoggedRequest;
using oa::netgame::http_fixture::Reply;
using oa::netgame::http_fixture::Server;
namespace sock = oa::netgame::sock;

/// The cancel flag a held lookup sets, so the fetch can walk away.
std::atomic<bool> lookup_cancel{false};

/// Prints the case the run is on.
///
/// @param name the case
void say(const char* name) {
    std::printf("%s\n", name);
    std::fflush(stdout);
}

/// Copies text into bytes.
///
/// @param text the text
/// @return the bytes
std::vector<uint8_t> bytes_of(std::string_view text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

/// True when the body is that text.
///
/// @param body the body
/// @param text the expected text
/// @return true when every byte matches
bool is_text(const std::vector<uint8_t>& body, std::string_view text) {
    return body.size() == text.size() &&
           std::equal(body.begin(), body.end(), reinterpret_cast<const uint8_t*>(text.data()));
}

/// A request for one address, with a total limit so a hang ends.
///
/// @param url the address
/// @return the request
Request get(std::string url) {
    Request request;
    request.url = std::move(url);
    request.timeouts.connect_ms = 2000;
    request.timeouts.idle_ms = 2000;
    request.timeouts.total_ms = 10000;
    return request;
}

/// Milliseconds since a start.
///
/// @param start the start
/// @return the elapsed milliseconds
int64_t since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - start
    )
        .count();
}

/// Prints a failure that was not the one a case wanted.
///
/// @param name the case
/// @param response the outcome
/// @param wanted the failure the case wanted
void note_failure(const char* name, const Response& response, Failure wanted) {
    if (response.failure == wanted)
        return;
    std::fprintf(
        stderr,
        "%s: got %s (%s) status %d, wanted %s\n",
        name,
        oa::netgame::http::failure_text(response.failure),
        response.detail.c_str(),
        response.status,
        oa::netgame::http::failure_text(wanted)
    );
}

/// Counts requests of one method.
///
/// @param log the request log
/// @param method the method
/// @return how many match
int count_method(const std::vector<LoggedRequest>& log, std::string_view method) {
    int count = 0;
    for (const LoggedRequest& item : log) {
        if (item.method == method)
            ++count;
    }
    return count;
}

/// True when the log holds that target.
///
/// @param log the request log
/// @param target the target
/// @return true when one request named it
bool saw_target(const std::vector<LoggedRequest>& log, std::string_view target) {
    for (const LoggedRequest& item : log) {
        if (item.target == target)
            return true;
    }
    return false;
}

/// A sink that records whether the final response arrived.
class FlagSink final : public BodySink {
  public:

    /// Records the head.
    ///
    /// @param head the final response's head
    /// @return begin_ok
    bool begin(const Response&) override {
        began = true;
        return begin_ok;
    }

    /// Records a piece.
    ///
    /// @param piece the piece
    /// @return write_ok
    bool write(std::span<const uint8_t> piece) override {
        wrote += piece.size();
        return write_ok;
    }

    bool began = false;
    bool begin_ok = true;
    bool write_ok = true;
    std::size_t wrote = 0;
};

/// Progress a fetch reported.
struct ProgressNote {
    int calls = 0;
    bool any_expected = false;
    uint64_t last = 0;
};

/// Records one progress call.
///
/// @param context the note
/// @param progress the progress
void note_progress(void* context, const Progress& progress) {
    auto* note = static_cast<ProgressNote*>(context);
    ++note->calls;
    if (progress.expected.has_value())
        note->any_expected = true;
    note->last = progress.received;
}

/// Sets the lookup cancel flag from the first progress call.
///
/// @param context the cancel flag
void cancel_on_progress(void* context, const Progress&) {
    static_cast<std::atomic<bool>*>(context)->store(true);
}

/// Maps allowed.test to 127.0.0.1 and refuses every other name.
///
/// @param host the host
/// @param[out] ip the address
/// @return true for allowed.test
bool map_allowed(const char* host, uint8_t ip[4]) {
    if (std::strcmp(host, "allowed.test") != 0)
        return false;
    ip[0] = 127;
    ip[1] = 0;
    ip[2] = 0;
    ip[3] = 1;
    return true;
}

/// Sets the cancel flag, then waits, so a lookup can be walked away from.
///
/// @param host the host
/// @param[out] ip the address, written after the wait
/// @return true
bool hold_name(const char*, uint8_t ip[4]) {
    lookup_cancel.store(true);
    for (int step = 0; step < 100; ++step)
        oa::base::threads::sleep_ms(100);
    ip[0] = 127;
    ip[1] = 0;
    ip[2] = 0;
    ip[3] = 1;
    return true;
}

/// Listens, closes, and returns the port, so a connect is refused.
///
/// @return the port, or 0 when the listener could not be opened
uint16_t closed_port() {
    uint16_t bound = 0;
    char error[256] = {};
    const intptr_t listener =
        sock::stream_listen(sock::Loopback::ipv4, 0, bound, error, sizeof error);
    if (listener == sock::invalid_socket)
        return 0;
    intptr_t fd = listener;
    sock::stream_close(&fd);
    return bound;
}

} // namespace

int main() {
    say("get by length");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.serve_bytes("/doc", bytes_of("hello"));
        ProgressNote note;
        Request request = get(server.url("/doc"));
        request.progress = note_progress;
        request.progress_context = &note;
        std::vector<uint8_t> body;
        Client client;
        const Response response = client.fetch(request, body);
        note_failure("get by length", response, Failure::none);
        OA_CHECK(response.failure == Failure::none);
        OA_CHECK(response.status == 200);
        OA_CHECK(is_text(body, "hello"));
        OA_CHECK(response.content_length.has_value() && *response.content_length == 5);
        OA_CHECK(response.body_bytes == 5);
        OA_CHECK(note.calls >= 1);
        OA_CHECK(note.any_expected);
        OA_CHECK(note.last == 5);
    }

    say("get chunked");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        Reply reply;
        reply.chunked = true;
        reply.chunk_bytes = 2;
        reply.body = bytes_of("abcdef");
        server.override_next("/parts", reply);
        std::vector<uint8_t> body;
        Client client;
        const Response response = client.fetch(get(server.url("/parts")), body);
        note_failure("get chunked", response, Failure::none);
        OA_CHECK(response.failure == Failure::none);
        OA_CHECK(response.status == 200);
        OA_CHECK(is_text(body, "abcdef"));
        OA_CHECK(!response.content_length.has_value());
    }

    say("get until close");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        Reply reply;
        reply.no_length = true;
        reply.body = bytes_of("stream");
        server.override_next("/stream", reply);
        std::vector<uint8_t> body;
        Client client;
        const Response response = client.fetch(get(server.url("/stream")), body);
        note_failure("get until close", response, Failure::none);
        OA_CHECK(response.failure == Failure::none);
        OA_CHECK(is_text(body, "stream"));
        OA_CHECK(!response.content_length.has_value());
    }

    say("head");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.serve_bytes("/doc", bytes_of("hello"));
        Request request = get(server.url("/doc"));
        request.method = Method::head;
        std::vector<uint8_t> body;
        Client client;
        const Response response = client.fetch(request, body);
        note_failure("head", response, Failure::none);
        OA_CHECK(response.failure == Failure::none);
        OA_CHECK(response.status == 200);
        OA_CHECK(body.empty());
        OA_CHECK(response.body_bytes == 0);
        OA_CHECK(response.content_length.has_value() && *response.content_length == 5);
    }

    say("post bytes");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.handle("POST", "/echo", [](const LoggedRequest& logged) {
            Reply reply;
            reply.body = logged.body;
            return reply;
        });
        Request request = get(server.url("/echo"));
        request.method = Method::post;
        request.body = bytes_of("payload");
        std::vector<uint8_t> body;
        Client client;
        const Response response = client.fetch(request, body);
        note_failure("post bytes", response, Failure::none);
        OA_CHECK(response.failure == Failure::none);
        OA_CHECK(is_text(body, "payload"));
        const std::vector<LoggedRequest> log = server.requests();
        OA_CHECK(log.size() == 1);
        OA_CHECK(log[0].method == "POST");
        OA_CHECK(is_text(log[0].body, "payload"));
        const std::string* type = log[0].header("Content-Type");
        OA_CHECK(type != nullptr && *type == "application/octet-stream");
    }

    say("keep-alive two gets");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.serve_bytes("/a", bytes_of("one"));
        server.serve_bytes("/b", bytes_of("two"));
        Client client;
        std::vector<uint8_t> body;
        const Response first = client.fetch(get(server.url("/a")), body);
        const Response second = client.fetch(get(server.url("/b")), body);
        note_failure("keep-alive two gets", second, Failure::none);
        OA_CHECK(first.failure == Failure::none);
        OA_CHECK(second.failure == Failure::none);
        OA_CHECK(is_text(body, "two"));
        OA_CHECK(server.connections_accepted() == 1);
    }

    say("idle close retried");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.set_idle_close_ms(400);
        server.serve_bytes("/doc", bytes_of("ok"));
        Client client;
        std::vector<uint8_t> body;
        const Response first = client.fetch(get(server.url("/doc")), body);
        const Response second = client.fetch(get(server.url("/doc")), body);
        OA_CHECK(first.failure == Failure::none);
        OA_CHECK(second.failure == Failure::none);
        OA_CHECK(server.connections_accepted() == 1);
        oa::base::threads::sleep_ms(1000);
        const Response third = client.fetch(get(server.url("/doc")), body);
        note_failure("idle close retried", third, Failure::none);
        OA_CHECK(third.failure == Failure::none);
        OA_CHECK(is_text(body, "ok"));
        OA_CHECK(server.connections_accepted() == 2);
    }

    say("connection close");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        Reply reply;
        reply.close = true;
        reply.body = bytes_of("bye");
        server.override_next("/end", reply, 2);
        Client client;
        std::vector<uint8_t> body;
        const Response first = client.fetch(get(server.url("/end")), body);
        const Response second = client.fetch(get(server.url("/end")), body);
        OA_CHECK(first.failure == Failure::none);
        OA_CHECK(second.failure == Failure::none);
        OA_CHECK(is_text(body, "bye"));
        OA_CHECK(server.connections_accepted() == 2);
    }

    say("post not retried");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.serve_bytes("/pool", bytes_of("ok"));
        Reply raw;
        raw.raw = std::vector<uint8_t>{};
        server.override_next("/post", raw);
        Client client;
        std::vector<uint8_t> body;
        const Response pooled = client.fetch(get(server.url("/pool")), body);
        OA_CHECK(pooled.failure == Failure::none);
        Request request = get(server.url("/post"));
        request.method = Method::post;
        request.body = bytes_of("once");
        const Response response = client.fetch(request, body);
        note_failure("post not retried", response, Failure::connection_lost);
        OA_CHECK(response.failure == Failure::connection_lost);
        OA_CHECK(server.connections_accepted() == 1);
        OA_CHECK(count_method(server.requests(), "POST") == 1);
    }

    say("range 206");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.serve_bytes("/file", bytes_of("0123456789"));
        Request request = get(server.url("/file"));
        request.range_from = 4;
        std::vector<uint8_t> body;
        Client client;
        const Response response = client.fetch(request, body);
        note_failure("range 206", response, Failure::none);
        OA_CHECK(response.failure == Failure::none);
        OA_CHECK(response.status == 206);
        OA_CHECK(is_text(body, "456789"));
        OA_CHECK(response.content_range.has_value());
        OA_CHECK(response.content_range->first == 4);
        OA_CHECK(response.content_range->last == 9);
        OA_CHECK(response.content_range->total.has_value() && *response.content_range->total == 10);
        const std::vector<LoggedRequest> log = server.requests();
        OA_CHECK(log.size() == 1);
        const std::string* range = log[0].header("Range");
        OA_CHECK(range != nullptr && *range == "bytes=4-");
    }

    say("range ignored 200");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.set_ranges(false);
        server.serve_bytes("/file", bytes_of("0123456789"));
        Request request = get(server.url("/file"));
        request.range_from = 4;
        std::vector<uint8_t> body;
        Client client;
        const Response response = client.fetch(request, body);
        note_failure("range ignored 200", response, Failure::none);
        OA_CHECK(response.failure == Failure::none);
        OA_CHECK(response.status == 200);
        OA_CHECK(!response.content_range.has_value());
        OA_CHECK(is_text(body, "0123456789"));
    }

    say("range 416");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.serve_bytes("/file", bytes_of("0123456789"));
        Request request = get(server.url("/file"));
        request.range_from = 50;
        std::vector<uint8_t> body;
        Client client;
        const Response response = client.fetch(request, body);
        note_failure("range 416", response, Failure::none);
        OA_CHECK(response.failure == Failure::none);
        OA_CHECK(response.status == 416);
        OA_CHECK(body.empty());
        OA_CHECK(!response.content_range.has_value());
    }

    say("range 206 wrong start");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        Reply reply;
        reply.status = 206;
        reply.body = bytes_of("0123");
        reply.headers.push_back({"Content-Range", "bytes 0-3/10"});
        server.override_next("/file", reply);
        Request request = get(server.url("/file"));
        request.range_from = 4;
        FlagSink sink;
        Client client;
        const Response response = client.fetch(request, sink);
        note_failure("range 206 wrong start", response, Failure::bad_response);
        OA_CHECK(response.failure == Failure::bad_response);
        OA_CHECK(response.status == 206);
        OA_CHECK(!sink.began);
    }

    say("redirect same host");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.serve_bytes("/item", bytes_of("box"));
        server.redirect("/old", "/item");
        std::vector<uint8_t> body;
        Client client;
        const Response response = client.fetch(get(server.url("/old")), body);
        note_failure("redirect same host", response, Failure::none);
        OA_CHECK(response.failure == Failure::none);
        OA_CHECK(response.status == 200);
        OA_CHECK(is_text(body, "box"));
        OA_CHECK(response.final_url == server.url("/item"));
        OA_CHECK(server.requests().size() == 2);
    }

    say("redirect allowed host");
    {
        Server origin;
        Server other;
        std::string error;
        OA_CHECK(origin.start(&error));
        OA_CHECK(other.start(&error));
        other.serve_bytes("/item", bytes_of("yes"));
        origin.redirect("/go", "http://allowed.test:" + std::to_string(other.port()) + "/item");
        ClientOptions options;
        options.resolve = map_allowed;
        Client client(options);
        Request request = get(origin.url("/go"));
        request.redirects.allowed_hosts.push_back("allowed.test");
        std::vector<uint8_t> body;
        const Response response = client.fetch(request, body);
        note_failure("redirect allowed host", response, Failure::none);
        OA_CHECK(response.failure == Failure::none);
        OA_CHECK(is_text(body, "yes"));
        OA_CHECK(
            response.final_url == "http://allowed.test:" + std::to_string(other.port()) + "/item"
        );
        OA_CHECK(other.connections_accepted() == 1);
    }

    say("redirect refused off list");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.redirect("/go", "http://other.test/x");
        FlagSink sink;
        Client client;
        const Response response = client.fetch(get(server.url("/go")), sink);
        note_failure("redirect refused off list", response, Failure::redirect_refused);
        OA_CHECK(response.failure == Failure::redirect_refused);
        OA_CHECK(response.status == 302);
        OA_CHECK(!sink.began);
        OA_CHECK(response.final_url == server.url("/go"));
        OA_CHECK(server.connections_accepted() == 1);
        OA_CHECK(!saw_target(server.requests(), "/x"));
    }

    say("redirect https read as http when tls is off");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.serve_bytes("/x", bytes_of("yes"));
        const std::string https = "https://127.0.0.1:" + std::to_string(server.port()) + "/x";
        server.redirect("/go", https);
        ClientOptions options;
        options.tls = false;
        Client client(options);
        std::vector<uint8_t> body;
        const Response response = client.fetch(get(server.url("/go")), body);
        note_failure("redirect https read as http when tls is off", response, Failure::none);
        OA_CHECK(response.failure == Failure::none);
        OA_CHECK(is_text(body, "yes"));
        OA_CHECK(response.final_url == "http://127.0.0.1:" + std::to_string(server.port()) + "/x");
    }

    say("redirect https is not the plain body when tls is on");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.serve_bytes("/x", bytes_of("yes"));
        const std::string https = "https://127.0.0.1:" + std::to_string(server.port()) + "/x";
        server.redirect("/go", https);
        ClientOptions options;
        options.tls = true;
        Client client(options);
        std::vector<uint8_t> body;
        const Response response = client.fetch(get(server.url("/go")), body);
        OA_CHECK(response.failure != Failure::none);
        OA_CHECK(!is_text(body, "yes"));
        OA_CHECK(response.final_url.find("https://") != std::string::npos);
    }

    say("redirect past five");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        for (int hop = 0; hop < 6; ++hop)
            server.redirect("/r" + std::to_string(hop), "/r" + std::to_string(hop + 1));
        server.serve_bytes("/r6", bytes_of("end"));
        FlagSink sink;
        Client client;
        const Response response = client.fetch(get(server.url("/r0")), sink);
        note_failure("redirect past five", response, Failure::redirect_refused);
        OA_CHECK(response.failure == Failure::redirect_refused);
        OA_CHECK(!sink.began);
        const std::vector<LoggedRequest> log = server.requests();
        OA_CHECK(log.size() == 6);
        OA_CHECK(!saw_target(log, "/r6"));
        OA_CHECK(saw_target(log, "/r5"));
    }

    say("redirect 303 post becomes get");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.redirect("/post", "/got", 303);
        server.serve_bytes("/got", bytes_of("done"));
        Request request = get(server.url("/post"));
        request.method = Method::post;
        request.body = bytes_of("payload");
        std::vector<uint8_t> body;
        Client client;
        const Response response = client.fetch(request, body);
        note_failure("redirect 303 post becomes get", response, Failure::none);
        OA_CHECK(response.failure == Failure::none);
        OA_CHECK(is_text(body, "done"));
        const std::vector<LoggedRequest> log = server.requests();
        OA_CHECK(log.size() == 2);
        OA_CHECK(log[0].method == "POST");
        OA_CHECK(is_text(log[0].body, "payload"));
        OA_CHECK(log[1].method == "GET");
        OA_CHECK(log[1].body.empty());
    }

    say("gzip progress no total");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        Reply reply;
        reply.gzip = true;
        reply.body = bytes_of("catalogues and packages");
        server.override_next("/pack", reply);
        ProgressNote note;
        Request request = get(server.url("/pack"));
        request.accept_gzip = true;
        request.progress = note_progress;
        request.progress_context = &note;
        std::vector<uint8_t> body;
        Client client;
        const Response response = client.fetch(request, body);
        note_failure("gzip progress no total", response, Failure::none);
        OA_CHECK(response.failure == Failure::none);
        OA_CHECK(is_text(body, "catalogues and packages"));
        OA_CHECK(response.body_bytes == body.size());
        OA_CHECK(note.calls >= 1);
        OA_CHECK(!note.any_expected);
        OA_CHECK(note.last == body.size());
        const std::vector<LoggedRequest> log = server.requests();
        OA_CHECK(log.size() == 1);
        const std::string* encoding = log[0].header("Accept-Encoding");
        OA_CHECK(encoding != nullptr && *encoding == "gzip");
    }

    say("gzip corrupt");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.handle("GET", "/bad", [](const LoggedRequest&) {
            Reply reply;
            reply.body = {0x1f, 0x8b, 0x00, 0x01, 0x02};
            reply.headers.push_back({"Content-Encoding", "gzip"});
            return reply;
        });
        Request request = get(server.url("/bad"));
        request.accept_gzip = true;
        std::vector<uint8_t> body;
        Client client;
        const Response response = client.fetch(request, body);
        note_failure("gzip corrupt", response, Failure::decode_failed);
        OA_CHECK(response.failure == Failure::decode_failed);
    }

    say("headers too large");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.serve_bytes("/h", bytes_of("hi"));
        Request request = get(server.url("/h"));
        request.limits.max_header_bytes = 16;
        FlagSink sink;
        Client client;
        const Response response = client.fetch(request, sink);
        note_failure("headers too large", response, Failure::too_large);
        OA_CHECK(response.failure == Failure::too_large);
        OA_CHECK(!sink.began);
    }

    say("body too large");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.serve_bytes("/big", bytes_of("0123456789"));
        Request request = get(server.url("/big"));
        request.limits.max_body_bytes = 4;
        FlagSink sink;
        Client client;
        const Response response = client.fetch(request, sink);
        note_failure("body too large", response, Failure::too_large);
        OA_CHECK(response.failure == Failure::too_large);
        OA_CHECK(!sink.began);
        OA_CHECK(response.body_bytes == 0);
    }

    say("connect refused");
    {
        const uint16_t port = closed_port();
        OA_CHECK(port != 0);
        Request request = get("http://127.0.0.1:" + std::to_string(port) + "/x");
        // Windows tries a refused connection again before it reports the
        // refusal, about two seconds later; the limits leave room for that.
        request.timeouts.connect_ms = 5000;
        request.timeouts.total_ms = 6000;
        std::vector<uint8_t> body;
        Client client;
        const Response response = client.fetch(request, body);
        note_failure("connect refused", response, Failure::cannot_connect);
        OA_CHECK(response.failure == Failure::cannot_connect);
    }

    say("idle timeout");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        Reply reply;
        reply.head_delay_ms = 5000;
        reply.body = bytes_of("late");
        server.override_next("/late", reply);
        Request request = get(server.url("/late"));
        request.timeouts.idle_ms = 300;
        request.timeouts.total_ms = 0;
        request.timeouts.connect_ms = 2000;
        std::vector<uint8_t> body;
        Client client;
        const auto start = std::chrono::steady_clock::now();
        const Response response = client.fetch(request, body);
        const int64_t elapsed = since(start);
        note_failure("idle timeout", response, Failure::timed_out);
        if (elapsed < 250 || elapsed >= 1300)
            std::fprintf(stderr, "idle timeout elapsed %lld\n", static_cast<long long>(elapsed));
        OA_CHECK(response.failure == Failure::timed_out);
        OA_CHECK(elapsed >= 250);
        OA_CHECK(elapsed < 1300);
    }

    say("total timeout");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        Reply reply;
        reply.head_delay_ms = 5000;
        reply.body = bytes_of("late");
        server.override_next("/late", reply);
        Request request = get(server.url("/late"));
        request.timeouts.idle_ms = 10000;
        request.timeouts.total_ms = 400;
        request.timeouts.connect_ms = 2000;
        std::vector<uint8_t> body;
        Client client;
        const auto start = std::chrono::steady_clock::now();
        const Response response = client.fetch(request, body);
        const int64_t elapsed = since(start);
        note_failure("total timeout", response, Failure::timed_out);
        if (elapsed < 300 || elapsed >= 1400)
            std::fprintf(stderr, "total timeout elapsed %lld\n", static_cast<long long>(elapsed));
        OA_CHECK(response.failure == Failure::timed_out);
        OA_CHECK(elapsed >= 300);
        OA_CHECK(elapsed < 1400);
    }

    say("cancel paced body");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        Reply reply;
        reply.body = bytes_of("0123456789abcdef0123456789abcdef");
        reply.piece_bytes = 1;
        reply.piece_delay_ms = 2000;
        server.override_next("/paced", reply);
        std::atomic<bool> cancel{false};
        Request request = get(server.url("/paced"));
        request.cancel = &cancel;
        request.progress = cancel_on_progress;
        request.progress_context = &cancel;
        request.timeouts.idle_ms = 10000;
        request.timeouts.total_ms = 10000;
        std::vector<uint8_t> body;
        Client client;
        const auto start = std::chrono::steady_clock::now();
        const Response response = client.fetch(request, body);
        const int64_t elapsed = since(start);
        note_failure("cancel paced body", response, Failure::cancelled);
        if (elapsed >= 200)
            std::fprintf(
                stderr, "cancel paced body elapsed %lld\n", static_cast<long long>(elapsed)
            );
        OA_CHECK(response.failure == Failure::cancelled);
        OA_CHECK(elapsed < 200);
    }

    say("connection cut");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        Reply reply;
        reply.body = bytes_of("0123456789abcdef0123456789abcdef01234567");
        reply.cut_after = 10;
        server.override_next("/cut", reply);
        std::vector<uint8_t> body;
        Client client;
        const Response response = client.fetch(get(server.url("/cut")), body);
        note_failure("connection cut", response, Failure::connection_lost);
        OA_CHECK(response.failure == Failure::connection_lost);
        OA_CHECK(response.body_bytes == 10);
        OA_CHECK(body.size() == 10);
    }

    say("sink stopped");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.serve_bytes("/doc", bytes_of("hello"));
        FlagSink sink;
        sink.write_ok = false;
        Client client;
        const Response response = client.fetch(get(server.url("/doc")), sink);
        note_failure("sink stopped", response, Failure::sink_stopped);
        OA_CHECK(response.failure == Failure::sink_stopped);
        OA_CHECK(sink.began);
    }

    say("https read as http when tls is off");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.serve_bytes("/x", bytes_of("yes"));
        ClientOptions options;
        options.tls = false;
        Client client(options);
        std::vector<uint8_t> body;
        const std::string https = "https://127.0.0.1:" + std::to_string(server.port()) + "/x";
        const Response response = client.fetch(get(https), body);
        note_failure("https read as http when tls is off", response, Failure::none);
        OA_CHECK(response.failure == Failure::none);
        OA_CHECK(is_text(body, "yes"));
        OA_CHECK(response.final_url == "http://127.0.0.1:" + std::to_string(server.port()) + "/x");
    }

#if defined(OA_HTTP_TLS_OPENSSL)
    say("tls where the system has libssl");
    {
        // The client loads the same pairs. Where one opens here, HTTPS must be
        // there too: a symbol the client asks for that the library does not
        // export reads every https address as http.
        const auto opens = [](const char* ssl, const char* crypto) {
            void* crypto_handle = dlopen(crypto, RTLD_NOW | RTLD_LOCAL);
            void* ssl_handle = dlopen(ssl, RTLD_NOW | RTLD_LOCAL);
            const bool both = crypto_handle != nullptr && ssl_handle != nullptr;
            if (ssl_handle != nullptr)
                dlclose(ssl_handle);
            if (crypto_handle != nullptr)
                dlclose(crypto_handle);
            return both;
        };
        if (opens("libssl.so.3", "libcrypto.so.3") || opens("libssl.so.1.1", "libcrypto.so.1.1"))
            OA_CHECK(oa::netgame::http::tls_available());
        else
            say("  no libssl on this system");
    }
#endif

    say("https does not fall back when tls fails");
    {
        // No TLS listener: a certificate would need one, and this test must
        // not start a program. The plain fixture answers only HTTP, so a
        // fallback would return its body.
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.serve_bytes("/x", bytes_of("yes"));
        ClientOptions options;
        options.tls = true;
        Client client(options);
        std::vector<uint8_t> body;
        const std::string https = "https://127.0.0.1:" + std::to_string(server.port()) + "/x";
        const Response response = client.fetch(get(https), body);
        OA_CHECK(response.failure != Failure::none);
        OA_CHECK(body.empty());
        OA_CHECK(!is_text(body, "yes"));
        OA_CHECK(response.final_url == https);
        if (!oa::netgame::http::tls_available()) {
            OA_CHECK(response.failure == Failure::https_unsupported);
            OA_CHECK(
                response.detail == oa::netgame::http::failure_text(Failure::https_unsupported)
            );
        }
    }

    say("closed port after an https rewrite needs tls");
    {
        ClientOptions options;
        options.tls = false;
        Client client(options);
        std::vector<uint8_t> body;
        // The port refuses the connection; Windows reports that about two
        // seconds later, which the connect limit leaves room for.
        Request request = get("https://127.0.0.1:1/oa/registry.yaml");
        request.timeouts.connect_ms = 5000;
        const Response response = client.fetch(request, body);
        note_failure(
            "closed port after an https rewrite needs tls", response, Failure::https_needs_tls
        );
        OA_CHECK(response.failure == Failure::https_needs_tls);
        OA_CHECK(response.detail == oa::netgame::http::failure_text(Failure::https_needs_tls));
        OA_CHECK(body.empty());
    }

    say("ipv6 refused");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        Client client;
        std::vector<uint8_t> body;
        const Response response = client.fetch(get("http://[::1]/x"), body);
        note_failure("ipv6 refused", response, Failure::bad_url);
        OA_CHECK(response.failure == Failure::bad_url);
        OA_CHECK(server.connections_accepted() == 0);
    }

    say("user info refused");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        Client client;
        std::vector<uint8_t> body;
        const Response response = client.fetch(
            get("http://user:secret@127.0.0.1:" + std::to_string(server.port()) + "/x"), body
        );
        note_failure("user info refused", response, Failure::bad_url);
        OA_CHECK(response.failure == Failure::bad_url);
        OA_CHECK(server.connections_accepted() == 0);
    }

    say("user agent default");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.serve_bytes("/doc", bytes_of("hi"));
        Client client;
        std::vector<uint8_t> body;
        const Response response = client.fetch(get(server.url("/doc")), body);
        OA_CHECK(response.failure == Failure::none);
        const std::vector<LoggedRequest> log = server.requests();
        OA_CHECK(log.size() == 1);
        const std::string* agent = log[0].header("User-Agent");
        // The program's name and its version, whichever version it is.
        constexpr std::string_view product = "OpenAnnihilation/";
        OA_CHECK(
            agent != nullptr && agent->starts_with(product) && agent->size() > product.size() &&
            agent->find_first_of(" \t") == std::string::npos
        );
    }

    say("user agent set");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.serve_bytes("/doc", bytes_of("hi"));
        ClientOptions options;
        options.user_agent = "CatalogFetcher";
        Client client(options);
        std::vector<uint8_t> body;
        const Response response = client.fetch(get(server.url("/doc")), body);
        OA_CHECK(response.failure == Failure::none);
        const std::vector<LoggedRequest> log = server.requests();
        OA_CHECK(log.size() == 1);
        const std::string* agent = log[0].header("User-Agent");
        OA_CHECK(agent != nullptr && *agent == "CatalogFetcher");
    }

    say("bad request caller host");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        Request request = get(server.url("/x"));
        request.headers.push_back(Header{"Host", "evil.test"});
        FlagSink sink;
        Client client;
        const Response response = client.fetch(request, sink);
        note_failure("bad request caller host", response, Failure::bad_request);
        OA_CHECK(response.failure == Failure::bad_request);
        OA_CHECK(server.connections_accepted() == 0);
    }

    say("body on get");
    {
        Request request = get("http://127.0.0.1/x");
        request.body = bytes_of("nope");
        std::vector<uint8_t> body;
        Client client;
        const Response response = client.fetch(request, body);
        note_failure("body on get", response, Failure::bad_request);
        OA_CHECK(response.failure == Failure::bad_request);
    }

    say("gzip and range");
    {
        Request request = get("http://127.0.0.1/x");
        request.accept_gzip = true;
        request.range_from = 0;
        std::vector<uint8_t> body;
        Client client;
        const Response response = client.fetch(request, body);
        note_failure("gzip and range", response, Failure::bad_request);
        OA_CHECK(response.failure == Failure::bad_request);
    }

    say("post 301 refused");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.redirect("/post", "/next", 301);
        server.serve_bytes("/next", bytes_of("no"));
        Request request = get(server.url("/post"));
        request.method = Method::post;
        request.body = bytes_of("payload");
        FlagSink sink;
        Client client;
        const Response response = client.fetch(request, sink);
        note_failure("post 301 refused", response, Failure::redirect_refused);
        OA_CHECK(response.failure == Failure::redirect_refused);
        OA_CHECK(!sink.began);
        OA_CHECK(count_method(server.requests(), "POST") == 1);
        OA_CHECK(!saw_target(server.requests(), "/next"));
    }

    say("post 307 keeps body");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.redirect("/post", "/next", 307);
        server.handle("POST", "/next", [](const LoggedRequest& logged) {
            Reply reply;
            reply.body = logged.body;
            return reply;
        });
        Request request = get(server.url("/post"));
        request.method = Method::post;
        request.body = bytes_of("payload");
        std::vector<uint8_t> body;
        Client client;
        const Response response = client.fetch(request, body);
        note_failure("post 307 keeps body", response, Failure::none);
        OA_CHECK(response.failure == Failure::none);
        OA_CHECK(is_text(body, "payload"));
        const std::vector<LoggedRequest> log = server.requests();
        OA_CHECK(count_method(log, "POST") == 2);
        OA_CHECK(log.size() == 2);
        OA_CHECK(is_text(log[1].body, "payload"));
    }

    say("close idle drops pool");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.serve_bytes("/doc", bytes_of("ok"));
        Client client;
        std::vector<uint8_t> body;
        const Response first = client.fetch(get(server.url("/doc")), body);
        OA_CHECK(first.failure == Failure::none);
        OA_CHECK(server.connections_accepted() == 1);
        client.close_idle();
        const Response second = client.fetch(get(server.url("/doc")), body);
        OA_CHECK(second.failure == Failure::none);
        OA_CHECK(server.connections_accepted() == 2);
    }

    say("redirect refused subdomain");
    {
        Server server;
        std::string error;
        OA_CHECK(server.start(&error));
        server.redirect("/go", "http://evil.allowed.test/x");
        Request request = get(server.url("/go"));
        request.redirects.allowed_hosts.push_back("allowed.test");
        FlagSink sink;
        ClientOptions options;
        options.resolve = map_allowed;
        Client client(options);
        const Response response = client.fetch(request, sink);
        note_failure("redirect refused subdomain", response, Failure::redirect_refused);
        OA_CHECK(response.failure == Failure::redirect_refused);
        OA_CHECK(!sink.began);
    }

    say("redirect refused other port");
    {
        Server origin;
        Server other;
        std::string error;
        OA_CHECK(origin.start(&error));
        OA_CHECK(other.start(&error));
        other.serve_bytes("/item", bytes_of("no"));
        origin.redirect("/go", other.url("/item"));
        FlagSink sink;
        Client client;
        const Response response = client.fetch(get(origin.url("/go")), sink);
        note_failure("redirect refused other port", response, Failure::redirect_refused);
        OA_CHECK(response.failure == Failure::redirect_refused);
        OA_CHECK(!sink.began);
        OA_CHECK(other.connections_accepted() == 0);
    }

    say("cancel lookup");
    {
        lookup_cancel.store(false);
        ClientOptions options;
        options.resolve = hold_name;
        Client client(options);
        Request request = get("http://slow.test/x");
        request.cancel = &lookup_cancel;
        request.timeouts.connect_ms = 3000;
        request.timeouts.total_ms = 3000;
        std::vector<uint8_t> body;
        const auto start = std::chrono::steady_clock::now();
        const Response response = client.fetch(request, body);
        const int64_t elapsed = since(start);
        note_failure("cancel lookup", response, Failure::cancelled);
        if (elapsed >= 200)
            std::fprintf(stderr, "cancel lookup elapsed %lld\n", static_cast<long long>(elapsed));
        OA_CHECK(response.failure == Failure::cancelled);
        OA_CHECK(elapsed < 200);
    }

    return oa::test::check_exit_status();
}

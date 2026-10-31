// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The automation endpoint (endpoint.hpp).
#include "endpoint.hpp"

#include "requests.hpp"
#include "token.hpp"

#include "oa/app/app.hpp"
#include "oa/core/game_state.h"
#include "oa/platform/log_files.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <system_error>
#include <utility>

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

namespace oa::app::automation {
namespace {

namespace fs = std::filesystem;
namespace sock = oa::netgame::sock;

// The bytes read from a connection at a time.
constexpr size_t kReadChunkBytes = size_t{64} * 1024;
// The connections accepted each time the endpoint is served, at most.
constexpr size_t kAcceptsPerServe = 16;
// Written bytes kept at the front of a connection's output before they are dropped.
constexpr size_t kWrittenKeptBytes = size_t{1} << 20;

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
uint64_t process_id() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<uint64_t>(getpid());
#endif
}

/// Returns a path as UTF-8 text.
///
/// @param path the path
/// @return the text
std::string utf8_of(const fs::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

/// Writes the endpoint's file: a file beside it first, readable only by its
/// owner where the system has such modes, then renamed over the file, so
/// that a reader never sees part of it.
///
/// Throws std::runtime_error when the file cannot be written.
///
/// @param file the file
/// @param text what it holds
void write_endpoint_file(const fs::path& file, std::string_view text) {
    std::error_code error;
    if (file.has_parent_path())
        fs::create_directories(file.parent_path(), error);
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
        size_t done = 0;
        while (done < text.size()) {
            const ssize_t put = ::write(out, text.data() + done, text.size() - done);
            if (put <= 0)
                break;
            done += static_cast<size_t>(put);
        }
        written = done == text.size() && ::fchmod(out, kOwnerReadWrite) == 0;
        written = ::close(out) == 0 && written;
    }
#endif
    if (written) {
        fs::rename(partial, file, error);
        written = !error;
    }
    if (!written) {
        fs::remove(partial, error);
        throw std::runtime_error("automation: cannot write " + utf8_of(file));
    }
}

/// Writes an answer that refuses a request.
///
/// @param id the request's id; null when it has none
/// @param code the error's code
/// @param message what it says
/// @param field the request's field it names; empty for none
/// @return the writer, its object open after the error, for further members
JsonWriter refusal(
    std::optional<int64_t> id,
    std::string_view code,
    std::string_view message,
    std::string_view field
) {
    JsonWriter json;
    json.begin_object();
    json.key("id");
    if (id)
        json.integer(*id);
    else
        json.null();
    json.key("ok");
    json.boolean(false);
    json.key("error");
    json.begin_object();
    json.key("code");
    json.string(code);
    json.key("message");
    json.string(message);
    if (!field.empty()) {
        json.key("field");
        json.string(field);
    }
    json.end_object();
    return json;
}

/// Returns a request's id, when it has one that is a whole number.
///
/// @param request the request; may be no object
/// @return the id, or nothing
std::optional<int64_t> request_id(const std::optional<Json>& request) {
    if (!request)
        return std::nullopt;
    const Json* id = request->find("id");
    return id != nullptr ? id->integer() : std::nullopt;
}

/// Queues the refusal of a run of bytes that made no frame.
///
/// @param[in,out] output the connection's output
/// @param skipped the run
void queue_bad_frame(std::vector<uint8_t>& output, const SkippedBytes& skipped) {
    JsonWriter json =
        refusal(std::nullopt, "bad_frame", "bytes that make no frame were skipped", {});
    json.key("reason");
    json.string(skip_reason_name(skipped.reason));
    json.key("skipped");
    json.integer(static_cast<int64_t>(skipped.count));
    json.end_object();
    (void)encode_frame(endpoint_route, json.text(), {}, output);
}

/// Returns a request's string field.
///
/// @param request the request
/// @param name the field's name
/// @return the text, or null when the field is missing or not a string
const std::string* string_field(const Json& request, std::string_view name) {
    const Json* field = request.find(name);
    return field != nullptr ? field->string() : nullptr;
}

} // namespace

void Answer::refuse(std::string_view code, std::string_view message, std::string_view field) {
    error_code = code;
    error_message = message;
    error_field = field;
}

Endpoint::~Endpoint() {
    for (auto& connection : waiting_)
        close(connection);
    if (client_)
        close(*client_);
    sock::stream_close(&listener_);
}

void Endpoint::open(const ListenAddress& address, const fs::path& file) {
    if (listening())
        return;
    const auto token = make_token();
    if (!token)
        throw std::runtime_error(
            "automation: the system's random number generator cannot be read for the token"
        );
    char error[160]{};
    uint16_t port = 0;
    const auto loopback =
        address.family == LoopbackFamily::ipv6 ? sock::Loopback::ipv6 : sock::Loopback::ipv4;
    intptr_t listener = sock::stream_listen(loopback, address.port, port, error, sizeof error);
    if (listener == sock::invalid_socket)
        throw std::runtime_error(
            "automation: cannot serve " + address_text(address.family, address.port) + ": " + error
        );
    const std::string served = address_text(address.family, port);
    JsonWriter json;
    json.begin_object();
    json.key("version");
    json.integer(automation_version);
    json.key("address");
    json.string(served);
    json.key("token");
    json.string(*token);
    json.key("pid");
    json.integer(static_cast<int64_t>(process_id()));
    json.end_object();
    try {
        write_endpoint_file(file, json.text() + "\n");
    } catch (...) {
        sock::stream_close(&listener);
        throw;
    }
    listener_ = listener;
    address_ = served;
    token_ = *token;
    // The token goes to the file alone, never to the log.
    oa::platform::log_files::write_line("automation: serving on " + address_);
    oa::platform::log_files::write_line(
        "automation: the address and token are in " + utf8_of(file)
    );
}

bool Endpoint::listening() const noexcept {
    return listener_ != sock::invalid_socket;
}

uint32_t Endpoint::tick() const noexcept {
    if (automation_host_.match_game == nullptr)
        return 0;
    const oa::Game* game = automation_host_.match_game(automation_host_.context);
    return game != nullptr ? game->tick : 0;
}

bool Endpoint::take_quit() noexcept {
    if (!quit_asked_ || (client_ && client_->output_sent < client_->output.size()))
        return false;
    quit_asked_ = false;
    return true;
}

Answer Endpoint::begin_answer(int64_t id) const {
    Answer answer;
    answer.json.begin_object();
    answer.json.key("id");
    answer.json.integer(id);
    answer.json.key("ok");
    answer.json.boolean(true);
    answer.json.key("frame");
    answer.json.integer(static_cast<int64_t>(frame_));
    answer.json.key("tick");
    answer.json.integer(tick());
    return answer;
}

void Endpoint::answer_held(Answer& answer) {
    if (!held_)
        return;
    const int64_t id = held_->id;
    held_.reset();
    if (client_)
        send(*client_, id, answer);
}

JsonWriter Endpoint::begin_event(std::string_view kind) const {
    JsonWriter event;
    event.begin_object();
    event.key("event");
    event.string(kind);
    event.key("seq");
    event.integer(static_cast<int64_t>(events_sent_ + 1));
    event.key("frame");
    event.integer(static_cast<int64_t>(frame_));
    event.key("tick");
    event.integer(tick());
    return event;
}

void Endpoint::send_event(JsonWriter& event) {
    event.end_object();
    if (!client_)
        return;
    if (encode_frame(endpoint_route, event.text(), {}, client_->output))
        ++events_sent_;
}

void Endpoint::serve_between_frames(
    Runtime& runtime, const std::function<void(Endpoint&)>& work
) noexcept {
    if (!listening())
        return;
    try {
        runtime_ = &runtime;
        check_host_ = oa::app::check_host(runtime);
        automation_host_ = oa::app::automation_host(runtime);
        run_options_ = &runtime_options(runtime);
        work(*this);
        write_client();
    } catch (const std::exception& error) {
        std::cerr << "automation: " << error.what() << '\n';
        drop_client("dropped");
    } catch (...) {
        drop_client("dropped");
    }
    runtime_ = nullptr;
    run_options_ = nullptr;
}

void Endpoint::serve(Runtime& runtime, FrameStage stage) noexcept {
    if (!listening())
        return;
    try {
        runtime_ = &runtime;
        check_host_ = oa::app::check_host(runtime);
        automation_host_ = oa::app::automation_host(runtime);
        run_options_ = &runtime_options(runtime);
        if (stage == FrameStage::pump) {
            ++frame_;
            accept_connections();
            serve_waiting();
            serve_client();
        }
        for (const FrameWork work : frame_work())
            work(*this, stage);
        write_client();
    } catch (const std::exception& error) {
        std::cerr << "automation: " << error.what() << '\n';
        drop_client("dropped");
    } catch (...) {
        drop_client("dropped");
    }
    runtime_ = nullptr;
    run_options_ = nullptr;
}

void Endpoint::accept_connections() {
    for (size_t accepted = 0; accepted < kAcceptsPerServe; ++accepted) {
        intptr_t socket = sock::stream_accept(listener_);
        if (socket == sock::invalid_socket)
            return;
        if (waiting_.size() >= max_waiting_connections) {
            sock::stream_close(&socket);
            continue;
        }
        Connection connection;
        connection.socket = socket;
        connection.opened_ms = steady_ms();
        waiting_.push_back(std::move(connection));
    }
}

bool Endpoint::receive(Connection& connection) {
    // Nothing is read while the reader holds the bytes of its largest frame
    // and of one read more: what the other end sends meanwhile waits in the
    // system's buffers for the connection, which hold the sender back once
    // they are full.
    const size_t held_at_most = connection.reader.largest_frame_bytes() + kReadChunkBytes;
    if (connection.ended || connection.reader.buffered() >= held_at_most)
        return true;
    std::vector<uint8_t> chunk(kReadChunkBytes);
    size_t taken = 0;
    while (taken < read_budget_bytes && connection.reader.buffered() < held_at_most) {
        const size_t wanted = std::min(chunk.size(), held_at_most - connection.reader.buffered());
        const std::ptrdiff_t got = sock::stream_read(connection.socket, chunk.data(), wanted);
        if (got == sock::stream_ended) {
            connection.ended = true;
            break;
        }
        if (got < 0)
            return false;
        if (got == 0)
            break;
        const auto count = static_cast<size_t>(got);
        // The first bytes of a connection are the magic, or it is no client's.
        for (size_t at = connection.received;
             at < frame_magic.size() && at < connection.received + count;
             ++at)
            if (chunk[at - connection.received] != static_cast<uint8_t>(frame_magic[at]))
                connection.unframed = true;
        connection.reader.feed({chunk.data(), count});
        connection.received += count;
        taken += count;
    }
    return true;
}

void Endpoint::serve_waiting() {
    for (size_t index = 0; index < waiting_.size();) {
        Connection& connection = waiting_[index];
        bool drop = false;
        if (!connection.closing) {
            if (!receive(connection) || connection.unframed) {
                drop = true;
            } else {
                Frame frame;
                SkippedBytes skipped;
                const ReadResult result = connection.reader.next(frame, skipped);
                if (result == ReadResult::frame)
                    greet(connection, frame);
                else
                    // Its first frame is a hello: bytes that make none, or
                    // an end before one, close it unanswered.
                    drop = result == ReadResult::skipped || connection.reader.skipping() ||
                           connection.ended ||
                           steady_ms() - connection.opened_ms > first_frame_timeout_ms;
            }
        }
        if (connection.socket == sock::invalid_socket) {
            // It became the client.
            waiting_.erase(waiting_.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        if (connection.closing && !drop) {
            // A refusal goes out before the connection closes; one that
            // cannot be written in time is dropped all the same.
            drop = write(connection) != Written::kept_up ||
                   connection.output_sent == connection.output.size() ||
                   steady_ms() - connection.opened_ms > first_frame_timeout_ms;
        }
        if (drop) {
            close(connection);
            waiting_.erase(waiting_.begin() + static_cast<std::ptrdiff_t>(index));
            continue;
        }
        ++index;
    }
}

void Endpoint::greet(Connection& connection, const Frame& frame) {
    connection.closing = true;
    JsonError error;
    const std::optional<Json> request = parse_json(frame.json, error);
    const std::optional<int64_t> id = request_id(request);
    if (client_) {
        refuse(connection, id, "busy", "another client is connected");
        return;
    }
    const std::string* op = nullptr;
    const std::string* token = nullptr;
    if (request && request->type() == JsonType::object) {
        op = string_field(*request, "op");
        token = string_field(*request, "token");
    }
    if (op == nullptr || *op != "hello" || token == nullptr || !token_matches(*token, token_)) {
        oa::platform::log_files::write_line("automation: a connection was denied");
        refuse(connection, id, "denied", "the first request is a hello with the run's token");
        return;
    }
    if (!id) {
        refuse(connection, id, "bad_request", "a request has a whole number as its id");
        return;
    }
    bool speaks = false;
    if (const Json* versions = request->find("versions"))
        for (const Json& version : versions->elements())
            speaks = speaks || version.integer() == automation_version;
    if (!speaks) {
        JsonWriter json = refusal(id, "unsupported_version", "the endpoint speaks version 1", {});
        json.key("versions");
        json.begin_array();
        json.integer(automation_version);
        json.end_array();
        json.end_object();
        (void)encode_frame(endpoint_route, json.text(), {}, connection.output);
        return;
    }
    // The bytes that came after the hello are read with the client's limits.
    connection.reader.set_limits(max_json_bytes, max_request_payload_bytes);
    connection.closing = false;
    client_ = std::move(connection);
    connection.socket = sock::invalid_socket;
    subscriptions_ = 0;
    events_sent_ = 0;
    oa::platform::log_files::write_line("automation: client connected");
    Request hello{*id, *op, *request};
    Answer answer = begin_answer(hello.id);
    answer_hello(*this, hello, answer);
    send(*client_, hello.id, answer);
}

void Endpoint::serve_client() {
    if (!client_)
        return;
    if (!receive(*client_)) {
        drop_client("left");
        return;
    }
    const uint64_t started = steady_us();
    while (client_ && !held_ && steady_us() - started < request_budget_us) {
        Frame frame;
        SkippedBytes skipped;
        const ReadResult result = client_->reader.next(frame, skipped);
        if (result == ReadResult::paused)
            return;
        if (result == ReadResult::need_more) {
            // A client that will send nothing more leaves once every request
            // it sent is answered and the answers are written (write_client).
            if (client_->ended) {
                if (client_->reader.finish(skipped) == ReadResult::skipped)
                    queue_bad_frame(client_->output, skipped);
                client_->closing = true;
            }
            return;
        }
        if (result == ReadResult::skipped) {
            queue_bad_frame(client_->output, skipped);
            continue;
        }
        take_request(frame);
    }
}

void Endpoint::take_request(const Frame& frame) {
    JsonError error;
    std::optional<Json> fields = parse_json(frame.json, error);
    if (!fields || fields->type() != JsonType::object) {
        refuse(
            *client_,
            std::nullopt,
            "bad_request",
            fields ? "the frame's JSON part is not an object"
                   : "the frame's JSON part is not JSON: " + error.message
        );
        return;
    }
    const std::optional<int64_t> id = request_id(fields);
    if (!id) {
        Answer answer;
        answer.refuse("bad_request", "a request has a whole number as its id", "id");
        send(*client_, std::nullopt, answer);
        return;
    }
    const std::string* op = string_field(*fields, "op");
    if (op == nullptr) {
        Answer answer;
        answer.refuse("bad_request", "a request names its op", "op");
        send(*client_, id, answer);
        return;
    }
    const RequestKind* kind = find_request(*op);
    if (kind == nullptr) {
        refuse(*client_, id, "unknown_op", "the endpoint does not answer " + *op);
        return;
    }
    Request request{*id, *op, std::move(*fields)};
    Answer answer = begin_answer(request.id);
    kind->handler(*this, request, answer);
    if (answer.held && answer.error_code.empty()) {
        held_ = std::move(request);
        return;
    }
    send(*client_, request.id, answer);
}

void Endpoint::send(Connection& connection, std::optional<int64_t> id, Answer& answer) {
    if (!answer.error_code.empty()) {
        JsonWriter json = refusal(id, answer.error_code, answer.error_message, answer.error_field);
        json.end_object();
        (void)encode_frame(endpoint_route, json.text(), {}, connection.output);
        return;
    }
    answer.json.end_object();
    if (!encode_frame(endpoint_route, answer.json.text(), answer.payload, connection.output))
        refuse(connection, id, "internal", "the answer is larger than a frame may be");
}

void Endpoint::refuse(
    Connection& connection,
    std::optional<int64_t> id,
    std::string_view code,
    std::string_view message
) {
    JsonWriter json = refusal(id, code, message, {});
    json.end_object();
    (void)encode_frame(endpoint_route, json.text(), {}, connection.output);
}

Endpoint::Written Endpoint::write(Connection& connection) {
    while (connection.output_sent < connection.output.size()) {
        const std::ptrdiff_t put = sock::stream_write(
            connection.socket,
            connection.output.data() + connection.output_sent,
            connection.output.size() - connection.output_sent
        );
        if (put < 0)
            return Written::failed;
        if (put == 0)
            break;
        connection.output_sent += static_cast<size_t>(put);
    }
    if (connection.output_sent == connection.output.size()) {
        connection.output.clear();
        connection.output_sent = 0;
    } else if (connection.output_sent >= kWrittenKeptBytes) {
        connection.output.erase(
            connection.output.begin(),
            connection.output.begin() + static_cast<std::ptrdiff_t>(connection.output_sent)
        );
        connection.output_sent = 0;
    }
    if (connection.output.size() - connection.output_sent > max_waiting_output_bytes)
        return Written::overfull;
    return Written::kept_up;
}

void Endpoint::write_client() {
    if (!client_)
        return;
    const Written written = write(*client_);
    if (written == Written::failed)
        drop_client("left");
    else if (written == Written::overfull)
        drop_client("dropped: not reading");
    else if (client_->closing && client_->output.empty())
        drop_client("left");
}

void Endpoint::drop_client(std::string_view why) noexcept {
    if (!client_)
        return;
    // This function does not throw. A log line is not worth ending the process.
    try {
        oa::platform::log_files::write_line(std::string("automation: client ").append(why));
    } catch (...) {
    }
    close(*client_);
    client_.reset();
    held_.reset();
    subscriptions_ = 0;
}

void Endpoint::close(Connection& connection) noexcept {
    sock::stream_close(&connection.socket);
}

} // namespace oa::app::automation

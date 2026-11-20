// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The status stream (status.hpp): hello, the events the producers already
// build, and one exit line. The lines go to a writer, a file, the standard
// output the process was started with, or any of those together.
#include "status.hpp"

#include "oa/core/world.h"
#include "oa/platform/files.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace oa::app::automation {
namespace {

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

/// Closes an inherited output handle. -1 is none.
///
/// @param handle the handle use_inherited_output was given
void close_output(intptr_t handle) noexcept {
    if (handle == -1)
        return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(handle));
#else
    ::close(static_cast<int>(handle));
#endif
}

/// Writes bytes to an inherited output handle, all of them when the system
/// takes them in pieces.
///
/// @param handle the handle
/// @param bytes the bytes
void write_output(intptr_t handle, std::string_view bytes) noexcept {
    if (handle == -1 || bytes.empty())
        return;
#ifdef _WIN32
    const HANDLE out = reinterpret_cast<HANDLE>(handle);
    while (!bytes.empty()) {
        const auto chunk = static_cast<DWORD>(std::min(bytes.size(), size_t{1} << 20));
        DWORD written = 0;
        if (WriteFile(out, bytes.data(), chunk, &written, nullptr) == FALSE || written == 0)
            break;
        bytes.remove_prefix(written);
    }
#else
    const int descriptor = static_cast<int>(handle);
    while (!bytes.empty()) {
        const ssize_t put = ::write(descriptor, bytes.data(), bytes.size());
        if (put < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (put == 0)
            break;
        bytes.remove_prefix(static_cast<size_t>(put));
    }
#endif
}

/// The standard output kept before a log redirect, or -1.
///
/// @return the handle
intptr_t& captured_output() noexcept {
    static intptr_t handle = -1;
    return handle;
}

/// The process's one status stream.
///
/// @return the stream
StatusStream& process_stream() {
    static StatusStream stream;
    return stream;
}

} // namespace

StatusStream::~StatusStream() {
    if (file_ != nullptr)
        std::fclose(file_);
    close_output(inherited_);
}

void StatusStream::close_inherited() noexcept {
    close_output(inherited_);
    inherited_ = -1;
}

void StatusStream::set_writer(void* context, StatusWriter writer) noexcept {
    writer_context_ = context;
    writer_ = writer;
}

bool StatusStream::open_file(const std::filesystem::path& path) {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    file_ = oa::platform::open_file(path, "wb");
    return file_ != nullptr;
}

void StatusStream::use_inherited_output(intptr_t handle) noexcept {
    if (inherited_ == handle)
        return;
    close_inherited();
    inherited_ = handle;
}

bool StatusStream::has_destination() const noexcept {
    return writer_ != nullptr || file_ != nullptr || inherited_ != -1;
}

intptr_t StatusStream::inherited_output() const noexcept {
    return inherited_;
}

void StatusStream::write_line(std::string_view json) {
    if (!has_destination())
        return;
    std::string line;
    line.reserve(json.size() + 1);
    line.append(json);
    line.push_back('\n');
    if (writer_ != nullptr)
        writer_(writer_context_, line);
    if (file_ != nullptr) {
        std::fwrite(line.data(), 1, line.size(), file_);
        std::fflush(file_);
    }
    write_output(inherited_, line);
}

void StatusStream::hello(std::string_view version, uint64_t pid) {
    if (hello_written_ || !has_destination())
        return;
    hello_written_ = true;
    JsonWriter json;
    json.begin_object();
    json.key("event");
    json.string("hello");
    json.key("status");
    json.integer(1);
    json.key("version");
    json.string(version);
    json.key("pid");
    json.integer(static_cast<int64_t>(pid));
    json.end_object();
    write_line(json.text());
}

void StatusStream::note_game_over() {
    saw_game_over_ = true;
    reason_ = "game-ended";
    if (quit_ == QuitAfter::game)
        quit_pending_ = true;
}

void StatusStream::note_departure(bool connection_lost) {
    if (saw_game_over_)
        return;
    reason_ = connection_lost ? "connection-lost" : "left";
    if (quit_ != QuitAfter::none)
        quit_pending_ = true;
}

void StatusStream::note_score_left() {
    if (quit_ == QuitAfter::score && saw_game_over_)
        quit_pending_ = true;
}

void StatusStream::note_refused() {
    reason_ = "refused";
}

void StatusStream::set_quit(QuitAfter quit) noexcept {
    quit_ = quit;
}

bool StatusStream::take_quit() noexcept {
    if (!quit_pending_)
        return false;
    quit_pending_ = false;
    return true;
}

int StatusStream::resolved_code(int played) const noexcept {
    if (quit_ == QuitAfter::none)
        return played;
    if (reason_ == "left" || reason_ == "connection-lost")
        return 3;
    if (reason_ == "game-ended")
        return 0;
    return played;
}

void StatusStream::finish(int played) {
    if (!hello_written_ || exit_written_)
        return;
    exit_written_ = true;
    JsonWriter json;
    json.begin_object();
    json.key("event");
    json.string("exit");
    json.key("reason");
    json.string(reason_);
    json.key("code");
    json.integer(resolved_code(played));
    json.end_object();
    write_line(json.text());
}

bool StatusStream::opened() const noexcept {
    return hello_written_;
}

EventWatch& StatusStream::watch() noexcept {
    return watch_;
}

void StatusStream::set_clock(uint64_t frame, int64_t tick) noexcept {
    frame_ = frame;
    tick_ = tick;
}

void StatusStream::advance_frame() noexcept {
    ++frame_;
}

uint64_t StatusStream::frame() const noexcept {
    return frame_;
}

int64_t StatusStream::tick() const noexcept {
    return tick_;
}

EventHooks StatusStream::hooks() {
    hook_.stream = this;
    EventHooks hooks;
    hooks.context = &hook_;
    hooks.wanted = [](void* /*context*/, uint32_t kind) {
        return kind != event_kind::transfer && kind != event_kind::focus;
    };
    hooks.begin = [](void* context, std::string_view kind) {
        auto& state = *static_cast<HookState*>(context);
        state.kind.assign(kind.begin(), kind.end());
        JsonWriter event;
        event.begin_object();
        event.key("event");
        event.string(kind);
        event.key("seq");
        event.integer(static_cast<int64_t>(state.stream->seq_ + 1));
        event.key("frame");
        event.integer(static_cast<int64_t>(state.stream->frame_));
        event.key("tick");
        event.integer(state.stream->tick_);
        return event;
    };
    hooks.send = [](void* context, JsonWriter& event) {
        auto& state = *static_cast<HookState*>(context);
        event.end_object();
        state.stream->write_line(event.text());
        ++state.stream->seq_;
        if (state.kind == "game_over")
            state.stream->note_game_over();
    };
    return hooks;
}

bool status_lines_wanted() noexcept {
    return command_status_lines();
}

bool status_stdout_wanted() noexcept {
    return command_status_stdout();
}

bool status_following() noexcept {
    return command_status_following();
}

void capture_status_stdout() {
    static bool captured = false;
    if (captured)
        return;
    captured = true;
#ifdef _WIN32
    // A Windows build is a GUI program. The C library's stdout may not be
    // the pipe a launcher inherited, so the handle is taken from the
    // process and duplicated before a log redirect replaces it.
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == nullptr || out == INVALID_HANDLE_VALUE)
        return;
    HANDLE duplicate = nullptr;
    if (DuplicateHandle(
            GetCurrentProcess(),
            out,
            GetCurrentProcess(),
            &duplicate,
            0,
            FALSE,
            DUPLICATE_SAME_ACCESS
        ) == FALSE)
        return;
    captured_output() = reinterpret_cast<intptr_t>(duplicate);
#else
    const int descriptor = fileno(stdout);
    if (descriptor < 0)
        return;
    const int duplicate = ::dup(descriptor);
    if (duplicate < 0)
        return;
    captured_output() = duplicate;
#endif
}

void begin_status_lines(std::string_view version) {
    StatusStream& stream = process_stream();
    stream.set_quit(command_quit_after());
    if (command_status_stdout() && captured_output() != -1) {
        const intptr_t handle = captured_output();
        captured_output() = -1;
        stream.use_inherited_output(handle);
    }
    if (const std::optional<std::filesystem::path>& file = command_status_file()) {
        if (!stream.open_file(*file))
            throw std::runtime_error("--status-file could not be opened");
    }
    if (!stream.has_destination())
        return;
    stream.hello(version, process_id());
    announce_screen(stream.watch());
}

void finish_status_lines(int played) {
    process_stream().finish(played);
}

void finish_refused_status_lines(int code) {
    StatusStream& stream = process_stream();
    stream.note_refused();
    stream.finish(code);
}

void write_started_output(std::string_view line) {
    std::string text(line);
    text.push_back('\n');
    intptr_t handle = captured_output();
    if (handle == -1)
        handle = process_stream().inherited_output();
    if (handle == -1) {
        std::cout << text << std::flush;
        return;
    }
    // What the game printed before goes out first.
    std::cout.flush();
    std::fflush(stdout);
    write_output(handle, text);
}

int status_exit_code(int played) noexcept {
    return process_stream().resolved_code(played);
}

void status_on_observed(const Observed& now) {
    StatusStream& stream = process_stream();
    stream.advance_frame();
    // A match that is leaving still names its own tick, so the end line's
    // tick matches the results taken while it was there. With no match, the
    // tick is 0.
    int64_t tick = 0;
    if (now.match != nullptr)
        tick = static_cast<int64_t>(now.match->game.tick);
    else if (stream.watch().match != nullptr)
        tick = stream.tick();
    stream.set_clock(stream.frame(), tick);
    watch_events(stream.watch(), now, stream.hooks());
}

void status_on_loading(const oa::World* match, std::span<const uint8_t> rows) {
    StatusStream& stream = process_stream();
    if (match != nullptr)
        stream.set_clock(stream.frame(), static_cast<int64_t>(match->game.tick));
    watch_loading(stream.watch(), match, rows, stream.hooks());
}

void status_on_match_gone() {
    StatusStream& stream = process_stream();
    watch_match_gone(stream.watch(), stream.hooks());
}

void status_note_departure(bool connection_lost) {
    process_stream().note_departure(connection_lost);
}

void status_note_score_left() {
    process_stream().note_score_left();
}

bool status_take_quit() noexcept {
    return process_stream().take_quit();
}

bool status_screen_due(std::string_view screen) noexcept {
    return screen_event_due(process_stream().watch(), screen);
}

} // namespace oa::app::automation

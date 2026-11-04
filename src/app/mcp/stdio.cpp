// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Standard input and output for one MCP session (stdio.hpp).
//
// The duplicates are taken before the log redirect replaces the process's
// standard output. Standard input is not switched to non-blocking: that
// flag belongs to the open file description, so setting it on a duplicate
// would change the descriptor the process already had. A poll with no
// wait reads only when a byte is ready. Standard output is made
// non-blocking on the first write, which is after the log redirect, when
// this duplicate is the only descriptor of the pipe the process started with.
#include "stdio.hpp"

#include "oa/app/mcp/options.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace oa::app::mcp {
namespace {

/// The most bytes read in one pump.
constexpr std::size_t kReadCap = 256U * 1024U;

/// The most messages answered from one pump's read.
constexpr int kMaxMessages = 32;

/// Reading waits while this many bytes are still to be written.
constexpr std::size_t kOutCap = 1U << 20;

/// The streams capture_stdio kept.
struct Pipes {
    bool ready{};
    intptr_t input{-1};
    intptr_t output{-1};
};

/// Returns the process's captured streams.
///
/// @return the streams
Pipes& pipes() noexcept {
    static Pipes held;
    return held;
}

#ifdef _WIN32

/// Duplicates a standard handle.
///
/// @param standard STD_INPUT_HANDLE or STD_OUTPUT_HANDLE
/// @param[out] duplicated the duplicate
/// @return true when it was duplicated
bool duplicate_standard(DWORD standard, HANDLE& duplicated) {
    const HANDLE handle = GetStdHandle(standard);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
        return false;
    duplicated = nullptr;
    return DuplicateHandle(
               GetCurrentProcess(),
               handle,
               GetCurrentProcess(),
               &duplicated,
               0,
               FALSE,
               DUPLICATE_SAME_ACCESS
           ) != FALSE &&
           duplicated != nullptr;
}

#else

/// Duplicates a standard stream's descriptor.
///
/// @param stream stdin or stdout
/// @param[out] duplicated the duplicate
/// @return true when it was duplicated
bool duplicate_standard(FILE* stream, int& duplicated) {
    const int descriptor = fileno(stream);
    if (descriptor < 0)
        return false;
    duplicated = ::dup(descriptor);
    return duplicated >= 0;
}

#endif

} // namespace

bool stdio_wanted() noexcept {
    const McpOptions* bound = bound_mcp_options();
    return bound != nullptr && bound->enabled;
}

bool stdio_captured() noexcept {
    return pipes().ready;
}

void capture_stdio() {
    Pipes& held = pipes();
    if (held.ready)
        return;
#ifdef _WIN32
    HANDLE input = nullptr;
    HANDLE output = nullptr;
    if (!duplicate_standard(STD_INPUT_HANDLE, input))
        return;
    if (!duplicate_standard(STD_OUTPUT_HANDLE, output)) {
        CloseHandle(input);
        return;
    }
    held.input = reinterpret_cast<intptr_t>(input);
    held.output = reinterpret_cast<intptr_t>(output);
#else
    int input = -1;
    int output = -1;
    if (!duplicate_standard(stdin, input))
        return;
    if (!duplicate_standard(stdout, output)) {
        ::close(input);
        return;
    }
    held.input = input;
    held.output = output;
#endif
    held.ready = true;
}

void Stdio::adopt() noexcept {
    if (ready_)
        return;
    const Pipes& held = pipes();
    if (!held.ready)
        return;
    input_ = held.input;
    output_ = held.output;
    ready_ = true;
}

void Stdio::arm_output() noexcept {
    if (armed_ || !ready_)
        return;
    armed_ = true;
#ifdef _WIN32
    DWORD mode = PIPE_NOWAIT;
    // A console has no pipe state. One answer written there is short; a
    // launcher, which this serves, passes a pipe.
    (void)SetNamedPipeHandleState(reinterpret_cast<HANDLE>(output_), &mode, nullptr, nullptr);
#else
    const int descriptor = static_cast<int>(output_);
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags >= 0)
        (void)::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
#endif
}

void Stdio::read_some() {
    if (!ready_ || eof_)
        return;
    std::size_t got = 0;
    char buffer[16U * 1024U];
#ifdef _WIN32
    const HANDLE handle = reinterpret_cast<HANDLE>(input_);
    while (got < kReadCap) {
        DWORD available = 0;
        if (PeekNamedPipe(handle, nullptr, 0, nullptr, &available, nullptr) == FALSE) {
            const DWORD problem = GetLastError();
            if (problem == ERROR_BROKEN_PIPE || problem == ERROR_PIPE_NOT_CONNECTED)
                eof_ = true;
            return;
        }
        if (available == 0)
            return;
        const DWORD room = static_cast<DWORD>(
            std::min(static_cast<std::size_t>(available), std::min(sizeof buffer, kReadCap - got))
        );
        DWORD read = 0;
        if (ReadFile(handle, buffer, room, &read, nullptr) == FALSE || read == 0) {
            eof_ = true;
            return;
        }
        lines_.append(std::string_view(buffer, static_cast<std::size_t>(read)));
        got += static_cast<std::size_t>(read);
    }
#else
    const int descriptor = static_cast<int>(input_);
    while (got < kReadCap) {
        pollfd waiting{};
        waiting.fd = descriptor;
        waiting.events = POLLIN;
        const int ready = ::poll(&waiting, 1, 0);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        if (ready == 0)
            return;
        const ssize_t read = ::read(descriptor, buffer, sizeof buffer);
        if (read < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            eof_ = true;
            return;
        }
        if (read == 0) {
            eof_ = true;
            return;
        }
        const auto count = static_cast<std::size_t>(read);
        lines_.append(std::string_view(buffer, count));
        got += count;
        if (count < sizeof buffer)
            return;
    }
#endif
}

void Stdio::write_outgoing() {
    if (!ready_ || outgoing_.empty())
        return;
    arm_output();
#ifdef _WIN32
    const HANDLE handle = reinterpret_cast<HANDLE>(output_);
    while (!outgoing_.empty()) {
        const auto want =
            static_cast<DWORD>(std::min(outgoing_.size(), static_cast<std::size_t>(1U << 16)));
        DWORD wrote = 0;
        if (WriteFile(handle, outgoing_.data(), want, &wrote, nullptr) == FALSE) {
            const DWORD problem = GetLastError();
            if (problem == ERROR_NO_DATA || problem == ERROR_PIPE_BUSY)
                return;
            outgoing_.clear();
            eof_ = true;
            return;
        }
        if (wrote == 0)
            return;
        outgoing_.erase(0, static_cast<std::size_t>(wrote));
    }
#else
    const int descriptor = static_cast<int>(output_);
    while (!outgoing_.empty()) {
        const std::size_t want = std::min(outgoing_.size(), static_cast<std::size_t>(1U << 16));
        const ssize_t wrote = ::write(descriptor, outgoing_.data(), want);
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            outgoing_.clear();
            eof_ = true;
            return;
        }
        if (wrote == 0)
            return;
        outgoing_.erase(0, static_cast<std::size_t>(wrote));
    }
#endif
}

void Stdio::pump(Session& session) {
    adopt();
    if (!ready_)
        return;
    if (outgoing_.size() < kOutCap)
        read_some();
    int messages = 0;
    FramedLine line;
    while (messages < kMaxMessages && lines_.take(line)) {
        ++messages;
        if (line.too_long) {
            outgoing_.append(parse_error_line("a message is too long"));
            outgoing_.push_back('\n');
            continue;
        }
        const std::vector<std::string> answers = session.receive(line.text);
        for (const std::string& answer : answers) {
            outgoing_.append(answer);
            outgoing_.push_back('\n');
        }
    }
    const std::vector<std::string> pumped = session.pump();
    for (const std::string& answer : pumped) {
        outgoing_.append(answer);
        outgoing_.push_back('\n');
    }
    write_outgoing();
}

void Stdio::deliver(Session& session) {
    adopt();
    if (!ready_)
        return;
    const std::vector<std::string> pumped = session.pump();
    for (const std::string& answer : pumped) {
        outgoing_.append(answer);
        outgoing_.push_back('\n');
    }
    write_outgoing();
}

} // namespace oa::app::mcp

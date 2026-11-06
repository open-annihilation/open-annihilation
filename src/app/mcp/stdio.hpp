// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// MCP on standard input and standard output (stdio.cpp). The bytes are the
// ones the process was started with, kept before the log redirect, and a
// pump never waits on them.
#pragma once

#include "oa/app/mcp/session.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace oa::app::mcp {

/// Tells whether this run asked for MCP on standard input and output.
///
/// @return true after --mcp was accepted
[[nodiscard]] bool stdio_wanted() noexcept;

/// Duplicates standard input and standard output so the log redirect can
/// have the originals. Does nothing when it has already kept them.
void capture_stdio();

/// Tells whether capture_stdio kept both streams.
///
/// @return true when a pump can read and write them
[[nodiscard]] bool stdio_captured() noexcept;

/// The byte streams of one process. pump reads what is already waiting,
/// answers, and writes what the client has room for.
class Stdio {
  public:

    /// An idle stream. The captured handles are taken on the first pump.
    Stdio() = default;

    /// Reads, answers and writes, without waiting.
    ///
    /// At most 256 KiB are read and 32 messages answered. A line longer
    /// than max_message_bytes is a parse error. Writing stops when the
    /// client does not take the bytes, and reading stops while a megabyte
    /// is waiting to be written. Held calls are polled after the read.
    ///
    /// @param[in,out] session the session that answers
    void pump(Session& session);

    /// Polls held calls and writes their answers. Does not read.
    ///
    /// Used once a frame has been presented, so a picture of that frame can
    /// finish in the same frame that drew it.
    ///
    /// @param[in,out] session the session that answers
    void deliver(Session& session);

    /// Tells whether the client has closed standard input.
    ///
    /// @return true after a read found the end
    [[nodiscard]] bool client_gone() const noexcept { return eof_; }

    /// Tells whether an answer is still waiting to be written.
    ///
    /// @return true when bytes are buffered
    [[nodiscard]] bool response_pending() const noexcept { return !outgoing_.empty(); }

  private:

    /// Takes the captured handles. Later pumps find them already taken.
    void adopt() noexcept;

    /// Reads the bytes already waiting, at most 256 KiB, and does not wait.
    void read_some();

    /// Writes the buffered answers. A client that takes none stops the write.
    void write_outgoing();

    /// Makes the captured standard output non-blocking, once, on the first write.
    void arm_output() noexcept;

    LineBuffer lines_{};
    std::string outgoing_;
    intptr_t input_{-1};
    intptr_t output_{-1};
    bool ready_{};
    bool armed_{};
    bool eof_{};
};

} // namespace oa::app::mcp

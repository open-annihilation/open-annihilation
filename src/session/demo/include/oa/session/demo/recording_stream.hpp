// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// A recording's file, written as the game goes. The file starts with the
// bytes that come before a recording's packets, which are known when the game
// starts, and each packet is appended as one chunk. A packet is queued by the
// thread that records, and a thread of the stream's own writes the queue to
// the file, so a frame never waits for the disk.

#include "oa/base/threads.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace oa::session::demo {

/// The most bytes of packets the stream queues before the writer is woken.
inline constexpr std::size_t stream_flush_bytes = 16 * 1024;

/// The longest the writer is left asleep while packets wait, in milliseconds
/// on the clock the packets are added on.
inline constexpr uint64_t stream_flush_interval_ms = 1000;

/// A file that packets are appended to as they are made.
///
/// One thread adds packets and closes the stream. The stream's own thread
/// writes. A stream whose thread cannot start writes from `append` instead.
class RecordingStream {
  public:

    RecordingStream() = default;

    /// Closes the stream when it is open, waiting for the writer.
    ~RecordingStream();

    RecordingStream(const RecordingStream&) = delete;
    RecordingStream& operator=(const RecordingStream&) = delete;

    /// Makes the file and writes the bytes that start it.
    ///
    /// The file is written and flushed before this returns, so a game that
    /// stops at once still leaves a recording with its players. An existing
    /// file is replaced.
    ///
    /// @param path the file
    /// @param header the bytes that come before the packets
    /// @param[out] error why it could not be opened; may be null
    /// @return false when the stream is open already or the file cannot be written
    [[nodiscard]] bool
    open(const std::filesystem::path& path, std::span<const uint8_t> header, std::string* error);

    /// Queues one packet for the file.
    ///
    /// The packet is stored as one chunk: a little-endian length that counts
    /// its own two bytes, then the packet. One that does not fit a chunk is
    /// dropped and counted. Nothing here waits for the disk.
    ///
    /// @param packet the packet: u16 delay, u8 sender, payload
    /// @param now_ms the clock the packets are timed on, in milliseconds; the
    ///        writer is woken when stream_flush_interval_ms have gone by since
    ///        it last was
    void append(std::span<const uint8_t> packet, uint64_t now_ms);

    /// Writes what is queued, stops the writer and closes the file.
    ///
    /// @param[out] error why the file is not whole; may be null
    /// @return false when a write failed at any time since the file was opened
    [[nodiscard]] bool close(std::string* error);

    /// Tells whether the file is open.
    ///
    /// @return true from open until close
    [[nodiscard]] bool is_open() const noexcept;

    /// Returns how many packets were appended, those dropped not counted.
    ///
    /// @return the count
    [[nodiscard]] uint64_t packets() const noexcept;

    /// Returns how many packets did not fit a chunk and were dropped.
    ///
    /// @return the count
    [[nodiscard]] uint64_t dropped() const noexcept;

    /// Returns the file.
    ///
    /// @return the path given to open
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:

    /// Runs the writer thread.
    ///
    /// @param self the stream
    static void run(void* self);

    /// Writes a batch to the file and flushes it; a failure is kept.
    ///
    /// @param bytes the batch
    void write_batch(const std::vector<uint8_t>& bytes);

    std::filesystem::path path_{};
    std::ofstream file_{};
    mutable oa::base::threads::Mutex mutex_{};
    oa::base::threads::ConditionVariable wake_{};
    oa::base::threads::Thread thread_{};
    std::vector<uint8_t> pending_{}; ///< queued, guarded by mutex_
    uint64_t last_wake_ms_{};        ///< guarded by mutex_
    uint64_t appended_{};            ///< guarded by mutex_
    uint64_t dropped_{};             ///< guarded by mutex_
    bool flush_requested_{};         ///< guarded by mutex_
    bool closing_{};                 ///< guarded by mutex_
    bool failed_{};                  ///< guarded by mutex_
    bool open_{};
    bool threaded_{};
    std::string failure_{};
};

} // namespace oa::session::demo

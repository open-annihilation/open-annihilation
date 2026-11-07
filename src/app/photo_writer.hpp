// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Writes photos off the frame's thread. The frame queues a picture. This
// encodes it and writes the file, then the frame reads the result.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace oa::app {

/// Encodes and writes queued photos on a thread of its own.
///
/// At most four photos are outstanding, counting the one being written. A
/// fifth is refused by `enqueue`. Each file is written under a temporary
/// name in the same folder and renamed when the write is whole. `finish`
/// writes every photo already accepted, including the one in hand, and joins
/// the thread. The strings of a future text chunk live in the job until
/// `write` returns.
class PhotoWriter {
  public:

    /// One picture accepted for saving.
    struct Picture {
        std::vector<uint8_t> rgb;               ///< 8-bit RGB, row by row
        uint32_t width = 0;                     ///< the picture's width
        uint32_t height = 0;                    ///< the picture's height
        std::filesystem::path path;             ///< the file, once the write is whole
        std::string file_name;                  ///< the file's name, without a folder
        std::vector<std::string> text_keywords; ///< kept for a later ticket's text
        std::vector<std::string> text_values;   ///< one value for each keyword
    };

    /// The outcome of one accepted photo, for the frame's thread to read.
    struct Notice {
        bool saved = false;    ///< the file was renamed into place
        std::string file_name; ///< the file's name, without a folder
        std::string reason;    ///< why it was not saved, in English, when `saved` is false
    };

    /// Makes a writer. Its thread starts when the first photo is queued.
    PhotoWriter();
    PhotoWriter(const PhotoWriter&) = delete;
    PhotoWriter& operator=(const PhotoWriter&) = delete;
    /// Writes every photo already accepted and joins the thread.
    ~PhotoWriter();

    /// Queues a picture. Refuses it when four are already outstanding.
    ///
    /// @param picture the picture and the file it takes
    /// @return false when four photos are still being saved
    bool enqueue(Picture picture);

    /// Tells whether a file name is reserved by a photo not yet renamed.
    ///
    /// @param file_name the file's name, without a folder
    /// @return true while that photo is queued or being written
    [[nodiscard]] bool holds(std::string_view file_name) const;

    /// How many photos are queued or being written.
    ///
    /// @return the count, at most four
    [[nodiscard]] int outstanding() const;

    /// Reads one finished photo, if one is waiting.
    ///
    /// @return the notice, or nothing when none has finished since the last poll
    [[nodiscard]] std::optional<Notice> poll();

    /// Waits until every accepted photo has finished.
    void wait_until_idle();

    /// Writes the photos already accepted and joins the thread.
    void finish() noexcept;

  private:

    /// Encodes and writes until asked to stop with the queue empty.
    void run();

    /// Encodes one picture and writes its file.
    ///
    /// @param picture the picture
    /// @return the notice
    [[nodiscard]] Notice save(const Picture& picture);

    mutable std::mutex mutex_;
    std::condition_variable work_;
    std::condition_variable idle_;
    std::deque<Picture> queue_;
    std::deque<Notice> notices_;
    std::unordered_set<std::string> reserved_;
    std::string writing_name_;
    int outstanding_ = 0;
    bool stopping_ = false;
    std::thread thread_;
};

} // namespace oa::app

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Writes photos off the frame's thread. The frame queues a picture. This
// encodes it and writes the file, then the frame reads the result.
#pragma once

#include "oa/present/photo/photo_texts.hpp"

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
/// the thread. The strings of a text chunk live in the job until `write`
/// returns.
class PhotoWriter {
  public:

    /// One picture accepted for saving.
    struct Picture {
        std::vector<uint8_t> rgb;   ///< 8-bit RGB, row by row
        uint32_t width = 0;         ///< the picture's width
        uint32_t height = 0;        ///< the picture's height
        std::filesystem::path path; ///< the file, once the write is whole
        std::string file_name;      ///< the file's name, without a folder
        /// Text chunks. Each keyword and value stays alive until `write` returns.
        /// Software, Creation Time and Source are Latin-1; the others are UTF-8.
        std::vector<oa::present::photo::PhotoText> texts;
    };

    /// The outcome of one accepted photo, for the frame's thread to read.
    ///
    /// The clipboard image is prepared on this thread: the PNG's bytes on
    /// macOS and Linux, a BMP encoded from the pixels on Windows. The frame's
    /// thread offers them. `clipboard` is empty when the photo was not saved,
    /// the copy is above its limits, or the clipboard holds text only.
    struct Notice {
        bool saved = false;             ///< the file was renamed into place
        std::string file_name;          ///< the file's name, without a folder
        std::string reason;             ///< why it was not saved, in English, when `saved` is false
        uint32_t width = 0;             ///< the photo's width
        uint32_t height = 0;            ///< the photo's height
        std::vector<uint8_t> clipboard; ///< the PNG or BMP offered to the clipboard
        const char* clipboard_mime = nullptr; ///< "image/png" or "image/bmp", or null
        std::string copy_refusal;             ///< why the copy was refused, in English, or empty
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

/// Wakes whoever is waiting when one photo's notice is queued.
///
/// Called once for each photo the writer accepted, saved or refused, after
/// the notice can be read. Null wakes no one. The pointer is called from
/// the writer's thread.
using PhotoWriterWake = void (*)() noexcept;

/// Stands a wake in. Null wakes no one.
///
/// @param wake the wake, or null
void set_photo_writer_wake(PhotoWriterWake wake) noexcept;

} // namespace oa::app

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/session/demo/recording_stream.hpp"

#include "oa/formats/tad.hpp"

#include <cstring>
#include <ios>
#include <string>
#include <utility>

namespace oa::session::demo {

namespace {

namespace threads = oa::base::threads;

/// The longest packet a chunk holds: the chunk's own length word is part of its length.
constexpr std::size_t longest_packet =
    formats::tad::limit::chunk_bytes - formats::tad::layout::chunk_length_bytes;

} // namespace

RecordingStream::~RecordingStream() {
    if (open_)
        (void)close(nullptr);
}

bool RecordingStream::open(
    const std::filesystem::path& path, std::span<const uint8_t> header, std::string* error
) {
    const auto fail = [error](std::string text) {
        if (error != nullptr)
            *error = std::move(text);
        return false;
    };
    if (open_)
        return fail("the stream is open already");
    file_.open(path, std::ios::binary | std::ios::trunc);
    if (!file_)
        return fail("the file cannot be made");
    file_.write(
        reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size())
    );
    file_.flush();
    if (!file_) {
        file_.close();
        return fail("the file cannot be written");
    }
    path_ = path;
    pending_.clear();
    last_wake_ms_ = 0;
    appended_ = 0;
    dropped_ = 0;
    flush_requested_ = false;
    closing_ = false;
    failed_ = false;
    failure_.clear();
    threaded_ = threads::start_thread(thread_, &RecordingStream::run, this);
    open_ = true;
    return true;
}

void RecordingStream::append(std::span<const uint8_t> packet, uint64_t now_ms) {
    if (!open_)
        return;
    bool wake = false;
    std::vector<uint8_t> direct;
    {
        threads::LockGuard guard(mutex_);
        if (packet.size() > longest_packet) {
            ++dropped_;
            return;
        }
        const auto length =
            static_cast<uint16_t>(packet.size() + formats::tad::layout::chunk_length_bytes);
        pending_.push_back(static_cast<uint8_t>(length));
        pending_.push_back(static_cast<uint8_t>(length >> 8));
        pending_.insert(pending_.end(), packet.begin(), packet.end());
        ++appended_;
        if (pending_.size() >= stream_flush_bytes ||
            now_ms - last_wake_ms_ >= stream_flush_interval_ms) {
            last_wake_ms_ = now_ms;
            if (threaded_) {
                flush_requested_ = true;
                wake = true;
            } else {
                // No writer thread could start: this thread writes.
                direct.swap(pending_);
            }
        }
    }
    if (wake)
        wake_.notify_one();
    if (!direct.empty())
        write_batch(direct);
}

bool RecordingStream::close(std::string* error) {
    if (!open_)
        return true;
    {
        threads::LockGuard guard(mutex_);
        closing_ = true;
    }
    if (threaded_) {
        wake_.notify_one();
        threads::join_thread(thread_);
        threaded_ = false;
    } else {
        std::vector<uint8_t> rest;
        {
            threads::LockGuard guard(mutex_);
            rest.swap(pending_);
        }
        if (!rest.empty())
            write_batch(rest);
    }
    file_.close();
    open_ = false;
    threads::LockGuard guard(mutex_);
    if (failed_ && error != nullptr)
        *error = failure_;
    return !failed_;
}

bool RecordingStream::is_open() const noexcept {
    return open_;
}

uint64_t RecordingStream::packets() const noexcept {
    threads::LockGuard guard(mutex_);
    return appended_;
}

uint64_t RecordingStream::dropped() const noexcept {
    threads::LockGuard guard(mutex_);
    return dropped_;
}

void RecordingStream::run(void* self) {
    auto& stream = *static_cast<RecordingStream*>(self);
    std::vector<uint8_t> batch;
    for (;;) {
        bool last = false;
        {
            threads::LockGuard guard(stream.mutex_);
            stream.wake_.wait(stream.mutex_, [&stream] {
                return stream.flush_requested_ || stream.closing_;
            });
            batch.swap(stream.pending_);
            stream.pending_.clear();
            stream.flush_requested_ = false;
            last = stream.closing_;
        }
        if (!batch.empty())
            stream.write_batch(batch);
        batch.clear();
        if (last)
            return;
    }
}

void RecordingStream::write_batch(const std::vector<uint8_t>& bytes) {
    {
        threads::LockGuard guard(mutex_);
        if (failed_)
            return;
    }
    file_.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    file_.flush();
    if (file_)
        return;
    threads::LockGuard guard(mutex_);
    failed_ = true;
    failure_ = "the file cannot be written";
}

} // namespace oa::session::demo

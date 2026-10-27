// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The worker that fetches one package, resumes after a cut, and checks the
// file against the feed before it may be installed.
#include "oa/app/update/download.hpp"

namespace oa::app::update {

void RateMeter::sample(uint64_t at_ms, uint64_t bytes) noexcept {
    if (count_ > 0) {
        const Sample& last = samples_[count_ - 1];
        if (bytes < last.bytes || at_ms < last.at_ms) {
            reset();
        } else if (at_ms == last.at_ms) {
            samples_[count_ - 1].bytes = bytes;
            return;
        }
    }
    if (count_ == most_samples) {
        for (std::size_t index = 1; index < count_; ++index)
            samples_[index - 1] = samples_[index];
        --count_;
    }
    samples_[count_] = Sample{at_ms, bytes};
    ++count_;
    // Samples older than the window are dropped, keeping at least two.
    std::size_t first = 0;
    while (count_ - first > 2 && at_ms - samples_[first].at_ms > rate_window_ms)
        ++first;
    if (first > 0) {
        for (std::size_t index = first; index < count_; ++index)
            samples_[index - first] = samples_[index];
        count_ -= first;
    }
}

void RateMeter::reset() noexcept {
    count_ = 0;
}

uint64_t RateMeter::bytes_per_second() const noexcept {
    if (count_ < 2)
        return 0;
    const Sample& first = samples_[0];
    const Sample& last = samples_[count_ - 1];
    const uint64_t span = last.at_ms - first.at_ms;
    if (span < rate_least_ms)
        return 0;
    const uint64_t gained = last.bytes - first.bytes;
    // Kept within 64 bits: a gain past 2^54 bytes is past any package.
    if (gained > (UINT64_MAX / 1000U))
        return gained / span * 1000U;
    return gained * 1000U / span;
}

} // namespace oa::app::update

#if OA_SELF_UPDATE

#include "oa/base/sha256.hpp"
#include "oa/formats/url.hpp"
#include "oa/netgame/http/client.hpp"
#include "oa/platform/files.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <io.h>
#else
#include <unistd.h>
#endif

namespace oa::app::update {
namespace {

namespace fs = std::filesystem;
namespace http = oa::netgame::http;
namespace sha = oa::base::sha256;
namespace url = oa::formats::url;

/// How many times one mirror may drop the connection before the next is tried.
constexpr int mirror_cut_limit = 4;

/// Why a body stopped short of the file.
enum class Halt : uint8_t {
    none,    ///< the body was accepted
    paused,  ///< a match or the player asked the fetch to stop at a step
    differs, ///< the answer was not the file
    disk,    ///< the part could not be written
    status,  ///< the status was not 200 or 206
};

/// Moves a file to `offset`.
///
/// @param file an open file
/// @param offset the byte to sit at
/// @return true when the file sits there
bool seek_file(std::FILE* file, uint64_t offset) noexcept {
    if (offset > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
        return false;
#if defined(_WIN32)
    return _fseeki64(file, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return ::fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

/// Flushes a file and asks the system to put it on disk.
///
/// @param file an open file
/// @return true when the system reports the bytes written
bool sync_file(std::FILE* file) noexcept {
    if (std::fflush(file) != 0)
        return false;
#if defined(_WIN32)
    return _commit(_fileno(file)) == 0;
#else
    return ::fsync(::fileno(file)) == 0;
#endif
}

/// Writes every byte, or stops on the first short write.
///
/// @param file an open file
/// @param data the bytes
/// @param size how many
/// @return true when every byte was written
bool write_all(std::FILE* file, const uint8_t* data, std::size_t size) noexcept {
    while (size > 0) {
        const std::size_t wrote = std::fwrite(data, 1, size, file);
        if (wrote == 0)
            return false;
        data += wrote;
        size -= wrote;
    }
    return true;
}

/// True when `text` is a plain file name: 1 to 128 of `[A-Za-z0-9._-]`, not `.` or `..`.
///
/// @param text the feed's file name
/// @return true when it may be joined into a path
bool plain_file_name(std::string_view text) {
    constexpr std::size_t most = 128;
    if (text.empty() || text.size() > most || text == "." || text == "..")
        return false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(text[index]);
        const bool letter = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
        const bool digit = byte >= '0' && byte <= '9';
        const bool mark = byte == '.' || byte == '_' || byte == '-';
        if (!letter && !digit && !mark)
            return false;
    }
    return true;
}

/// Reads a folder name that is only digits.
///
/// @param name the folder's name
/// @return the number, or nothing when it is not one
std::optional<int64_t> release_folder_number(std::string_view name) {
    if (name.empty() || name.size() > 18)
        return std::nullopt;
    int64_t value = 0;
    for (std::size_t index = 0; index < name.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(name[index]);
        if (byte < '0' || byte > '9')
            return std::nullopt;
        const int digit = byte - '0';
        if (value > (std::numeric_limits<int64_t>::max() - digit) / 10)
            return std::nullopt;
        value = value * 10 + digit;
    }
    return value;
}

/// A path's last component, as bytes.
///
/// @param path the path
/// @return the file name
std::string leaf_name(const fs::path& path) {
    const std::u8string text = path.filename().u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

/// Removes numbered release folders older than `release`.
///
/// `feed.json` and any folder whose name is not a release number stay.
///
/// @param folder `<data>/updates`
/// @param release the release being fetched
void remove_older_releases(const fs::path& folder, int64_t release) {
    std::error_code error;
    if (!fs::is_directory(folder, error))
        return;
    fs::directory_iterator cursor(folder, error);
    const fs::directory_iterator end;
    while (!error && cursor != end) {
        const fs::directory_entry entry = *cursor;
        cursor.increment(error);
        std::error_code status_error;
        if (!entry.is_directory(status_error) || status_error)
            continue;
        const std::optional<int64_t> number = release_folder_number(leaf_name(entry.path()));
        if (!number.has_value() || *number >= release)
            continue;
        std::error_code remove_error;
        fs::remove_all(entry.path(), remove_error);
    }
}

/// The address of one file on one mirror.
///
/// @param mirror a mirror, ending in `/`
/// @param file the plain file name
/// @return the address
std::string mirror_file_url(const url::Url& mirror, std::string_view file) {
    std::string text = url::url_text(mirror);
    if (text.empty() || text.back() != '/')
        text.push_back('/');
    text.append(file);
    return text;
}

/// Bytes already committed: a whole number of steps, or the whole file.
///
/// A short tail that never reached a step is dropped.
///
/// @param part the part
/// @param expected the size the feed promised
/// @return the committed size; 0 when the part is missing or unusable
uint64_t committed_bytes(const fs::path& part, uint64_t expected) {
    std::error_code error;
    if (!fs::is_regular_file(part, error) || error)
        return 0;
    const uintmax_t wide = fs::file_size(part, error);
    if (error || wide > static_cast<uintmax_t>(expected)) {
        if (!error)
            fs::resize_file(part, 0, error);
        return 0;
    }
    uint64_t have = static_cast<uint64_t>(wide);
    if (have != expected) {
        const uint64_t aligned = have - (have % download_step_bytes);
        if (aligned != have) {
            fs::resize_file(part, static_cast<uintmax_t>(aligned), error);
            if (error)
                return 0;
            have = aligned;
        }
    }
    return have;
}

/// The SHA-256 of a file that is exactly `expected` bytes.
///
/// @param path the file
/// @param expected how many bytes it must hold
/// @return the digest, or nothing when the size differs or the file cannot be read
std::optional<sha::Digest> hash_file(const fs::path& path, uint64_t expected) {
    std::FILE* file = oa::platform::open_file(path, "rb");
    if (file == nullptr)
        return std::nullopt;
    sha::Hasher hasher;
    std::vector<uint8_t> buffer(64 * 1024);
    uint64_t got = 0;
    while (got < expected) {
        const uint64_t room = expected - got;
        std::size_t ask = buffer.size();
        if (static_cast<uint64_t>(ask) > room)
            ask = static_cast<std::size_t>(room);
        const std::size_t read = std::fread(buffer.data(), 1, ask, file);
        if (read == 0) {
            std::fclose(file);
            return std::nullopt;
        }
        sha::update(hasher, std::span<const uint8_t>(buffer.data(), read));
        got += static_cast<uint64_t>(read);
    }
    unsigned char extra = 0;
    const std::size_t more = std::fread(&extra, 1, 1, file);
    std::fclose(file);
    if (more != 0)
        return std::nullopt;
    return sha::finish(hasher);
}

/// Deletes the part and a finished file left beside it.
///
/// @param part the part
/// @param final_path the finished file
void remove_download(const fs::path& part, const fs::path& final_path) {
    std::error_code error;
    fs::remove(part, error);
    fs::remove(final_path, error);
}

/// Moves a closed part onto the package.
///
/// The part's handle must already be closed. Windows will not rename a file
/// that is open, and it will not replace one that is already there unless the
/// replace is asked for.
///
/// @param part the part
/// @param final_path the package
/// @return false when the package could not take the part's place
bool store_part(const fs::path& part, const fs::path& final_path) {
#if defined(_WIN32)
    return MoveFileExW(
               part.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
           ) != 0;
#else
    std::error_code error;
    fs::remove(final_path, error);
    error.clear();
    fs::rename(part, final_path, error);
    return !error;
#endif
}

/// Writes one response into the part, one step at a time.
class PartSink final : public http::BodySink {
  public:

    /// Records the part and how far it has already been written.
    ///
    /// @param part the part
    /// @param size the size the feed promised
    /// @param done bytes already committed
    /// @param ranged true when a Range was sent
    /// @param cancel set when the download should stop
    /// @param match set while a match is running
    /// @param held set while the player has paused the download
    /// @param on_step called with the bytes committed after each step; may be null
    /// @param step_context what on_step is called with
    PartSink(
        fs::path part,
        uint64_t size,
        uint64_t done,
        bool ranged,
        const std::atomic<bool>& cancel,
        const std::atomic<bool>& match,
        const std::atomic<bool>& held,
        void (*on_step)(void* context, uint64_t done),
        void* step_context
    )
        : done_(done), part_(std::move(part)), size_(size), ranged_(ranged), cancel_(&cancel),
          match_(&match), held_(&held), on_step_(on_step), step_context_(step_context) {}

    /// Closes the part.
    ~PartSink() override { close(); }

    /// Closes the part so it can be renamed or removed.
    ///
    /// Windows refuses both while this handle is open.
    void release() noexcept { close(); }

    PartSink(const PartSink&) = delete;
    PartSink& operator=(const PartSink&) = delete;

    /// Accepts a length that matches the feed, and restarts on a 200.
    ///
    /// @param head the response, before any body byte
    /// @return false when the body must not be written
    bool begin(const http::Response& head) override {
        if (head.status == 200) {
            if (!head.content_length.has_value() || *head.content_length != size_) {
                halt_ = Halt::differs;
                return false;
            }
            if (ranged_) {
                done_ = 0;
                std::error_code error;
                if (fs::exists(part_, error))
                    fs::resize_file(part_, 0, error);
                if (error) {
                    halt_ = Halt::disk;
                    return false;
                }
            }
        } else if (head.status == 206) {
            const bool range_ok =
                head.content_range.has_value() && head.content_range->total.has_value() &&
                *head.content_range->total == size_ && head.content_range->first == done_;
            if (!range_ok) {
                halt_ = Halt::differs;
                return false;
            }
        } else {
            halt_ = Halt::status;
            return false;
        }
        const char* mode = (head.status == 200) ? "wb" : "r+b";
        file_ = oa::platform::open_file(part_, mode);
        if (file_ == nullptr && head.status == 206)
            file_ = oa::platform::open_file(part_, "wb");
        if (file_ == nullptr) {
            halt_ = Halt::disk;
            return false;
        }
        if (done_ > 0 && !seek_file(file_, done_)) {
            halt_ = Halt::disk;
            return false;
        }
        return true;
    }

    /// Buffers bytes until a step is full, then writes and flushes that step.
    ///
    /// A cancel writes nothing from this piece. A match or the player's pause
    /// finishes the current step and then stops, unless that step is the end
    /// of the file.
    ///
    /// @param bytes the next piece
    /// @return false to stop the fetch
    bool write(std::span<const uint8_t> bytes) override {
        if (halt_ != Halt::none)
            return false;
        if (cancel_->load()) {
            halt_ = Halt::status;
            return false;
        }
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            if (done_ + static_cast<uint64_t>(buffer_.size()) > size_) {
                halt_ = Halt::differs;
                return false;
            }
            const uint64_t room = size_ - done_ - static_cast<uint64_t>(buffer_.size());
            if (room == 0) {
                halt_ = Halt::differs;
                return false;
            }
            std::size_t take = bytes.size() - offset;
            const uint64_t step_room =
                static_cast<uint64_t>(download_step_bytes) - static_cast<uint64_t>(buffer_.size());
            if (static_cast<uint64_t>(take) > step_room)
                take = static_cast<std::size_t>(step_room);
            if (static_cast<uint64_t>(take) > room)
                take = static_cast<std::size_t>(room);
            buffer_.insert(
                buffer_.end(),
                bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                bytes.begin() + static_cast<std::ptrdiff_t>(offset + take)
            );
            offset += take;
            const bool step_full = buffer_.size() == download_step_bytes;
            const bool file_full = done_ + static_cast<uint64_t>(buffer_.size()) == size_;
            if (step_full || file_full) {
                if (!flush_buffer())
                    return false;
                if (on_step_ != nullptr)
                    on_step_(step_context_, done_);
                if (!file_full && (match_->load() || held_->load())) {
                    halt_ = Halt::paused;
                    return false;
                }
                if (cancel_->load())
                    return false;
            }
        }
        return true;
    }

    /// Bytes committed to the part.
    uint64_t done_ = 0;
    /// Why the body stopped.
    Halt halt_ = Halt::none;

  private:

    /// Writes the buffer and flushes it.
    ///
    /// @return false on a disk error
    bool flush_buffer() {
        if (file_ == nullptr || buffer_.empty())
            return true;
        errno = 0;
        if (!write_all(file_, buffer_.data(), buffer_.size()) || std::fflush(file_) != 0) {
            halt_ = Halt::disk;
            buffer_.clear();
            return false;
        }
        done_ += static_cast<uint64_t>(buffer_.size());
        buffer_.clear();
        return true;
    }

    /// Closes the part without syncing.
    void close() noexcept {
        if (file_ != nullptr) {
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    fs::path part_;
    uint64_t size_ = 0;
    bool ranged_ = false;
    const std::atomic<bool>* cancel_ = nullptr;
    const std::atomic<bool>* match_ = nullptr;
    const std::atomic<bool>* held_ = nullptr;
    void (*on_step_)(void* context, uint64_t done) = nullptr;
    void* step_context_ = nullptr;
    std::FILE* file_ = nullptr;
    std::vector<uint8_t> buffer_;
};

/// Milliseconds on a steady clock, for the rate.
///
/// @return the clock
uint64_t steady_ms() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now).count()
    );
}

} // namespace

bool package_downloaded(
    const fs::path& folder,
    const oa::data::engine_feed::Release& release,
    const oa::data::engine_feed::Package& package
) {
    const fs::path directory = folder / std::to_string(release.release);
    std::error_code error;
    if (plain_file_name(package.zip.file) &&
        fs::is_regular_file(directory / package.zip.file, error))
        return true;
    return package.installer.has_value() && plain_file_name(package.installer->file) &&
           fs::is_regular_file(directory / package.installer->file, error);
}

PackageDownload::PackageDownload() = default;

PackageDownload::~PackageDownload() {
    {
        base::threads::LockGuard guard(mutex_);
        stop_ = true;
        cancel_.store(true);
    }
    wake_.notify_all();
    base::threads::join_thread(thread_);
}

void PackageDownload::start(
    const oa::data::engine_feed::Release& release,
    const oa::data::engine_feed::Package& package,
    bool installer,
    fs::path folder
) {
    const oa::data::engine_feed::PackageFile* chosen = nullptr;
    std::string problem;
    if (installer) {
        if (!package.installer.has_value())
            problem = "the package has no installer";
        else
            chosen = &*package.installer;
    } else {
        chosen = &package.zip;
    }
    if (problem.empty() && (chosen == nullptr || chosen->file.empty() || chosen->size == 0))
        problem = "the package has no file";
    if (problem.empty() && chosen != nullptr && !plain_file_name(chosen->file))
        problem = "the file name is not a plain name";
    if (problem.empty() && package.mirrors.empty())
        problem = "the package names no mirror";

    Job job;
    uint64_t total = 0;
    fs::path part;
    if (problem.empty()) {
        job.release = release;
        job.package = package;
        job.file = *chosen;
        job.folder = std::move(folder);
        job.directory = job.folder / std::to_string(release.release);
        job.final_path = job.directory / job.file.file;
        job.part = job.directory / (job.file.file + std::string(part_suffix));
        total = job.file.size;
        part = job.part;
    }

    base::threads::LockGuard guard(mutex_);
    if (busy_ || stop_)
        return;
    if (!problem.empty()) {
        DownloadSnapshot failed;
        failed.state = DownloadState::failed;
        failed.failure = std::move(problem);
        failed.failure_kind = DownloadFailure::unusable;
        failed.generation = snapshot_.generation + 1;
        snapshot_ = std::move(failed);
        return;
    }
    cancel_.store(false);
    discard_.store(false);
    last_part_ = job.part;
    last_final_ = job.final_path;
    job_ = std::move(job);
    has_job_ = true;
    busy_ = true;
    DownloadSnapshot next;
    next.state = moving_state();
    next.total = total;
    next.path = std::move(part);
    next.generation = snapshot_.generation + 1;
    snapshot_ = std::move(next);
    if (!thread_started_) {
        if (!base::threads::start_thread(thread_, &PackageDownload::thread_main, this)) {
            has_job_ = false;
            busy_ = false;
            DownloadSnapshot failed;
            failed.state = DownloadState::failed;
            failed.failure = "the download could not start";
            failed.failure_kind = DownloadFailure::storage;
            failed.generation = snapshot_.generation + 1;
            snapshot_ = std::move(failed);
            return;
        }
        thread_started_ = true;
    }
    wake_.notify_one();
}

void PackageDownload::set_match_running(bool running) noexcept {
    match_.store(running);
    wake_.notify_all();
}

void PackageDownload::cancel() noexcept {
    cancel_.store(true);
    wake_.notify_all();
}

void PackageDownload::pause() noexcept {
    held_.store(true);
    {
        base::threads::LockGuard guard(mutex_);
        if (busy_ && (snapshot_.state == DownloadState::downloading ||
                      snapshot_.state == DownloadState::paused_for_match)) {
            snapshot_.state = DownloadState::paused_by_player;
            snapshot_.bytes_per_second = 0;
            snapshot_.generation += 1;
        }
    }
    wake_.notify_all();
}

void PackageDownload::resume() noexcept {
    held_.store(false);
    {
        base::threads::LockGuard guard(mutex_);
        if (busy_ && snapshot_.state == DownloadState::paused_by_player) {
            snapshot_.state = moving_state();
            snapshot_.generation += 1;
        }
    }
    wake_.notify_all();
}

bool PackageDownload::paused() const noexcept {
    return held_.load();
}

void PackageDownload::discard() noexcept {
    bool running = false;
    {
        base::threads::LockGuard guard(mutex_);
        running = busy_;
        if (running) {
            discard_.store(true);
            cancel_.store(true);
        } else {
            remove_download(last_part_, last_final_);
            DownloadSnapshot cleared;
            cleared.generation = snapshot_.generation + 1;
            snapshot_ = std::move(cleared);
        }
    }
    held_.store(false);
    if (running)
        wake_.notify_all();
}

DownloadSnapshot PackageDownload::snapshot() const {
    base::threads::LockGuard guard(mutex_);
    return snapshot_;
}

void PackageDownload::set_tls_available(bool available) noexcept {
    tls_mode_.store(available ? 1 : 0);
}

void PackageDownload::thread_main(void* argument) {
    static_cast<PackageDownload*>(argument)->run();
}

void PackageDownload::run() {
    for (;;) {
        Job job;
        {
            base::threads::LockGuard guard(mutex_);
            while (!stop_ && !has_job_)
                wake_.wait(mutex_);
            if (stop_)
                return;
            job = std::move(job_);
            has_job_ = false;
        }
        if (cancel_.load()) {
            if (discard_files(job.part, job.final_path))
                continue;
            DownloadSnapshot failed;
            failed.state = DownloadState::failed;
            failed.failure = "the download was cancelled";
            failed.failure_kind = DownloadFailure::cancelled;
            publish(std::move(failed), true);
            continue;
        }
        try {
            run_job(job);
        } catch (const std::exception& failure) {
            DownloadSnapshot failed;
            failed.state = DownloadState::failed;
            failed.failure = failure.what();
            failed.failure_kind = DownloadFailure::storage;
            publish(std::move(failed), true);
        } catch (...) {
            DownloadSnapshot failed;
            failed.state = DownloadState::failed;
            failed.failure = "the download failed";
            failed.failure_kind = DownloadFailure::storage;
            publish(std::move(failed), true);
        }
    }
}

void PackageDownload::publish(DownloadSnapshot next, bool finished) {
    base::threads::LockGuard guard(mutex_);
    next.generation = snapshot_.generation + 1;
    snapshot_ = std::move(next);
    if (finished)
        busy_ = false;
}

DownloadState PackageDownload::moving_state() const noexcept {
    if (held_.load())
        return DownloadState::paused_by_player;
    if (match_.load())
        return DownloadState::paused_for_match;
    return DownloadState::downloading;
}

void PackageDownload::note_step(void* context, uint64_t done) {
    auto* download = static_cast<PackageDownload*>(context);
    download->rate_.sample(steady_ms(), done);
    DownloadSnapshot moving;
    moving.state = download->moving_state();
    moving.received = done;
    moving.total = download->step_total_;
    moving.path = download->step_part_;
    if (moving.state == DownloadState::downloading)
        moving.bytes_per_second = download->rate_.bytes_per_second();
    download->publish(std::move(moving), false);
}

bool PackageDownload::discard_files(const fs::path& part, const fs::path& final_path) {
    if (!discard_.load())
        return false;
    remove_download(part, final_path);
    discard_.store(false);
    publish(DownloadSnapshot{}, true);
    return true;
}

bool PackageDownload::wait_while_held(uint64_t received, uint64_t total, const fs::path& part) {
    base::threads::LockGuard guard(mutex_);
    DownloadState shown = DownloadState::downloading;
    while ((match_.load() || held_.load()) && !cancel_.load() && !stop_) {
        const DownloadState now =
            held_.load() ? DownloadState::paused_by_player : DownloadState::paused_for_match;
        if (now != shown) {
            DownloadSnapshot paused;
            paused.state = now;
            paused.received = received;
            paused.total = total;
            paused.path = part;
            paused.generation = snapshot_.generation + 1;
            snapshot_ = std::move(paused);
            shown = now;
        }
        wake_.wait(mutex_);
    }
    return !cancel_.load() && !stop_;
}

bool PackageDownload::tls_on() const noexcept {
    const int mode = tls_mode_.load();
    if (mode < 0)
        return http::tls_available();
    return mode != 0;
}

void PackageDownload::run_job(const Job& job) {
    std::error_code error;
    fs::create_directories(job.directory, error);
    if (error) {
        DownloadSnapshot failed;
        failed.state = DownloadState::failed;
        failed.failure = "the update folder could not be created";
        failed.failure_kind = DownloadFailure::storage;
        publish(std::move(failed), true);
        return;
    }
    remove_older_releases(job.folder, job.release.release);
    rate_.reset();
    step_total_ = job.file.size;
    step_part_ = job.part;

    const auto fail_ready = [this,
                             &job](DownloadFailure kind, std::string failure, bool drop_file) {
        if (kind == DownloadFailure::cancelled && discard_files(job.part, job.final_path))
            return;
        if (drop_file)
            remove_download(job.part, job.final_path);
        DownloadSnapshot failed;
        failed.state = DownloadState::failed;
        failed.failure = std::move(failure);
        failed.failure_kind = kind;
        failed.total = job.file.size;
        publish(std::move(failed), true);
    };
    const auto cancelled = [&fail_ready]() {
        fail_ready(DownloadFailure::cancelled, "the download was cancelled", false);
    };

    const auto publish_ready = [this, &job]() {
        DownloadSnapshot ready;
        ready.state = DownloadState::ready;
        ready.received = job.file.size;
        ready.total = job.file.size;
        ready.path = job.final_path;
        publish(std::move(ready), true);
    };

    // A package an earlier start downloaded and checked is checked again and used.
    if (fs::is_regular_file(job.final_path, error) && !error) {
        DownloadSnapshot checking;
        checking.state = DownloadState::verifying;
        checking.received = job.file.size;
        checking.total = job.file.size;
        checking.path = job.final_path;
        publish(checking, false);
        const std::optional<sha::Digest> kept = hash_file(job.final_path, job.file.size);
        if (kept.has_value() && *kept == job.file.sha256) {
            std::error_code part_error;
            fs::remove(job.part, part_error);
            publish_ready();
            return;
        }
        fs::remove(job.final_path, error);
        error.clear();
    }

    const auto accept_file = [this, &job, &fail_ready, &publish_ready]() -> bool {
        DownloadSnapshot checking;
        checking.state = DownloadState::verifying;
        checking.received = job.file.size;
        checking.total = job.file.size;
        checking.path = job.part;
        publish(checking, false);
        const std::optional<sha::Digest> digest = hash_file(job.part, job.file.size);
        if (!digest.has_value() || *digest != job.file.sha256) {
            fail_ready(DownloadFailure::wrong_file, std::string(wrong_file_failure), true);
            return false;
        }
        if (!store_part(job.part, job.final_path)) {
            fail_ready(DownloadFailure::storage, "the download could not be stored", false);
            return false;
        }
        publish_ready();
        return true;
    };

    uint64_t done = committed_bytes(job.part, job.file.size);
    if (done == job.file.size) {
        accept_file();
        return;
    }

    if (match_.load() || held_.load()) {
        if (!wait_while_held(done, job.file.size, job.part)) {
            cancelled();
            return;
        }
    }

    http::Client client;
    bool any_plain = false;
    std::string last_failure = "every mirror failed";
    for (const url::Url& mirror : job.package.mirrors) {
        if (cancel_.load()) {
            cancelled();
            return;
        }
        if (mirror.scheme == url::Scheme::https && !tls_on())
            continue;
        any_plain = true;
        const std::string address = mirror_file_url(mirror, job.file.file);
        int cuts = 0;
        while (cuts < mirror_cut_limit) {
            if (cancel_.load()) {
                cancelled();
                return;
            }
            if (match_.load() || held_.load()) {
                if (!wait_while_held(done, job.file.size, job.part)) {
                    cancelled();
                    return;
                }
            }
            done = committed_bytes(job.part, job.file.size);
            if (done == job.file.size) {
                accept_file();
                return;
            }
            rate_.sample(steady_ms(), done);
            DownloadSnapshot fetching;
            fetching.state = moving_state();
            fetching.received = done;
            fetching.total = job.file.size;
            fetching.path = job.part;
            publish(std::move(fetching), false);

            PartSink sink(
                job.part,
                job.file.size,
                done,
                done > 0,
                cancel_,
                match_,
                held_,
                &PackageDownload::note_step,
                this
            );
            http::Request request;
            request.url = address;
            request.timeouts.connect_ms = 10000;
            request.timeouts.idle_ms = 20000;
            request.timeouts.total_ms = 0;
            request.limits.max_body_bytes = job.file.size;
            request.cancel = &cancel_;
            if (done > 0)
                request.range_from = done;
            const http::Response response = client.fetch(request, sink);
            done = sink.done_;

            const bool held = match_.load() || held_.load();
            if (sink.halt_ == Halt::paused || (held && sink.halt_ == Halt::none &&
                                               response.failure == http::Failure::sink_stopped)) {
                sink.release();
                if (!wait_while_held(done, job.file.size, job.part)) {
                    cancelled();
                    return;
                }
                rate_.reset();
                continue;
            }
            if (cancel_.load() || response.failure == http::Failure::cancelled) {
                sink.release();
                cancelled();
                return;
            }
            if (sink.halt_ == Halt::disk) {
                fail_ready(DownloadFailure::storage, "the download could not be stored", false);
                return;
            }
            if (response.failure == http::Failure::none && sink.halt_ == Halt::none &&
                sink.done_ == job.file.size) {
                sink.release();
                std::FILE* flushed = oa::platform::open_file(job.part, "r+b");
                if (flushed != nullptr) {
                    if (!sync_file(flushed)) {
                        std::fclose(flushed);
                        fail_ready(
                            DownloadFailure::storage, "the download could not be stored", false
                        );
                        return;
                    }
                    std::fclose(flushed);
                }
                accept_file();
                return;
            }
            const bool dropped = response.failure == http::Failure::connection_lost ||
                                 response.failure == http::Failure::timed_out ||
                                 response.failure == http::Failure::sink_stopped;
            if (dropped && sink.halt_ != Halt::differs && sink.halt_ != Halt::status) {
                ++cuts;
                last_failure = "the download was interrupted";
                continue;
            }
            if (response.status != 0)
                last_failure = "the mirror answered " + std::to_string(response.status);
            else if (response.failure != http::Failure::none)
                last_failure = http::failure_text(response.failure);
            else
                last_failure = "the mirror did not send the file";
            break;
        }
    }
    if (!any_plain)
        last_failure = "this system cannot fetch https, and the feed offered no other mirror";
    fail_ready(DownloadFailure::connect, std::move(last_failure), false);
}

} // namespace oa::app::update

#endif

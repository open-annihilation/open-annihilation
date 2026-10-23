// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The download queue. One package at a time, on a worker of its own. Parts
// are written in steps and kept across a cut, a cancel, a quit and a match.
// The main thread only changes the queue in memory.
#include "oa/app/content/download_api.hpp"

#include "oa/app/content/settings.hpp"
#include "oa/base/threads.hpp"
#include "oa/data/catalogue/catalogue.hpp"
#include "oa/platform/files.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
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
#include <io.h>
#include <stdio.h>
#else
#include <unistd.h>
#endif

namespace oa::app::content {
namespace {

namespace http = oa::netgame::http;
namespace url = oa::formats::url;
namespace threads = oa::base::threads;
namespace sha = oa::base::sha256;
namespace fs = std::filesystem;

/// How long the worker sleeps while a wait can still be cut short, in milliseconds.
constexpr uint32_t wait_slice_ms = 50;

/// API answers that fail before the file URLs are tried. The first, then three waits.
constexpr uint32_t transport_waits = 3;

/// How many times a 429 is asked again.
constexpr uint32_t rate_reasks = 3;

/// How many times a package reply of 403, or a key about to expire, asks again.
constexpr uint32_t rekey_limit = 3;

/// How many times a challenge is polled before it is given up.
constexpr uint32_t poll_limit = 1000;

/// Connection cuts that add no step, after which the fetch is given up.
constexpr uint32_t stall_limit = 3;

/// A key closer than this to expiry is asked for again, in seconds.
constexpr int64_t key_fresh_seconds = 30;

/// The wait when a 429 names none, in seconds.
constexpr uint32_t retry_after_default_seconds = 30;

/// The most a 429 is waited, in seconds.
constexpr uint32_t retry_after_cap_seconds = 300;

/// The saved queue this build writes. A different number is not read.
constexpr int queue_version = 1;

/// The most bytes of a saved queue this build reads.
constexpr std::size_t queue_most_bytes = 1u << 20;

/// Bytes hashed or written between checks of the stop flag.
constexpr std::size_t io_chunk_bytes = 64 * 1024;

/// Where a fetch stopped, as the sink records it.
enum class Halt : uint8_t {
    none,            ///< the body ended, or has not stopped
    paused,          ///< a match, at a step boundary
    cancelled,       ///< the player cancelled
    quit,            ///< the queue is stopping
    differs,         ///< the server's file is not the catalogue's size
    no_space,        ///< the disk filled up
    disk,            ///< the file could not be written
    forbidden,       ///< the package address answered 403
    not_found,       ///< the package address answered 404
    range_satisfied, ///< 416
    other_status,    ///< some other status, with nothing written
    browser_check,   ///< a check meant for a web browser, with nothing written
};

/// How a stretch of waiting ended.
enum class WaitEnd : uint8_t {
    done,      ///< the whole wait elapsed
    quit,      ///< the queue is stopping
    cancelled, ///< the player cancelled the item
    match,     ///< a match started
    ended,     ///< the item already failed or was cancelled
};

/// What asking for a key decided.
enum class KeyGate : uint8_t {
    ready,     ///< a key is held and is not about to expire
    mirrors,   ///< the API could not be reached; the file URLs may be tried
    paused,    ///< a match started
    cancelled, ///< the player cancelled
    quit,      ///< the queue is stopping
    failed,    ///< the item has failed
};

/// Writes a reason into `why` when it is not null.
///
/// @param why the caller's reason; may be null
/// @param text the reason
void set_why(std::string* why, std::string text) {
    if (why != nullptr)
        *why = std::move(text);
}

/// Reports whether an error number says the disk is full.
///
/// @param number an errno value
/// @return true for no space, or no quota left
bool disk_full(int number) noexcept {
#if defined(EDQUOT)
    if (number == EDQUOT)
        return true;
#endif
    return number == ENOSPC;
}

/// Moves a file to a byte offset. A long is 32 bits on Windows, so the
/// 64-bit seek is used there.
///
/// @param file an open file
/// @param offset the byte, from the start
/// @return true when the seek landed
bool seek_file(std::FILE* file, uint64_t offset) noexcept {
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

/// Reads a whole file, up to a limit.
///
/// @param path the file
/// @param most the most bytes
/// @param[out] bytes the contents; empty when it could not be read
/// @return false when the file could not be read or is larger than `most`
bool read_limited(const fs::path& path, std::size_t most, std::vector<uint8_t>& bytes) {
    bytes.clear();
    std::FILE* file = oa::platform::open_file(path, "rb");
    if (file == nullptr)
        return false;
    uint8_t chunk[4096];
    while (bytes.size() <= most) {
        const std::size_t got = std::fread(chunk, 1, sizeof chunk, file);
        if (got > 0)
            bytes.insert(bytes.end(), chunk, chunk + got);
        if (got < sizeof chunk)
            break;
    }
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed || bytes.size() > most) {
        bytes.clear();
        return false;
    }
    return true;
}

/// The downloads folder, or empty when downloads are off.
///
/// @param data the data folder
/// @return the folder
fs::path downloads_of(const fs::path& data) {
    if (data.empty())
        return {};
    return downloads_folder(data);
}

/// The package file's name, `<sha256>.<kind>`, with `.part` while it is incomplete.
///
/// @param folder the downloads folder
/// @param digest the catalogue digest
/// @param kind the package kind
/// @param part true for the partial file
/// @return the path
fs::path package_path(
    const fs::path& folder, const sha::Digest& digest, data::catalogue::Kind kind, bool part
) {
    const auto hex = sha::to_hex(digest);
    std::string name(hex.begin(), hex.end());
    name.push_back('.');
    name += data::catalogue::package_extension(kind);
    if (part)
        name += part_suffix;
    return folder / name;
}

/// Quotes one YAML scalar.
///
/// @param text the text
/// @return the quoted scalar
std::string yaml_quote(std::string_view text) {
    std::string out;
    out.push_back('"');
    for (const char character : text) {
        if (character == '\\' || character == '"')
            out.push_back('\\');
        out.push_back(character);
    }
    out.push_back('"');
    return out;
}

/// Drops leading and trailing ASCII space.
///
/// @param text the text
/// @return the inner text
std::string_view trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

/// Reads a quoted scalar.
///
/// @param text the text after the colon
/// @param[out] value the scalar
/// @return true when the quotes are whole
bool parse_quoted(std::string_view text, std::string& value) {
    text = trim(text);
    if (text.size() < 2 || text.front() != '"')
        return false;
    value.clear();
    for (std::size_t index = 1; index < text.size(); ++index) {
        const char character = text[index];
        if (character == '\\') {
            if (index + 1 >= text.size())
                return false;
            value.push_back(text[++index]);
            continue;
        }
        if (character == '"')
            return trim(text.substr(index + 1)).empty();
        value.push_back(character);
    }
    return false;
}

/// Reads a non-negative whole number.
///
/// @param text the text
/// @param[out] value the number
/// @return true when every character was a digit
bool parse_u64(std::string_view text, uint64_t& value) noexcept {
    text = trim(text);
    if (text.empty())
        return false;
    uint64_t number = 0;
    for (const char character : text) {
        if (character < '0' || character > '9')
            return false;
        const uint64_t digit = static_cast<uint64_t>(character - '0');
        if (number > (std::numeric_limits<uint64_t>::max() - digit) / 10)
            return false;
        number = number * 10 + digit;
    }
    value = number;
    return true;
}

/// The reason named by the API's word, compared through reason_text.
///
/// @param text the word
/// @return the reason, or nothing
std::optional<DownloadReason> reason_from_text(std::string_view text) noexcept {
    const DownloadReason reasons[] = {
        DownloadReason::install,
        DownloadReason::update,
        DownloadReason::repair,
        DownloadReason::offered_in_lobby,
    };
    for (const DownloadReason reason : reasons) {
        if (text == reason_text(reason))
            return reason;
    }
    return std::nullopt;
}

/// The kind named by the catalogue's word.
///
/// @param text the word
/// @return the kind, or nothing
std::optional<data::catalogue::Kind> kind_from_name(std::string_view text) noexcept {
    const data::catalogue::Kind kinds[] = {
        data::catalogue::Kind::oalang,
        data::catalogue::Kind::oamod,
        data::catalogue::Kind::oamap,
        data::catalogue::Kind::oamut,
        data::catalogue::Kind::oabrain,
        data::catalogue::Kind::expansion,
    };
    for (const data::catalogue::Kind kind : kinds) {
        if (text == data::catalogue::kind_name(kind))
            return kind;
    }
    return std::nullopt;
}

/// Seconds since 1970, from the injected clock or the system clock.
///
/// @param clock the injected clock; null uses the system clock
/// @return the seconds
int64_t seconds_now(Clock clock) noexcept {
    if (clock != nullptr)
        return clock();
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(now).count();
}

/// Reports whether an item has not ended.
///
/// @param state the item's state
/// @return true while it is not done, failed or cancelled
bool still_open(DownloadState state) noexcept {
    return state != DownloadState::done && state != DownloadState::failed &&
           state != DownloadState::cancelled;
}

/// One saved queue row.
struct SavedItem {
    std::string registry;
    std::string key;
    data::catalogue::Kind kind = data::catalogue::Kind::oamod;
    int64_t release = 0;
    sha::Digest sha256{};
    DownloadReason reason = DownloadReason::update;
    bool complete = false;
};

/// Reads the saved queue. A missing file is an empty queue.
///
/// @param path the queue file
/// @param[out] items the rows
/// @param[out] missing true when the file is not there
/// @return false when the file is there and cannot be read
bool read_queue_file(const fs::path& path, std::vector<SavedItem>& items, bool& missing) {
    items.clear();
    missing = false;
    std::error_code error;
    const bool present = fs::exists(path, error);
    if (error || !present) {
        missing = !error;
        return !error;
    }
    std::vector<uint8_t> bytes;
    if (!read_limited(path, queue_most_bytes, bytes))
        return false;
    std::string text(bytes.begin(), bytes.end());
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        if (index == text.size() || text[index] == '\n') {
            std::string_view line(text.data() + start, index - start);
            line = trim(line);
            if (!line.empty())
                lines.push_back(line);
            start = index + 1;
        }
    }
    const std::string header = "queue: " + std::to_string(queue_version);
    if (lines.size() < 2 || lines[0] != header || lines[1] != "items:")
        return false;
    SavedItem current;
    bool in_item = false;
    int fields = 0;
    const auto finish = [&]() {
        if (!in_item)
            return true;
        if (fields != 6)
            return false;
        items.push_back(current);
        in_item = false;
        fields = 0;
        current = SavedItem{};
        return true;
    };
    for (std::size_t index = 2; index < lines.size(); ++index) {
        std::string_view line = lines[index];
        const bool dash = line.size() >= 2 && line[0] == '-' && line[1] == ' ';
        if (dash) {
            if (!finish())
                return false;
            in_item = true;
            line = trim(line.substr(1));
        } else if (!in_item) {
            return false;
        }
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos)
            return false;
        const std::string_view key = trim(line.substr(0, colon));
        const std::string_view raw = line.substr(colon + 1);
        std::string value;
        if (key == "release") {
            uint64_t number = 0;
            if (!parse_u64(raw, number) ||
                number > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
                return false;
            current.release = static_cast<int64_t>(number);
            ++fields;
            continue;
        }
        if (!parse_quoted(raw, value))
            return false;
        if (key == "registry")
            current.registry = value;
        else if (key == "key")
            current.key = value;
        else if (key == "kind") {
            const std::optional<data::catalogue::Kind> kind = kind_from_name(value);
            if (!kind)
                return false;
            current.kind = *kind;
        } else if (key == "sha256") {
            const std::optional<sha::Digest> digest = sha::parse_hex(value);
            if (!digest)
                return false;
            current.sha256 = *digest;
        } else if (key == "reason") {
            const std::optional<DownloadReason> reason = reason_from_text(value);
            if (!reason)
                return false;
            current.reason = *reason;
        } else {
            return false;
        }
        ++fields;
    }
    return finish();
}

/// Hashes a file, stopping when the queue is quitting.
///
/// @param path the file
/// @param quit set when the hash should stop
/// @param[out] digest the digest, when the whole file was hashed
/// @param[out] size the bytes hashed
/// @return false when the file could not be read, or the quit flag was set
bool hash_file(
    const fs::path& path, const std::atomic<bool>& quit, sha::Digest& digest, uint64_t& size
) {
    size = 0;
    std::FILE* file = oa::platform::open_file(path, "rb");
    if (file == nullptr)
        return false;
    sha::Hasher hasher;
    uint8_t chunk[io_chunk_bytes];
    bool stopped = false;
    bool failed = false;
    for (;;) {
        if (quit.load()) {
            stopped = true;
            break;
        }
        const std::size_t got = std::fread(chunk, 1, sizeof chunk, file);
        if (got > 0) {
            sha::update(hasher, std::span<const uint8_t>(chunk, got));
            size += static_cast<uint64_t>(got);
        }
        if (got < sizeof chunk) {
            failed = std::ferror(file) != 0;
            break;
        }
    }
    std::fclose(file);
    if (stopped || failed)
        return false;
    digest = sha::finish(hasher);
    return true;
}

/// Hashes a prefix of a file into `hasher`.
///
/// @param path the file
/// @param bytes how many bytes to hash
/// @param quit set when the hash should stop
/// @param[out] hasher the running hash of that prefix
/// @return false when the file could not be read, or the quit flag was set
bool hash_prefix(
    const fs::path& path, uint64_t bytes, const std::atomic<bool>& quit, sha::Hasher& hasher
) {
    if (bytes == 0) {
        hasher = sha::Hasher{};
        return true;
    }
    std::FILE* file = oa::platform::open_file(path, "rb");
    if (file == nullptr)
        return false;
    hasher = sha::Hasher{};
    uint8_t chunk[io_chunk_bytes];
    uint64_t left = bytes;
    bool stopped = false;
    bool failed = false;
    while (left > 0) {
        if (quit.load()) {
            stopped = true;
            break;
        }
        const std::size_t ask = static_cast<std::size_t>(std::min<uint64_t>(left, sizeof chunk));
        const std::size_t got = std::fread(chunk, 1, ask, file);
        if (got > 0) {
            sha::update(hasher, std::span<const uint8_t>(chunk, got));
            left -= static_cast<uint64_t>(got);
        }
        if (got < ask) {
            failed = true;
            break;
        }
    }
    std::fclose(file);
    return !stopped && !failed;
}

/// Tells whether a package address's answer is a check meant for a web
/// browser: the firewall's header names one whatever the status, and an HTML
/// page where the package was expected is one unless it answered 403, 404 or
/// 416, which keep their own meaning.
///
/// @param seen the answer
/// @return true for a check
bool package_check(const Answered& seen) {
    if (!seen.check_header.empty())
        return true;
    return seen.browser_check && seen.status != 403 && seen.status != 404 && seen.status != 416;
}

} // namespace

/// The queue, its items and the flags the worker and the main thread share.
struct Downloads::State {
    /// One queued package.
    struct Item {
        uint64_t id = 0;
        DownloadTarget target;
        DownloadReason reason = DownloadReason::update;
        DownloadState state = DownloadState::waiting;
        uint64_t done = 0;
        DownloadFailure failure = DownloadFailure::none;
        std::string detail;
        std::optional<ChallengeView> challenge;
        std::optional<IssuedKey> key;
        std::string download_id;
        uint64_t bytes_fetched = 0;
        std::optional<int64_t> first_key_at;
        bool result_posted = false;
        uint32_t transport_failures = 0;
        uint32_t rate_limits = 0;
        uint32_t expiry_rekeys = 0;
        uint32_t package_rekeys = 0;
        uint32_t package_misses = 0;
        uint32_t hash_failures = 0;
        bool fetched_whole_after_range = false;
        std::optional<InstallOutcome> outcome;
        std::string install_reason;      ///< the installer's reason for its outcome
        std::optional<Answered> asked;   ///< what the last key request was answered
        std::optional<Answered> fetched; ///< what the last package fetch was answered
        bool logged = false;             ///< the failure's log line is written
    };

    threads::Mutex mutex;
    threads::ConditionVariable wake;
    DownloadsOptions options;
    std::deque<Item> items;
    uint64_t next_id = 1;
    uint64_t generation = 0;
    uint64_t dirty_stamp = 0;
    bool dirty = false;
    bool started = false;
    bool quit = false;
    bool match = false;
    bool thread_started = false;
    bool thread_failed = false;
    uint64_t current_id = 0;
    std::vector<std::pair<std::string, std::optional<std::string>>> install_ids;
    std::shared_ptr<const Snapshot> snapshot;
    threads::Thread thread{};

    std::atomic<bool> quit_flag{false};
    std::atomic<bool> match_flag{false};
    std::atomic<bool> item_cancel{false};
    std::atomic<bool> http_cancel{false};
    /// 0 idle, 1 a key or a poll, 2 a package body. A match does not cancel a body.
    std::atomic<uint8_t> phase{0};

    /// The worker's entry.
    ///
    /// @param argument the state
    static void entry(void* argument);

    /// Runs until the queue is destroyed.
    void run();

    /// Writes one log line. The lock is not held.
    ///
    /// @param line the line
    void log_line(std::string_view line);

    /// Fails an item. The caller holds the lock.
    ///
    /// The detail is kept only when it says more than the failure's text,
    /// so a message never names its reason twice. The worker logs the
    /// failure once (log_failures).
    ///
    /// @param item the item
    /// @param failure why
    /// @param detail the registry's message or the installer's reason; may be empty
    void fail(Item& item, DownloadFailure failure, std::string detail);

    /// Writes one log line for each failed item not yet logged. The lock is not held.
    void log_failures();

    /// True when a failed item has not been logged. The caller holds the lock.
    ///
    /// @return true while a failure waits for its line
    bool failure_unlogged() const;

    /// Copies the log sink. The caller holds the lock.
    ///
    /// @param[out] sink the sink
    /// @param[out] context the sink's context
    void copy_log(LogSink& sink, void*& context);

    /// Finds an item. The caller holds the lock. The pointer dies at the next unlock
    /// only if the deque is erased; items are not erased.
    ///
    /// @param id the item id
    /// @return the item, or null
    Item* find(uint64_t id);

    /// The install id held for a registry. The caller holds the lock.
    ///
    /// @param registry the registry id
    /// @return the id, or nothing when it is off or was never set
    std::optional<std::string> held_id(std::string_view registry) const;

    /// True when a registry that requires an id has none. The caller holds the lock.
    ///
    /// @param registry the registry id
    /// @return true when no id is held
    bool id_off(std::string_view registry) const;

    /// Moves the generation on. The caller holds the lock.
    void touch();

    /// Marks the saved queue stale. The caller holds the lock.
    void mark_dirty();

    /// The next item to run, or 0. The caller holds the lock.
    ///
    /// @return the item id, or 0 when none should start
    uint64_t pick();

    /// Writes the saved queue when it is stale.
    void save_queue();

    /// Reads the saved queue and queues the rows the catalogue still offers.
    void restore_queue();

    /// Downloads one item to the end, or until it pauses, fails or is cancelled.
    ///
    /// @param client the worker's client
    /// @param id the item
    void process(http::Client& client, uint64_t id);

    /// Asks for a key, including a challenge and the bounded retries.
    ///
    /// @param client the worker's client
    /// @param id the item
    /// @return what the caller should do
    KeyGate ensure_key(http::Client& client, uint64_t id);

    /// Gives a checked file to the installer and waits for the outcome.
    ///
    /// @param client the worker's client
    /// @param id the item
    /// @param file the package file
    void deliver(http::Client& client, uint64_t id, const std::filesystem::path& file);

    /// Posts one result when a key was issued and none has been posted.
    ///
    /// @param client the worker's client
    /// @param id the item
    /// @param result how it ended
    void send_result(http::Client& client, uint64_t id, KeyResult result);

    /// Sleeps in short slices so a quit, a cancel or a match is noticed.
    ///
    /// @param milliseconds how long, unless something stops it
    /// @param id the item
    /// @return why the wait ended
    WaitEnd wait_for(uint32_t milliseconds, uint64_t id);

    /// True when the item is still the one being downloaded and has not ended.
    /// The caller holds the lock.
    ///
    /// @param id the item
    /// @return false when it has ended or the queue is stopping
    bool live(uint64_t id);
};

namespace {

/// The package body, written a step at a time.
class PartSink final : public http::BodySink {
  public:

    /// Records the file the body is written to.
    ///
    /// @param state the queue
    /// @param id the item
    /// @param path the part
    /// @param size the catalogue size
    /// @param done bytes already committed
    /// @param hasher the hash of those bytes
    /// @param ranged true when a Range was sent
    PartSink(
        Downloads::State& state,
        uint64_t id,
        fs::path path,
        uint64_t size,
        uint64_t done,
        sha::Hasher hasher,
        bool ranged
    )
        : done_(done), hasher_(hasher), state_(&state), id_(id), path_(std::move(path)),
          size_(size), ranged_(ranged) {}

    /// Closes the part.
    ~PartSink() override { close(); }

    PartSink(const PartSink&) = delete;
    PartSink& operator=(const PartSink&) = delete;

    /// Accepts a length that matches the catalogue, and restarts on a 200.
    ///
    /// A check meant for a web browser is never written: the firewall's
    /// header names one whatever the status, and an HTML page where the
    /// package was expected is one unless it answered 403, 404 or 416.
    ///
    /// @param head the response, before any body byte
    /// @return false when the body must not be written
    bool begin(const http::Response& head) override {
        status_ = head.status;
        if (package_check(answered({}, head))) {
            halt_ = Halt::browser_check;
            return false;
        }
        if (head.status == 403) {
            halt_ = Halt::forbidden;
            return false;
        }
        if (head.status == 404) {
            halt_ = Halt::not_found;
            return false;
        }
        if (head.status == 416) {
            halt_ = Halt::range_satisfied;
            return false;
        }
        if (head.status == 200) {
            if (!head.content_length || *head.content_length != size_) {
                halt_ = Halt::differs;
                return false;
            }
            if (ranged_) {
                done_ = 0;
                hasher_ = sha::Hasher{};
                std::error_code error;
                if (fs::exists(path_, error))
                    fs::resize_file(path_, 0, error);
                if (error) {
                    halt_ = disk_full(error.value()) ? Halt::no_space : Halt::disk;
                    return false;
                }
                publish_done();
            }
        } else if (head.status == 206) {
            if (!head.content_range || !head.content_range->total ||
                *head.content_range->total != size_ || head.content_range->first != done_) {
                halt_ = Halt::differs;
                return false;
            }
        } else {
            halt_ = Halt::other_status;
            return false;
        }
        const char* mode = (head.status == 200) ? "wb" : "r+b";
        file_ = oa::platform::open_file(path_, mode);
        if (file_ == nullptr && head.status == 206)
            file_ = oa::platform::open_file(path_, "wb");
        if (file_ == nullptr) {
            halt_ = disk_full(errno) ? Halt::no_space : Halt::disk;
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
    /// A cancel or a quit writes nothing from this piece. A match finishes
    /// the current step and then stops.
    ///
    /// @param bytes the next piece
    /// @return false to stop the fetch
    bool write(std::span<const uint8_t> bytes) override {
        if (halt_ != Halt::none)
            return false;
        if (state_->quit_flag.load() || state_->item_cancel.load()) {
            halt_ = state_->quit_flag.load() ? Halt::quit : Halt::cancelled;
            return false;
        }
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            if (done_ + buffer_.size() > size_) {
                halt_ = Halt::differs;
                return false;
            }
            const uint64_t room = size_ - done_ - static_cast<uint64_t>(buffer_.size());
            if (room == 0) {
                halt_ = Halt::differs;
                return false;
            }
            std::size_t take = bytes.size() - offset;
            const uint64_t step_room = download_step_bytes - static_cast<uint64_t>(buffer_.size());
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
                if (!file_full && state_->match_flag.load()) {
                    halt_ = Halt::paused;
                    return false;
                }
                if (state_->quit_flag.load() || state_->item_cancel.load()) {
                    halt_ = state_->quit_flag.load() ? Halt::quit : Halt::cancelled;
                    return false;
                }
            }
        }
        return true;
    }

    /// Writes a short final tail when the body ended cleanly and completes the file.
    ///
    /// @param clean true when the response was read whole
    /// @return false when the tail could not be written
    bool finish_tail(bool clean) {
        if (buffer_.empty() || halt_ != Halt::none)
            return halt_ == Halt::none;
        if (!clean || done_ + static_cast<uint64_t>(buffer_.size()) != size_)
            return true;
        return flush_buffer();
    }

    /// Puts the part on disk and closes it.
    void commit() {
        if (file_ != nullptr) {
            if (!sync_file(file_))
                halt_ = disk_full(errno) ? Halt::no_space : Halt::disk;
            close();
        }
    }

    /// The status begin saw, or 0.
    int status_ = 0;
    /// Why the body stopped.
    Halt halt_ = Halt::none;
    /// Bytes committed to the part.
    uint64_t done_ = 0;
    /// The hash of the committed bytes.
    sha::Hasher hasher_{};

  private:

    /// Writes the buffer, flushes it and counts it in the item.
    ///
    /// @return false on a disk error
    bool flush_buffer() {
        if (file_ == nullptr || buffer_.empty())
            return true;
        errno = 0;
        if (!write_all(file_, buffer_.data(), buffer_.size()) || std::fflush(file_) != 0) {
            halt_ = disk_full(errno) ? Halt::no_space : Halt::disk;
            buffer_.clear();
            return false;
        }
        sha::update(hasher_, std::span<const uint8_t>(buffer_.data(), buffer_.size()));
        done_ += static_cast<uint64_t>(buffer_.size());
        buffer_.clear();
        publish_done();
        return true;
    }

    /// Shows the committed size on the item.
    void publish_done() {
        threads::LockGuard guard(state_->mutex);
        Downloads::State::Item* item = state_->find(id_);
        if (item == nullptr || !still_open(item->state))
            return;
        item->done = done_;
        if (item->state == DownloadState::waiting ||
            item->state == DownloadState::paused_for_match || item->state == DownloadState::asking)
            item->state = DownloadState::downloading;
        state_->touch();
    }

    /// Closes the part without syncing.
    void close() noexcept {
        if (file_ != nullptr) {
            std::fclose(file_);
            file_ = nullptr;
        }
    }

    Downloads::State* state_ = nullptr;
    uint64_t id_ = 0;
    fs::path path_;
    uint64_t size_ = 0;
    bool ranged_ = false;
    std::FILE* file_ = nullptr;
    std::vector<uint8_t> buffer_;
};

/// What one package fetch decided.
enum class FetchEnd : uint8_t {
    complete,  ///< the part holds the catalogue's size
    paused,    ///< stopped for a match
    cancelled, ///< the player cancelled
    quit,      ///< the queue is stopping
    failed,    ///< the item failed
    denied,    ///< the package address answered 403
    missing,   ///< the package address answered 404
    next_url,  ///< try the next file address
    resume,    ///< the connection dropped; ask for the rest
};

/// A key request's fields, copied out from under the lock.
struct KeyFields {
    KeyRequest request;
    url::Url api;
    bool have_api = false;
    std::optional<IssuedKey> key;
    uint32_t transport_failures = 0;
    uint32_t rate_limits = 0;
    uint32_t expiry_rekeys = 0;
    uint32_t poll_ms = challenge_poll_ms;
};

/// The result call's fields, copied out from under the lock.
struct ResultFields {
    bool post = false;
    url::Url api;
    std::string download;
    uint64_t bytes = 0;
    int64_t seconds = 0;
};

} // namespace

void Downloads::State::entry(void* argument) {
    auto* state = static_cast<State*>(argument);
    try {
        state->run();
    } catch (const std::exception& error) {
        state->log_line(std::string("the download worker stopped: ") + error.what());
    } catch (...) {
        state->log_line("the download worker stopped");
    }
}

void Downloads::State::copy_log(LogSink& sink, void*& context) {
    sink = options.log;
    context = options.log_context;
}

void Downloads::State::log_line(std::string_view line) {
    LogSink sink = nullptr;
    void* context = nullptr;
    {
        threads::LockGuard guard(mutex);
        copy_log(sink, context);
    }
    if (sink != nullptr)
        sink(context, line);
}

void Downloads::State::fail(Item& item, DownloadFailure failure, std::string detail) {
    item.state = DownloadState::failed;
    item.failure = failure;
    // The failure's own text is what the message and the log line start
    // with; a detail that only repeats it is dropped.
    item.detail = detail == failure_text(failure) ? std::string() : std::move(detail);
    item.challenge.reset();
    item.logged = false;
    mark_dirty();
    touch();
}

bool Downloads::State::failure_unlogged() const {
    for (const Item& item : items)
        if (item.state == DownloadState::failed && !item.logged)
            return true;
    return false;
}

void Downloads::State::log_failures() {
    std::vector<std::string> lines;
    LogSink sink = nullptr;
    void* context = nullptr;
    {
        threads::LockGuard guard(mutex);
        copy_log(sink, context);
        for (Item& item : items) {
            if (item.state != DownloadState::failed || item.logged)
                continue;
            item.logged = true;
            std::string line = "download of ";
            line += item.target.name.empty() ? item.target.key : item.target.name;
            line += " (" + item.target.registry + " " + item.target.key + ", release " +
                    std::to_string(item.target.release) + ") failed: ";
            line += failure_text(item.failure);
            if (!item.detail.empty())
                line += " (" + item.detail + ")";
            if (item.asked)
                line += "; key request: " + answered_text(*item.asked);
            if (item.fetched)
                line += "; package: " + answered_text(*item.fetched);
            // One failure is one line, whatever a reason holds.
            for (char& character : line)
                if (character == '\n' || character == '\r')
                    character = ' ';
            lines.push_back(std::move(line));
        }
    }
    if (sink == nullptr)
        return;
    for (const std::string& line : lines)
        sink(context, line);
}

Downloads::State::Item* Downloads::State::find(uint64_t id) {
    for (Item& item : items) {
        if (item.id == id)
            return &item;
    }
    return nullptr;
}

std::optional<std::string> Downloads::State::held_id(std::string_view registry) const {
    for (const auto& entry : install_ids) {
        if (entry.first == registry)
            return entry.second;
    }
    return std::nullopt;
}

bool Downloads::State::id_off(std::string_view registry) const {
    const std::optional<std::string> id = held_id(registry);
    return !id || id->empty();
}

void Downloads::State::touch() {
    ++generation;
}

void Downloads::State::mark_dirty() {
    dirty = true;
    ++dirty_stamp;
}

uint64_t Downloads::State::pick() {
    if (quit || match)
        return 0;
    for (const Item& item : items) {
        if (item.state == DownloadState::waiting || item.state == DownloadState::paused_for_match)
            return item.id;
    }
    return 0;
}

bool Downloads::State::live(uint64_t id) {
    if (quit)
        return false;
    const Item* item = find(id);
    return item != nullptr && still_open(item->state);
}

void Downloads::State::save_queue() {
    std::string text;
    fs::path path;
    uint64_t stamp = 0;
    {
        threads::LockGuard guard(mutex);
        if (!dirty || options.data_folder.empty())
            return;
        path = downloads_of(options.data_folder) / std::string(queue_file_name);
        stamp = dirty_stamp;
        text = "queue: " + std::to_string(queue_version) + "\nitems:\n";
        for (const Item& item : items) {
            if (!still_open(item.state))
                continue;
            const auto hex = sha::to_hex(item.target.sha256);
            text += "  - registry: ";
            text += yaml_quote(item.target.registry);
            text += "\n    key: ";
            text += yaml_quote(item.target.key);
            text += "\n    kind: ";
            text += yaml_quote(data::catalogue::kind_name(item.target.kind));
            text += "\n    release: ";
            text += std::to_string(item.target.release);
            text += "\n    sha256: ";
            text += yaml_quote(std::string(hex.begin(), hex.end()));
            text += "\n    reason: ";
            text += yaml_quote(reason_text(item.reason));
            text += '\n';
        }
    }
    const std::span<const uint8_t> bytes(
        reinterpret_cast<const uint8_t*>(text.data()), text.size()
    );
    std::string error;
    const bool wrote = oa::platform::replace_file(path, bytes, &error);
    if (!wrote)
        log_line(std::string("the download queue could not be saved: ") + error);
    threads::LockGuard guard(mutex);
    if (wrote && dirty_stamp == stamp)
        dirty = false;
}

void Downloads::State::restore_queue() {
    std::shared_ptr<const Snapshot> snap;
    fs::path path;
    {
        threads::LockGuard guard(mutex);
        snap = snapshot;
        snapshot.reset();
        if (options.data_folder.empty())
            return;
        path = downloads_of(options.data_folder) / std::string(queue_file_name);
    }
    std::vector<SavedItem> saved;
    bool missing = false;
    const bool read = read_queue_file(path, saved, missing);
    if (!read) {
        log_line("a saved download queue could not be read");
        threads::LockGuard guard(mutex);
        mark_dirty();
        return;
    }
    if (missing || saved.empty())
        return;
    bool dropped = false;
    {
        threads::LockGuard guard(mutex);
        std::deque<Item> restored;
        for (const SavedItem& row : saved) {
            const data::catalogue::Package* package = nullptr;
            if (snap) {
                for (const Entry& entry : snap->entries) {
                    if (entry.registry == row.registry && entry.package != nullptr &&
                        entry.package->id == row.key && entry.package->release == row.release &&
                        entry.package->sha256 == row.sha256 && entry.package->kind == row.kind) {
                        package = entry.package;
                        break;
                    }
                }
            }
            if (package == nullptr) {
                dropped = true;
                continue;
            }
            bool duplicate = false;
            for (const Item& item : items) {
                if (still_open(item.state) && item.target.registry == row.registry &&
                    item.target.key == row.key)
                    duplicate = true;
            }
            if (duplicate)
                continue;
            Item item;
            item.id = next_id++;
            item.reason = row.reason;
            item.target.registry = row.registry;
            item.target.key = row.key;
            item.target.kind = row.kind;
            item.target.release = row.release;
            item.target.sha256 = row.sha256;
            item.target.size = package->size;
            item.target.name = package->name;
            for (const RegistryView& view : snap->registries) {
                if (view.registry.descriptor.id != row.registry)
                    continue;
                item.target.registry_name = view.registry.descriptor.name;
                item.target.mode = view.registry.descriptor.mode;
                item.target.api = view.registry.descriptor.api;
                item.target.install_id = view.registry.descriptor.install_id;
                item.target.files = reference_urls(view, package->file);
                break;
            }
            restored.push_back(std::move(item));
        }
        if (!restored.empty() || dropped) {
            for (Item& item : items)
                restored.push_back(std::move(item));
            items = std::move(restored);
            if (dropped || !saved.empty())
                mark_dirty();
            touch();
        }
    }
    if (dropped) {
        log_line("dropped a saved download: the catalogue no longer has that release");
    }
}

WaitEnd Downloads::State::wait_for(uint32_t milliseconds, uint64_t id) {
    uint32_t slept = 0;
    while (slept < milliseconds) {
        if (quit_flag.load())
            return WaitEnd::quit;
        if (item_cancel.load())
            return WaitEnd::cancelled;
        if (match_flag.load())
            return WaitEnd::match;
        {
            threads::LockGuard guard(mutex);
            if (!live(id))
                return WaitEnd::ended;
        }
        const uint32_t slice = std::min<uint32_t>(wait_slice_ms, milliseconds - slept);
        threads::sleep_ms(slice);
        slept += slice;
    }
    return WaitEnd::done;
}

void Downloads::State::send_result(http::Client& client, uint64_t id, KeyResult result) {
    ResultFields fields;
    {
        threads::LockGuard guard(mutex);
        Item* item = find(id);
        if (quit || item == nullptr || item->result_posted || item->download_id.empty() ||
            !item->target.api)
            return;
        item->result_posted = true;
        fields.post = true;
        fields.api = *item->target.api;
        fields.download = item->download_id;
        fields.bytes = item->bytes_fetched;
        const int64_t now = seconds_now(options.clock);
        if (item->first_key_at && now >= *item->first_key_at)
            fields.seconds = now - *item->first_key_at;
    }
    if (!fields.post || quit_flag.load())
        return;
    std::string why;
    if (!post_result(
            client, fields.api, fields.download, result, fields.bytes, fields.seconds, &why
        ))
        log_line(std::string("a download result was not delivered: ") + why);
}

KeyGate Downloads::State::ensure_key(http::Client& client, uint64_t id) {
    for (;;) {
        KeyFields fields;
        {
            threads::LockGuard guard(mutex);
            Item* item = find(id);
            if (quit || item == nullptr)
                return KeyGate::quit;
            if (item->state == DownloadState::cancelled)
                return KeyGate::cancelled;
            if (!still_open(item->state))
                return KeyGate::failed;
            if (match) {
                if (item->state != DownloadState::waiting)
                    item->state = DownloadState::paused_for_match;
                touch();
                return KeyGate::paused;
            }
            if (item->target.install_id == data::registry::InstallIdUse::required &&
                id_off(item->target.registry)) {
                fail(*item, DownloadFailure::install_id_off, {});
                return KeyGate::failed;
            }
            if (!item->target.api)
                return KeyGate::mirrors;
            const int64_t now = seconds_now(options.clock);
            if (item->key && item->key->expires < now + key_fresh_seconds) {
                if (item->expiry_rekeys >= rekey_limit) {
                    fail(*item, DownloadFailure::unreachable, {});
                    return KeyGate::failed;
                }
                ++item->expiry_rekeys;
                item->key.reset();
            }
            if (item->key)
                return KeyGate::ready;
            item->state = DownloadState::asking;
            item->challenge.reset();
            touch();
            fields.request.package = item->target.key;
            fields.request.kind = item->target.kind;
            fields.request.release = item->target.release;
            fields.request.sha256 = item->target.sha256;
            fields.request.engine = std::string(engine_version());
            fields.request.platform = std::string(platform_name());
            fields.request.arch = std::string(arch_name());
            fields.request.language = options.language;
            fields.request.reason = item->reason;
            if (item->target.install_id != data::registry::InstallIdUse::none) {
                std::optional<std::string> held = held_id(item->target.registry);
                if (held && !held->empty())
                    fields.request.install = std::move(held);
            }
            fields.api = *item->target.api;
            fields.have_api = true;
            fields.transport_failures = item->transport_failures;
            fields.rate_limits = item->rate_limits;
            fields.poll_ms = options.poll_ms;
        }
        phase.store(1);
        http_cancel.store(quit_flag.load() || item_cancel.load() || match_flag.load());
        const KeyAnswer answer = ask_for_key(client, fields.api, fields.request, &http_cancel);
        phase.store(0);
        {
            threads::LockGuard guard(mutex);
            if (Item* item = find(id))
                item->asked = answer.answered;
        }
        if (quit_flag.load())
            return KeyGate::quit;
        if (answer.kind == KeyAnswer::Kind::browser_check) {
            // Never answered, asked again or tried at another address: the
            // player is told, and RETRY asks again when they choose.
            threads::LockGuard guard(mutex);
            Item* item = find(id);
            if (item != nullptr && still_open(item->state))
                fail(*item, DownloadFailure::server_check, {});
            return KeyGate::failed;
        }
        if (answer.kind == KeyAnswer::Kind::key && answer.key) {
            threads::LockGuard guard(mutex);
            Item* item = find(id);
            if (item == nullptr)
                return quit_flag.load() ? KeyGate::quit : KeyGate::failed;
            if (!item->first_key_at)
                item->first_key_at = seconds_now(options.clock);
            item->download_id = answer.key->download;
            item->key = answer.key;
            if (!live(id))
                return item->state == DownloadState::cancelled ? KeyGate::cancelled
                                                               : KeyGate::failed;
            item->challenge.reset();
            item->state = DownloadState::asking;
            touch();
            // A key that is already about to expire is asked for again before the fetch.
        }
        if (answer.kind == KeyAnswer::Kind::key && answer.key)
            continue;
        if (answer.kind == KeyAnswer::Kind::challenge && answer.challenge) {
            const Challenge challenge = *answer.challenge;
            {
                threads::LockGuard guard(mutex);
                Item* item = find(id);
                if (!live(id) || item == nullptr)
                    return quit_flag.load() ? KeyGate::quit : KeyGate::failed;
                ChallengeView view;
                view.code = challenge.code;
                view.verify_url = url::url_text(challenge.verify);
                view.address = challenge.address;
                view.expires = challenge.expires;
                item->challenge = std::move(view);
                item->state = DownloadState::challenge;
                touch();
            }
            uint32_t polls = 0;
            while (polls < poll_limit) {
                if (seconds_now(options.clock) >= challenge.expires) {
                    threads::LockGuard guard(mutex);
                    Item* item = find(id);
                    if (item != nullptr && still_open(item->state)) {
                        fail(*item, DownloadFailure::challenge_expired, {});
                    }
                    return KeyGate::failed;
                }
                const uint32_t interval = fields.poll_ms == challenge_poll_ms
                                              ? challenge.poll_seconds * 1000
                                              : fields.poll_ms;
                const WaitEnd waited = wait_for(interval, id);
                if (waited == WaitEnd::quit)
                    return KeyGate::quit;
                if (waited == WaitEnd::cancelled)
                    return KeyGate::cancelled;
                if (waited == WaitEnd::match) {
                    threads::LockGuard guard(mutex);
                    Item* item = find(id);
                    if (item != nullptr && still_open(item->state)) {
                        item->state = DownloadState::paused_for_match;
                        touch();
                    }
                    return KeyGate::paused;
                }
                if (waited == WaitEnd::ended)
                    return KeyGate::failed;
                phase.store(1);
                http_cancel.store(quit_flag.load() || item_cancel.load() || match_flag.load());
                std::string why;
                const std::optional<ChallengeState> state =
                    poll_challenge(client, fields.api, challenge.code, &http_cancel, &why);
                phase.store(0);
                ++polls;
                if (quit_flag.load())
                    return KeyGate::quit;
                if (item_cancel.load())
                    return KeyGate::cancelled;
                if (match_flag.load()) {
                    threads::LockGuard guard(mutex);
                    Item* item = find(id);
                    if (item != nullptr && still_open(item->state)) {
                        item->state = DownloadState::paused_for_match;
                        touch();
                    }
                    return KeyGate::paused;
                }
                if (!state)
                    continue;
                if (*state == ChallengeState::solved)
                    break;
                if (*state == ChallengeState::expired) {
                    threads::LockGuard guard(mutex);
                    Item* item = find(id);
                    if (item != nullptr && still_open(item->state)) {
                        fail(*item, DownloadFailure::challenge_expired, {});
                    }
                    return KeyGate::failed;
                }
            }
            if (polls >= poll_limit) {
                threads::LockGuard guard(mutex);
                Item* item = find(id);
                if (item != nullptr && still_open(item->state)) {
                    fail(*item, DownloadFailure::challenge_expired, {});
                }
                return KeyGate::failed;
            }
            continue;
        }
        if (answer.kind == KeyAnswer::Kind::not_offered) {
            threads::LockGuard guard(mutex);
            Item* item = find(id);
            if (item != nullptr && still_open(item->state))
                fail(*item, DownloadFailure::not_offered, answer.detail);
            return KeyGate::failed;
        }
        if (answer.kind == KeyAnswer::Kind::refused) {
            if (!answer.error.field.empty())
                log_line(std::string("the registry refused a field: ") + answer.error.field);
            threads::LockGuard guard(mutex);
            Item* item = find(id);
            if (item != nullptr && still_open(item->state))
                fail(*item, DownloadFailure::refused, answer.detail);
            return KeyGate::failed;
        }
        if (answer.kind == KeyAnswer::Kind::rate_limited) {
            uint32_t waits = 0;
            {
                threads::LockGuard guard(mutex);
                Item* item = find(id);
                if (!live(id) || item == nullptr)
                    return quit_flag.load() ? KeyGate::quit : KeyGate::failed;
                if (item->rate_limits >= rate_reasks) {
                    fail(*item, DownloadFailure::unreachable, answer.detail);
                    return KeyGate::failed;
                }
                waits = item->rate_limits;
                ++item->rate_limits;
            }
            (void)waits;
            uint32_t seconds = answer.error.retry_after_seconds;
            if (seconds == 0)
                seconds = retry_after_default_seconds;
            if (seconds > retry_after_cap_seconds)
                seconds = retry_after_cap_seconds;
            const WaitEnd waited = wait_for(seconds * 1000, id);
            if (waited == WaitEnd::quit)
                return KeyGate::quit;
            if (waited == WaitEnd::cancelled)
                return KeyGate::cancelled;
            if (waited == WaitEnd::match) {
                threads::LockGuard guard(mutex);
                Item* item = find(id);
                if (item != nullptr && still_open(item->state)) {
                    item->state = DownloadState::paused_for_match;
                    touch();
                }
                return KeyGate::paused;
            }
            if (waited == WaitEnd::ended)
                return KeyGate::failed;
            continue;
        }
        if (http_cancel.load() && match_flag.load()) {
            threads::LockGuard guard(mutex);
            Item* item = find(id);
            if (item != nullptr && still_open(item->state) && item->state != DownloadState::waiting)
                item->state = DownloadState::paused_for_match;
            touch();
            return KeyGate::paused;
        }
        if (item_cancel.load())
            return KeyGate::cancelled;
        if (quit_flag.load())
            return KeyGate::quit;
        uint32_t factor = 1;
        bool give_up = false;
        {
            threads::LockGuard guard(mutex);
            Item* item = find(id);
            if (!live(id) || item == nullptr)
                return quit_flag.load() ? KeyGate::quit : KeyGate::failed;
            if (item->transport_failures >= transport_waits) {
                give_up = true;
            } else {
                const uint32_t factors[] = {1, 4, 15};
                factor = factors[item->transport_failures];
                ++item->transport_failures;
            }
        }
        if (give_up)
            return KeyGate::mirrors;
        uint32_t base = 2000;
        {
            threads::LockGuard guard(mutex);
            base = options.retry_base_ms;
        }
        const WaitEnd waited = wait_for(base * factor, id);
        if (waited == WaitEnd::quit)
            return KeyGate::quit;
        if (waited == WaitEnd::cancelled)
            return KeyGate::cancelled;
        if (waited == WaitEnd::match) {
            threads::LockGuard guard(mutex);
            Item* item = find(id);
            if (item != nullptr && still_open(item->state)) {
                item->state = DownloadState::paused_for_match;
                touch();
            }
            return KeyGate::paused;
        }
        if (waited == WaitEnd::ended)
            return KeyGate::failed;
    }
}

namespace {

/// Fails an item. The caller holds the lock.
///
/// @param state the queue
/// @param item the item
/// @param failure why
/// @param detail what the failure's text does not say; dropped when it repeats it
void fail_locked(
    Downloads::State& state,
    Downloads::State::Item& item,
    DownloadFailure failure,
    std::string detail
) {
    state.fail(item, failure, std::move(detail));
}

/// Room for the bytes still to come, plus one step.
enum class Room : uint8_t { ok, no_space, disk };

/// Checks free space. The downloads folder is made first.
///
/// @param folder the downloads folder
/// @param still the bytes still to come
/// @return whether they fit
Room room_for(const fs::path& folder, uint64_t still) {
    std::error_code error;
    fs::create_directories(folder, error);
    if (error)
        return error == std::errc::no_space_on_device ? Room::no_space : Room::disk;
    if (still > std::numeric_limits<uint64_t>::max() - download_step_bytes)
        return Room::no_space;
    const auto space = fs::space(folder, error);
    if (error)
        return Room::disk;
    const uint64_t need = still + download_step_bytes;
    if (static_cast<uintmax_t>(need) > space.available)
        return Room::no_space;
    return Room::ok;
}

/// Cuts a part back to whole steps, or keeps it when it is already the catalogue size.
///
/// @param path the part
/// @param catalogue the catalogue size
/// @param quit set when the work should stop
/// @param[out] done the bytes kept
/// @param[out] hasher the hash of those bytes
/// @param[out] halt a disk error, a quit, or none
/// @return true when `done` and `hasher` were set
bool prepare_part(
    const fs::path& path,
    uint64_t catalogue,
    const std::atomic<bool>& quit,
    uint64_t& done,
    sha::Hasher& hasher,
    Halt& halt
) {
    done = 0;
    hasher = sha::Hasher{};
    halt = Halt::none;
    std::error_code error;
    const bool present = fs::is_regular_file(path, error);
    if (!present) {
        // A missing file is an empty part. Asking about it can set an error.
        if (!error || error == std::errc::no_such_file_or_directory)
            return true;
        halt = Halt::disk;
        return false;
    }
    error.clear();
    const auto file_size = fs::file_size(path, error);
    if (error) {
        halt = Halt::disk;
        return false;
    }
    uint64_t keep = file_size;
    if (file_size > catalogue) {
        fs::remove(path, error);
        return !error;
    }
    if (file_size != catalogue) {
        keep = file_size - (file_size % download_step_bytes);
        if (keep != file_size) {
            fs::resize_file(path, static_cast<uintmax_t>(keep), error);
            if (error) {
                halt = error == std::errc::no_space_on_device ? Halt::no_space : Halt::disk;
                return false;
            }
        }
    }
    if (!hash_prefix(path, keep, quit, hasher)) {
        halt = quit.load() ? Halt::quit : Halt::disk;
        return false;
    }
    done = keep;
    return true;
}

/// Renames the part onto the final file. On Windows a rename does not replace.
///
/// @param part the part
/// @param final the package file
/// @return true when the final file is in place
bool rename_part(const fs::path& part, const fs::path& final) {
    std::error_code error;
    fs::remove(final, error);
    error.clear();
    fs::rename(part, final, error);
    return !error;
}

/// Fetches one address into the part.
///
/// @param state the queue
/// @param client the worker's client
/// @param id the item
/// @param address the package address
/// @param part the part path
/// @param catalogue the catalogue size
/// @param tokens true when a 403 or a 404 asks for a key again
/// @return what the caller should do
FetchEnd fetch_url(
    Downloads::State& state,
    http::Client& client,
    uint64_t id,
    const url::Url& address,
    const fs::path& part,
    uint64_t catalogue,
    bool tokens,
    sha::Hasher* running
) {
    const auto keep = [&](const sha::Hasher& hasher) {
        if (running != nullptr)
            *running = hasher;
    };
    uint64_t done = 0;
    sha::Hasher hasher;
    Halt prepared = Halt::none;
    if (!prepare_part(part, catalogue, state.quit_flag, done, hasher, prepared)) {
        if (prepared == Halt::quit)
            return FetchEnd::quit;
        threads::LockGuard guard(state.mutex);
        Downloads::State::Item* item = state.find(id);
        if (item != nullptr && still_open(item->state)) {
            fail_locked(
                state,
                *item,
                prepared == Halt::no_space ? DownloadFailure::no_space
                                           : DownloadFailure::disk_error,
                failure_text(
                    prepared == Halt::no_space ? DownloadFailure::no_space
                                               : DownloadFailure::disk_error
                )
            );
        }
        return FetchEnd::failed;
    }
    if (done == catalogue) {
        keep(hasher);
        return FetchEnd::complete;
    }
    {
        threads::LockGuard guard(state.mutex);
        if (!state.live(id))
            return state.quit ? FetchEnd::quit : FetchEnd::failed;
        if (state.match) {
            Downloads::State::Item* item = state.find(id);
            if (item != nullptr && item->state != DownloadState::waiting)
                item->state = DownloadState::paused_for_match;
            state.touch();
            return FetchEnd::paused;
        }
        Downloads::State::Item* item = state.find(id);
        if (item != nullptr && still_open(item->state)) {
            item->state = DownloadState::downloading;
            item->done = done;
            state.touch();
        }
    }
    const uint64_t still = catalogue - done;
    const Room room = room_for(part.parent_path(), still);
    if (room != Room::ok) {
        threads::LockGuard guard(state.mutex);
        Downloads::State::Item* item = state.find(id);
        if (item != nullptr && still_open(item->state)) {
            const DownloadFailure failure =
                room == Room::no_space ? DownloadFailure::no_space : DownloadFailure::disk_error;
            fail_locked(state, *item, failure, failure_text(failure));
        }
        return FetchEnd::failed;
    }
    http::Request request;
    request.method = http::Method::get;
    request.url = url::url_text(address);
    request.accept_gzip = false;
    if (done > 0)
        request.range_from = done;
    request.limits.max_body_bytes = catalogue == 0 ? 1 : catalogue;
    request.cancel = &state.http_cancel;
    state.phase.store(2);
    state.http_cancel.store(state.quit_flag.load() || state.item_cancel.load());
    PartSink sink(state, id, part, catalogue, done, hasher, done > 0);
    const http::Response response = client.fetch(request, sink);
    sink.finish_tail(response.failure == http::Failure::none && sink.halt_ == Halt::none);
    sink.commit();
    state.phase.store(0);
    const Answered seen = answered(request.url, response);
    {
        threads::LockGuard guard(state.mutex);
        Downloads::State::Item* item = state.find(id);
        if (item != nullptr) {
            item->fetched = seen;
            if (still_open(item->state))
                item->bytes_fetched += response.body_bytes;
        }
    }
    // A declared length past the catalogue never reaches the sink: the client
    // stops it as too large. That is a check when the answer is one (a page
    // longer than a small package), and otherwise still a file that is not
    // the catalogue's.
    Halt halt = sink.halt_;
    if (halt == Halt::none && response.status != 0 && package_check(seen))
        halt = Halt::browser_check;
    else if (halt == Halt::none && response.failure == http::Failure::too_large)
        halt = Halt::differs;
    if (halt == Halt::paused) {
        threads::LockGuard guard(state.mutex);
        Downloads::State::Item* item = state.find(id);
        if (item != nullptr && still_open(item->state) && item->state != DownloadState::waiting)
            item->state = DownloadState::paused_for_match;
        state.touch();
        return FetchEnd::paused;
    }
    if (halt == Halt::quit || state.quit_flag.load())
        return FetchEnd::quit;
    if (halt == Halt::cancelled || state.item_cancel.load()) {
        threads::LockGuard guard(state.mutex);
        Downloads::State::Item* item = state.find(id);
        if (item != nullptr && still_open(item->state)) {
            item->state = DownloadState::cancelled;
            item->failure = DownloadFailure::none;
            item->challenge.reset();
            state.mark_dirty();
            state.touch();
        }
        return FetchEnd::cancelled;
    }
    if (halt == Halt::browser_check) {
        // Never answered, asked again or tried at another address: the
        // player is told, and RETRY asks again when they choose.
        threads::LockGuard guard(state.mutex);
        Downloads::State::Item* item = state.find(id);
        if (item != nullptr && still_open(item->state))
            fail_locked(state, *item, DownloadFailure::server_check, {});
        return FetchEnd::failed;
    }
    if (halt == Halt::differs) {
        threads::LockGuard guard(state.mutex);
        Downloads::State::Item* item = state.find(id);
        if (item != nullptr && still_open(item->state)) {
            fail_locked(
                state,
                *item,
                DownloadFailure::server_file_differs,
                failure_text(DownloadFailure::server_file_differs)
            );
        }
        return FetchEnd::failed;
    }
    if (halt == Halt::no_space || halt == Halt::disk) {
        threads::LockGuard guard(state.mutex);
        Downloads::State::Item* item = state.find(id);
        if (item != nullptr && still_open(item->state)) {
            const DownloadFailure failure =
                halt == Halt::no_space ? DownloadFailure::no_space : DownloadFailure::disk_error;
            fail_locked(state, *item, failure, failure_text(failure));
        }
        return FetchEnd::failed;
    }
    if (halt == Halt::forbidden)
        return tokens ? FetchEnd::denied : FetchEnd::next_url;
    if (halt == Halt::not_found)
        return tokens ? FetchEnd::missing : FetchEnd::next_url;
    if (halt == Halt::range_satisfied || response.status == 416) {
        if (sink.done_ == catalogue || done == catalogue) {
            keep(sink.hasher_);
            return FetchEnd::complete;
        }
        bool again = false;
        {
            threads::LockGuard guard(state.mutex);
            Downloads::State::Item* item = state.find(id);
            if (item == nullptr || !still_open(item->state))
                return FetchEnd::failed;
            if (item->fetched_whole_after_range)
                again = true;
            else
                item->fetched_whole_after_range = true;
        }
        std::error_code error;
        fs::remove(part, error);
        if (again) {
            threads::LockGuard guard(state.mutex);
            Downloads::State::Item* item = state.find(id);
            if (item != nullptr && still_open(item->state)) {
                fail_locked(
                    state,
                    *item,
                    DownloadFailure::unreachable,
                    failure_text(DownloadFailure::unreachable)
                );
            }
            return FetchEnd::failed;
        }
        return FetchEnd::resume;
    }
    if (response.failure == http::Failure::cancelled) {
        if (state.quit_flag.load())
            return FetchEnd::quit;
        if (state.item_cancel.load())
            return FetchEnd::cancelled;
        if (state.match_flag.load())
            return FetchEnd::paused;
        return FetchEnd::resume;
    }
    if (response.failure == http::Failure::connection_lost ||
        response.failure == http::Failure::timed_out) {
        return FetchEnd::resume;
    }
    if (response.failure != http::Failure::none)
        return tokens ? FetchEnd::resume : FetchEnd::next_url;
    if (sink.done_ == catalogue) {
        keep(sink.hasher_);
        return FetchEnd::complete;
    }
    if (response.failure == http::Failure::none) {
        threads::LockGuard guard(state.mutex);
        Downloads::State::Item* item = state.find(id);
        if (item != nullptr && still_open(item->state)) {
            fail_locked(
                state,
                *item,
                DownloadFailure::server_file_differs,
                failure_text(DownloadFailure::server_file_differs)
            );
        }
        return FetchEnd::failed;
    }
    return FetchEnd::next_url;
}

/// The verdict of checking a finished file.
enum class Verdict : uint8_t {
    match,    ///< the file is the one the catalogue named
    mismatch, ///< it is not
    disagree, ///< the disk does not match the bytes just written
    quit,     ///< the check was stopped
    missing,  ///< the file is not there
};

/// Hashes the package file and compares it with the catalogue and the running hash.
///
/// @param path the file
/// @param catalogue_size the catalogue size
/// @param expected the catalogue digest
/// @param running the hash of the bytes written, when there is one
/// @param have_running true when `running` covers the file
/// @param quit set when the check should stop
/// @return the verdict
Verdict check_file(
    const fs::path& path,
    uint64_t catalogue_size,
    const sha::Digest& expected,
    const sha::Hasher& running,
    bool have_running,
    const std::atomic<bool>& quit
) {
    sha::Digest digest{};
    uint64_t size = 0;
    if (!hash_file(path, quit, digest, size))
        return quit.load() ? Verdict::quit : Verdict::missing;
    if (size != catalogue_size)
        return Verdict::mismatch;
    if (have_running && sha::finish(running) != digest)
        return Verdict::disagree;
    if (digest != expected)
        return Verdict::mismatch;
    return Verdict::match;
}

} // namespace

void Downloads::State::deliver(http::Client& client, uint64_t id, const fs::path& file) {
    Handoff handoff = nullptr;
    void* context = nullptr;
    InstallOrigin origin;
    {
        threads::LockGuard guard(mutex);
        Item* item = find(id);
        if (item == nullptr || !still_open(item->state))
            return;
        item->state = DownloadState::handing_over;
        touch();
        handoff = options.handoff;
        context = options.handoff_context;
        origin.registry = item->target.registry;
        origin.kind = item->target.kind;
        origin.key = item->target.key;
        origin.release = item->target.release;
        origin.sha256 = item->target.sha256;
        origin.reason = item->reason;
    }
    bool accepted = false;
    try {
        if (handoff != nullptr)
            accepted = handoff(context, id, file, origin);
    } catch (...) {
        accepted = false;
    }
    KeyResult result = KeyResult::failed;
    bool post = false;
    {
        threads::LockGuard guard(mutex);
        Item* item = find(id);
        if (item == nullptr || quit)
            return;
        if (!accepted) {
            if (still_open(item->state))
                fail_locked(
                    *this,
                    *item,
                    DownloadFailure::install_failed,
                    failure_text(DownloadFailure::install_failed)
                );
            post = true;
            result = KeyResult::failed;
        } else if (still_open(item->state)) {
            item->state = DownloadState::installing;
            touch();
        }
        while (item != nullptr && still_open(item->state) && !item->outcome && !quit)
            wake.wait(mutex), item = find(id);
        if (item == nullptr || quit)
            return;
        if (item->state == DownloadState::cancelled) {
            post = true;
            result = KeyResult::cancelled;
        } else if (!item->outcome) {
            if (still_open(item->state))
                fail_locked(
                    *this,
                    *item,
                    DownloadFailure::install_failed,
                    failure_text(DownloadFailure::install_failed)
                );
            post = true;
            result = KeyResult::failed;
        } else {
            const InstallOutcome outcome = *item->outcome;
            if (outcome == InstallOutcome::installed) {
                item->state = DownloadState::done;
                item->failure = DownloadFailure::none;
                result = KeyResult::installed;
            } else if (outcome == InstallOutcome::refused) {
                // The installer's own reason is the detail, so the message
                // and the log line name it.
                fail(*item, DownloadFailure::install_refused, item->install_reason);
                result = KeyResult::refused;
            } else if (outcome == InstallOutcome::failed) {
                fail(*item, DownloadFailure::install_failed, item->install_reason);
                result = KeyResult::failed;
            } else {
                item->state = DownloadState::cancelled;
                item->failure = DownloadFailure::none;
                result = KeyResult::cancelled;
            }
            mark_dirty();
            touch();
            post = true;
        }
    }
    if (post)
        send_result(client, id, result);
}

void Downloads::State::process(http::Client& client, uint64_t id) {
    fs::path final_path;
    fs::path part;
    uint64_t catalogue = 0;
    sha::Digest expected{};
    bool tokens = false;
    {
        threads::LockGuard guard(mutex);
        Item* item = find(id);
        if (item == nullptr || !still_open(item->state) || quit || match)
            return;
        const fs::path folder = downloads_of(options.data_folder);
        final_path = package_path(folder, item->target.sha256, item->target.kind, false);
        part = package_path(folder, item->target.sha256, item->target.kind, true);
        catalogue = item->target.size;
        expected = item->target.sha256;
        tokens = item->target.mode == data::registry::DownloadMode::tokens;
    }
    {
        std::error_code error;
        if (fs::is_regular_file(final_path, error)) {
            {
                threads::LockGuard guard(mutex);
                Item* item = find(id);
                if (item != nullptr && still_open(item->state)) {
                    item->state = DownloadState::verifying;
                    touch();
                }
            }
            const Verdict verdict =
                check_file(final_path, catalogue, expected, sha::Hasher{}, false, quit_flag);
            if (verdict == Verdict::quit)
                return;
            if (verdict == Verdict::match) {
                deliver(client, id, final_path);
                return;
            }
            fs::remove(final_path, error);
        }
    }

    const auto post_terminal = [&](FetchEnd end) {
        if (end == FetchEnd::failed)
            send_result(client, id, KeyResult::failed);
        else if (end == FetchEnd::cancelled)
            send_result(client, id, KeyResult::cancelled);
    };

    for (int attempt = 0; attempt < 2; ++attempt) {
        uint64_t prepared_done = 0;
        sha::Hasher prepared_hash;
        Halt prepared = Halt::none;
        if (!prepare_part(part, catalogue, quit_flag, prepared_done, prepared_hash, prepared)) {
            if (prepared == Halt::quit)
                return;
            {
                threads::LockGuard guard(mutex);
                Item* item = find(id);
                if (item != nullptr && still_open(item->state)) {
                    const DownloadFailure failure = prepared == Halt::no_space
                                                        ? DownloadFailure::no_space
                                                        : DownloadFailure::disk_error;
                    fail_locked(*this, *item, failure, failure_text(failure));
                }
            }
            send_result(client, id, KeyResult::failed);
            return;
        }
        if (prepared_done == catalogue) {
            const Verdict verdict =
                check_file(part, catalogue, expected, prepared_hash, true, quit_flag);
            if (verdict == Verdict::quit)
                return;
            if (verdict == Verdict::match) {
                if (!rename_part(part, final_path)) {
                    {
                        threads::LockGuard guard(mutex);
                        Item* item = find(id);
                        if (item != nullptr && still_open(item->state)) {
                            fail_locked(
                                *this,
                                *item,
                                DownloadFailure::disk_error,
                                failure_text(DownloadFailure::disk_error)
                            );
                        }
                    }
                    send_result(client, id, KeyResult::failed);
                    return;
                }
                deliver(client, id, final_path);
                return;
            }
            if (verdict == Verdict::disagree) {
                {
                    threads::LockGuard guard(mutex);
                    Item* item = find(id);
                    if (item != nullptr && still_open(item->state)) {
                        fail_locked(
                            *this,
                            *item,
                            DownloadFailure::disk_error,
                            failure_text(DownloadFailure::disk_error)
                        );
                    }
                }
                send_result(client, id, KeyResult::failed);
                return;
            }
            std::error_code error;
            fs::remove(part, error);
            if (attempt == 1) {
                {
                    threads::LockGuard guard(mutex);
                    Item* item = find(id);
                    if (item != nullptr && still_open(item->state)) {
                        fail_locked(
                            *this,
                            *item,
                            DownloadFailure::hash_mismatch,
                            failure_text(DownloadFailure::hash_mismatch)
                        );
                    }
                }
                send_result(client, id, KeyResult::failed);
                return;
            }
            continue;
        }

        const Room room = room_for(part.parent_path(), catalogue - prepared_done);
        if (room != Room::ok) {
            {
                threads::LockGuard guard(mutex);
                Item* item = find(id);
                if (item != nullptr && still_open(item->state)) {
                    const DownloadFailure failure = room == Room::no_space
                                                        ? DownloadFailure::no_space
                                                        : DownloadFailure::disk_error;
                    fail_locked(*this, *item, failure, failure_text(failure));
                }
            }
            send_result(client, id, KeyResult::failed);
            return;
        }

        bool use_files = !tokens;
        std::optional<url::Url> key_url;
        if (tokens) {
            const KeyGate gate = ensure_key(client, id);
            if (gate == KeyGate::quit || gate == KeyGate::paused)
                return;
            if (gate == KeyGate::cancelled) {
                send_result(client, id, KeyResult::cancelled);
                return;
            }
            if (gate == KeyGate::failed) {
                send_result(client, id, KeyResult::failed);
                return;
            }
            if (gate == KeyGate::mirrors) {
                use_files = true;
            } else {
                threads::LockGuard guard(mutex);
                Item* item = find(id);
                if (item == nullptr || !item->key || !still_open(item->state))
                    return;
                key_url = item->key->url;
            }
        }

        std::vector<url::Url> files;
        if (use_files) {
            threads::LockGuard guard(mutex);
            Item* item = find(id);
            if (item == nullptr || !still_open(item->state))
                return;
            files = item->target.files;
            if (files.empty() && item != nullptr && still_open(item->state)) {
                fail_locked(
                    *this,
                    *item,
                    DownloadFailure::unreachable,
                    failure_text(DownloadFailure::unreachable)
                );
            }
        }
        if (use_files && files.empty()) {
            send_result(client, id, KeyResult::failed);
            return;
        }

        sha::Hasher running;
        bool have_running = false;
        bool complete = false;
        int stalls = 0;
        std::size_t index = 0;
        const bool package_tokens = tokens && !use_files;
        while (!complete) {
            bool stopped = false;
            bool cancelled_item = false;
            {
                threads::LockGuard guard(mutex);
                if (!live(id)) {
                    stopped = true;
                    const Item* item = find(id);
                    cancelled_item = item != nullptr && item->state == DownloadState::cancelled;
                }
            }
            if (stopped) {
                send_result(client, id, cancelled_item ? KeyResult::cancelled : KeyResult::failed);
                return;
            }
            url::Url address;
            if (package_tokens) {
                if (!key_url)
                    return;
                address = *key_url;
            } else if (index >= files.size()) {
                {
                    threads::LockGuard guard(mutex);
                    Item* item = find(id);
                    if (item != nullptr && still_open(item->state)) {
                        fail_locked(
                            *this,
                            *item,
                            DownloadFailure::unreachable,
                            failure_text(DownloadFailure::unreachable)
                        );
                    }
                }
                send_result(client, id, KeyResult::failed);
                return;
            } else {
                address = files[index];
            }
            std::error_code error;
            uintmax_t before = 0;
            if (fs::is_regular_file(part, error))
                before = fs::file_size(part, error);
            const FetchEnd end =
                fetch_url(*this, client, id, address, part, catalogue, package_tokens, &running);
            if (end == FetchEnd::complete) {
                have_running = true;
                complete = true;
                break;
            }
            if (end == FetchEnd::paused || end == FetchEnd::quit)
                return;
            if (end == FetchEnd::cancelled || end == FetchEnd::failed) {
                post_terminal(end);
                return;
            }
            if (end == FetchEnd::denied || end == FetchEnd::missing) {
                bool give_up = false;
                DownloadFailure give_failure = DownloadFailure::unreachable;
                {
                    threads::LockGuard guard(mutex);
                    Item* item = find(id);
                    if (item == nullptr || !still_open(item->state))
                        return;
                    uint32_t& count =
                        end == FetchEnd::denied ? item->package_rekeys : item->package_misses;
                    if (count >= rekey_limit) {
                        give_up = true;
                        give_failure = end == FetchEnd::denied ? DownloadFailure::refused
                                                               : DownloadFailure::unreachable;
                        fail_locked(*this, *item, give_failure, failure_text(give_failure));
                    } else {
                        ++count;
                        item->key.reset();
                    }
                }
                if (give_up) {
                    send_result(client, id, KeyResult::failed);
                    return;
                }
                const KeyGate gate = ensure_key(client, id);
                if (gate == KeyGate::quit || gate == KeyGate::paused)
                    return;
                if (gate != KeyGate::ready) {
                    send_result(
                        client,
                        id,
                        gate == KeyGate::cancelled ? KeyResult::cancelled : KeyResult::failed
                    );
                    return;
                }
                {
                    threads::LockGuard guard(mutex);
                    Item* item = find(id);
                    if (item == nullptr || !item->key || !still_open(item->state))
                        return;
                    key_url = item->key->url;
                }
                continue;
            }
            if (end == FetchEnd::resume) {
                uintmax_t after = 0;
                if (fs::is_regular_file(part, error))
                    after = fs::file_size(part, error);
                if (after <= before)
                    ++stalls;
                else
                    stalls = 0;
                if (stalls >= static_cast<int>(stall_limit)) {
                    {
                        threads::LockGuard guard(mutex);
                        Item* item = find(id);
                        if (item != nullptr && still_open(item->state)) {
                            fail_locked(
                                *this,
                                *item,
                                DownloadFailure::unreachable,
                                failure_text(DownloadFailure::unreachable)
                            );
                        }
                    }
                    send_result(client, id, KeyResult::failed);
                    return;
                }
                if (package_tokens) {
                    threads::LockGuard guard(mutex);
                    Item* item = find(id);
                    if (item != nullptr && item->key)
                        key_url = item->key->url;
                }
                continue;
            }
            ++index;
        }

        {
            threads::LockGuard guard(mutex);
            Item* item = find(id);
            if (item != nullptr && still_open(item->state)) {
                item->state = DownloadState::verifying;
                touch();
            }
        }
        const Verdict verdict =
            check_file(part, catalogue, expected, running, have_running, quit_flag);
        if (verdict == Verdict::quit)
            return;
        if (verdict == Verdict::match) {
            if (!rename_part(part, final_path)) {
                {
                    threads::LockGuard guard(mutex);
                    Item* item = find(id);
                    if (item != nullptr && still_open(item->state)) {
                        fail_locked(
                            *this,
                            *item,
                            DownloadFailure::disk_error,
                            failure_text(DownloadFailure::disk_error)
                        );
                    }
                }
                send_result(client, id, KeyResult::failed);
                return;
            }
            deliver(client, id, final_path);
            return;
        }
        if (verdict == Verdict::disagree) {
            {
                threads::LockGuard guard(mutex);
                Item* item = find(id);
                if (item != nullptr && still_open(item->state)) {
                    fail_locked(
                        *this,
                        *item,
                        DownloadFailure::disk_error,
                        failure_text(DownloadFailure::disk_error)
                    );
                }
            }
            send_result(client, id, KeyResult::failed);
            return;
        }
        std::error_code error;
        fs::remove(part, error);
        if (attempt == 1) {
            {
                threads::LockGuard guard(mutex);
                Item* item = find(id);
                if (item != nullptr && still_open(item->state)) {
                    fail_locked(
                        *this,
                        *item,
                        DownloadFailure::hash_mismatch,
                        failure_text(DownloadFailure::hash_mismatch)
                    );
                }
            }
            send_result(client, id, KeyResult::failed);
            return;
        }
    }
}

void Downloads::State::run() {
    http::ClientOptions http_options;
    {
        threads::LockGuard guard(mutex);
        http_options = options.http;
    }
    http::Client client(std::move(http_options));
    {
        threads::LockGuard guard(mutex);
        while (!started && !quit)
            wake.wait(mutex);
    }
    if (quit_flag.load()) {
        log_failures();
        save_queue();
        return;
    }
    restore_queue();
    while (!quit_flag.load()) {
        save_queue();
        // Each failure is logged here once, the main thread's (an install
        // ID turned off) as well as the worker's.
        log_failures();
        uint64_t id = 0;
        {
            threads::LockGuard guard(mutex);
            id = pick();
            while (!quit && id == 0 && !failure_unlogged()) {
                wake.wait(mutex);
                id = pick();
            }
            if (quit)
                break;
            if (id == 0)
                continue;
            current_id = id;
            item_cancel.store(false);
        }
        // An item picked as soon as it was queued is saved before it starts:
        // the next save comes only once it ends, and it holds the worker
        // while the installer has the file.
        save_queue();
        process(client, id);
        {
            threads::LockGuard guard(mutex);
            if (current_id == id)
                current_id = 0;
            item_cancel.store(false);
            phase.store(0);
        }
    }
    log_failures();
    save_queue();
}

Downloads::Downloads(DownloadsOptions options) : state_(std::make_unique<State>()) {
    state_->options = std::move(options);
    if (!threads::start_thread(state_->thread, &State::entry, state_.get()))
        state_->thread_failed = true;
    else
        state_->thread_started = true;
}

Downloads::~Downloads() {
    {
        threads::LockGuard guard(state_->mutex);
        state_->quit = true;
        state_->quit_flag.store(true);
        state_->http_cancel.store(true);
    }
    state_->wake.notify_all();
    if (state_->thread_started)
        threads::join_thread(state_->thread);
}

void Downloads::start(const Snapshot& snapshot) {
    threads::LockGuard guard(state_->mutex);
    if (state_->started)
        return;
    state_->snapshot = std::make_shared<const Snapshot>(snapshot);
    state_->started = true;
    state_->wake.notify_all();
}

std::optional<uint64_t>
Downloads::queue(DownloadTarget target, DownloadReason reason, std::string* why) {
    threads::LockGuard guard(state_->mutex);
    if (state_->thread_failed) {
        set_why(why, "the download could not be started");
        return std::nullopt;
    }
    if (state_->options.data_folder.empty()) {
        set_why(why, "downloads are off");
        return std::nullopt;
    }
    if (target.install_id == data::registry::InstallIdUse::required &&
        state_->id_off(target.registry)) {
        set_why(why, failure_text(DownloadFailure::install_id_off));
        return std::nullopt;
    }
    for (const State::Item& item : state_->items) {
        if (still_open(item.state) && item.target.registry == target.registry &&
            item.target.key == target.key) {
            set_why(why, "that package is already being downloaded");
            return std::nullopt;
        }
    }
    State::Item item;
    item.id = state_->next_id++;
    item.reason = reason;
    item.target = std::move(target);
    const uint64_t id = item.id;
    state_->items.push_back(std::move(item));
    state_->mark_dirty();
    state_->touch();
    state_->wake.notify_all();
    return id;
}

void Downloads::cancel(uint64_t item) {
    threads::LockGuard guard(state_->mutex);
    State::Item* found = state_->find(item);
    if (found == nullptr || !still_open(found->state))
        return;
    found->state = DownloadState::cancelled;
    found->failure = DownloadFailure::none;
    found->challenge.reset();
    state_->mark_dirty();
    state_->touch();
    if (state_->current_id == item) {
        state_->item_cancel.store(true);
        state_->http_cancel.store(true);
    }
    state_->wake.notify_all();
}

void Downloads::cancel_registry(std::string_view registry) {
    threads::LockGuard guard(state_->mutex);
    bool changed = false;
    for (State::Item& item : state_->items) {
        if (item.target.registry != registry || !still_open(item.state))
            continue;
        item.state = DownloadState::cancelled;
        item.failure = DownloadFailure::none;
        item.challenge.reset();
        if (state_->current_id == item.id) {
            state_->item_cancel.store(true);
            state_->http_cancel.store(true);
        }
        changed = true;
    }
    if (!changed)
        return;
    state_->mark_dirty();
    state_->touch();
    state_->wake.notify_all();
}

void Downloads::retry(uint64_t item) {
    threads::LockGuard guard(state_->mutex);
    State::Item* found = state_->find(item);
    if (found == nullptr)
        return;
    if (found->state != DownloadState::failed && found->state != DownloadState::cancelled)
        return;
    found->state = DownloadState::waiting;
    found->failure = DownloadFailure::none;
    found->detail.clear();
    found->challenge.reset();
    found->key.reset();
    found->download_id.clear();
    found->bytes_fetched = 0;
    found->first_key_at.reset();
    found->result_posted = false;
    found->transport_failures = 0;
    found->rate_limits = 0;
    found->expiry_rekeys = 0;
    found->package_rekeys = 0;
    found->package_misses = 0;
    found->hash_failures = 0;
    found->fetched_whole_after_range = false;
    found->outcome.reset();
    found->install_reason.clear();
    found->asked.reset();
    found->fetched.reset();
    found->logged = false;
    state_->mark_dirty();
    state_->touch();
    state_->wake.notify_all();
}

void Downloads::set_install_id(std::string_view registry, std::optional<std::string> id) {
    threads::LockGuard guard(state_->mutex);
    const std::string key(registry);
    bool found = false;
    for (auto& entry : state_->install_ids) {
        if (entry.first == key) {
            entry.second = std::move(id);
            found = true;
            break;
        }
    }
    if (!found)
        state_->install_ids.emplace_back(key, std::move(id));
    const bool off = state_->id_off(key);
    if (!off)
        return;
    bool changed = false;
    for (State::Item& item : state_->items) {
        if (item.target.registry != key || !still_open(item.state))
            continue;
        if (item.target.install_id != data::registry::InstallIdUse::required)
            continue;
        state_->fail(item, DownloadFailure::install_id_off, {});
        if (state_->current_id == item.id)
            state_->http_cancel.store(true);
        changed = true;
    }
    if (!changed)
        return;
    state_->mark_dirty();
    state_->touch();
    state_->wake.notify_all();
}

void Downloads::set_match_running(bool running) noexcept {
    threads::LockGuard guard(state_->mutex);
    state_->match = running;
    state_->match_flag.store(running);
    if (running) {
        if (state_->phase.load() != 2)
            state_->http_cancel.store(true);
    } else if (!state_->quit_flag.load() && !state_->item_cancel.load()) {
        state_->http_cancel.store(false);
    }
    state_->wake.notify_all();
}

void Downloads::set_language(std::string tag) {
    threads::LockGuard guard(state_->mutex);
    state_->options.language = std::move(tag);
}

void Downloads::report_install(uint64_t item, InstallOutcome outcome, std::string reason) {
    threads::LockGuard guard(state_->mutex);
    State::Item* found = state_->find(item);
    if (found == nullptr)
        return;
    if (found->state != DownloadState::installing && found->state != DownloadState::handing_over)
        return;
    found->outcome = outcome;
    found->install_reason = std::move(reason);
    state_->wake.notify_all();
}

std::vector<DownloadView> Downloads::view() const {
    threads::LockGuard guard(state_->mutex);
    std::vector<DownloadView> views;
    views.reserve(state_->items.size());
    for (const State::Item& item : state_->items) {
        DownloadView view;
        view.item = item.id;
        view.target = item.target;
        view.reason = item.reason;
        view.state = item.state;
        view.done = item.done;
        view.failure = item.failure;
        view.detail = item.detail;
        view.challenge = item.challenge;
        views.push_back(std::move(view));
    }
    return views;
}

uint64_t Downloads::generation() const noexcept {
    threads::LockGuard guard(state_->mutex);
    return state_->generation;
}

bool Downloads::busy() const noexcept {
    threads::LockGuard guard(state_->mutex);
    for (const State::Item& item : state_->items) {
        if (still_open(item.state))
            return true;
    }
    return false;
}

std::vector<std::filesystem::path> Downloads::files_in_use() const {
    threads::LockGuard guard(state_->mutex);
    std::vector<std::filesystem::path> paths;
    if (state_->options.data_folder.empty())
        return paths;
    const fs::path folder = downloads_of(state_->options.data_folder);
    for (const State::Item& item : state_->items) {
        if (!still_open(item.state))
            continue;
        paths.push_back(package_path(folder, item.target.sha256, item.target.kind, true));
        paths.push_back(package_path(folder, item.target.sha256, item.target.kind, false));
    }
    return paths;
}

std::string failure_message(const DownloadView& item, LookUp look_up) {
    const auto words = [look_up](std::string_view english) {
        if (english.empty() || look_up == nullptr)
            return std::string(english);
        return std::string(look_up(english));
    };
    std::string message;
    if (item.failure == DownloadFailure::server_check) {
        message = words(server_check_words);
        const std::string name =
            item.target.registry_name.empty() ? item.target.registry : item.target.registry_name;
        constexpr std::string_view place = "{registry}";
        const std::size_t at = message.find(place);
        if (at != std::string::npos)
            message.replace(at, place.size(), name);
    } else if (
        !item.detail.empty() && (item.failure == DownloadFailure::install_refused ||
                                 item.failure == DownloadFailure::install_failed)
    ) {
        message = item.detail;
    } else {
        message = words(failure_text(item.failure));
        if (!item.detail.empty() && message.find(item.detail) == std::string::npos)
            message += message.empty() ? item.detail : " (" + item.detail + ")";
    }
    if (!message.empty() && message[0] >= 'a' && message[0] <= 'z')
        message[0] = static_cast<char>(message[0] - 'a' + 'A');
    return message;
}

std::optional<DownloadTarget> download_target(
    const Snapshot& snapshot, std::string_view registry, std::string_view key, std::string* why
) {
    const RegistryView* view = nullptr;
    for (const RegistryView& candidate : snapshot.registries) {
        if (candidate.registry.descriptor.id == registry) {
            view = &candidate;
            break;
        }
    }
    if (view == nullptr || view->catalogue == nullptr) {
        set_why(why, "the catalogue has no such package");
        return std::nullopt;
    }
    const data::catalogue::Package* package = nullptr;
    for (const Entry& entry : snapshot.entries) {
        if (entry.registry == registry && entry.package != nullptr && entry.package->id == key) {
            package = entry.package;
            break;
        }
    }
    if (package == nullptr) {
        set_why(why, "the catalogue has no such package");
        return std::nullopt;
    }
    DownloadTarget target;
    target.registry = std::string(registry);
    target.registry_name = view->registry.descriptor.name;
    target.kind = package->kind;
    target.key = package->id;
    target.name = package->name;
    target.release = package->release;
    target.size = package->size;
    target.sha256 = package->sha256;
    target.files = reference_urls(*view, package->file);
    target.mode = view->registry.descriptor.mode;
    target.api = view->registry.descriptor.api;
    target.install_id = view->registry.descriptor.install_id;
    return target;
}

} // namespace oa::app::content

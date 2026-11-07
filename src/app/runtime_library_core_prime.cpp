// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Core Prime's replay list in the running game. One worker reads the JSON
// feed. Another downloads one .oademo at a time, so a file that is waiting
// does not hold the list or a card. The main thread publishes the pages.
// A list the player is looking at stays as it is; it is read again when they
// show it once more. A saved file is watched once its needs have been offered.
// The game never downloads a .tad.

#include "library_host.hpp"
#include "oa_layer.hpp"

#include "oa/app/content/download_api.hpp"
#include "oa/app/content/downloads.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/base/sha256.hpp"
#include "oa/base/threads.hpp"
#include "oa/data/replays/feed.hpp"
#include "oa/data/replays/interface_words.hpp"
#include "oa/data/replays/replays.hpp"
#include "oa/formats/url.hpp"
#include "oa/netgame/http/client.hpp"
#include "oa/platform/files.hpp"
#include "oa/platform/system.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/replays.hpp"
#include "oa/ui/library/text.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app {

/// What is installed, so a Core Prime card's gaps are read again when it changes.
/// Defined with the Replays section.
std::string index_pass_content_stamp(Runtime& runtime);

namespace fs = std::filesystem;
namespace http = oa::netgame::http;
namespace library = oa::ui::library;
namespace platform = oa::platform;
namespace replay_data = oa::data::replays;
namespace sha256 = oa::base::sha256;
namespace url = oa::formats::url;

/// The worker's free functions cannot name Runtime's private members. Runtime
/// names this a friend so the feed address and the download API can be read.
struct CorePrimeGate {
    static const std::string& configured_api(const Runtime& runtime) {
        return runtime.options_.core_prime_api;
    }

    static content::Service& service(Runtime& runtime) { return runtime.content_service(); }

    static const fs::path& user_folder(const Runtime& runtime) { return runtime.user_folder_; }

    static std::string language_tag(const Runtime& runtime) {
        return std::string(runtime.shown_language().tag);
    }

    static std::optional<std::string> install(Runtime& runtime, std::string_view registry) {
        return runtime.content_install_id(registry);
    }

    static bool library_open(Runtime& runtime) {
        return runtime.oa_layer().find(library_screen_name) != nullptr;
    }
};

namespace {

constexpr std::string_view kLogPrefix = "open-annihilation: library: ";
constexpr std::string_view kKnownDownloadApi = "http://downloads.coreprime.net/api/v1";
constexpr std::string_view kCorePrimeId = "coreprime";
constexpr std::string_view kOademoKind = "oademo";
constexpr std::string_view kReleaseMismatch = "release_mismatch";
constexpr std::string_view kUnknownPackage = "unknown_package";
constexpr std::string_view kUnknownReplay = "unknown_replay";
constexpr std::string_view kReplaysUnavailable = "replays_unavailable";
constexpr uint32_t kBackoffMinMs = 5000;
constexpr uint32_t kBackoffMaxMs = 60000;
constexpr uint32_t kBackoffHardMs = 60000;
/// The same bounds the package downloads use (download.cpp).
constexpr uint32_t kRateReasks = 3;
constexpr uint32_t kPollLimit = 1000;
constexpr uint32_t kRetryAfterDefaultSeconds = 30;
constexpr uint32_t kRetryAfterCapSeconds = 300;
constexpr uint32_t kWaitSliceMs = 50;
/// A guard, not a product limit: the id plus a count names a file after the
/// map and its time are already on disk.
constexpr uint32_t kNameGuard = 100000;
constexpr int32_t kMinutesPerHour = 60;
constexpr int32_t kTmYearBase = 1900;
constexpr uint64_t kFeedBodyBytes = 2ULL * 1024ULL * 1024ULL;

void core_log(std::string_view line) {
    std::cerr << kLogPrefix << line << '\n';
}

std::string strip_slash(std::string text) {
    while (!text.empty() && text.back() == '/')
        text.pop_back();
    return text;
}

std::optional<fs::path> recording_file(const fs::path& recordings, std::string_view path) {
    fs::path file = recordings;
    std::size_t at = 0;
    while (at <= path.size()) {
        const std::size_t slash = path.find('/', at);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        const std::string_view part = path.substr(at, end - at);
        if (part.empty() || part == "." || part == ".." ||
            part.find('\\') != std::string_view::npos || part.find(':') != std::string_view::npos)
            return std::nullopt;
        file /= platform::path_from_utf8(std::string(part));
        if (slash == std::string_view::npos)
            break;
        at = slash + 1;
    }
    return file;
}

std::optional<sha256::Digest> digest_of_file(const fs::path& file) {
    std::error_code error;
    const auto size = fs::file_size(file, error);
    if (error || size > replay_data::most_file_bytes)
        return std::nullopt;
    std::ifstream input(file, std::ios::binary);
    if (!input)
        return std::nullopt;
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (input.gcount() != static_cast<std::streamsize>(bytes.size()))
        return std::nullopt;
    return sha256::digest_of(bytes);
}

bool write_bytes(const fs::path& file, const std::vector<uint8_t>& bytes) {
    std::error_code made;
    fs::create_directories(file.parent_path(), made);
    if (made)
        return false;
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    if (!output)
        return false;
    output.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    return static_cast<bool>(output);
}

void remove_file(const fs::path& file) {
    std::error_code failed;
    fs::remove(file, failed);
}

struct LocalWhen {
    int32_t day{};
    int32_t minute{};
};

LocalWhen local_when(int64_t seconds) {
    const auto instant = static_cast<std::time_t>(seconds);
    std::tm parts{};
#if defined(_WIN32)
    if (localtime_s(&parts, &instant) != 0)
        return {};
#else
    if (localtime_r(&instant, &parts) == nullptr)
        return {};
#endif
    LocalWhen when;
    when.day = replay_data::day_number(
        parts.tm_year + kTmYearBase,
        static_cast<uint32_t>(parts.tm_mon + 1),
        static_cast<uint32_t>(parts.tm_mday)
    );
    when.minute = parts.tm_hour * kMinutesPerHour + parts.tm_min;
    return when;
}

/// `YYYY-MM-DD HHMM` in UTC, so two replays of one map can each have a file.
std::string recorded_stamp(int64_t seconds) {
    if (seconds <= 0)
        return {};
    const auto instant = static_cast<std::time_t>(seconds);
    std::tm parts{};
#if defined(_WIN32)
    if (gmtime_s(&parts, &instant) != 0)
        return {};
#else
    if (gmtime_r(&instant, &parts) == nullptr)
        return {};
#endif
    char buffer[32];
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%04d-%02d-%02d %02d%02d",
        parts.tm_year + kTmYearBase,
        parts.tm_mon + 1,
        parts.tm_mday,
        parts.tm_hour,
        parts.tm_min
    );
    return buffer;
}

std::string feed_origin_of(const Runtime& runtime) {
    const std::string& configured = CorePrimeGate::configured_api(runtime);
    if (configured.empty())
        return std::string(replay_data::core_prime_feed_origin);
    return strip_slash(configured);
}

std::optional<url::Url> download_api_of(Runtime& runtime) {
    const std::string& configured = CorePrimeGate::configured_api(runtime);
    if (!configured.empty())
        return url::parse_http_url(strip_slash(configured) + "/api/v1");
    if (const std::shared_ptr<const content::Snapshot> snapshot =
            CorePrimeGate::service(runtime).snapshot()) {
        for (const content::RegistryView& registry : snapshot->registries) {
            if (registry.registry.descriptor.id == kCorePrimeId && registry.registry.descriptor.api)
                return *registry.registry.descriptor.api;
        }
    }
    return url::parse_http_url(kKnownDownloadApi);
}

std::string query_key_of(const library::Library& model) {
    using library::CorePrimeFilter;
    const CorePrimeFilter filter = model.replays.core_filter;
    if (filter == CorePrimeFilter::longest)
        return "longest";
    if (filter == CorePrimeFilter::downloads)
        return "downloads";
    if (filter == CorePrimeFilter::planet && !model.replays.planet.empty())
        return "newest\n" + model.replays.planet;
    return "newest";
}

replay_data::FeedQuery query_from_key(std::string_view key, std::string_view after) {
    replay_data::FeedQuery query;
    query.first = replay_data::replay_page_size;
    const std::size_t split = key.find('\n');
    if (split == std::string_view::npos) {
        query.sort = std::string(key);
    } else {
        query.sort = std::string(key.substr(0, split));
        query.planet = std::string(key.substr(split + 1));
    }
    query.after = std::string(after);
    return query;
}

std::string body_text(const std::vector<uint8_t>& body) {
    return std::string(reinterpret_cast<const char*>(body.data()), body.size());
}

http::Response fetch_get(
    http::Client& client,
    const std::string& address,
    std::vector<uint8_t>& body,
    uint64_t limit,
    const std::atomic<bool>* cancel
) {
    http::Request request;
    request.method = http::Method::get;
    request.url = address;
    request.cancel = cancel;
    request.limits.max_body_bytes = limit;
    request.timeouts.total_ms = 120000;
    return client.fetch(request, body);
}

} // namespace

/// One page of one query, and one replay's file coming down on its own worker.
struct CorePrimeShelf {
    enum class AskKind : uint8_t { list, entry, file, locate };
    enum class FileHow : uint8_t {
        saved,
        already,
        mismatch,
        removed,
        unavailable,
        offline,
        failed
    };

    /// Which sentence a finished download shows. The main thread looks it up.
    enum class FileNotice : uint8_t {
        none,
        offline,
        unavailable,
        mismatch,
        no_file,
        not_saved,
        browser,
        expired,
        server
    };

    struct Ask {
        AskKind kind{AskKind::list};
        std::string key{};
        std::string after{};
        std::string id{};
        bool watch{};
        int64_t revision{};
        std::string sha{};
        std::string title{};
        int64_t recorded{};
        std::string origin{};
        url::Url api{};
        bool have_api{};
        fs::path recordings{};
        std::string language{};
        std::optional<std::string> install{};
        /// The file generation this ask was queued at. A newer one cancels it.
        uint64_t generation{};
    };

    struct Done {
        AskKind kind{AskKind::list};
        std::string key{};
        std::string after{};
        bool network{};
        int http_status{};
        uint32_t retry_after_seconds{};
        replay_data::FeedStatus status{replay_data::FeedStatus::unreadable};
        replay_data::FeedPage page{};
        replay_data::FeedEntry entry{};
        bool have_entry{};
        replay_data::FeedFailure failure{};
        FileHow how{FileHow::failed};
        FileNotice notice{FileNotice::none};
        std::string message{};
        std::string id{};
        bool watch{};
        std::string relative{};
        /// The worker stopped because the shelf was closed. Say nothing.
        bool quiet{};
    };

    struct Cache {
        std::string key{};
        std::vector<replay_data::FeedEntry> items{};
        int64_t total{};
        std::string next{};
        uint32_t fetched_ms{};
        bool have{};
        bool loading{};
        bool next_loading{};
        bool offline{};
        bool unreadable{};
        bool unavailable{};
        uint32_t failed_ms{};
        bool failed{};
        /// The wait the next failure uses, doubling from 5 seconds to 60.
        uint32_t wait_ms{kBackoffMinMs};
        /// How long the failure now in `failed` waits before another ask.
        uint32_t delay_ms{kBackoffMinMs};
        /// Page 1 may replace the rows. Set when this list is shown again.
        bool refresh{true};
    };

    struct Gaps {
        std::string id{};
        uint64_t generation{};
        /// R19's install stamp. A kept release or an update changes the card.
        std::string install{};
        std::vector<replay_data::ContentGap> gaps{};
    };

    base::threads::Mutex mutex{};
    base::threads::ConditionVariable wake{};
    base::threads::Thread thread{};
    base::threads::ConditionVariable file_wake{};
    base::threads::Thread file_thread{};
    std::atomic<bool> cancel{false};
    /// Bumped to stop the file that is waiting. Queued files take the new value.
    std::atomic<uint64_t> file_generation{1};
    bool stop{};
    bool started{};
    bool file_started{};
    std::vector<Ask> waiting{};
    std::vector<Ask> file_waiting{};
    std::vector<Done> done{};
    /// The file the file worker has taken. Empty when it is between files.
    std::string file_running_id{};
    bool file_running{};

    std::vector<Cache> caches{};
    std::string shown_key{};
    std::string downloading_id{};
    bool watch_when_saved{};
    std::string watch_after{};
    std::string watch_relative{};
    std::string detail_id{};
    std::string detail_failed{};
    uint32_t detail_failed_ms{};
    uint32_t detail_wait_ms{kBackoffMinMs};
    uint32_t detail_delay_ms{kBackoffMinMs};
    std::optional<Gaps> gaps{};
    bool worker_failed{};
    /// Planets named by any page loaded this run, in the order they were seen.
    std::vector<std::string> planets{};

    /// A file saved this run, so the card can say so before the folder is read.
    struct SavedHere {
        std::string id{};
        std::string relative{};
    };

    std::vector<SavedHere> saved{};
    /// A chosen replay whose file is not under Recordings/Core Prime.
    std::vector<std::string> locate_missed{};
    /// The replay whose file the list worker is looking for.
    std::string locate_id{};
    /// The Core Prime tab was showing on the previous pump.
    bool was_wanted{};
    /// The downloading replay is waiting on a check or a 429.
    bool download_waiting{};

    /// What the card says while a check or a 429 is still in progress.
    struct LiveNote {
        enum class Kind : uint8_t { none, challenge, rate };
        Kind kind{Kind::none};
        std::string id{};
        std::string code{};
        std::string address{};
        uint32_t seconds{};
    };

    LiveNote note{};
    /// The note the main thread last wrote onto the card.
    std::string noted_id{};
    std::string noted_sentence{};
};

namespace {

CorePrimeShelf::Cache* cache_for(CorePrimeShelf& shelf, std::string_view key) {
    for (CorePrimeShelf::Cache& cache : shelf.caches)
        if (cache.key == key)
            return &cache;
    return nullptr;
}

const CorePrimeShelf::Cache* cache_for(const CorePrimeShelf& shelf, std::string_view key) {
    for (const CorePrimeShelf::Cache& cache : shelf.caches)
        if (cache.key == key)
            return &cache;
    return nullptr;
}

CorePrimeShelf::Cache& ensure_cache(CorePrimeShelf& shelf, const std::string& key) {
    if (CorePrimeShelf::Cache* found = cache_for(shelf, key))
        return *found;
    CorePrimeShelf::Cache cache;
    cache.key = key;
    shelf.caches.push_back(std::move(cache));
    return shelf.caches.back();
}

std::optional<replay_data::FeedEntry> entry_copy(const CorePrimeShelf& shelf, std::string_view id) {
    std::optional<replay_data::FeedEntry> found;
    const CorePrimeShelf::Cache* shown = cache_for(shelf, shelf.shown_key);
    const auto take = [&](const CorePrimeShelf::Cache& cache) {
        for (const replay_data::FeedEntry& entry : cache.items) {
            if (entry.id == id)
                found = entry;
        }
    };
    if (shown != nullptr)
        take(*shown);
    if (!found) {
        for (const CorePrimeShelf::Cache& cache : shelf.caches)
            take(cache);
    }
    return found;
}

void keep_needs(replay_data::FeedEntry& incoming, const replay_data::FeedEntry& held) {
    if (incoming.needs_present || !held.needs_present)
        return;
    incoming.needs_present = true;
    incoming.replay.needs = held.replay.needs;
    incoming.replay.needs_read = held.replay.needs_read;
}

void merge_items(
    std::vector<replay_data::FeedEntry>& held,
    std::vector<replay_data::FeedEntry> incoming,
    bool replace
) {
    if (replace) {
        const std::vector<replay_data::FeedEntry> previous = std::move(held);
        held.clear();
        held.reserve(incoming.size());
        for (replay_data::FeedEntry& entry : incoming) {
            for (const replay_data::FeedEntry& old : previous) {
                if (old.id == entry.id) {
                    keep_needs(entry, old);
                    break;
                }
            }
            held.push_back(std::move(entry));
        }
        return;
    }
    for (replay_data::FeedEntry& entry : incoming) {
        bool found = false;
        for (replay_data::FeedEntry& kept : held) {
            if (kept.id != entry.id)
                continue;
            keep_needs(entry, kept);
            kept = std::move(entry);
            found = true;
            break;
        }
        if (!found)
            held.push_back(std::move(entry));
    }
}

void store_entry(CorePrimeShelf& shelf, const replay_data::FeedEntry& entry) {
    for (CorePrimeShelf::Cache& cache : shelf.caches) {
        for (replay_data::FeedEntry& held : cache.items) {
            if (held.id == entry.id)
                held = entry;
        }
    }
}

void drop_entry(CorePrimeShelf& shelf, std::string_view id) {
    for (CorePrimeShelf::Cache& cache : shelf.caches) {
        std::vector<replay_data::FeedEntry> kept;
        kept.reserve(cache.items.size());
        for (replay_data::FeedEntry& entry : cache.items) {
            if (entry.id != id)
                kept.push_back(std::move(entry));
        }
        cache.items = std::move(kept);
    }
}

bool queued(const CorePrimeShelf& shelf, const CorePrimeShelf::Ask& ask) {
    for (const CorePrimeShelf::Ask& held : shelf.waiting) {
        if (held.kind == ask.kind && held.key == ask.key && held.after == ask.after &&
            held.id == ask.id)
            return true;
    }
    return false;
}

bool post_ask(CorePrimeShelf& shelf, CorePrimeShelf::Ask ask) {
    const bool file = ask.kind == CorePrimeShelf::AskKind::file;
    {
        base::threads::LockGuard held(shelf.mutex);
        if (shelf.stop)
            return false;
        if (file) {
            if (shelf.file_running_id == ask.id)
                return false;
            for (const CorePrimeShelf::Ask& held_ask : shelf.file_waiting) {
                if (held_ask.id == ask.id)
                    return false;
            }
            shelf.file_waiting.push_back(std::move(ask));
        } else {
            if (queued(shelf, ask))
                return false;
            shelf.waiting.push_back(std::move(ask));
        }
    }
    if (file)
        shelf.file_wake.notify_all();
    else
        shelf.wake.notify_all();
    return true;
}

bool may_retry(const CorePrimeShelf::Cache& cache, uint32_t now) {
    return !cache.failed || now - cache.failed_ms >= cache.delay_ms;
}

struct Backoff {
    uint32_t delay_ms{kBackoffMinMs};
    uint32_t next_wait_ms{kBackoffMinMs};
};

/// The wait a failed list or detail uses. A 503 or a 429 waits at least a
/// minute, or its Retry-After when that is longer. Anything else waits at
/// least 5 seconds and doubles up to a minute.
Backoff backoff_for(uint32_t wait_ms, int http_status, uint32_t retry_after, bool unavailable) {
    const bool hard = http_status == 503 || http_status == 429 || unavailable;
    uint32_t delay = wait_ms < kBackoffMinMs ? kBackoffMinMs : wait_ms;
    if (hard) {
        delay = kBackoffHardMs;
        const uint64_t named = static_cast<uint64_t>(retry_after) * 1000ULL;
        if (named > delay)
            delay = static_cast<uint32_t>(std::min<uint64_t>(named, 300000ULL));
    }
    Backoff out;
    out.delay_ms = delay;
    out.next_wait_ms = hard ? kBackoffMinMs : std::min(kBackoffMaxMs, delay * 2);
    return out;
}

void note_http(
    CorePrimeShelf::Done& done, const http::Response& response, std::span<const uint8_t> body
) {
    const content::ApiError error =
        content::read_api_error(response.status, body, response.header("retry-after"));
    done.http_status = response.status;
    done.retry_after_seconds = error.retry_after_seconds;
}

std::string list_sentence(const library::Library& model, std::string_view word) {
    return std::string(library::shown(model.text, word));
}

void fill_request(content::KeyRequest& request, const CorePrimeShelf::Ask& ask) {
    request.kind = oa::data::catalogue::Kind::oamod;
    request.kind_text = std::string(kOademoKind);
    request.package = ask.id;
    request.release = ask.revision;
    request.engine = std::string(content::engine_version());
    request.platform = std::string(content::platform_name());
    request.arch = std::string(content::arch_name());
    request.language = ask.language.empty() ? std::string("en") : ask.language;
    request.reason = content::DownloadReason::install;
    request.install = ask.install;
}

void post_key_result(
    http::Client& client,
    const url::Url& api,
    std::string_view download,
    content::KeyResult result,
    uint64_t bytes,
    int64_t began,
    const std::atomic<bool>* cancel
) {
    if (cancel != nullptr && cancel->load())
        return;
    const int64_t seconds = std::max<int64_t>(0, static_cast<int64_t>(std::time(nullptr)) - began);
    std::string why;
    if (!content::post_result(client, api, download, result, bytes, seconds, &why) && !why.empty())
        core_log("the replay's result was not accepted: " + why);
}

CorePrimeShelf::Done
fetch_list(http::Client& client, const CorePrimeShelf::Ask& ask, const std::atomic<bool>* cancel) {
    CorePrimeShelf::Done done;
    done.kind = CorePrimeShelf::AskKind::list;
    done.key = ask.key;
    done.after = ask.after;
    std::vector<uint8_t> body;
    const http::Response response = fetch_get(
        client,
        replay_data::feed_list_url(ask.origin, query_from_key(ask.key, ask.after)),
        body,
        kFeedBodyBytes,
        cancel
    );
    note_http(done, response, body);
    if (response.failure != http::Failure::none || response.status == 0) {
        done.network = true;
        return done;
    }
    if (response.status == 503 || response.status == 429) {
        const replay_data::FeedPageResult page = replay_data::parse_replay_page(body_text(body));
        done.status = replay_data::FeedStatus::error;
        if (page.status == replay_data::FeedStatus::error)
            done.failure = page.failure;
        else if (response.status == 503)
            done.failure.error = std::string(kReplaysUnavailable);
        return done;
    }
    const replay_data::FeedPageResult page = replay_data::parse_replay_page(body_text(body));
    done.status = page.status;
    done.page = page.page;
    done.failure = page.failure;
    return done;
}

CorePrimeShelf::Done fetch_entry_done(
    http::Client& client, const CorePrimeShelf::Ask& ask, const std::atomic<bool>* cancel
) {
    CorePrimeShelf::Done done;
    done.kind = CorePrimeShelf::AskKind::entry;
    done.id = ask.id;
    done.watch = ask.watch;
    const std::string address = replay_data::feed_entry_url(ask.origin, ask.id);
    if (address.empty()) {
        done.status = replay_data::FeedStatus::refused;
        return done;
    }
    std::vector<uint8_t> body;
    const http::Response response = fetch_get(client, address, body, kFeedBodyBytes, cancel);
    note_http(done, response, body);
    if (response.failure != http::Failure::none || response.status == 0) {
        done.network = true;
        return done;
    }
    if (response.status == 503 || response.status == 429) {
        const replay_data::FeedEntryResult entry = replay_data::parse_replay_entry(body_text(body));
        done.status = replay_data::FeedStatus::error;
        if (entry.status == replay_data::FeedStatus::error)
            done.failure = entry.failure;
        else if (response.status == 503)
            done.failure.error = std::string(kReplaysUnavailable);
        return done;
    }
    const replay_data::FeedEntryResult entry = replay_data::parse_replay_entry(body_text(body));
    done.status = entry.status;
    done.entry = entry.entry;
    done.have_entry = entry.status == replay_data::FeedStatus::entry;
    done.failure = entry.failure;
    return done;
}

std::optional<replay_data::FeedEntry> read_entry_now(
    http::Client& client,
    const CorePrimeShelf::Ask& ask,
    const std::atomic<bool>* cancel,
    bool& network
) {
    network = false;
    const std::string address = replay_data::feed_entry_url(ask.origin, ask.id);
    if (address.empty())
        return std::nullopt;
    std::vector<uint8_t> body;
    const http::Response response = fetch_get(client, address, body, kFeedBodyBytes, cancel);
    if (response.failure != http::Failure::none || response.status == 0) {
        network = true;
        return std::nullopt;
    }
    const replay_data::FeedEntryResult entry = replay_data::parse_replay_entry(body_text(body));
    if (entry.status != replay_data::FeedStatus::entry)
        return std::nullopt;
    return entry.entry;
}

struct PlacedFile {
    std::string relative{};
    fs::path file{};
    bool matches{};
};

/// A free path, or a file that is already the replay. A name that is taken by
/// other bytes, or that the folder will not hold, is skipped.
std::optional<PlacedFile> consider_name(
    const CorePrimeShelf::Ask& ask, const std::string& named, const sha256::Digest& expected
) {
    if (named.empty())
        return std::nullopt;
    const std::string relative =
        std::string(replay_data::core_prime_folder_name) + "/" + named + ".oademo";
    const std::optional<fs::path> file = recording_file(ask.recordings, relative);
    if (!file)
        return std::nullopt;
    std::error_code missing;
    const bool there = fs::exists(*file, missing);
    if (missing)
        return std::nullopt;
    if (!there) {
        PlacedFile placed;
        placed.relative = relative;
        placed.file = *file;
        return placed;
    }
    const std::optional<sha256::Digest> have = digest_of_file(*file);
    if (have && *have == expected) {
        PlacedFile placed;
        placed.relative = relative;
        placed.file = *file;
        placed.matches = true;
        return placed;
    }
    return std::nullopt;
}

/// The map's name, then that name with the recorded time, then the id, then
/// the id and a count. The count is a guard so a full folder still ends.
std::optional<PlacedFile> place_named(
    const CorePrimeShelf::Ask& ask, std::string_view title, const sha256::Digest& expected
) {
    const std::string stem = replay_data::recording_stem(title.empty() ? ask.id : title);
    const std::string stamp = recorded_stamp(ask.recorded);
    if (const std::optional<PlacedFile> placed = consider_name(ask, stem, expected))
        return placed;
    if (!stamp.empty()) {
        if (const std::optional<PlacedFile> placed =
                consider_name(ask, stem + " " + stamp, expected))
            return placed;
    }
    const std::string base = ask.id.empty() ? stem : ask.id;
    if (base != stem) {
        if (const std::optional<PlacedFile> placed = consider_name(ask, base, expected))
            return placed;
    }
    for (uint32_t copy = 2; copy <= kNameGuard; ++copy) {
        if (const std::optional<PlacedFile> placed =
                consider_name(ask, base + " " + std::to_string(copy), expected))
            return placed;
    }
    return std::nullopt;
}

/// The same names as `place_named`. The first name that is not on disk means
/// the replay was not saved: a download would have used that name. A file
/// whose digest matches is the saved replay.
enum class NameLook : uint8_t { next, missing, found };

NameLook look_saved_name(
    const CorePrimeShelf::Ask& ask,
    const std::string& named,
    const sha256::Digest& expected,
    std::string& relative
) {
    if (named.empty())
        return NameLook::next;
    const std::string path =
        std::string(replay_data::core_prime_folder_name) + "/" + named + ".oademo";
    const std::optional<fs::path> file = recording_file(ask.recordings, path);
    if (!file)
        return NameLook::next;
    std::error_code missing;
    const bool there = fs::exists(*file, missing);
    if (missing)
        return NameLook::next;
    if (!there)
        return NameLook::missing;
    const std::optional<sha256::Digest> have = digest_of_file(*file);
    if (have && *have == expected) {
        relative = path;
        return NameLook::found;
    }
    return NameLook::next;
}

std::optional<std::string> locate_saved(
    const CorePrimeShelf::Ask& ask, std::string_view title, const sha256::Digest& expected
) {
    const std::string stem = replay_data::recording_stem(title.empty() ? ask.id : title);
    const std::string stamp = recorded_stamp(ask.recorded);
    std::string relative;
    const auto take = [&](const std::string& named) {
        return look_saved_name(ask, named, expected, relative);
    };
    NameLook look = take(stem);
    if (look == NameLook::found)
        return relative;
    if (look == NameLook::missing)
        return std::nullopt;
    if (!stamp.empty()) {
        look = take(stem + " " + stamp);
        if (look == NameLook::found)
            return relative;
        if (look == NameLook::missing)
            return std::nullopt;
    }
    const std::string base = ask.id.empty() ? stem : ask.id;
    if (base != stem) {
        look = take(base);
        if (look == NameLook::found)
            return relative;
        if (look == NameLook::missing)
            return std::nullopt;
    }
    for (uint32_t copy = 2; copy <= kNameGuard; ++copy) {
        look = take(base + " " + std::to_string(copy));
        if (look == NameLook::found)
            return relative;
        if (look == NameLook::missing)
            return std::nullopt;
    }
    return std::nullopt;
}

CorePrimeShelf::Done locate_file(const CorePrimeShelf::Ask& ask) {
    CorePrimeShelf::Done done;
    done.kind = CorePrimeShelf::AskKind::locate;
    done.id = ask.id;
    const std::optional<sha256::Digest> expected = sha256::parse_hex(ask.sha);
    if (!expected) {
        done.how = CorePrimeShelf::FileHow::failed;
        return done;
    }
    const std::string title = ask.title.empty() ? ask.id : ask.title;
    if (const std::optional<std::string> relative = locate_saved(ask, title, *expected)) {
        done.how = CorePrimeShelf::FileHow::already;
        done.relative = *relative;
    } else {
        done.how = CorePrimeShelf::FileHow::failed;
    }
    return done;
}

void set_live_note(CorePrimeShelf& shelf, CorePrimeShelf::LiveNote note) {
    base::threads::LockGuard held(shelf.mutex);
    shelf.note = std::move(note);
}

void clear_live_note(CorePrimeShelf& shelf) {
    base::threads::LockGuard held(shelf.mutex);
    shelf.note = {};
}

/// The shelf is closing, or this file's wait was cancelled.
bool stopped(const CorePrimeShelf& shelf, uint64_t generation, const std::atomic<bool>* cancel) {
    if (cancel != nullptr && cancel->load())
        return true;
    return shelf.file_generation.load() != generation;
}

/// Sleeps in short slices so Cancel, or closing the Library, can stop the wait.
bool wait_out(
    const CorePrimeShelf& shelf,
    uint64_t generation,
    const std::atomic<bool>* cancel,
    uint32_t milliseconds
) {
    uint32_t left = milliseconds;
    while (left > 0) {
        if (stopped(shelf, generation, cancel))
            return false;
        const uint32_t slice = left < kWaitSliceMs ? left : kWaitSliceMs;
        base::threads::sleep_ms(slice);
        left -= slice;
    }
    return !stopped(shelf, generation, cancel);
}

void quiet_stop(CorePrimeShelf& shelf, CorePrimeShelf::Done& done) {
    clear_live_note(shelf);
    done.quiet = true;
}

/// Writes the bytes beside the final name and renames them into place, so a
/// cut write never leaves a damaged `.oademo` under the name the player sees.
bool commit_bytes(const PlacedFile& placed, const std::vector<uint8_t>& body) {
    fs::path temporary = placed.file;
    temporary += ".part";
    if (!write_bytes(temporary, body)) {
        remove_file(temporary);
        return false;
    }
    std::error_code existing;
    if (fs::exists(placed.file, existing) || existing) {
        remove_file(temporary);
        return false;
    }
    std::error_code renamed;
    fs::rename(temporary, placed.file, renamed);
    if (renamed) {
        remove_file(temporary);
        return false;
    }
    return true;
}

CorePrimeShelf::Done fetch_file(
    http::Client& client,
    CorePrimeShelf& shelf,
    CorePrimeShelf::Ask ask,
    const std::atomic<bool>* cancel
) {
    CorePrimeShelf::Done done;
    done.kind = CorePrimeShelf::AskKind::file;
    done.id = ask.id;
    done.watch = ask.watch;
    const std::optional<sha256::Digest> expected = sha256::parse_hex(ask.sha);
    if (!expected || !ask.have_api) {
        done.how = CorePrimeShelf::FileHow::failed;
        done.notice = CorePrimeShelf::FileNotice::no_file;
        return done;
    }
    sha256::Digest file_digest = *expected;
    const std::string title = ask.title.empty() ? ask.id : ask.title;
    std::optional<PlacedFile> placed = place_named(ask, title, file_digest);
    if (!placed) {
        done.how = CorePrimeShelf::FileHow::failed;
        done.notice = CorePrimeShelf::FileNotice::not_saved;
        return done;
    }
    if (placed->matches) {
        done.how = CorePrimeShelf::FileHow::already;
        done.relative = placed->relative;
        return done;
    }

    content::KeyRequest request;
    fill_request(request, ask);
    request.sha256 = file_digest;
    const int64_t began = static_cast<int64_t>(std::time(nullptr));
    bool release_reread = false;
    uint32_t rate_limits = 0;
    for (;;) {
        if (stopped(shelf, ask.generation, cancel)) {
            quiet_stop(shelf, done);
            return done;
        }
        const content::KeyAnswer answer = content::ask_for_key(client, ask.api, request, cancel);
        if (stopped(shelf, ask.generation, cancel)) {
            quiet_stop(shelf, done);
            return done;
        }
        if (answer.kind == content::KeyAnswer::Kind::browser_check) {
            clear_live_note(shelf);
            done.how = CorePrimeShelf::FileHow::failed;
            done.notice = CorePrimeShelf::FileNotice::browser;
            return done;
        }
        if (answer.kind == content::KeyAnswer::Kind::key && answer.key) {
            clear_live_note(shelf);
            const content::IssuedKey issued = *answer.key;
            std::vector<uint8_t> body;
            const http::Response response = fetch_get(
                client, url::url_text(issued.url), body, replay_data::most_file_bytes, cancel
            );
            if (stopped(shelf, ask.generation, cancel) ||
                response.failure == http::Failure::cancelled) {
                quiet_stop(shelf, done);
                return done;
            }
            const auto give_up = [&](CorePrimeShelf::FileHow how,
                                     CorePrimeShelf::FileNotice notice) {
                post_key_result(
                    client,
                    ask.api,
                    issued.download,
                    content::KeyResult::failed,
                    body.size(),
                    began,
                    cancel
                );
                done.how = how;
                done.notice = notice;
            };
            if (response.failure != http::Failure::none || response.status != 200) {
                give_up(CorePrimeShelf::FileHow::offline, CorePrimeShelf::FileNotice::offline);
                return done;
            }
            if (sha256::digest_of(body) != file_digest) {
                give_up(CorePrimeShelf::FileHow::mismatch, CorePrimeShelf::FileNotice::mismatch);
                return done;
            }
            if (!commit_bytes(*placed, body)) {
                give_up(CorePrimeShelf::FileHow::failed, CorePrimeShelf::FileNotice::not_saved);
                return done;
            }
            post_key_result(
                client,
                ask.api,
                issued.download,
                content::KeyResult::installed,
                body.size(),
                began,
                cancel
            );
            done.how = CorePrimeShelf::FileHow::saved;
            done.relative = placed->relative;
            return done;
        }
        if (answer.kind == content::KeyAnswer::Kind::challenge && answer.challenge) {
            const content::Challenge challenge = *answer.challenge;
            CorePrimeShelf::LiveNote note;
            note.kind = CorePrimeShelf::LiveNote::Kind::challenge;
            note.id = ask.id;
            note.code = challenge.code;
            note.address =
                challenge.address.empty() ? url::url_text(challenge.verify) : challenge.address;
            set_live_note(shelf, std::move(note));
            uint32_t polls = 0;
            bool solved = false;
            while (polls < kPollLimit) {
                const int64_t now = static_cast<int64_t>(std::time(nullptr));
                if (challenge.expires > 0 && now >= challenge.expires) {
                    clear_live_note(shelf);
                    done.how = CorePrimeShelf::FileHow::failed;
                    done.notice = CorePrimeShelf::FileNotice::expired;
                    return done;
                }
                uint32_t seconds = challenge.poll_seconds;
                if (seconds < 2)
                    seconds = 2;
                if (seconds > 30)
                    seconds = 30;
                if (!wait_out(shelf, ask.generation, cancel, seconds * 1000)) {
                    quiet_stop(shelf, done);
                    return done;
                }
                std::string why;
                const std::optional<content::ChallengeState> state =
                    content::poll_challenge(client, ask.api, challenge.code, cancel, &why);
                ++polls;
                if (stopped(shelf, ask.generation, cancel)) {
                    quiet_stop(shelf, done);
                    return done;
                }
                if (!state)
                    continue;
                if (*state == content::ChallengeState::solved) {
                    solved = true;
                    break;
                }
                if (*state == content::ChallengeState::expired) {
                    clear_live_note(shelf);
                    done.how = CorePrimeShelf::FileHow::failed;
                    done.notice = CorePrimeShelf::FileNotice::expired;
                    return done;
                }
            }
            if (!solved) {
                clear_live_note(shelf);
                done.how = CorePrimeShelf::FileHow::failed;
                done.notice = CorePrimeShelf::FileNotice::expired;
                return done;
            }
            clear_live_note(shelf);
            continue;
        }
        if (answer.kind == content::KeyAnswer::Kind::not_offered &&
            answer.error.error == kReleaseMismatch && !release_reread) {
            release_reread = true;
            bool network = false;
            const std::optional<replay_data::FeedEntry> again =
                read_entry_now(client, ask, cancel, network);
            if (stopped(shelf, ask.generation, cancel)) {
                quiet_stop(shelf, done);
                return done;
            }
            if (network || !again) {
                done.how =
                    network ? CorePrimeShelf::FileHow::offline : CorePrimeShelf::FileHow::failed;
                done.notice = network ? CorePrimeShelf::FileNotice::offline
                                      : CorePrimeShelf::FileNotice::server;
                done.message = answer.error.message;
                return done;
            }
            done.have_entry = true;
            done.entry = *again;
            ask.revision = again->revision;
            ask.sha = again->oademo.sha256;
            const std::optional<sha256::Digest> next = sha256::parse_hex(ask.sha);
            if (!next) {
                done.how = CorePrimeShelf::FileHow::failed;
                done.notice = CorePrimeShelf::FileNotice::no_file;
                return done;
            }
            file_digest = *next;
            request.release = ask.revision;
            request.sha256 = file_digest;
            if (!again->map_name.empty())
                ask.title = again->map_name;
            else if (!again->title.empty())
                ask.title = again->title;
            else
                ask.title = again->id;
            const std::string next_title = ask.title.empty() ? ask.id : ask.title;
            placed = place_named(ask, next_title, file_digest);
            if (!placed) {
                done.how = CorePrimeShelf::FileHow::failed;
                done.notice = CorePrimeShelf::FileNotice::not_saved;
                return done;
            }
            if (placed->matches) {
                done.how = CorePrimeShelf::FileHow::already;
                done.relative = placed->relative;
                return done;
            }
            continue;
        }
        if (answer.error.error == kReplaysUnavailable) {
            clear_live_note(shelf);
            done.how = CorePrimeShelf::FileHow::unavailable;
            done.notice = CorePrimeShelf::FileNotice::unavailable;
            return done;
        }
        if (answer.kind == content::KeyAnswer::Kind::not_offered &&
            answer.error.error == kUnknownPackage) {
            clear_live_note(shelf);
            done.how = CorePrimeShelf::FileHow::removed;
            return done;
        }
        if (answer.kind == content::KeyAnswer::Kind::not_offered &&
            answer.error.error == kReleaseMismatch) {
            clear_live_note(shelf);
            done.how = CorePrimeShelf::FileHow::failed;
            done.notice = CorePrimeShelf::FileNotice::server;
            done.message = answer.error.message;
            return done;
        }
        if (answer.kind == content::KeyAnswer::Kind::not_offered ||
            answer.kind == content::KeyAnswer::Kind::refused) {
            clear_live_note(shelf);
            done.how = CorePrimeShelf::FileHow::failed;
            done.notice = CorePrimeShelf::FileNotice::server;
            done.message = answer.error.message.empty() ? answer.detail : answer.error.message;
            return done;
        }
        if (answer.kind == content::KeyAnswer::Kind::rate_limited) {
            if (rate_limits >= kRateReasks) {
                clear_live_note(shelf);
                done.how = CorePrimeShelf::FileHow::failed;
                done.notice = answer.error.message.empty() ? CorePrimeShelf::FileNotice::offline
                                                           : CorePrimeShelf::FileNotice::server;
                done.message = answer.error.message;
                return done;
            }
            ++rate_limits;
            uint32_t seconds = answer.error.retry_after_seconds;
            if (seconds == 0)
                seconds = kRetryAfterDefaultSeconds;
            if (seconds > kRetryAfterCapSeconds)
                seconds = kRetryAfterCapSeconds;
            CorePrimeShelf::LiveNote note;
            note.kind = CorePrimeShelf::LiveNote::Kind::rate;
            note.id = ask.id;
            note.seconds = seconds;
            set_live_note(shelf, std::move(note));
            if (!wait_out(shelf, ask.generation, cancel, seconds * 1000)) {
                quiet_stop(shelf, done);
                return done;
            }
            clear_live_note(shelf);
            continue;
        }
        clear_live_note(shelf);
        done.how = CorePrimeShelf::FileHow::offline;
        done.notice = CorePrimeShelf::FileNotice::offline;
        if (!answer.error.message.empty()) {
            done.how = CorePrimeShelf::FileHow::failed;
            done.notice = CorePrimeShelf::FileNotice::server;
            done.message = answer.error.message;
        }
        return done;
    }
}

void core_prime_worker(void* argument) {
    auto& shelf = *static_cast<CorePrimeShelf*>(argument);
    http::Client client;
    for (;;) {
        CorePrimeShelf::Ask ask;
        {
            base::threads::LockGuard held(shelf.mutex);
            while (!shelf.stop && shelf.waiting.empty())
                shelf.wake.wait(shelf.mutex);
            if (shelf.stop)
                return;
            ask = std::move(shelf.waiting.front());
            shelf.waiting.erase(shelf.waiting.begin());
        }
        if (shelf.cancel.load())
            continue;
        CorePrimeShelf::Done done;
        if (ask.kind == CorePrimeShelf::AskKind::list)
            done = fetch_list(client, ask, &shelf.cancel);
        else if (ask.kind == CorePrimeShelf::AskKind::entry)
            done = fetch_entry_done(client, ask, &shelf.cancel);
        else if (ask.kind == CorePrimeShelf::AskKind::locate)
            done = locate_file(ask);
        else
            continue;
        {
            base::threads::LockGuard held(shelf.mutex);
            if (shelf.stop)
                return;
            shelf.done.push_back(std::move(done));
        }
    }
}

/// Files wait here, so a list or a card is read while a check or a 429 waits.
void core_prime_file_worker(void* argument) {
    auto& shelf = *static_cast<CorePrimeShelf*>(argument);
    http::Client client;
    for (;;) {
        CorePrimeShelf::Ask ask;
        {
            base::threads::LockGuard held(shelf.mutex);
            while (!shelf.stop && shelf.file_waiting.empty())
                shelf.file_wake.wait(shelf.mutex);
            if (shelf.stop)
                return;
            ask = std::move(shelf.file_waiting.front());
            shelf.file_waiting.erase(shelf.file_waiting.begin());
            shelf.file_running = true;
            shelf.file_running_id = ask.id;
        }
        CorePrimeShelf::Done done;
        if (shelf.cancel.load() || shelf.file_generation.load() != ask.generation) {
            done.kind = CorePrimeShelf::AskKind::file;
            done.id = ask.id;
            done.watch = ask.watch;
            done.quiet = true;
            clear_live_note(shelf);
        } else {
            done = fetch_file(client, shelf, ask, &shelf.cancel);
        }
        {
            base::threads::LockGuard held(shelf.mutex);
            shelf.file_running = false;
            if (shelf.file_running_id == ask.id)
                shelf.file_running_id.clear();
            if (shelf.stop)
                return;
            shelf.done.push_back(std::move(done));
        }
    }
}

std::string shown_title(const replay_data::FeedEntry& entry) {
    if (!entry.map_name.empty())
        return entry.map_name;
    if (!entry.title.empty())
        return entry.title;
    return entry.id;
}

library::ReplayRow row_from(const replay_data::FeedEntry& entry) {
    library::ReplayRow row;
    row.replay = entry.replay;
    row.replay.map = shown_title(entry);
    row.replay.path = entry.id;
    row.downloads = entry.downloads;
    const LocalWhen when = local_when(entry.recorded);
    row.day = when.day;
    row.minute = when.minute;
    // The list says each player's result and kills, so a Core Prime row needs
    // no index pass and shows no Reading… chip. It has no losses. No player
    // recorded it on this computer, so the row itself shows no result, and a
    // player who only watched has neither, as the index pass leaves them.
    if (entry.players.size() == row.replay.players.size()) {
        row.standings.resize(entry.players.size());
        for (std::size_t place = 0; place < entry.players.size(); ++place) {
            const replay_data::FeedPlayer& player = entry.players[place];
            if (player.side == "watcher")
                continue;
            library::PlayerStanding& standing = row.standings[place];
            if (player.result == "won")
                standing.result = library::RecordedResult::won;
            else if (player.result == "lost")
                standing.result = library::RecordedResult::lost;
            if (player.kills && *player.kills >= 0 && *player.kills <= int64_t{UINT32_MAX}) {
                standing.known = true;
                standing.kills_only = true;
                standing.kills = static_cast<uint32_t>(*player.kills);
            }
        }
    }
    return row;
}

} // namespace

void destroy_core_prime_shelf(CorePrimeShelf* shelf) noexcept {
    if (shelf == nullptr)
        return;
    bool join = false;
    bool join_file = false;
    {
        base::threads::LockGuard held(shelf->mutex);
        shelf->stop = true;
        join = shelf->started;
        join_file = shelf->file_started;
    }
    shelf->cancel.store(true);
    shelf->wake.notify_all();
    shelf->file_wake.notify_all();
    if (join)
        base::threads::join_thread(shelf->thread);
    if (join_file)
        base::threads::join_thread(shelf->file_thread);
    delete shelf;
}

CorePrimeShelf& Runtime::LibraryHost::core_prime_shelf() {
    if (!core_prime)
        core_prime.reset(new CorePrimeShelf());
    return *core_prime;
}

namespace {

bool ensure_core_worker(CorePrimeShelf& shelf) {
    base::threads::LockGuard held(shelf.mutex);
    if (!shelf.started) {
        if (!base::threads::start_thread(shelf.thread, core_prime_worker, &shelf))
            return false;
        shelf.started = true;
    }
    if (!shelf.file_started) {
        if (!base::threads::start_thread(shelf.file_thread, core_prime_file_worker, &shelf))
            return false;
        shelf.file_started = true;
    }
    return true;
}

CorePrimeShelf::Ask base_ask(Runtime& runtime) {
    CorePrimeShelf::Ask ask;
    ask.origin = feed_origin_of(runtime);
    if (const std::optional<url::Url> api = download_api_of(runtime)) {
        ask.api = *api;
        ask.have_api = true;
    }
    ask.recordings = CorePrimeGate::user_folder(runtime) / std::string(recordings_folder_name);
    ask.language = CorePrimeGate::language_tag(runtime);
    ask.install = CorePrimeGate::install(runtime, kCorePrimeId);
    return ask;
}

} // namespace

/// Names the host, which is private on Runtime, so a saved file can be watched.
struct CorePrimeHost {
    static void watch_saved_core(
        Runtime::LibraryHost& host,
        Runtime& runtime,
        const std::string& id,
        const std::string& relative
    );
    static void apply_done(Runtime::LibraryHost& host, Runtime& runtime, CorePrimeShelf::Done done);
};

namespace {

std::string file_sentence(const library::Library& model, const CorePrimeShelf::Done& done) {
    using Notice = CorePrimeShelf::FileNotice;
    switch (done.notice) {
    case Notice::no_file:
        return list_sentence(model, library::words::core_replay_no_file);
    case Notice::not_saved:
        return list_sentence(model, library::words::core_replay_not_saved);
    case Notice::mismatch:
        return list_sentence(model, library::words::core_file_differs);
    case Notice::unavailable:
        return list_sentence(model, library::words::core_replays_unavailable);
    case Notice::browser:
        return library::filled(
            model.text, content::server_check_words, {{"registry", "Core Prime"}}
        );
    case Notice::expired:
        return list_sentence(model, library::words::core_check_expired);
    case Notice::offline:
        return list_sentence(model, library::words::core_offline);
    case Notice::server:
        return done.message.empty() ? list_sentence(model, library::words::core_offline)
                                    : done.message;
    case Notice::none:
        break;
    }
    if (done.how == CorePrimeShelf::FileHow::mismatch)
        return list_sentence(model, library::words::core_file_differs);
    if (done.how == CorePrimeShelf::FileHow::unavailable)
        return list_sentence(model, library::words::core_replays_unavailable);
    if (done.how == CorePrimeShelf::FileHow::offline || done.message.empty())
        return list_sentence(model, library::words::core_offline);
    return done.message;
}

std::string live_sentence(const library::Library& model, const CorePrimeShelf::LiveNote& note) {
    if (note.kind == CorePrimeShelf::LiveNote::Kind::challenge) {
        return library::filled(
            model.text,
            library::words::core_enter_code,
            {{"address", note.address}, {"code", note.code}}
        );
    }
    if (note.kind == CorePrimeShelf::LiveNote::Kind::rate) {
        const std::string seconds = std::to_string(note.seconds);
        return library::filled(model.text, library::words::core_try_again, {{"seconds", seconds}});
    }
    return {};
}

void remember_planets(CorePrimeShelf& shelf, const std::vector<replay_data::FeedEntry>& items) {
    for (const replay_data::FeedEntry& entry : items) {
        if (entry.planet.empty())
            continue;
        bool known = false;
        for (const std::string& planet : shelf.planets) {
            if (planet == entry.planet)
                known = true;
        }
        if (!known)
            shelf.planets.push_back(entry.planet);
    }
}

void remember_saved(CorePrimeShelf& shelf, const std::string& id, const std::string& relative) {
    if (id.empty() || relative.empty())
        return;
    for (const CorePrimeShelf::SavedHere& saved : shelf.saved) {
        if (saved.id == id)
            return;
    }
    CorePrimeShelf::SavedHere saved;
    saved.id = id;
    saved.relative = relative;
    shelf.saved.push_back(std::move(saved));
}

void clear_refusal(library::Library& model, std::string_view id) {
    if (model.replays.refused_path == id)
        library::refuse_replay(model, {}, {});
}

/// A failed page waits. The rows already shown stay. An empty first page says
/// why nothing is there. A 503 stays failed, so the next frame does not ask.
void fail_list(CorePrimeShelf::Cache& cache, const CorePrimeShelf::Done& done) {
    const bool first = done.after.empty();
    const bool unavailable = done.failure.error == kReplaysUnavailable || done.http_status == 503;
    if (first && cache.items.empty()) {
        cache.have = false;
        cache.next.clear();
        cache.offline = false;
        cache.unreadable = false;
        cache.unavailable = false;
        if (done.network)
            cache.offline = true;
        else if (unavailable)
            cache.unavailable = true;
        else
            cache.unreadable = true;
    }
    const Backoff wait = backoff_for(
        cache.wait_ms,
        done.http_status,
        done.retry_after_seconds,
        unavailable || done.http_status == 429
    );
    cache.failed = true;
    cache.failed_ms = platform::tick_ms();
    cache.delay_ms = wait.delay_ms;
    cache.wait_ms = wait.next_wait_ms;
}

} // namespace

namespace {

/// The button follows the file that is running, or the next one queued.
void promote_download(CorePrimeShelf& shelf) {
    base::threads::LockGuard held(shelf.mutex);
    if (!shelf.downloading_id.empty())
        return;
    if (!shelf.file_running_id.empty()) {
        shelf.downloading_id = shelf.file_running_id;
        return;
    }
    if (!shelf.file_waiting.empty())
        shelf.downloading_id = shelf.file_waiting.front().id;
}

} // namespace

void CorePrimeHost::apply_done(
    Runtime::LibraryHost& host, Runtime& runtime, CorePrimeShelf::Done done
) {
    CorePrimeShelf& shelf = host.core_prime_shelf();
    if (done.kind == CorePrimeShelf::AskKind::list) {
        CorePrimeShelf::Cache& cache = ensure_cache(shelf, done.key);
        cache.loading = false;
        cache.next_loading = false;
        const bool page = !done.network && done.status == replay_data::FeedStatus::page;
        if (!page) {
            fail_list(cache, done);
            return;
        }
        const bool first = done.after.empty();
        // A page that arrived after the player asked for this list again waits
        // for that first page, which replaces what is held.
        if (!first && cache.refresh)
            return;
        remember_planets(shelf, done.page.items);
        merge_items(cache.items, std::move(done.page.items), first && cache.refresh);
        cache.total = done.page.total;
        cache.next = std::move(done.page.next);
        cache.have = true;
        cache.fetched_ms = platform::tick_ms();
        cache.offline = false;
        cache.unreadable = false;
        cache.unavailable = false;
        cache.failed = false;
        cache.wait_ms = kBackoffMinMs;
        cache.delay_ms = kBackoffMinMs;
        if (first)
            cache.refresh = false;
        return;
    }
    if (done.kind == CorePrimeShelf::AskKind::entry) {
        if (shelf.detail_id == done.id)
            shelf.detail_id.clear();
        if (done.network || done.status != replay_data::FeedStatus::entry) {
            if (done.status == replay_data::FeedStatus::error &&
                done.failure.error == kUnknownReplay) {
                drop_entry(shelf, done.id);
            } else {
                const Backoff wait = backoff_for(
                    shelf.detail_wait_ms,
                    done.http_status,
                    done.retry_after_seconds,
                    done.failure.error == kReplaysUnavailable || done.http_status == 503
                );
                shelf.detail_failed = done.id;
                shelf.detail_failed_ms = platform::tick_ms();
                shelf.detail_delay_ms = wait.delay_ms;
                shelf.detail_wait_ms = wait.next_wait_ms;
            }
            if (shelf.watch_after == done.id) {
                shelf.watch_after.clear();
                shelf.watch_relative.clear();
                library::refuse_replay(
                    host.model, done.id, list_sentence(host.model, library::words::core_offline)
                );
            }
            return;
        }
        store_entry(shelf, done.entry);
        if (shelf.watch_after == done.id) {
            const std::string id = done.id;
            const std::string relative = shelf.watch_relative;
            shelf.watch_after.clear();
            shelf.watch_relative.clear();
            CorePrimeHost::watch_saved_core(host, runtime, id, relative);
        }
        return;
    }
    if (done.kind == CorePrimeShelf::AskKind::locate) {
        if (shelf.locate_id == done.id)
            shelf.locate_id.clear();
        if (done.how == CorePrimeShelf::FileHow::already)
            remember_saved(shelf, done.id, done.relative);
        else if (!done.id.empty()) {
            bool known = false;
            for (const std::string& missed : shelf.locate_missed) {
                if (missed == done.id)
                    known = true;
            }
            if (!known)
                shelf.locate_missed.push_back(done.id);
        }
        return;
    }

    if (done.quiet) {
        if (shelf.downloading_id == done.id) {
            shelf.downloading_id.clear();
            shelf.watch_when_saved = false;
        }
        clear_refusal(host.model, done.id);
        promote_download(shelf);
        return;
    }
    if (done.have_entry)
        store_entry(shelf, done.entry);
    if (shelf.downloading_id == done.id) {
        shelf.downloading_id.clear();
        const bool watch = done.watch || shelf.watch_when_saved;
        shelf.watch_when_saved = false;
        done.watch = watch;
    }
    if (done.how == CorePrimeShelf::FileHow::saved ||
        done.how == CorePrimeShelf::FileHow::already) {
        remember_saved(shelf, done.id, done.relative);
        clear_refusal(host.model, done.id);
        if (done.how == CorePrimeShelf::FileHow::saved)
            host.scan_replays(runtime);
        if (done.watch)
            CorePrimeHost::watch_saved_core(host, runtime, done.id, done.relative);
        promote_download(shelf);
        return;
    }
    if (done.how == CorePrimeShelf::FileHow::removed) {
        drop_entry(shelf, done.id);
        promote_download(shelf);
        return;
    }
    library::refuse_replay(host.model, done.id, file_sentence(host.model, done));
    promote_download(shelf);
}

void CorePrimeHost::watch_saved_core(
    Runtime::LibraryHost& host, Runtime& runtime, const std::string& id, const std::string& relative
) {
    CorePrimeShelf& shelf = host.core_prime_shelf();
    const std::optional<replay_data::FeedEntry> entry = entry_copy(shelf, id);
    if (!entry) {
        library::refuse_replay(
            host.model, id, list_sentence(host.model, library::words::core_offline)
        );
        return;
    }
    if (!entry->needs_present) {
        shelf.watch_after = id;
        shelf.watch_relative = relative;
        CorePrimeShelf::Ask ask = base_ask(runtime);
        ask.kind = CorePrimeShelf::AskKind::entry;
        ask.id = id;
        ask.watch = true;
        shelf.detail_id = id;
        post_ask(shelf, std::move(ask));
        return;
    }
    const fs::path recordings =
        CorePrimeGate::user_folder(runtime) / std::string(recordings_folder_name);
    const std::optional<fs::path> file = recording_file(recordings, relative);
    if (!file) {
        library::refuse_replay(
            host.model,
            id,
            std::string(library::shown(host.model.text, replay_data::file_unreadable))
        );
        return;
    }
    replay_data::Replay replay = entry->replay;
    replay.path = id;
    replay.map = shown_title(*entry);
    const Runtime::LibraryHost::ReplayFetch fetch =
        host.queue_replay_get(runtime, relative, *file, replay, std::nullopt);
    if (fetch == Runtime::LibraryHost::ReplayFetch::watch)
        static_cast<void>(host.start_replay_watch(runtime, relative, std::nullopt));
}

void Runtime::LibraryHost::publish_core_prime(library::ReplayInputs& inputs) {
    if (!core_prime)
        return;
    const CorePrimeShelf& shelf = *core_prime;
    inputs.core_rows.clear();
    inputs.planets.clear();
    inputs.core_total = 0;
    inputs.core_loading = false;
    inputs.core_offline = false;
    inputs.core_unreadable = false;
    inputs.core_unavailable = false;
    inputs.core_notice.clear();
    inputs.core_downloading = !shelf.downloading_id.empty();
    inputs.core_download_id = shelf.downloading_id;
    inputs.core_download_waiting = shelf.download_waiting;
    inputs.core_queued.clear();
    {
        // The file worker takes the first of these when it starts one.
        base::threads::LockGuard held(core_prime->mutex);
        for (const CorePrimeShelf::Ask& ask : core_prime->file_waiting) {
            if (ask.id != shelf.downloading_id)
                inputs.core_queued.push_back(ask.id);
        }
    }
    inputs.core_latest_total.reset();
    inputs.core_longest_total.reset();
    inputs.core_downloads_total.reset();
    inputs.core_planet_total.reset();
    const auto counted = [&](std::string_view key) -> std::optional<int64_t> {
        const CorePrimeShelf::Cache* found = cache_for(shelf, key);
        if (found != nullptr && found->have)
            return found->total;
        return std::nullopt;
    };
    inputs.core_latest_total = counted("newest");
    inputs.core_longest_total = counted("longest");
    inputs.core_downloads_total = counted("downloads");
    if (!model.replays.planet.empty())
        inputs.core_planet_total = counted("newest\n" + model.replays.planet);
    inputs.planets = shelf.planets;
    std::sort(inputs.planets.begin(), inputs.planets.end());
    const CorePrimeShelf::Cache* cache = cache_for(shelf, shelf.shown_key);
    if (cache == nullptr)
        return;
    inputs.core_total = cache->total;
    inputs.core_loading = cache->loading && cache->items.empty() && !cache->offline &&
                          !cache->unreadable && !cache->unavailable;
    inputs.core_offline = cache->offline && cache->items.empty();
    inputs.core_unreadable = cache->unreadable && cache->items.empty();
    inputs.core_unavailable = cache->unavailable && cache->items.empty();
    inputs.core_rows.reserve(cache->items.size());
    for (const replay_data::FeedEntry& entry : cache->items) {
        library::ReplayRow row = row_from(entry);
        if (shelf.gaps && shelf.gaps->id == entry.id)
            row.gaps = shelf.gaps->gaps;
        for (const CorePrimeShelf::SavedHere& saved : shelf.saved) {
            if (saved.id == entry.id) {
                row.saved_here = true;
                break;
            }
        }
        inputs.core_rows.push_back(std::move(row));
    }
}

void Runtime::LibraryHost::start_core_download(
    Runtime& runtime, const std::string& id, bool watch
) {
    if (!replay_data::replay_id_ok(id))
        return;
    CorePrimeShelf& shelf = core_prime_shelf();
    if (!ensure_core_worker(shelf)) {
        shelf.worker_failed = true;
        core_log("Core Prime's replays could not be read");
        library::refuse_replay(model, id, list_sentence(model, library::words::core_offline));
        ++model_revision;
        return;
    }
    const std::optional<replay_data::FeedEntry> entry = entry_copy(shelf, id);
    if (!entry)
        return;
    CorePrimeShelf::Ask ask = base_ask(runtime);
    if (!ask.have_api) {
        library::refuse_replay(model, id, list_sentence(model, library::words::core_offline));
        ++model_revision;
        return;
    }
    ask.kind = CorePrimeShelf::AskKind::file;
    ask.id = id;
    ask.watch = watch;
    ask.revision = entry->revision;
    ask.sha = entry->oademo.sha256;
    ask.title = shown_title(*entry);
    ask.recorded = entry->recorded;
    // A replay already saved here plays from its file, so another replay's
    // check or 429 does not hold its Watch. A file that has gone is fetched again.
    if (watch) {
        for (const CorePrimeShelf::SavedHere& saved : shelf.saved) {
            if (saved.id != id)
                continue;
            const std::optional<fs::path> file = recording_file(
                CorePrimeGate::user_folder(runtime) / std::string(recordings_folder_name),
                saved.relative
            );
            std::error_code missing;
            if (file && fs::is_regular_file(*file, missing) && !missing) {
                const std::string relative = saved.relative;
                CorePrimeHost::watch_saved_core(*this, runtime, id, relative);
                ++model_revision;
                return;
            }
            break;
        }
    }
    bool queued_now = false;
    {
        base::threads::LockGuard held(shelf.mutex);
        if (shelf.file_running_id == id || shelf.downloading_id == id) {
            if (watch)
                shelf.watch_when_saved = true;
            return;
        }
        for (CorePrimeShelf::Ask& held_ask : shelf.file_waiting) {
            if (held_ask.id != id)
                continue;
            if (watch)
                held_ask.watch = true;
            return;
        }
        ask.generation = shelf.file_generation.load();
        const bool idle = shelf.downloading_id.empty() && shelf.file_running_id.empty();
        shelf.file_waiting.push_back(std::move(ask));
        if (idle) {
            shelf.downloading_id = id;
            shelf.watch_when_saved = false;
        }
        queued_now = true;
    }
    if (!queued_now)
        return;
    shelf.file_wake.notify_all();
    publish_core_prime(model.inputs.replays);
    library::update_replays(model);
    ++model_revision;
}

void Runtime::LibraryHost::cancel_core_download() {
    if (!core_prime)
        return;
    CorePrimeShelf& shelf = *core_prime;
    bool running = false;
    {
        base::threads::LockGuard held(shelf.mutex);
        const uint64_t next = shelf.file_generation.fetch_add(1) + 1;
        std::vector<CorePrimeShelf::Ask> kept;
        kept.reserve(shelf.file_waiting.size());
        for (CorePrimeShelf::Ask& ask : shelf.file_waiting) {
            if (!shelf.file_running && ask.id == shelf.downloading_id)
                continue;
            ask.generation = next;
            kept.push_back(std::move(ask));
        }
        shelf.file_waiting = std::move(kept);
        running = shelf.file_running;
        if (!running) {
            shelf.note = {};
            shelf.downloading_id.clear();
            shelf.watch_when_saved = false;
            if (!shelf.file_running_id.empty())
                shelf.downloading_id = shelf.file_running_id;
            else if (!shelf.file_waiting.empty())
                shelf.downloading_id = shelf.file_waiting.front().id;
        }
    }
    shelf.file_wake.notify_all();
    if (!running) {
        shelf.download_waiting = false;
        shelf.noted_id.clear();
        shelf.noted_sentence.clear();
        clear_refusal(model, model.inputs.replays.core_download_id);
    }
    publish_core_prime(model.inputs.replays);
    library::update_replays(model);
    ++model_revision;
}

void Runtime::LibraryHost::pump_core_prime(Runtime& runtime) {
    // A check's rows stay. Publishing the shelf would replace them, and a
    // list ask would read a feed the check did not start.
    if (replays_held)
        return;
    const bool open = CorePrimeGate::library_open(runtime);
    const bool wanted = model.replays.source == library::ReplaySource::core_prime && open;
    if (!core_prime && !wanted)
        return;
    CorePrimeShelf& shelf = core_prime_shelf();
    std::vector<CorePrimeShelf::Done> ready;
    {
        base::threads::LockGuard held(shelf.mutex);
        ready.swap(shelf.done);
    }
    bool changed = !ready.empty();
    for (CorePrimeShelf::Done& done : ready)
        CorePrimeHost::apply_done(*this, runtime, std::move(done));

    CorePrimeShelf::LiveNote note;
    {
        base::threads::LockGuard held(shelf.mutex);
        note = shelf.note;
    }
    if (note.kind == CorePrimeShelf::LiveNote::Kind::none) {
        if (!shelf.noted_id.empty() && shelf.downloading_id == shelf.noted_id) {
            clear_refusal(model, shelf.noted_id);
            changed = true;
        }
        shelf.noted_id.clear();
        shelf.noted_sentence.clear();
    } else {
        const std::string sentence = live_sentence(model, note);
        if (shelf.noted_id != note.id || shelf.noted_sentence != sentence) {
            library::refuse_replay(model, note.id, sentence);
            shelf.noted_id = note.id;
            shelf.noted_sentence = std::move(sentence);
            changed = true;
        }
    }
    const bool waiting_now = (note.kind == CorePrimeShelf::LiveNote::Kind::challenge ||
                              note.kind == CorePrimeShelf::LiveNote::Kind::rate) &&
                             note.id == shelf.downloading_id;
    if (shelf.download_waiting != waiting_now) {
        shelf.download_waiting = waiting_now;
        changed = true;
    }

    // Closing the Library stops a check or a Retry-After. Switching list does not.
    if (!open) {
        bool running = false;
        bool queued_files = false;
        {
            base::threads::LockGuard held(shelf.mutex);
            running = shelf.file_running;
            queued_files = !shelf.file_waiting.empty();
        }
        if (running || queued_files || !shelf.downloading_id.empty()) {
            {
                base::threads::LockGuard held(shelf.mutex);
                shelf.file_generation.fetch_add(1);
                shelf.file_waiting.clear();
                running = shelf.file_running;
                if (!running) {
                    shelf.note = {};
                    shelf.downloading_id.clear();
                }
            }
            shelf.file_wake.notify_all();
            if (!running) {
                shelf.watch_when_saved = false;
                shelf.download_waiting = false;
                shelf.noted_id.clear();
                shelf.noted_sentence.clear();
            }
            changed = true;
        }
    }

    const bool busy = !shelf.downloading_id.empty() || !shelf.watch_after.empty();
    if (!wanted && !busy) {
        shelf.was_wanted = false;
        if (changed) {
            publish_core_prime(model.inputs.replays);
            library::update_replays(model);
            ++model_revision;
        }
        return;
    }
    if (!ensure_core_worker(shelf)) {
        if (!shelf.worker_failed) {
            shelf.worker_failed = true;
            core_log("Core Prime's replays could not be read");
        }
        shelf.was_wanted = wanted;
        if (wanted) {
            CorePrimeShelf::Cache& cache = ensure_cache(shelf, query_key_of(model));
            shelf.shown_key = cache.key;
            if (cache.items.empty())
                cache.offline = true;
            publish_core_prime(model.inputs.replays);
            library::update_replays(model);
            ++model_revision;
        }
        return;
    }

    if (wanted) {
        const std::string key = query_key_of(model);
        const bool shown_again = shelf.shown_key != key || !shelf.was_wanted;
        shelf.shown_key = key;
        CorePrimeShelf::Cache& cache = ensure_cache(shelf, key);
        if (shown_again) {
            // A filter, the switch or opening the Library may ask at once.
            cache.refresh = true;
            cache.failed = false;
            cache.wait_ms = kBackoffMinMs;
            shelf.detail_failed.clear();
            shelf.detail_wait_ms = kBackoffMinMs;
        }
        const uint32_t now = platform::tick_ms();
        if ((!cache.have || cache.refresh) && !cache.loading && may_retry(cache, now)) {
            CorePrimeShelf::Ask ask = base_ask(runtime);
            ask.kind = CorePrimeShelf::AskKind::list;
            ask.key = key;
            cache.loading = true;
            post_ask(shelf, std::move(ask));
            changed = true;
        }
        if (cache.have && !cache.refresh && !cache.next.empty() && !cache.next_loading &&
            state.core_near_end && may_retry(cache, now)) {
            CorePrimeShelf::Ask ask = base_ask(runtime);
            ask.kind = CorePrimeShelf::AskKind::list;
            ask.key = key;
            ask.after = cache.next;
            cache.next_loading = true;
            post_ask(shelf, std::move(ask));
            changed = true;
        }
        if (!shelf.detail_failed.empty() && now - shelf.detail_failed_ms >= shelf.detail_delay_ms)
            shelf.detail_failed.clear();
        if (model.replays.selected && shelf.detail_id.empty()) {
            const std::string& selected = *model.replays.selected;
            const std::optional<replay_data::FeedEntry> entry = entry_copy(shelf, selected);
            if (entry && !entry->needs_present && shelf.detail_failed != selected &&
                shelf.watch_after != selected) {
                CorePrimeShelf::Ask ask = base_ask(runtime);
                ask.kind = CorePrimeShelf::AskKind::entry;
                ask.id = selected;
                shelf.detail_id = selected;
                post_ask(shelf, std::move(ask));
                changed = true;
            }
        }
        if (model.replays.selected && shelf.locate_id.empty()) {
            const std::string& selected = *model.replays.selected;
            bool known = false;
            for (const CorePrimeShelf::SavedHere& saved : shelf.saved) {
                if (saved.id == selected)
                    known = true;
            }
            if (!known) {
                for (const std::string& missed : shelf.locate_missed) {
                    if (missed == selected)
                        known = true;
                }
            }
            const std::optional<replay_data::FeedEntry> located = entry_copy(shelf, selected);
            if (!known && located && !located->oademo.sha256.empty() &&
                shelf.downloading_id != selected) {
                CorePrimeShelf::Ask ask = base_ask(runtime);
                ask.kind = CorePrimeShelf::AskKind::locate;
                ask.id = selected;
                ask.sha = located->oademo.sha256;
                ask.title = shown_title(*located);
                ask.recorded = located->recorded;
                shelf.locate_id = selected;
                if (!post_ask(shelf, std::move(ask)))
                    shelf.locate_id.clear();
                else
                    changed = true;
            }
        }
        if (model.replays.selected) {
            const std::optional<replay_data::FeedEntry> entry =
                entry_copy(shelf, *model.replays.selected);
            if (entry && entry->needs_present) {
                const bool empty_needs = entry->replay.needs.empty();
                uint64_t generation = 0;
                if (!empty_needs) {
                    if (const std::shared_ptr<const content::Snapshot> snapshot =
                            runtime.content_service().snapshot())
                        generation = snapshot->generation;
                }
                const std::string install = index_pass_content_stamp(runtime);
                if (!shelf.gaps || shelf.gaps->id != entry->id ||
                    shelf.gaps->generation != generation || shelf.gaps->install != install) {
                    CorePrimeShelf::Gaps gaps;
                    gaps.id = entry->id;
                    gaps.generation = generation;
                    gaps.install = install;
                    if (!empty_needs) {
                        replay_data::Replay replay = entry->replay;
                        replay.path = entry->id;
                        replay.map = shown_title(*entry);
                        gaps.gaps = replay_gaps(runtime, replay);
                    }
                    shelf.gaps = std::move(gaps);
                    changed = true;
                }
            }
        }
    }
    shelf.was_wanted = wanted;

    if (!changed)
        return;
    publish_core_prime(model.inputs.replays);
    library::update_replays(model);
    ++model_revision;
}

} // namespace oa::app

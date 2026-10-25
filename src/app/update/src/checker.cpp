// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The worker that fetches a feed and its signature, verifies them, and
// stores the verified bytes.
#include "oa/app/update/checker.hpp"

#include "oa/data/registry/trust.hpp"
#include "oa/netgame/http/client.hpp"
#include "oa/platform/files.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app::update {
namespace {

namespace http = oa::netgame::http;
namespace feed = oa::data::engine_feed;

/// One fetch: the client's outcome and the body it kept.
struct Fetched {
    http::Response response;
    std::vector<uint8_t> body;
};

/// Seconds since 1970.
///
/// @return the wall clock, as seconds
[[nodiscard]] int64_t wall_seconds() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(now).count();
}

/// Fetches one address. The client sends its own user agent and no other header.
///
/// @param client the worker's client
/// @param url the address
/// @param cancel set when the check should stop
/// @return the outcome and the body
[[nodiscard]] Fetched
fetch_url(http::Client& client, const std::string& url, const std::atomic<bool>& cancel) {
    http::Request request;
    request.url = url;
    request.timeouts.connect_ms = 10000;
    request.timeouts.idle_ms = 15000;
    request.timeouts.total_ms = 30000;
    request.limits.max_body_bytes = feed::max_feed_bytes;
    request.cancel = &cancel;
    Fetched fetched;
    fetched.response = client.fetch(request, fetched.body);
    return fetched;
}

/// True when a whole 200 response was read.
///
/// @param response the outcome
/// @return true when the body is the resource
[[nodiscard]] bool accepted_response(const http::Response& response) noexcept {
    return response.failure == http::Failure::none && response.status == 200;
}

/// The line for a fetch that was not a 200.
///
/// @param which `the feed` or `the signature`
/// @param response the outcome
/// @return a short English line
[[nodiscard]] std::string http_failure(std::string_view which, const http::Response& response) {
    if (response.status != 0 && response.status != 200)
        return std::string(which) + " answered " + std::to_string(response.status);
    if (response.failure != http::Failure::none)
        return http::failure_text(response.failure);
    return std::string(which) + " was not read";
}

/// True when the response shows the server answered.
///
/// @param response the outcome
/// @return true when an HTTP status was read
[[nodiscard]] bool server_answered(const http::Response& response) noexcept {
    return response.status != 0;
}

} // namespace

Checker::Checker(std::filesystem::path updates_folder) : folder_(std::move(updates_folder)) {
}

Checker::~Checker() {
    {
        base::threads::LockGuard guard(mutex_);
        stop_ = true;
        cancel_.store(true);
    }
    wake_.notify_all();
    base::threads::join_thread(thread_);
}

void Checker::start(const Source& source, const ThisBuild& build, bool asked) {
    (void)asked;
    Job next_job;
    next_job.source = source;
    next_job.build = build;
    base::threads::LockGuard guard(mutex_);
    if (busy_ || stop_)
        return;
    cancel_.store(false);
    job_ = std::move(next_job);
    has_job_ = true;
    busy_ = true;
    CheckSnapshot checking;
    checking.state = CheckState::checking;
    checking.generation = snapshot_.generation + 1;
    snapshot_ = std::move(checking);
    if (!thread_started_) {
        if (!base::threads::start_thread(thread_, &Checker::thread_main, this)) {
            has_job_ = false;
            busy_ = false;
            CheckSnapshot failed;
            failed.state = CheckState::failed;
            failed.failure = "the check could not start";
            failed.generation = snapshot_.generation + 1;
            snapshot_ = std::move(failed);
            return;
        }
        thread_started_ = true;
    }
    wake_.notify_one();
}

void Checker::cancel() noexcept {
    cancel_.store(true);
}

CheckSnapshot Checker::snapshot() const {
    base::threads::LockGuard guard(mutex_);
    return snapshot_;
}

void Checker::thread_main(void* argument) {
    static_cast<Checker*>(argument)->run();
}

void Checker::run() {
    http::Client client;
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
            publish_cancelled();
            continue;
        }
        try {
            run_job(client, job);
        } catch (const std::exception& failure) {
            publish_failed(failure.what(), std::nullopt);
        } catch (...) {
            publish_failed("the check failed", std::nullopt);
        }
    }
}

void Checker::run_job(http::Client& client, const Job& job) {
    if (cancel_.load()) {
        publish_cancelled();
        return;
    }
    const Fetched feed_bytes = fetch_url(client, job.source.feed_url, cancel_);
    std::optional<int64_t> reached;
    if (server_answered(feed_bytes.response))
        reached = wall_seconds();
    if (cancel_.load() || feed_bytes.response.failure == http::Failure::cancelled) {
        publish_cancelled();
        return;
    }
    if (!accepted_response(feed_bytes.response)) {
        publish_failed(http_failure("the feed", feed_bytes.response), reached);
        return;
    }

    const Fetched signature = fetch_url(client, job.source.feed_url + ".sig", cancel_);
    if (server_answered(signature.response))
        reached = wall_seconds();
    if (cancel_.load() || signature.response.failure == http::Failure::cancelled) {
        publish_cancelled();
        return;
    }
    if (!accepted_response(signature.response)) {
        publish_failed(http_failure("the signature", signature.response), reached);
        return;
    }

    std::vector<oa::data::registry::PinnedKey> pins;
    pins.reserve(job.source.keys.size());
    for (const FeedKey& key : job.source.keys) {
        oa::data::registry::PinnedKey pin;
        pin.registry = "test";
        pin.key_id = key.id;
        pin.public_key = key.text;
        pins.push_back(pin);
    }
    feed::CheckRequest request;
    request.feed = feed_bytes.body;
    request.signature = signature.body;
    if (!pins.empty())
        request.keys = pins;
    const feed::Checked checked = feed::check_feed(request);
    if (cancel_.load()) {
        publish_cancelled();
        return;
    }
    if (!checked.usable() || !checked.feed) {
        const char* const verdict = feed::verdict_text(checked.verdict);
        publish_failed(verdict != nullptr ? verdict : "the feed was refused", reached);
        return;
    }

    std::string stored;
    const bool feed_stored =
        oa::platform::replace_file(folder_ / "feed.json", feed_bytes.body, &stored);
    if (feed_stored)
        stored.clear();
    const bool signature_stored =
        feed_stored &&
        oa::platform::replace_file(folder_ / "feed.json.sig", signature.body, &stored);
    if (cancel_.load()) {
        publish_cancelled();
        return;
    }
    if (!feed_stored || !signature_stored) {
        if (stored.empty())
            stored = "the verified feed could not be stored";
        publish_failed(std::move(stored), reached);
        return;
    }

    feed::Build asking;
    asking.version = job.build.version;
    asking.release = job.build.release;
    asking.package = job.build.package;
    CheckSnapshot done;
    done.feed = checked.feed;
    done.offer = feed::choose(*checked.feed, asking);
    done.checked_at = reached;
    if (done.offer.way == feed::Way::none)
        done.state = CheckState::up_to_date;
    else
        done.state = CheckState::offered;
    publish_done(std::move(done));
}

void Checker::publish_failed(std::string failure, std::optional<int64_t> checked) {
    base::threads::LockGuard guard(mutex_);
    CheckSnapshot next;
    if (cancel_.load() || stop_) {
        next.state = CheckState::cancelled;
    } else {
        next.state = CheckState::failed;
        next.failure = std::move(failure);
        next.checked_at = checked;
    }
    next.generation = snapshot_.generation + 1;
    snapshot_ = std::move(next);
    busy_ = false;
}

void Checker::publish_cancelled() {
    base::threads::LockGuard guard(mutex_);
    CheckSnapshot next;
    next.state = CheckState::cancelled;
    next.generation = snapshot_.generation + 1;
    snapshot_ = std::move(next);
    busy_ = false;
}

void Checker::publish_done(CheckSnapshot next) {
    base::threads::LockGuard guard(mutex_);
    if (cancel_.load() || stop_) {
        next = CheckSnapshot{};
        next.state = CheckState::cancelled;
    }
    next.generation = snapshot_.generation + 1;
    snapshot_ = std::move(next);
    busy_ = false;
}

} // namespace oa::app::update

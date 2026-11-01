// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The worker that posts one report of a check, and the report's body.
#include "oa/app/update/ping.hpp"

#include "oa/formats/json.hpp"
#include "oa/formats/url.hpp"
#include "oa/netgame/http/client.hpp"

#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace oa::app::update {
namespace {

namespace http = oa::netgame::http;
namespace json = oa::formats::json;
namespace url = oa::formats::url;

/// The most bytes of an answer the worker reads. The answer is not used.
constexpr uint64_t kAnswerBytes = 4096;

} // namespace

bool build_reports(const ThisBuild& build) noexcept {
    if (build.development || build.version_text.empty())
        return false;
    for (const std::string_view platform : reported_platforms)
        if (build.package == platform)
            return true;
    return false;
}

std::string ping_body(const PingReport& report) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("version");
    writer.string(report.version);
    writer.key("platform");
    writer.string(report.platform);
    writer.key("channel");
    writer.string(report.channel);
    writer.key("install_id");
    writer.string(report.install_id);
    writer.key("trigger");
    writer.string(report.asked ? "manual" : "automatic");
    writer.end_object();
    return writer.text();
}

bool local_address(std::string_view text) {
    const std::optional<url::Url> parsed = url::parse_http_url(text);
    if (!parsed)
        return false;
    if (parsed->host == "localhost")
        return true;
    return url::is_ipv4_literal(parsed->host) && parsed->host.starts_with("127.");
}

Pinger::~Pinger() {
    {
        base::threads::LockGuard guard(mutex_);
        stop_ = true;
        cancel_.store(true);
    }
    wake_.notify_all();
    base::threads::join_thread(thread_);
}

bool Pinger::send(std::string url, const PingReport& report) {
    std::string body = ping_body(report);
    base::threads::LockGuard guard(mutex_);
    if (busy_ || stop_)
        return false;
    cancel_.store(false);
    url_ = std::move(url);
    body_ = std::move(body);
    has_job_ = true;
    busy_ = true;
    PingSnapshot sending;
    sending.state = PingState::sending;
    sending.generation = snapshot_.generation + 1;
    snapshot_ = std::move(sending);
    if (!thread_started_) {
        if (!base::threads::start_thread(thread_, &Pinger::thread_main, this)) {
            has_job_ = false;
            busy_ = false;
            PingSnapshot failed;
            failed.state = PingState::failed;
            failed.failure = "the report could not start";
            failed.generation = snapshot_.generation + 1;
            snapshot_ = std::move(failed);
            return false;
        }
        thread_started_ = true;
    }
    wake_.notify_one();
    return true;
}

PingSnapshot Pinger::snapshot() const {
    base::threads::LockGuard guard(mutex_);
    return snapshot_;
}

void Pinger::thread_main(void* argument) {
    static_cast<Pinger*>(argument)->run();
}

void Pinger::run() {
    http::Client client;
    for (;;) {
        http::Request request;
        {
            base::threads::LockGuard guard(mutex_);
            while (!stop_ && !has_job_)
                wake_.wait(mutex_);
            if (stop_)
                return;
            request.url = std::move(url_);
            const std::string body = std::move(body_);
            request.body.assign(body.begin(), body.end());
            has_job_ = false;
        }
        request.method = http::Method::post;
        request.content_type = "application/json";
        request.timeouts.connect_ms = 10000;
        request.timeouts.idle_ms = 10000;
        request.timeouts.total_ms = 15000;
        request.limits.max_body_bytes = kAnswerBytes;
        request.cancel = &cancel_;
        PingSnapshot done;
        try {
            std::vector<uint8_t> answer;
            const http::Response response = client.fetch(request, answer);
            done.status = response.status;
            if (response.failure == http::Failure::none && response.status >= 200 &&
                response.status < 300) {
                done.state = PingState::sent;
            } else {
                done.state = PingState::failed;
                done.failure = response.status != 0
                                   ? "the server answered " + std::to_string(response.status)
                                   : std::string(http::failure_text(response.failure));
            }
        } catch (const std::exception& failure) {
            done.state = PingState::failed;
            done.failure = failure.what();
        }
        base::threads::LockGuard guard(mutex_);
        done.generation = snapshot_.generation + 1;
        snapshot_ = std::move(done);
        busy_ = false;
    }
}

} // namespace oa::app::update

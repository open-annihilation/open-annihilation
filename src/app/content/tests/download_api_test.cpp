// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The download API's bodies and readers, the checks meant for a web
// browser and the log text of an answer, and what a failed download tells
// the player, with no network.
#include "oa/app/content/download_api.hpp"

#include "oa/base/sha256.hpp"
#include "oa/data/catalogue/catalogue.hpp"
#include "oa/netgame/http/client.hpp"
#include "oa/test/check.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace {

namespace content = oa::app::content;
namespace sha = oa::base::sha256;

/// Whether a version is three dot-separated numbers, the form of the
/// engine's project version, so the check holds through every release.
///
/// @param version the version
bool release_version(std::string_view version) {
    for (int part = 1;; ++part) {
        const std::size_t dot = version.find('.');
        const std::string_view number = version.substr(0, dot);
        if (number.empty() || number.find_first_not_of("0123456789") != std::string_view::npos)
            return false;
        if (dot == std::string_view::npos)
            return part == 3;
        version.remove_prefix(dot + 1);
    }
}

/// The bytes of a text, for a reader.
std::span<const uint8_t> bytes_of(std::string_view text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

/// A digest whose hex is 64 `a`s, so the body is obvious.
sha::Digest digest_a() {
    const auto parsed = sha::parse_hex(std::string(64, 'a'));
    OA_CHECK(parsed.has_value());
    return parsed ? *parsed : sha::Digest{};
}

/// One key request, with an install ID when `install` is not empty.
content::KeyRequest request_for(std::string_view install) {
    content::KeyRequest request;
    if (!install.empty())
        request.install = std::string(install);
    request.package = "ridge";
    request.kind = oa::data::catalogue::Kind::oamod;
    request.release = 28;
    request.sha256 = digest_a();
    request.engine = "0.8.0";
    request.platform = std::string(content::platform_name());
    request.arch = std::string(content::arch_name());
    request.language = "de";
    request.reason = content::DownloadReason::update;
    return request;
}

void test_key_body() {
    const std::string with_id =
        content::key_request_body(request_for("7f3a90d2-4c1b-4e8a-9d07-c91e00000001"));
    const std::string wanted =
        std::string(
            "{\"install\":\"7f3a90d2-4c1b-4e8a-9d07-c91e00000001\",\"package\":\"ridge\","
            "\"kind\":\"oamod\",\"release\":28,\"sha256\":\""
        ) +
        std::string(64, 'a') + "\",\"engine\":\"0.8.0\",\"platform\":\"" +
        std::string(content::platform_name()) + "\",\"arch\":\"" +
        std::string(content::arch_name()) + "\",\"language\":\"de\",\"reason\":\"update\"}";
    if (with_id != wanted)
        std::fprintf(stderr, "body:\n%s\nwanted:\n%s\n", with_id.c_str(), wanted.c_str());
    OA_CHECK(with_id == wanted);

    const std::string without = content::key_request_body(request_for(""));
    const std::string wanted_without =
        std::string("{\"package\":\"ridge\",\"kind\":\"oamod\",\"release\":28,\"sha256\":\"") +
        std::string(64, 'a') + "\",\"engine\":\"0.8.0\",\"platform\":\"" +
        std::string(content::platform_name()) + "\",\"arch\":\"" +
        std::string(content::arch_name()) + "\",\"language\":\"de\",\"reason\":\"update\"}";
    OA_CHECK(without == wanted_without);
    OA_CHECK(without.find("install") == std::string::npos);
}

void test_issued_key() {
    std::string why = "untouched";
    const auto key = content::read_issued_key(
        bytes_of(
            "{\"download\":\"ridge-dl-1\",\"expires\":\"2026-02-01T00:00:00Z\","
            "\"url\":\"http://downloads.example.net/p/ridge.oamod\"}"
        ),
        &why
    );
    OA_CHECK(key.has_value());
    if (!key)
        return;
    OA_CHECK(key->download == "ridge-dl-1");
    const auto when = oa::data::catalogue::parse_utc_time("2026-02-01T00:00:00Z");
    OA_CHECK(when.has_value());
    OA_CHECK(key->expires == *when);
    OA_CHECK(key->url.host == "downloads.example.net");

    OA_CHECK(!content::read_issued_key(
        bytes_of(
            "{\"expires\":\"2026-02-01T00:00:00Z\","
            "\"url\":\"http://downloads.example.net/p\"}"
        ),
        &why
    ));
    OA_CHECK(!content::read_issued_key(
        bytes_of(
            "{\"download\":\"has space\",\"expires\":\"2026-02-01T00:00:00Z\","
            "\"url\":\"http://downloads.example.net/p\"}"
        ),
        &why
    ));
    const auto secure = content::read_issued_key(
        bytes_of(
            "{\"download\":\"ridge-dl-1\",\"expires\":\"2026-02-01T00:00:00Z\","
            "\"url\":\"https://downloads.example.net/p\"}"
        ),
        &why
    );
    OA_CHECK(secure.has_value());
    OA_CHECK(secure && secure->url.scheme == oa::formats::url::Scheme::https);
    OA_CHECK(!content::read_issued_key(
        bytes_of(
            "{\"download\":\"ridge-dl-1\",\"expires\":\"tomorrow\","
            "\"url\":\"http://downloads.example.net/p\"}"
        ),
        &why
    ));
    OA_CHECK(!content::read_issued_key(bytes_of("{"), &why));
}

void test_challenge() {
    std::string why;
    const auto challenge = content::read_challenge(
        bytes_of(
            "{\"error\":\"challenge_required\",\"message\":\"confirm you are a person\","
            "\"challenge\":{\"code\":\"HXQ7-4KP2\","
            "\"verify\":\"https://downloads.example.net/verify?c=HXQ7-4KP2\","
            "\"expires\":\"2026-02-01T00:00:00Z\",\"poll\":3}}"
        ),
        &why
    );
    OA_CHECK(challenge.has_value());
    if (!challenge)
        return;
    OA_CHECK(challenge->code == "HXQ7-4KP2");
    OA_CHECK(challenge->address == "downloads.example.net/verify");
    OA_CHECK(challenge->poll_seconds == 3);
    OA_CHECK(challenge->verify.scheme == oa::formats::url::Scheme::https);

    const auto low = content::read_challenge(
        bytes_of(
            "{\"challenge\":{\"code\":\"HXQ7-4KP2\","
            "\"verify\":\"https://downloads.example.net/verify?c=HXQ7-4KP2\","
            "\"expires\":\"2026-02-01T00:00:00Z\",\"poll\":1}}"
        ),
        &why
    );
    OA_CHECK(low.has_value());
    if (low)
        OA_CHECK(low->poll_seconds == 2);
    const auto high = content::read_challenge(
        bytes_of(
            "{\"challenge\":{\"code\":\"HXQ7-4KP2\","
            "\"verify\":\"https://downloads.example.net/verify?c=HXQ7-4KP2\","
            "\"expires\":\"2026-02-01T00:00:00Z\",\"poll\":99}}"
        ),
        &why
    );
    OA_CHECK(high.has_value());
    if (high)
        OA_CHECK(high->poll_seconds == 30);
    const auto absent = content::read_challenge(
        bytes_of(
            "{\"challenge\":{\"code\":\"HXQ7-4KP2\","
            "\"verify\":\"https://downloads.example.net/verify?c=HXQ7-4KP2\","
            "\"expires\":\"2026-02-01T00:00:00Z\"}}"
        ),
        &why
    );
    OA_CHECK(absent.has_value());
    if (absent)
        OA_CHECK(absent->poll_seconds == 3);

    OA_CHECK(!content::read_challenge(bytes_of("{\"error\":\"challenge_required\"}"), &why));
    OA_CHECK(!content::read_challenge(
        bytes_of(
            "{\"challenge\":{\"code\":\"bad code\","
            "\"verify\":\"https://downloads.example.net/verify\","
            "\"expires\":\"2026-02-01T00:00:00Z\"}}"
        ),
        &why
    ));
    OA_CHECK(!content::read_challenge(
        bytes_of(
            "{\"challenge\":{\"code\":\"HXQ7-4KP2\","
            "\"verify\":\"https://downloads.example.net/verify\","
            "\"expires\":\"not-a-time\"}}"
        ),
        &why
    ));
}

void test_challenge_state() {
    std::string why;
    const auto pending = content::read_challenge_state(
        bytes_of(
            "{\"code\":\"HXQ7-4KP2\",\"state\":\"pending\",\"expires\":\"2026-02-01T00:00:00Z\"}"
        ),
        &why
    );
    OA_CHECK(pending.has_value());
    if (pending)
        OA_CHECK(*pending == content::ChallengeState::pending);
    const auto solved = content::read_challenge_state(
        bytes_of(
            "{\"code\":\"HXQ7-4KP2\",\"state\":\"solved\",\"expires\":\"2026-02-01T00:00:00Z\"}"
        ),
        &why
    );
    OA_CHECK(solved.has_value());
    if (solved)
        OA_CHECK(*solved == content::ChallengeState::solved);
    const auto expired = content::read_challenge_state(
        bytes_of(
            "{\"code\":\"HXQ7-4KP2\",\"state\":\"expired\",\"expires\":\"2026-02-01T00:00:00Z\"}"
        ),
        &why
    );
    OA_CHECK(expired.has_value());
    if (expired)
        OA_CHECK(*expired == content::ChallengeState::expired);
    OA_CHECK(!content::read_challenge_state(
        bytes_of("{\"code\":\"HXQ7-4KP2\",\"expires\":\"2026-02-01T00:00:00Z\"}"), &why
    ));
    OA_CHECK(!content::read_challenge_state(
        bytes_of(
            "{\"code\":\"HXQ7-4KP2\",\"state\":\"later\",\"expires\":\"2026-02-01T00:00:00Z\"}"
        ),
        &why
    ));
    OA_CHECK(!content::read_challenge_state(bytes_of("{"), &why));
}

void test_errors() {
    const std::string retry = "12";
    const content::ApiError known = content::read_api_error(
        400,
        bytes_of(
            "{\"error\":\"bad_request\",\"message\":\"the package is missing\","
            "\"field\":\"package\"}"
        ),
        nullptr
    );
    OA_CHECK(known.status == 400);
    OA_CHECK(known.error == "bad_request");
    OA_CHECK(known.message == "the package is missing");
    OA_CHECK(known.field == "package");
    OA_CHECK(known.retry_after_seconds == 0);

    const content::ApiError limited = content::read_api_error(
        429, bytes_of("{\"error\":\"rate_limited\",\"message\":\"wait\"}"), &retry
    );
    OA_CHECK(limited.retry_after_seconds == 12);
    const std::string huge = "9999";
    OA_CHECK(content::read_api_error(429, bytes_of("{"), &huge).retry_after_seconds == 300);
    const std::string words = "soon";
    OA_CHECK(content::read_api_error(429, bytes_of("not json"), &words).retry_after_seconds == 0);
    const content::ApiError bare = content::read_api_error(503, bytes_of(""), nullptr);
    OA_CHECK(bare.status == 503);
    OA_CHECK(bare.error.empty());
    OA_CHECK(bare.message.empty());
}

void test_results_and_names() {
    OA_CHECK(
        content::result_body(content::KeyResult::installed, 43011223, 38) ==
        "{\"result\":\"installed\",\"bytes\":43011223,\"seconds\":38}"
    );
    OA_CHECK(
        content::result_body(content::KeyResult::refused, 0, 1) ==
        "{\"result\":\"refused\",\"bytes\":0,\"seconds\":1}"
    );
    OA_CHECK(
        content::result_body(content::KeyResult::failed, 12, 0) ==
        "{\"result\":\"failed\",\"bytes\":12,\"seconds\":0}"
    );
    OA_CHECK(
        content::result_body(content::KeyResult::cancelled, 3, 2) ==
        "{\"result\":\"cancelled\",\"bytes\":3,\"seconds\":2}"
    );
    OA_CHECK(content::reason_text(content::DownloadReason::install) == "install");
    OA_CHECK(content::reason_text(content::DownloadReason::update) == "update");
    OA_CHECK(content::reason_text(content::DownloadReason::repair) == "repair");
    OA_CHECK(content::reason_text(content::DownloadReason::offered_in_lobby) == "offered-in-lobby");
    OA_CHECK(content::reason_text(content::DownloadReason::mirror) == "mirror");

#if defined(_WIN32)
    OA_CHECK(content::platform_name() == "windows");
#elif defined(__APPLE__)
#if TARGET_OS_IPHONE
    OA_CHECK(content::platform_name() == "ios");
#else
    OA_CHECK(content::platform_name() == "macos");
#endif
#elif defined(__ANDROID__)
    OA_CHECK(content::platform_name() == "android");
#elif defined(__linux__)
    OA_CHECK(content::platform_name() == "linux");
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
    OA_CHECK(content::arch_name() == "arm64");
#elif defined(__x86_64__) || defined(_M_X64)
    OA_CHECK(content::arch_name() == "x64");
#elif defined(__i386__) || defined(_M_IX86)
    OA_CHECK(content::arch_name() == "x86");
#elif defined(__arm__) || defined(_M_ARM)
    OA_CHECK(content::arch_name() == "arm");
#endif
    OA_CHECK(!content::platform_name().empty());
    OA_CHECK(!content::arch_name().empty());
    OA_CHECK(release_version(content::engine_version()));
}

/// An answer's head, as the client reads it.
///
/// @param status the HTTP status
/// @param headers the headers, as the server sent them
oa::netgame::http::Response head_of(int status, std::vector<oa::netgame::http::Header> headers) {
    oa::netgame::http::Response response;
    response.status = status;
    response.headers = std::move(headers);
    return response;
}

void test_browser_checks() {
    // The firewall's captcha (405) and challenge (202), whatever the case of
    // the header's name and value.
    OA_CHECK(
        content::browser_check(head_of(
            405, {{"Content-Type", "text/html; charset=UTF-8"}, {"x-amzn-waf-action", "captcha"}}
        ))
    );
    OA_CHECK(content::browser_check(head_of(202, {{"X-Amzn-Waf-Action", "Challenge"}})));
    OA_CHECK(content::browser_check(head_of(403, {{"cf-mitigated", "challenge"}})));
    // A web page where a package or JSON was expected.
    OA_CHECK(content::browser_check(head_of(200, {{"Content-Type", "TEXT/HTML"}})));
    OA_CHECK(content::browser_check(head_of(403, {{"Content-Type", "application/xhtml+xml"}})));
    // A server's error page, a refused signed address, JSON, a package and no answer are not.
    OA_CHECK(!content::browser_check(head_of(502, {{"Content-Type", "text/html"}})));
    OA_CHECK(!content::browser_check(head_of(403, {{"Content-Type", "text/xml"}})));
    OA_CHECK(!content::browser_check(head_of(403, {{"Content-Type", "application/json"}})));
    OA_CHECK(!content::browser_check(head_of(200, {{"Content-Type", "application/octet-stream"}})));
    OA_CHECK(!content::browser_check(head_of(200, {{"x-amzn-waf-action", ""}})));
    OA_CHECK(!content::browser_check(head_of(0, {{"x-amzn-waf-action", "captcha"}})));
}

void test_answered_text() {
    namespace http = oa::netgame::http;
    OA_CHECK(
        content::address_without_query(
            "http://downloads.example/v1/p/a.oamap?Expires=1&Signature=s&Key-Pair-Id=k"
        ) == "http://downloads.example/v1/p/a.oamap"
    );
    OA_CHECK(
        content::address_without_query("http://h/v1/p/a.oamap#part") == "http://h/v1/p/a.oamap"
    );
    OA_CHECK(content::address_without_query("http://h/v1/p/a.oamap") == "http://h/v1/p/a.oamap");

    http::Response captcha = head_of(
        405, {{"Content-Type", "text/html; charset=UTF-8"}, {"x-amzn-waf-action", "captcha"}}
    );
    captcha.content_length = 2134;
    captcha.body_bytes = 2134;
    const content::Answered seen =
        content::answered("http://h/v1/p/a.oamap?Signature=secret&Key-Pair-Id=k", captcha);
    OA_CHECK(seen.address == "http://h/v1/p/a.oamap");
    OA_CHECK(seen.browser_check);
    const std::string text = content::answered_text(seen);
    const std::string wanted = "http://h/v1/p/a.oamap answered 405, text/html; charset=UTF-8, "
                               "2134 bytes, a check meant for a web browser "
                               "(x-amzn-waf-action: captcha)";
    if (text != wanted)
        std::fprintf(stderr, "answered %s\nwanted   %s\n", text.c_str(), wanted.c_str());
    OA_CHECK(text == wanted);
    OA_CHECK(text.find("secret") == std::string::npos);

    // What Core Prime sends for a package address with no signature.
    http::Response refused = head_of(403, {{"Content-Type", "text/xml"}});
    refused.content_length = 146;
    OA_CHECK(
        content::answered_text(content::answered("http://h/v1/p/b.oamap", refused)) ==
        "http://h/v1/p/b.oamap answered 403, text/xml, 146 bytes"
    );

    // A body cut short, with no length sent.
    http::Response cut = head_of(200, {});
    cut.body_bytes = 10;
    cut.failure = http::Failure::connection_lost;
    cut.detail = "the connection closed early";
    OA_CHECK(
        content::answered_text(content::answered("http://h/p", cut)) ==
        "http://h/p answered 200, no content type, 10 bytes read; the connection closed early"
    );

    // No answer at all.
    http::Response lost;
    lost.failure = http::Failure::cannot_connect;
    OA_CHECK(
        content::answered_text(content::answered("http://h/api/v1/downloads", lost)) ==
        "http://h/api/v1/downloads: the connection could not be opened"
    );

    // A header value fit for one log line.
    http::Response odd =
        head_of(200, {{"Content-Type", std::string("text/plain\x01") + std::string(300, 'x')}});
    const content::Answered kept = content::answered("http://h/p", odd);
    OA_CHECK(kept.content_type.size() == 120);
    OA_CHECK(kept.content_type.find('\x01') == std::string::npos);
}

/// Shows every interface word in brackets, as a stand-in translation.
std::string_view bracketed(std::string_view english) {
    static std::string shown;
    shown = "[" + std::string(english) + "]";
    return shown;
}

void test_failure_messages() {
    content::DownloadView view;
    view.state = content::DownloadState::failed;
    view.target.registry = "coreprime";
    view.target.registry_name = "Core Prime";

    view.failure = content::DownloadFailure::server_check;
    OA_CHECK(
        content::failure_message(view, nullptr) ==
        "Core Prime asked for a check the game cannot do. Try again later."
    );
    OA_CHECK(
        content::failure_message(view, bracketed) ==
        "[Core Prime asked for a check the game cannot do. Try again later.]"
    );
    view.target.registry_name.clear();
    OA_CHECK(
        content::failure_message(view, nullptr) ==
        "Coreprime asked for a check the game cannot do. Try again later."
    );

    // The installer's reason stands alone.
    view.failure = content::DownloadFailure::install_refused;
    view.detail = "The folder cavedog-evad-river-delta does not hold this map pack.";
    OA_CHECK(content::failure_message(view, bracketed) == view.detail);
    OA_CHECK(content::failure_reason(view, bracketed) == view.detail);
    // A reason the installer wrote with a small letter keeps it after a colon.
    view.detail = "needs Open Annihilation >= 9.0.0; this is Open Annihilation 0.8.7";
    OA_CHECK(content::failure_reason(view, nullptr) == view.detail);
    OA_CHECK(
        content::failure_message(view, nullptr) ==
        "Needs Open Annihilation >= 9.0.0; this is Open Annihilation 0.8.7"
    );
    view.failure = content::DownloadFailure::install_failed;
    OA_CHECK(content::failure_reason(view, nullptr) == view.detail);
    view.failure = content::DownloadFailure::install_refused;
    view.detail.clear();
    OA_CHECK(content::failure_message(view, nullptr) == "The package was refused");
    OA_CHECK(content::failure_message(view, bracketed) == "[the package was refused]");
    OA_CHECK(content::failure_reason(view, nullptr) == "the package was refused");
    OA_CHECK(content::failure_reason(view, bracketed) == "[the package was refused]");

    // The registry's message follows the failure's text.
    view.failure = content::DownloadFailure::refused;
    view.detail = "no";
    OA_CHECK(content::failure_message(view, nullptr) == "The registry refused the download (no)");
    OA_CHECK(content::failure_reason(view, nullptr) == "the registry refused the download (no)");

    // Whatever the detail holds, no failure's text is named twice.
    for (int value = 1; value <= static_cast<int>(content::DownloadFailure::server_check);
         ++value) {
        view.failure = static_cast<content::DownloadFailure>(value);
        view.detail = content::failure_text(view.failure);
        const std::string message = content::failure_message(view, nullptr);
        const std::string reason = content::failure_text(view.failure);
        std::string lower = message;
        if (!lower.empty() && lower[0] >= 'A' && lower[0] <= 'Z')
            lower[0] = static_cast<char>(lower[0] - 'A' + 'a');
        const std::size_t first = lower.find(reason);
        const bool once = first == std::string::npos ||
                          lower.find(reason, first + reason.size()) == std::string::npos;
        if (!once)
            std::fprintf(stderr, "repeated: %s\n", message.c_str());
        OA_CHECK(once);
        OA_CHECK(message.find(" (") == std::string::npos);
    }

    view.failure = content::DownloadFailure::none;
    view.detail.clear();
    OA_CHECK(content::failure_message(view, nullptr).empty());
}

} // namespace

int main() {
    test_key_body();
    test_issued_key();
    test_challenge();
    test_challenge_state();
    test_errors();
    test_results_and_names();
    test_browser_checks();
    test_answered_text();
    test_failure_messages();
    return oa::test::check_exit_status();
}

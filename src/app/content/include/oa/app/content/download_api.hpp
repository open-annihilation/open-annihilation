// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One exchange with a registry's download API. The queue decides when to
// retry, wait, ask again or try a mirror. Each function here makes one
// request, or none.
#pragma once

#include "oa/app/content/downloads.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/catalogue/catalogue.hpp"
#include "oa/formats/url.hpp"
#include "oa/netgame/http/client.hpp"

#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace oa::app::content {

/// What a key request tells the registry.
struct KeyRequest {
    std::optional<std::string> install; ///< omitted when the registry does not use one
    std::string package;                ///< the package key
    data::catalogue::Kind kind = data::catalogue::Kind::oamod;
    /// When set, the JSON `kind` is this text. Empty writes `kind`'s catalogue
    /// name. A replay file uses `oademo`, which is not a catalogue kind.
    std::string kind_text{};
    int64_t release = 0;           ///< the catalogue release
    base::sha256::Digest sha256{}; ///< the catalogue's SHA-256
    std::string engine;            ///< this build's version
    std::string platform;          ///< platform_name
    std::string arch;              ///< arch_name
    std::string language;          ///< the tag of the language the game shows
    DownloadReason reason = DownloadReason::install;
};

/// Writes a key request in the order the API expects.
///
/// `install` is left out when the request has none. The text is compact JSON.
///
/// @param request the request
/// @return the body
[[nodiscard]] std::string key_request_body(const KeyRequest& request);

/// A key the registry issued.
struct IssuedKey {
    std::string download;  ///< 1 to 128 characters of letters, digits, `_` and `-`
    int64_t expires = 0;   ///< when the address expires, seconds since 1970
    formats::url::Url url; ///< the package, an http or https address
};

/// A check the registry asked for before it will issue a key.
struct Challenge {
    std::string code;          ///< the code the player enters
    formats::url::Url verify;  ///< the page, http or https
    std::string address;       ///< the page without its scheme and query
    int64_t expires = 0;       ///< when the check expires, seconds since 1970
    uint32_t poll_seconds = 3; ///< how often to ask, kept from 2 to 30
};

/// Where a check stands.
enum class ChallengeState : uint8_t {
    pending, ///< not passed yet
    solved,  ///< passed; ask for a key again
    expired, ///< the check ended
};

/// An error body, and the wait a 429 named.
struct ApiError {
    int status = 0;                   ///< the HTTP status
    std::string error;                ///< the error code; empty when the body did not read
    std::string message;              ///< the message; empty when the body did not read
    std::string field;                ///< the field a client fault names; empty when absent
    uint32_t retry_after_seconds = 0; ///< the Retry-After value, at most 300; 0 when absent
};

/// Reads a 201 body.
///
/// The download id is 1 to 128 characters of `A-Z`, `a-z`, `0-9`, `_` and `-`.
/// The address is http or https. The expiry is a UTC time.
///
/// @param body the response body
/// @param[out] why why it was refused; may be null
/// @return the key, or nothing when the body is malformed
[[nodiscard]] std::optional<IssuedKey>
read_issued_key(std::span<const uint8_t> body, std::string* why);

/// Reads a 428 body.
///
/// `poll` is 3 when it is absent. A value outside 2 to 30 is kept inside that
/// range. The page may be http or https.
///
/// @param body the response body
/// @param[out] why why it was refused; may be null
/// @return the check, or nothing when the body is malformed
[[nodiscard]] std::optional<Challenge>
read_challenge(std::span<const uint8_t> body, std::string* why);

/// Reads a challenge poll body.
///
/// `state` is `pending`, `solved` or `expired`.
///
/// @param body the response body
/// @param[out] why why it was refused; may be null
/// @return the state, or nothing when the body is malformed
[[nodiscard]] std::optional<ChallengeState>
read_challenge_state(std::span<const uint8_t> body, std::string* why);

/// Reads an error body.
///
/// A body that is not the error object leaves the codes empty. Retry-After is
/// read as a whole number of seconds and kept at at most 300. An absent or
/// unreadable header leaves the wait at 0.
///
/// @param status the HTTP status
/// @param body the response body
/// @param retry_after_header the Retry-After value, or null when the response has none
/// @return the error, with empty codes when the body did not read
[[nodiscard]] ApiError
read_api_error(int status, std::span<const uint8_t> body, const std::string* retry_after_header);

/// How one download ended, as the result call names it.
enum class KeyResult : uint8_t {
    installed, ///< the installer installed it
    refused,   ///< the installer refused it
    failed,    ///< the download or the install failed
    cancelled, ///< the player cancelled, or set the question aside
};

/// Writes a result body.
///
/// @param result how the download ended
/// @param bytes the bytes fetched for the item
/// @param seconds whole seconds from the item's first key
/// @return the body
[[nodiscard]] std::string result_body(KeyResult result, uint64_t bytes, int64_t seconds);

/// What one answer from a server was, for a log line.
///
/// The address keeps nothing of its query or fragment, where a signed
/// package address carries its key.
struct Answered {
    std::string address;            ///< the address asked, without its query or fragment
    int status = 0;                 ///< the HTTP status; 0 when no answer was read
    std::string content_type;       ///< the Content-Type the answer sent, as a log keeps it
    std::optional<uint64_t> length; ///< the Content-Length the answer sent
    uint64_t body_bytes = 0;        ///< the body bytes read
    std::string failure;        ///< why the fetch stopped early, in English; empty when it ended
    bool browser_check = false; ///< the answer is a check meant for a web browser
    std::string check_header;   ///< the firewall header that names the check, as "name: value"
};

/// Tells whether an answer is a check meant for a web browser, which the
/// game never answers.
///
/// The firewall in front of a server names such a check with a header of
/// its own: x-amzn-waf-action (a captcha or a challenge), or cf-mitigated
/// set to challenge. An HTML page (text/html or application/xhtml+xml)
/// where a package or the API's JSON was expected is one too, unless its
/// status is a server error (5xx), which is an error page.
///
/// @param response the answer's head
/// @return true for a check meant for a web browser
[[nodiscard]] bool browser_check(const netgame::http::Response& response);

/// Drops an address's query and fragment, which may hold a key.
///
/// @param address the address
/// @return the address up to its first `?` or `#`
[[nodiscard]] std::string address_without_query(std::string_view address);

/// Records an answer for a log line.
///
/// A fetch the caller's sink stopped records no failure: the sink chose it.
/// Nor does a check meant for a web browser, whose page is never read whole.
///
/// @param address the address asked; its query and fragment are dropped
/// @param response the outcome of the fetch
/// @return the answer
[[nodiscard]] Answered answered(std::string_view address, const netgame::http::Response& response);

/// Writes an answer for a log line.
///
/// "http://host/v1/p/name.oamap answered 405, text/html, 2134 bytes, a
/// check meant for a web browser (x-amzn-waf-action: captcha)", or, when no
/// answer was read, the address and why: "http://host/api/v1/downloads: the
/// connection could not be opened".
///
/// @param answer the answer
/// @return the text; never empty
[[nodiscard]] std::string answered_text(const Answered& answer);

/// What one key request came back as.
struct KeyAnswer {
    /// Which kind of answer it was.
    enum class Kind : uint8_t {
        key,           ///< a key was issued
        challenge,     ///< the registry asked for a check
        refused,       ///< a client fault or a refusal; do not try a mirror
        not_offered,   ///< the registry no longer offers this release
        rate_limited,  ///< wait and ask again
        retry,         ///< a failure that may be tried again, then a mirror
        browser_check, ///< a check meant for a web browser; the game gives up for now
    } kind = Kind::retry;

    std::optional<IssuedKey> key;       ///< set for a key
    std::optional<Challenge> challenge; ///< set for a check
    ApiError error;                     ///< the error body, when there was one
    std::string detail;                 ///< why, for a log
    Answered answered;                  ///< what the server sent, for a log
};

/// Asks a registry for one key.
///
/// One request. A check meant for a web browser (browser_check) is that,
/// whatever its status. 201 is a key, 428 a check, 404 and 409 not offered,
/// 429 rate limited, and any other 4xx refused. Anything else, a 201 or 428
/// that does not read, and a network failure are retry. The cancel flag is
/// read by the fetch.
///
/// @param client the worker's client
/// @param api the registry's download API
/// @param request the key request
/// @param cancel set when the fetch should stop; may be null
/// @return the answer
[[nodiscard]] KeyAnswer ask_for_key(
    netgame::http::Client& client,
    const formats::url::Url& api,
    const KeyRequest& request,
    const std::atomic<bool>* cancel
);

/// Polls one check.
///
/// A 404 is expired. A network failure, or a body that does not read, returns
/// nothing.
///
/// @param client the worker's client
/// @param api the registry's download API
/// @param code the check's code
/// @param cancel set when the fetch should stop; may be null
/// @param[out] why why nothing was read; may be null
/// @return the state, or nothing on a network failure or a bad body
[[nodiscard]] std::optional<ChallengeState> poll_challenge(
    netgame::http::Client& client,
    const formats::url::Url& api,
    std::string_view code,
    const std::atomic<bool>* cancel,
    std::string* why
);

/// Reports how one download ended.
///
/// One try, with a 10 second limit. True for a 2xx or a 409.
///
/// @param client the worker's client
/// @param api the registry's download API
/// @param download the key's download id
/// @param result how it ended
/// @param bytes the bytes fetched
/// @param seconds whole seconds from the first key
/// @param[out] why why it was not accepted; may be null
/// @return true when the registry accepted the result, or already had one
[[nodiscard]] bool post_result(
    netgame::http::Client& client,
    const formats::url::Url& api,
    std::string_view download,
    KeyResult result,
    uint64_t bytes,
    int64_t seconds,
    std::string* why
);

/// This build's version, as a key request names it.
///
/// @return the version text; never null
[[nodiscard]] std::string_view engine_version() noexcept;

} // namespace oa::app::content

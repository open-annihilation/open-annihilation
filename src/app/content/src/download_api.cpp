// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The download API's bodies and its one-request exchanges.
#include "oa/app/content/download_api.hpp"

#include "oa/data/catalogue/catalogue.hpp"
#include "oa/formats/json.hpp"
#include "oa/netgame/http/client.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace oa::app::content {
namespace {

namespace json = oa::formats::json;
namespace url = oa::formats::url;
namespace http = oa::netgame::http;

/// The most characters in a download id or a check code.
constexpr std::size_t id_most = 128;

/// How long a result call may take, in milliseconds.
constexpr uint32_t result_limit_ms = 10000;

/// The most seconds a 429 may ask the client to wait.
constexpr uint32_t retry_after_most = 300;

/// The most bytes of an API body this client reads.
constexpr uint64_t api_body_most = 64 * 1024;

/// The most characters of a header value a log line keeps.
constexpr std::size_t logged_value_most = 120;

/// Writes a reason into `why` when it is not null.
///
/// @param why the caller's reason; may be null
/// @param text the reason
void set_why(std::string* why, std::string text) {
    if (why != nullptr)
        *why = std::move(text);
}

/// Reports whether a character may appear in a download id or a check code.
///
/// @param character the character
/// @return true for a letter, a digit, `_` or `-`
bool id_character(char character) noexcept {
    return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '_' || character == '-';
}

/// Reports whether text is a download id or a check code.
///
/// @param text the text
/// @param most the most characters
/// @return true when every character is allowed and the length is in range
bool valid_id(std::string_view text, std::size_t most) noexcept {
    if (text.empty() || text.size() > most)
        return false;
    for (const char character : text) {
        if (!id_character(character))
            return false;
    }
    return true;
}

/// Returns an object's member, or null.
///
/// @param value a JSON value
/// @param name the member
/// @return the member, or null when this is not an object or it has none
const json::Json* member(const json::Json& value, std::string_view name) noexcept {
    if (value.type() != json::JsonType::object)
        return nullptr;
    return value.find(name);
}

/// Returns a string member.
///
/// @param value a JSON value
/// @param name the member
/// @return the text, or null when the member is missing or not a string
const std::string* string_member(const json::Json& value, std::string_view name) noexcept {
    const json::Json* found = member(value, name);
    if (found == nullptr)
        return nullptr;
    return found->string();
}

/// Keeps a poll interval inside 2 to 30 seconds.
///
/// @param seconds the value the body named
/// @return the value, or the nearer end of the range
uint32_t clamp_poll(int64_t seconds) noexcept {
    if (seconds < 2)
        return 2;
    if (seconds > 30)
        return 30;
    return static_cast<uint32_t>(seconds);
}

/// The page address without its scheme and query.
///
/// @param verify the page
/// @return the host, the port when it is not the scheme's default, and the path
std::string page_address(const url::Url& verify) {
    std::string path = verify.target;
    const std::size_t query = path.find('?');
    if (query != std::string::npos)
        path.erase(query);
    std::string address = verify.host;
    const bool default_port = (verify.scheme == url::Scheme::http && verify.port == 80) ||
                              (verify.scheme == url::Scheme::https && verify.port == 443);
    if (!default_port) {
        address.push_back(':');
        address += std::to_string(verify.port);
    }
    address += path;
    return address;
}

/// Appends a suffix to an API address's path.
///
/// @param api the download API
/// @param suffix the path, beginning with `/`
/// @return the address
url::Url api_url(const url::Url& api, std::string_view suffix) {
    url::Url out = api;
    std::string path = api.target;
    std::string query;
    const std::size_t mark = path.find('?');
    if (mark != std::string::npos) {
        query = path.substr(mark);
        path.erase(mark);
    }
    while (!path.empty() && path.back() == '/')
        path.pop_back();
    if (suffix.empty() || suffix.front() != '/')
        path.push_back('/');
    path.append(suffix);
    path += query;
    out.target = std::move(path);
    return out;
}

/// The word a result body uses.
///
/// @param result how the download ended
/// @return the API's word
std::string_view result_word(KeyResult result) noexcept {
    switch (result) {
    case KeyResult::installed:
        return "installed";
    case KeyResult::refused:
        return "refused";
    case KeyResult::failed:
        return "failed";
    case KeyResult::cancelled:
        return "cancelled";
    }
    return "";
}

/// Reads a Retry-After header as a whole number of seconds, at most 300.
///
/// @param header the header value, or null
/// @return the seconds, or 0 when the header is absent or not a number
uint32_t retry_after_seconds(const std::string* header) noexcept {
    if (header == nullptr || header->empty())
        return 0;
    uint32_t value = 0;
    for (const char character : *header) {
        if (character < '0' || character > '9')
            return 0;
        const uint32_t digit = static_cast<uint32_t>(character - '0');
        if (value > (retry_after_most - digit) / 10)
            return retry_after_most;
        value = value * 10 + digit;
    }
    return std::min(value, retry_after_most);
}

/// Fetches one API address and keeps a small body.
///
/// @param client the worker's client
/// @param method the method
/// @param address the address
/// @param body the request body; empty for a GET
/// @param cancel set when the fetch should stop; may be null
/// @param total_ms the whole-fetch limit; 0 means none
/// @param[out] response_body the response body
/// @return the outcome
http::Response fetch_api(
    http::Client& client,
    http::Method method,
    const url::Url& address,
    std::string_view body,
    const std::atomic<bool>* cancel,
    uint32_t total_ms,
    std::vector<uint8_t>& response_body
) {
    http::Request request;
    request.method = method;
    request.url = url::url_text(address);
    request.cancel = cancel;
    request.timeouts.total_ms = total_ms;
    request.limits.max_body_bytes = api_body_most;
    if (method == http::Method::post) {
        request.content_type = "application/json";
        request.body.assign(body.begin(), body.end());
    }
    return client.fetch(request, response_body);
}

/// Lower-cases ASCII letters and drops the space around a header value.
///
/// @param text the value
/// @return the lower-case value, trimmed
std::string folded(std::string_view text) {
    std::size_t first = 0;
    std::size_t last = text.size();
    while (first < last && (text[first] == ' ' || text[first] == '\t'))
        ++first;
    while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t'))
        --last;
    std::string out(text.substr(first, last - first));
    for (char& character : out)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    return out;
}

/// Keeps a header value fit for one log line: at most logged_value_most
/// bytes, with every control character shown as `?`.
///
/// @param value the value as the server sent it
/// @return the value to log
std::string loggable(std::string_view value) {
    std::string out(value.substr(0, logged_value_most));
    for (char& character : out) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (byte < 0x20 || byte == 0x7f)
            character = '?';
    }
    return out;
}

/// The firewall header that names a check meant for a web browser.
///
/// @param response the answer's head
/// @return "name: value", or empty when the answer carries none
std::string check_header_of(const http::Response& response) {
    if (const std::string* action = response.header("x-amzn-waf-action");
        action != nullptr && !folded(*action).empty())
        return "x-amzn-waf-action: " + loggable(folded(*action));
    if (const std::string* mitigated = response.header("cf-mitigated");
        mitigated != nullptr && folded(*mitigated) == "challenge")
        return "cf-mitigated: challenge";
    return {};
}

/// Parses a JSON body into one value.
///
/// @param body the bytes
/// @param[out] value the value, when the body is one JSON value
/// @return true when it was read
bool parse_body(std::span<const uint8_t> body, json::Json& value) {
    const std::string_view text(reinterpret_cast<const char*>(body.data()), body.size());
    json::JsonError error;
    std::optional<json::Json> parsed = json::parse_json(text, error);
    if (!parsed)
        return false;
    value = std::move(*parsed);
    return true;
}

} // namespace

std::string_view reason_text(DownloadReason reason) noexcept {
    switch (reason) {
    case DownloadReason::install:
        return "install";
    case DownloadReason::update:
        return "update";
    case DownloadReason::repair:
        return "repair";
    case DownloadReason::offered_in_lobby:
        return "offered-in-lobby";
    case DownloadReason::mirror:
        return "mirror";
    }
    return "";
}

std::string_view platform_name() noexcept {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
#if TARGET_OS_IPHONE
    return "ios";
#else
    return "macos";
#endif
#elif defined(__ANDROID__)
    return "android";
#elif defined(__linux__)
    return "linux";
#else
    return "";
#endif
}

std::string_view arch_name() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    return "x64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#else
    return "";
#endif
}

std::string_view engine_version() noexcept {
#ifdef OA_ENGINE_VERSION
    return OA_ENGINE_VERSION;
#else
    return "";
#endif
}

std::string key_request_body(const KeyRequest& request) {
    const auto hex = base::sha256::to_hex(request.sha256);
    json::JsonWriter writer;
    writer.begin_object();
    if (request.install) {
        writer.key("install");
        writer.string(*request.install);
    }
    writer.key("package");
    writer.string(request.package);
    writer.key("kind");
    writer.string(
        request.kind_text.empty() ? data::catalogue::kind_name(request.kind) : request.kind_text
    );
    writer.key("release");
    writer.integer(request.release);
    writer.key("sha256");
    writer.string(std::string_view(hex.data(), hex.size()));
    writer.key("engine");
    writer.string(request.engine);
    writer.key("platform");
    writer.string(request.platform);
    writer.key("arch");
    writer.string(request.arch);
    writer.key("language");
    writer.string(request.language);
    writer.key("reason");
    writer.string(reason_text(request.reason));
    writer.end_object();
    return writer.text();
}

std::optional<IssuedKey> read_issued_key(std::span<const uint8_t> body, std::string* why) {
    json::Json value;
    if (!parse_body(body, value)) {
        set_why(why, "the key is not JSON");
        return std::nullopt;
    }
    const std::string* download = string_member(value, "download");
    if (download == nullptr) {
        set_why(why, "the key is missing a download id");
        return std::nullopt;
    }
    if (!valid_id(*download, id_most)) {
        set_why(why, "the download id is not valid");
        return std::nullopt;
    }
    const std::string* expires = string_member(value, "expires");
    if (expires == nullptr) {
        set_why(why, "the key is missing an expiry");
        return std::nullopt;
    }
    const std::optional<int64_t> when = data::catalogue::parse_utc_time(*expires);
    if (!when) {
        set_why(why, "the key's expiry is not a time");
        return std::nullopt;
    }
    const std::string* address = string_member(value, "url");
    if (address == nullptr) {
        set_why(why, "the key is missing an address");
        return std::nullopt;
    }
    url::UrlError url_error = url::UrlError::none;
    const std::optional<url::Url> parsed = url::parse_http_url(*address, &url_error);
    if (!parsed) {
        set_why(why, "the key's address is not an http or https address");
        return std::nullopt;
    }
    IssuedKey key;
    key.download = *download;
    key.expires = *when;
    key.url = *parsed;
    return key;
}

std::optional<Challenge> read_challenge(std::span<const uint8_t> body, std::string* why) {
    json::Json value;
    if (!parse_body(body, value)) {
        set_why(why, "the check is not JSON");
        return std::nullopt;
    }
    const json::Json* challenge = member(value, "challenge");
    if (challenge == nullptr || challenge->type() != json::JsonType::object) {
        set_why(why, "the check is missing");
        return std::nullopt;
    }
    const std::string* code = string_member(*challenge, "code");
    if (code == nullptr) {
        set_why(why, "the check is missing a code");
        return std::nullopt;
    }
    if (!valid_id(*code, id_most)) {
        set_why(why, "the check's code is not valid");
        return std::nullopt;
    }
    const std::string* verify = string_member(*challenge, "verify");
    if (verify == nullptr) {
        set_why(why, "the check is missing a page");
        return std::nullopt;
    }
    url::UrlError url_error = url::UrlError::none;
    const std::optional<url::Url> parsed = url::parse_web_url(*verify, &url_error);
    if (!parsed) {
        set_why(why, "the check's page is not an address");
        return std::nullopt;
    }
    const std::string* expires = string_member(*challenge, "expires");
    if (expires == nullptr) {
        set_why(why, "the check is missing an expiry");
        return std::nullopt;
    }
    const std::optional<int64_t> when = data::catalogue::parse_utc_time(*expires);
    if (!when) {
        set_why(why, "the check's expiry is not a time");
        return std::nullopt;
    }
    uint32_t poll = 3;
    if (const json::Json* named = member(*challenge, "poll")) {
        const std::optional<int64_t> seconds = named->integer();
        if (!seconds) {
            set_why(why, "the check's poll is not a whole number of seconds");
            return std::nullopt;
        }
        poll = clamp_poll(*seconds);
    }
    Challenge out;
    out.code = *code;
    out.verify = *parsed;
    out.address = page_address(*parsed);
    out.expires = *when;
    out.poll_seconds = poll;
    return out;
}

std::optional<ChallengeState>
read_challenge_state(std::span<const uint8_t> body, std::string* why) {
    json::Json value;
    if (!parse_body(body, value)) {
        set_why(why, "the check is not JSON");
        return std::nullopt;
    }
    const std::string* code = string_member(value, "code");
    if (code == nullptr || !valid_id(*code, id_most)) {
        set_why(why, "the check's code is not valid");
        return std::nullopt;
    }
    const std::string* state = string_member(value, "state");
    if (state == nullptr) {
        set_why(why, "the check is missing a state");
        return std::nullopt;
    }
    const std::string* expires = string_member(value, "expires");
    if (expires == nullptr || !data::catalogue::parse_utc_time(*expires)) {
        set_why(why, "the check's expiry is not a time");
        return std::nullopt;
    }
    if (*state == "pending")
        return ChallengeState::pending;
    if (*state == "solved")
        return ChallengeState::solved;
    if (*state == "expired")
        return ChallengeState::expired;
    set_why(why, "the check's state is not known");
    return std::nullopt;
}

ApiError
read_api_error(int status, std::span<const uint8_t> body, const std::string* retry_after_header) {
    ApiError error;
    error.status = status;
    error.retry_after_seconds = retry_after_seconds(retry_after_header);
    json::Json value;
    if (!parse_body(body, value) || value.type() != json::JsonType::object)
        return error;
    if (const std::string* code = string_member(value, "error"))
        error.error = *code;
    if (const std::string* message = string_member(value, "message"))
        error.message = *message;
    if (const std::string* field = string_member(value, "field"))
        error.field = *field;
    return error;
}

std::string result_body(KeyResult result, uint64_t bytes, int64_t seconds) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("result");
    writer.string(result_word(result));
    writer.key("bytes");
    writer.integer(static_cast<int64_t>(bytes));
    writer.key("seconds");
    writer.integer(seconds < 0 ? 0 : seconds);
    writer.end_object();
    return writer.text();
}

bool browser_check(const http::Response& response) {
    if (response.status == 0)
        return false;
    if (!check_header_of(response).empty())
        return true;
    if (response.status >= 500)
        return false;
    const std::string* type = response.header("content-type");
    if (type == nullptr)
        return false;
    const std::string media = folded(std::string_view(*type).substr(0, type->find(';')));
    return media == "text/html" || media == "application/xhtml+xml";
}

std::string address_without_query(std::string_view address) {
    return std::string(address.substr(0, address.find_first_of("?#")));
}

Answered answered(std::string_view address, const http::Response& response) {
    Answered answer;
    answer.address = address_without_query(address);
    answer.status = response.status;
    if (const std::string* type = response.header("content-type"))
        answer.content_type = loggable(*type);
    answer.length = response.content_length;
    answer.body_bytes = response.body_bytes;
    answer.browser_check = browser_check(response);
    answer.check_header = check_header_of(response);
    // A stop the caller's sink chose says nothing of the answer, and a
    // check's page is never read whole.
    if (response.failure != http::Failure::none &&
        response.failure != http::Failure::sink_stopped && !answer.browser_check)
        answer.failure =
            response.detail.empty() ? http::failure_text(response.failure) : response.detail;
    return answer;
}

std::string answered_text(const Answered& answer) {
    const std::string address = answer.address.empty() ? std::string("an address") : answer.address;
    if (answer.status == 0)
        return address + ": " +
               (answer.failure.empty() ? std::string("no answer") : answer.failure);
    std::string text = address + " answered " + std::to_string(answer.status) + ", ";
    text += answer.content_type.empty() ? std::string("no content type") : answer.content_type;
    text += ", ";
    text += answer.length ? std::to_string(*answer.length) + " bytes"
                          : std::to_string(answer.body_bytes) + " bytes read";
    if (answer.browser_check) {
        text += ", a check meant for a web browser";
        if (!answer.check_header.empty())
            text += " (" + answer.check_header + ")";
    }
    if (!answer.failure.empty())
        text += "; " + answer.failure;
    return text;
}

KeyAnswer ask_for_key(
    http::Client& client,
    const url::Url& api,
    const KeyRequest& request,
    const std::atomic<bool>* cancel
) {
    KeyAnswer answer;
    std::vector<uint8_t> body;
    const std::string payload = key_request_body(request);
    const url::Url address = api_url(api, "/downloads");
    const http::Response response =
        fetch_api(client, http::Method::post, address, payload, cancel, 0, body);
    answer.answered = answered(url::url_text(address), response);
    // A check meant for a web browser is never answered or worked around:
    // the game gives up for now, whatever the body or its size.
    if (answer.answered.browser_check) {
        answer.kind = KeyAnswer::Kind::browser_check;
        answer.detail = "the server asked for a check meant for a web browser";
        return answer;
    }
    if (response.failure != http::Failure::none) {
        answer.kind = KeyAnswer::Kind::retry;
        answer.detail =
            response.detail.empty() ? http::failure_text(response.failure) : response.detail;
        return answer;
    }
    const std::span<const uint8_t> bytes(body.data(), body.size());
    if (response.status == 201) {
        std::string why;
        answer.key = read_issued_key(bytes, &why);
        if (!answer.key) {
            answer.kind = KeyAnswer::Kind::retry;
            answer.detail = why;
            return answer;
        }
        answer.kind = KeyAnswer::Kind::key;
        return answer;
    }
    if (response.status == 428) {
        std::string why;
        answer.challenge = read_challenge(bytes, &why);
        if (!answer.challenge) {
            answer.kind = KeyAnswer::Kind::retry;
            answer.detail = why;
            return answer;
        }
        answer.kind = KeyAnswer::Kind::challenge;
        return answer;
    }
    answer.error = read_api_error(response.status, bytes, response.header("retry-after"));
    if (response.status == 404 || response.status == 409) {
        answer.kind = KeyAnswer::Kind::not_offered;
        answer.detail = answer.error.message;
        return answer;
    }
    if (response.status == 429) {
        answer.kind = KeyAnswer::Kind::rate_limited;
        answer.detail = answer.error.message;
        return answer;
    }
    if (response.status >= 400 && response.status < 500) {
        answer.kind = KeyAnswer::Kind::refused;
        answer.detail = answer.error.message;
        return answer;
    }
    answer.kind = KeyAnswer::Kind::retry;
    answer.detail =
        answer.error.message.empty() ? "the registry did not answer" : answer.error.message;
    return answer;
}

std::optional<ChallengeState> poll_challenge(
    http::Client& client,
    const url::Url& api,
    std::string_view code,
    const std::atomic<bool>* cancel,
    std::string* why
) {
    std::vector<uint8_t> body;
    const http::Response response = fetch_api(
        client,
        http::Method::get,
        api_url(api, std::string("/challenges/") + std::string(code)),
        {},
        cancel,
        0,
        body
    );
    if (response.failure != http::Failure::none) {
        set_why(
            why,
            response.detail.empty() ? std::string(http::failure_text(response.failure))
                                    : response.detail
        );
        return std::nullopt;
    }
    if (response.status == 404)
        return ChallengeState::expired;
    if (response.status < 200 || response.status >= 300) {
        set_why(why, "the check could not be read");
        return std::nullopt;
    }
    return read_challenge_state(std::span<const uint8_t>(body.data(), body.size()), why);
}

bool post_result(
    http::Client& client,
    const url::Url& api,
    std::string_view download,
    KeyResult result,
    uint64_t bytes,
    int64_t seconds,
    std::string* why
) {
    std::vector<uint8_t> body;
    const std::string payload = result_body(result, bytes, seconds);
    const http::Response response = fetch_api(
        client,
        http::Method::post,
        api_url(api, std::string("/downloads/") + std::string(download) + "/result"),
        payload,
        nullptr,
        result_limit_ms,
        body
    );
    if (response.failure != http::Failure::none) {
        set_why(
            why,
            response.detail.empty() ? std::string(http::failure_text(response.failure))
                                    : response.detail
        );
        return false;
    }
    if ((response.status >= 200 && response.status < 300) || response.status == 409)
        return true;
    set_why(why, "the registry did not accept the result");
    return false;
}

} // namespace oa::app::content

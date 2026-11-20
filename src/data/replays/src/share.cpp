// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/replays/share.hpp"

#include "oa/data/replays/feed.hpp"
#include "oa/formats/json.hpp"

#include <algorithm>

namespace oa::data::replays {
namespace {

namespace json = oa::formats::json;

constexpr std::string_view kCookieName = "cp_session";

/// Drops a trailing slash.
///
/// @param origin the origin
/// @return the origin without one trailing slash
std::string_view trimmed_origin(std::string_view origin) {
    if (!origin.empty() && origin.back() == '/')
        origin.remove_suffix(1);
    return origin;
}

/// The host of an absolute address, with an IPv6 address kept in brackets.
///
/// @param url the address
/// @return the host, or empty when the address has no scheme
std::string_view host_of(std::string_view url) noexcept {
    const std::size_t scheme = url.find("://");
    if (scheme == std::string_view::npos)
        return {};
    std::string_view rest = url.substr(scheme + 3);
    const std::size_t slash = rest.find('/');
    std::string_view hostport = slash == std::string_view::npos ? rest : rest.substr(0, slash);
    const std::size_t at = hostport.rfind('@');
    if (at != std::string_view::npos)
        hostport.remove_prefix(at + 1);
    if (!hostport.empty() && hostport.front() == '[') {
        const std::size_t end = hostport.find(']');
        if (end == std::string_view::npos)
            return {};
        return hostport.substr(0, end + 1);
    }
    const std::size_t colon = hostport.rfind(':');
    if (colon != std::string_view::npos)
        return hostport.substr(0, colon);
    return hostport;
}

/// True for a host a test may name.
///
/// @param host the host
/// @return true for loopback
bool loopback_host(std::string_view host) noexcept {
    return host == "127.0.0.1" || host == "localhost" || host == "[::1]" || host == "::1";
}

/// One error object from a share.
struct ErrorBody {
    std::string error{};
    std::string message{};
    std::string id{};
    std::string reason{};
    bool have{false};
};

/// Reads an error object. A body that is not one leaves `have` false.
///
/// @param body the body
/// @return the fields it named
ErrorBody read_error(std::string_view body) {
    ErrorBody error;
    json::JsonError problem;
    const std::optional<json::Json> root = json::parse_json(body, problem);
    if (!root.has_value() || root->type() != json::JsonType::object)
        return error;
    const json::Json* code = root->find("error");
    if (code == nullptr || code->string() == nullptr)
        return error;
    error.have = true;
    error.error = *code->string();
    if (const json::Json* message = root->find("message")) {
        if (message->string() != nullptr)
            error.message = *message->string();
    }
    if (const json::Json* id = root->find("id")) {
        if (id->string() != nullptr)
            error.id = *id->string();
    }
    if (const json::Json* reason = root->find("code")) {
        if (reason->string() != nullptr)
            error.reason = *reason->string();
    }
    return error;
}

/// Adds the wait a rate limit named.
///
/// @param[in,out] sentence the sentence so far
/// @param retry_after the seconds, when the header named them
void add_retry(std::string& sentence, std::optional<int64_t> retry_after) {
    if (!retry_after.has_value() || *retry_after < 0)
        return;
    sentence.push_back(' ');
    sentence += "Try again in ";
    sentence += std::to_string(*retry_after);
    sentence += " s.";
}

/// Reads `cp_session` from one cookie: its first pair, before any attribute.
///
/// @param set_cookie one cookie
/// @return the token, or empty when the cookie is another
std::string one_session_token(std::string_view set_cookie) {
    const std::size_t semi = set_cookie.find(';');
    std::string_view pair =
        semi == std::string_view::npos ? set_cookie : set_cookie.substr(0, semi);
    while (!pair.empty() && (pair.front() == ' ' || pair.front() == '\t'))
        pair.remove_prefix(1);
    while (!pair.empty() && (pair.back() == ' ' || pair.back() == '\t'))
        pair.remove_suffix(1);
    const std::size_t equal = pair.find('=');
    if (equal == std::string_view::npos)
        return {};
    std::string_view name = pair.substr(0, equal);
    std::string_view value = pair.substr(equal + 1);
    while (!name.empty() && (name.front() == ' ' || name.front() == '\t'))
        name.remove_prefix(1);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t'))
        name.remove_suffix(1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
        value.remove_prefix(1);
    if (name != kCookieName)
        return {};
    return std::string(value);
}

} // namespace

std::string login_request_body(std::string_view handle, std::string_view password) {
    std::string body;
    body += "{\"query\": ";
    json::append_json_string(body, login_query);
    body += ", \"variables\": {\"handle\": ";
    json::append_json_string(body, handle);
    body += ", \"password\": ";
    json::append_json_string(body, password);
    body += "}}";
    return body;
}

std::string login_url(std::string_view origin) {
    std::string url(trimmed_origin(origin));
    url += "/graphql";
    return url;
}

std::string upload_url(std::string_view origin) {
    std::string url(trimmed_origin(origin));
    url += replay_feed_path;
    return url;
}

std::string cp_session_token(std::string_view set_cookie) {
    // The system's URL loading on macOS joins every Set-Cookie of a response
    // into one value, separated by commas. A cookie's value holds no comma,
    // and a piece that is only an attribute's date ("21 Oct 2026 …") names
    // no cookie, so each piece is read as a cookie of its own.
    for (;;) {
        const std::size_t comma = set_cookie.find(',');
        std::string token = one_session_token(
            comma == std::string_view::npos ? set_cookie : set_cookie.substr(0, comma)
        );
        if (!token.empty() || comma == std::string_view::npos)
            return token;
        set_cookie.remove_prefix(comma + 1);
    }
}

std::string cp_session_token(std::span<const std::string_view> set_cookies) {
    for (const std::string_view header : set_cookies) {
        std::string token = cp_session_token(header);
        if (!token.empty())
            return token;
    }
    return {};
}

LoginAnswer parse_login_answer(std::string_view body) {
    LoginAnswer answer;
    json::JsonError problem;
    const std::optional<json::Json> root = json::parse_json(body, problem);
    if (!root.has_value() || root->type() != json::JsonType::object)
        return answer;
    const json::Json* data = root->find("data");
    if (data == nullptr || data->type() != json::JsonType::object)
        return answer;
    const json::Json* login = data->find("login");
    if (login == nullptr || login->type() != json::JsonType::object)
        return answer;
    const json::Json* ok = login->find("ok");
    if (ok == nullptr || !ok->boolean().has_value())
        return answer;
    answer.readable = true;
    answer.ok = *ok->boolean();
    if (const json::Json* error = login->find("error")) {
        if (error->string() != nullptr)
            answer.error = *error->string();
    }
    if (const json::Json* account = login->find("account")) {
        if (account->type() == json::JsonType::object) {
            if (const json::Json* handle = account->find("handle")) {
                if (handle->string() != nullptr)
                    answer.handle = *handle->string();
            }
        }
    }
    return answer;
}

ShareAnswer interpret_share(int status, std::string_view body, std::optional<int64_t> retry_after) {
    ShareAnswer answer;
    answer.status = status;
    if (status == 201) {
        const FeedEntryResult entry = parse_replay_entry(body);
        if (entry.status != FeedStatus::entry) {
            answer.outcome = ShareOutcome::unreadable;
            return answer;
        }
        answer.outcome = ShareOutcome::shared;
        answer.sentence = std::string(shared_on_core_prime);
        answer.id = entry.entry.id;
        return answer;
    }
    const ErrorBody error = read_error(body);
    answer.code = error.reason.empty() ? error.error : error.reason;
    if (status == 401 && error.error == "signed_out") {
        answer.outcome = ShareOutcome::signed_out;
        answer.sentence = std::string(sign_in_to_share);
        answer.forget_token = true;
        answer.ask_sign_in = true;
        return answer;
    }
    if (status == 403 && error.error == "account_not_active") {
        answer.outcome = ShareOutcome::refused;
        answer.sentence = std::string(account_cannot_share);
        return answer;
    }
    if (status == 403 && error.error == "not_in_battle") {
        answer.outcome = ShareOutcome::refused;
        answer.sentence = std::string(only_a_player_can_replace);
        return answer;
    }
    if (status == 409 && error.error == "duplicate") {
        answer.outcome = ShareOutcome::already;
        answer.sentence = std::string(already_on_core_prime);
        answer.id = error.id;
        return answer;
    }
    if (status == 413 && error.error == "too_large") {
        answer.outcome = ShareOutcome::refused;
        answer.sentence = std::string(replay_larger_than_the_limit);
        return answer;
    }
    if (status == 422 && error.error == "invalid_replay") {
        answer.outcome = ShareOutcome::refused;
        answer.sentence = error.message;
        if (!error.reason.empty())
            answer.code = error.reason;
        return answer;
    }
    if (status == 429 && error.error == "rate_limited") {
        answer.outcome = ShareOutcome::refused;
        answer.sentence = std::string(shared_for_today);
        answer.retry_after = retry_after;
        add_retry(answer.sentence, retry_after);
        return answer;
    }
    if (status == 503 && error.error == "uploads_off") {
        answer.outcome = ShareOutcome::refused;
        answer.sentence = std::string(uploads_not_available);
        return answer;
    }
    if (!error.have) {
        answer.outcome = ShareOutcome::unreadable;
        answer.sentence = std::string(share_not_answered);
        return answer;
    }
    answer.outcome = ShareOutcome::refused;
    answer.sentence = error.message;
    return answer;
}

bool share_title_ok(std::string_view title) noexcept {
    if (title.empty())
        return true;
    if (title.size() > share_title_bytes)
        return false;
    return json::utf8_valid(title);
}

ShareTransport share_transport(std::string_view url, bool https_downgraded) noexcept {
    if (https_downgraded)
        return ShareTransport::needs_tls;
    if (url.starts_with("https://"))
        return ShareTransport::allowed;
    if (url.starts_with("http://") && loopback_host(host_of(url)))
        return ShareTransport::allowed;
    return ShareTransport::plain_refused;
}

void wipe_secret(std::string& secret) {
    std::fill(secret.begin(), secret.end(), '\0');
    secret.clear();
}

} // namespace oa::data::replays

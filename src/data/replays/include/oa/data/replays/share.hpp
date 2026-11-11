// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Signing in to Core Prime, and what a share's answer says. Nothing here
// fetches, and nothing keeps a password or a session token.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace oa::data::replays {

/// The most bytes a share will send. A larger file is refused before the request.
inline constexpr uint64_t share_byte_limit = 67108864;

/// The login mutation, asking only whether it worked and the account's handle.
inline constexpr std::string_view login_query =
    "mutation Login($handle: String!, $password: String!) { login(handle: $handle, password: "
    "$password) { ok error account { handle } } }";

/// What the player sees when a share is accepted.
inline constexpr std::string_view shared_on_core_prime = "Shared on Core Prime";

/// What the player sees when that replay is already there.
inline constexpr std::string_view already_on_core_prime = "Already on Core Prime";

/// What a signed-out share is told.
inline constexpr std::string_view sign_in_to_share = "Sign in to share a replay.";

/// What an account that cannot share is told.
inline constexpr std::string_view account_cannot_share = "This account cannot share replays.";

/// What a player who was not in the battle is told.
inline constexpr std::string_view only_a_player_can_replace =
    "Only a player in that battle can replace its replay.";

/// What a file past the limit is told.
inline constexpr std::string_view replay_larger_than_the_limit =
    "The replay is larger than the limit.";

/// What an account that has shared enough for one day is told.
inline constexpr std::string_view shared_for_today =
    "This account has shared its replays for today.";

/// What the player sees when uploads are off.
inline constexpr std::string_view uploads_not_available = "Uploads are not available.";

/// What the player sees when a login is accepted and names no session.
inline constexpr std::string_view session_not_started = "Core Prime did not start a session.";

/// What the player sees when a share's answer is empty or not the error object.
inline constexpr std::string_view share_not_answered = "Core Prime did not answer.";

/// Who can see a share, the one offered first.
inline constexpr std::string_view visibility_public = "public";

/// Who can see a share the player does not list.
inline constexpr std::string_view visibility_unlisted = "unlisted";

/// How many bytes a title may hold.
inline constexpr std::size_t share_title_bytes = 80;

/// What a login answer said.
struct LoginAnswer {
    /// The answer was the mutation's object.
    bool readable{false};
    /// The handle and the password were accepted.
    bool ok{false};
    /// The sentence, when `ok` is false. Empty when the answer named none.
    std::string error{};
    /// The account's handle, when the answer named one.
    std::string handle{};
};

/// What a share's answer asks the game to do.
enum class ShareOutcome : uint8_t {
    shared,     ///< accepted, with the entry's id
    already,    ///< that replay is already there, with its id
    signed_out, ///< the session is gone
    refused,    ///< a sentence, and no new replay
    unreadable, ///< the answer could not be read
};

/// What the player is told about a share, and which replay it names.
struct ShareAnswer {
    ShareOutcome outcome{ShareOutcome::unreadable}; ///< what the status asked for
    int status{};                                   ///< the HTTP status
    std::string code{};                             ///< the error code, when one was named
    std::string sentence{};                         ///< the sentence the player sees
    std::string id{};                               ///< the replay's id, when the answer named one
    bool forget_token{false};                       ///< the session token is no longer good
    bool ask_sign_in{false};                        ///< the sign-in question opens again
    /// Seconds `Retry-After` named, when a 429 gave a number. The sentence
    /// already includes them in English; a translation composes its own.
    std::optional<int64_t> retry_after{};
};

/// Whether a password may be sent to this address.
enum class ShareTransport : uint8_t {
    allowed,       ///< https, or a loopback host a test named
    needs_tls,     ///< this system would send https as http
    plain_refused, ///< plain http to a host that is not loopback
};

/// Writes the login request's body.
///
/// The query is login_query. The variables are the handle and the password
/// and nothing else. The caller drops the password once this returns.
///
/// @param handle the Core Prime handle
/// @param password the password, for this request only
/// @return the JSON body
[[nodiscard]] std::string login_request_body(std::string_view handle, std::string_view password);

/// Joins an origin and the login path.
///
/// A trailing `/` on the origin is dropped.
///
/// @param origin the origin, with its scheme
/// @return the address
[[nodiscard]] std::string login_url(std::string_view origin);

/// Joins an origin and the upload path.
///
/// A trailing `/` on the origin is dropped.
///
/// @param origin the origin, with its scheme
/// @return the address
[[nodiscard]] std::string upload_url(std::string_view origin);

/// Reads `cp_session` from one `Set-Cookie` value.
///
/// The name is that cookie exactly. A name that only starts with it is left
/// out. The value runs to the next semicolon. A value that joins several
/// cookies with commas, as macOS's URL loading gives them, is read cookie by
/// cookie.
///
/// @param set_cookie one header value
/// @return the token, or empty when the cookie is not there
[[nodiscard]] std::string cp_session_token(std::string_view set_cookie);

/// Reads `cp_session` from several `Set-Cookie` values. The first match is kept.
///
/// @param set_cookies the header values
/// @return the token, or empty when none names it
[[nodiscard]] std::string cp_session_token(std::span<const std::string_view> set_cookies);

/// Reads a login answer.
///
/// @param body the response body
/// @return what it said; `readable` is false when it is not the mutation's object
[[nodiscard]] LoginAnswer parse_login_answer(std::string_view body);

/// Maps a share's status and body onto the sentence the player sees.
///
/// A 201 keeps the entry's id. A 409 keeps the id the body names. A 401
/// forgets the token and asks to sign in. A 422 and an unknown code show
/// the body's message. A 429 adds when to try again when `retry_after` is set.
///
/// @param status the HTTP status
/// @param body the response body
/// @param retry_after the `Retry-After` seconds, when the header named a number
/// @return the sentence and the replay it names
[[nodiscard]] ShareAnswer
interpret_share(int status, std::string_view body, std::optional<int64_t> retry_after);

/// Tells whether a title may be sent.
///
/// Empty is left off the request. Any other title is 1 to share_title_bytes
/// of UTF-8.
///
/// @param title the title
/// @return true when it may be sent
[[nodiscard]] bool share_title_ok(std::string_view title) noexcept;

/// Tells whether a password may be sent to this address.
///
/// `https_downgraded` is true where https is read as http. A loopback host
/// (`127.0.0.1`, `localhost`, `[::1]`) on plain http is allowed only when
/// this system still has TLS, so a test can name its own server.
///
/// @param url the address
/// @param https_downgraded this system reads https as http
/// @return whether the password may be sent
[[nodiscard]] ShareTransport share_transport(std::string_view url, bool https_downgraded) noexcept;

/// Replaces a secret with zeros and then clears it.
///
/// @param[in,out] secret the password or the token
void wipe_secret(std::string& secret);

} // namespace oa::data::replays

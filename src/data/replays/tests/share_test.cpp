// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The login body, the session cookie, a refused login, and each share answer.

#include "oa/data/replays/share.hpp"
#include "oa/test/check.hpp"

#include <string>
#include <string_view>

namespace {

namespace share = oa::data::replays;

constexpr std::string_view kQuery =
    "mutation Login($handle: String!, $password: String!) { login(handle: $handle, password: "
    "$password) { ok error account { handle } } }";

void the_login_body_is_the_mutation() {
    const std::string body = share::login_request_body("nj", "made-up");
    const std::string wanted =
        std::string("{\"query\": \"") + std::string(kQuery) +
        "\", \"variables\": {\"handle\": \"nj\", \"password\": \"made-up\"}}";
    OA_CHECK(body == wanted);
    OA_CHECK(share::login_url("https://api.coreprime.net/") == "https://api.coreprime.net/graphql");
    OA_CHECK(
        share::upload_url("https://api.coreprime.net") ==
        "https://api.coreprime.net/content/v1/replays"
    );
    const std::string quoted = share::login_request_body("a\"b", "c\\d");
    OA_CHECK(quoted.find("\"a\\\"b\"") != std::string::npos);
    OA_CHECK(quoted.find("\"c\\\\d\"") != std::string::npos);
}

void the_session_cookie_is_read_among_others() {
    OA_CHECK(share::cp_session_token("other=1; Path=/").empty());
    OA_CHECK(share::cp_session_token("not_cp_session=nope; Path=/").empty());
    OA_CHECK(
        share::cp_session_token("cp_session=made-up-token; Path=/; HttpOnly; Secure") ==
        "made-up-token"
    );
    const std::string_view headers[] = {
        "theme=dark; Path=/",
        " not_cp_session=nope; Path=/",
        "cp_session=from-the-third; Path=/; Max-Age=10; HttpOnly; Secure; SameSite=Lax",
    };
    OA_CHECK(share::cp_session_token(headers) == "from-the-third");
    // macOS hands every Set-Cookie of a response over as one value, joined by
    // commas, with an Expires date's own comma among them.
    OA_CHECK(
        share::cp_session_token(
            "theme=dark; Path=/; Expires=Wed, 21 Oct 2026 07:28:00 GMT, "
            "cp_session=joined-token; Path=/; HttpOnly; Secure, cp_pending=; Max-Age=0"
        ) == "joined-token"
    );
    OA_CHECK(share::cp_session_token("theme=dark, not_cp_session=nope").empty());
}

void a_refused_login_keeps_the_sentence() {
    const share::LoginAnswer refused = share::parse_login_answer(
        "{\"data\":{\"login\":{\"ok\":false,\"error\":\"Invalid handle or password.\","
        "\"account\":null}}}"
    );
    OA_CHECK(refused.readable);
    OA_CHECK(!refused.ok);
    OA_CHECK(refused.error == "Invalid handle or password.");
    OA_CHECK(refused.handle.empty());
    const share::LoginAnswer verify = share::parse_login_answer(
        "{\"data\":{\"login\":{\"ok\":false,\"error\":\"Please verify your email before signing "
        "in.\",\"account\":null}}}"
    );
    OA_CHECK(verify.error == "Please verify your email before signing in.");
    const share::LoginAnswer accepted = share::parse_login_answer(
        "{\"data\":{\"login\":{\"ok\":true,\"error\":null,\"account\":{\"handle\":\"nj\"}}}}"
    );
    OA_CHECK(accepted.ok);
    OA_CHECK(accepted.handle == "nj");
    OA_CHECK(accepted.error.empty());
    OA_CHECK(!share::parse_login_answer("").readable);
}

std::string error_body(std::string_view code, std::string_view message, std::string_view extra) {
    std::string text = "{\"error\":\"";
    text += code;
    text += "\",\"message\":\"";
    text += message;
    text += "\"";
    text += extra;
    text += "}";
    return text;
}

void each_share_answer_has_its_sentence() {
    const std::string created =
        "{\"id\":\"r_0123456789abcdefghijAB\",\"revision\":1,\"title\":\"Show Down\","
        "\"source\":\"upload\",\"visibility\":\"public\",\"recorded_at\":\"2026-11-04T14:32:10Z\","
        "\"length_ms\":1,\"game\":{\"kind\":\"skirmish\"},\"map\":{\"name\":\"Show Down\"},"
        "\"players\":[],\"downloads\":0,\"files\":{\"oademo\":{\"sha256\":"
        "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"size\":1},"
        "\"tad\":{\"sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
        "\"size\":1}}}";
    const share::ShareAnswer shared = share::interpret_share(201, created, std::nullopt);
    OA_CHECK(shared.outcome == share::ShareOutcome::shared);
    OA_CHECK(shared.sentence == share::shared_on_core_prime);
    OA_CHECK(shared.id == "r_0123456789abcdefghijAB");

    const share::ShareAnswer signed_out = share::interpret_share(
        401, error_body("signed_out", "Sign in to share a replay.", ""), std::nullopt
    );
    OA_CHECK(signed_out.outcome == share::ShareOutcome::signed_out);
    OA_CHECK(signed_out.sentence == "Sign in to share a replay.");
    OA_CHECK(signed_out.forget_token);
    OA_CHECK(signed_out.ask_sign_in);

    const share::ShareAnswer inactive = share::interpret_share(
        403,
        error_body("account_not_active", "This account cannot share replays.", ""),
        std::nullopt
    );
    OA_CHECK(inactive.sentence == "This account cannot share replays.");

    const share::ShareAnswer outsider = share::interpret_share(
        403,
        error_body("not_in_battle", "Only a player in that battle can replace its replay.", ""),
        std::nullopt
    );
    OA_CHECK(outsider.sentence == "Only a player in that battle can replace its replay.");

    const share::ShareAnswer duplicate = share::interpret_share(
        409,
        error_body(
            "duplicate", "This replay is already shared.", ",\"id\":\"r_0123456789abcdefghijAC\""
        ),
        std::nullopt
    );
    OA_CHECK(duplicate.outcome == share::ShareOutcome::already);
    OA_CHECK(duplicate.sentence == "Already on Core Prime");
    OA_CHECK(duplicate.id == "r_0123456789abcdefghijAC");

    const share::ShareAnswer large = share::interpret_share(
        413, error_body("too_large", "The replay is larger than the limit.", ""), std::nullopt
    );
    OA_CHECK(large.sentence == "The replay is larger than the limit.");

    const share::ShareAnswer invalid = share::interpret_share(
        422,
        error_body("invalid_replay", "The recording names no map.", ",\"code\":\"no_map\""),
        std::nullopt
    );
    OA_CHECK(invalid.sentence == "The recording names no map.");
    OA_CHECK(invalid.code == "no_map");

    const share::ShareAnswer limited = share::interpret_share(
        429,
        error_body("rate_limited", "This account has shared its replays for today.", ""),
        int64_t{30}
    );
    OA_CHECK(
        limited.sentence == "This account has shared its replays for today. Try again in 30 s."
    );
    const share::ShareAnswer limited_plain = share::interpret_share(
        429,
        error_body("rate_limited", "This account has shared its replays for today.", ""),
        std::nullopt
    );
    OA_CHECK(limited_plain.sentence == "This account has shared its replays for today.");

    const share::ShareAnswer off = share::interpret_share(
        503, error_body("uploads_off", "Uploads are not available.", ""), std::nullopt
    );
    OA_CHECK(off.sentence == "Uploads are not available.");

    const share::ShareAnswer unknown =
        share::interpret_share(400, error_body("nope", "That did not work.", ""), std::nullopt);
    OA_CHECK(unknown.outcome == share::ShareOutcome::refused);
    OA_CHECK(unknown.sentence == "That did not work.");

    const share::ShareAnswer empty = share::interpret_share(500, "", std::nullopt);
    OA_CHECK(empty.outcome == share::ShareOutcome::unreadable);
    OA_CHECK(empty.sentence == share::share_not_answered);

    const share::ShareAnswer page = share::interpret_share(500, "<html>no</html>", std::nullopt);
    OA_CHECK(page.outcome == share::ShareOutcome::unreadable);
    OA_CHECK(page.sentence == share::share_not_answered);

    OA_CHECK(share::share_byte_limit == 67108864ULL);
    OA_CHECK(share::share_title_ok(""));
    OA_CHECK(share::share_title_ok("Show Down"));
    OA_CHECK(!share::share_title_ok(std::string(81, 'a')));
    OA_CHECK(
        share::share_transport("https://api.coreprime.net/graphql", false) ==
        share::ShareTransport::allowed
    );
    OA_CHECK(
        share::share_transport("http://127.0.0.1:9/graphql", false) ==
        share::ShareTransport::allowed
    );
    OA_CHECK(
        share::share_transport("http://api.coreprime.net/graphql", false) ==
        share::ShareTransport::plain_refused
    );
    OA_CHECK(
        share::share_transport("https://api.coreprime.net/graphql", true) ==
        share::ShareTransport::needs_tls
    );
    OA_CHECK(
        share::share_transport("http://127.0.0.1:9/graphql", true) ==
        share::ShareTransport::needs_tls
    );
}

} // namespace

void test_replay_share() {
    the_login_body_is_the_mutation();
    the_session_cookie_is_read_among_others();
    a_refused_login_keeps_the_sentence();
    each_share_answer_has_its_sentence();
}

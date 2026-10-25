// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// When a check is due, and one check against a feed served on 127.0.0.1.
// The keys are made from fixed seeds. Nothing is fetched from a publisher.

#include "oa/app/update/check.hpp"
#include "oa/app/update/checker.hpp"

#include "oa/base/signing/ed25519.hpp"
#include "oa/base/threads.hpp"
#include "oa/data/catalogue/check.hpp"
#include "oa/netgame/http/client.hpp"
#include "oa/netgame/http_fixture/server.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace update = oa::app::update;
namespace signing = oa::base::signing;
namespace catalogue = oa::data::catalogue;
namespace fixture = oa::netgame::http_fixture;
namespace http = oa::netgame::http;

constexpr std::string_view kSha =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

/// A feed and the signature a test key made over its exact bytes.
struct SignedFeed {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> signature;
    std::string key_id;
    std::string key_text;
};

/// A scratch folder removed when the case ends.
struct Scratch {
    fs::path path;

    explicit Scratch(std::string_view name) {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() /
               (std::string("oa-v03-") + std::string(name) + "-" + std::to_string(ticks));
        fs::create_directories(path);
    }

    ~Scratch() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

/// Signs `json` with a key derived from a fixed seed. The seed and the secret are wiped.
SignedFeed sign_text(std::string_view json, std::string_view key_id, uint8_t fill) {
    std::array<uint8_t, 32> seed{};
    seed.fill(fill);
    signing::SecretKey secret{};
    signing::PublicKey public_key{};
    signing::key_pair_from_seed(seed, secret, public_key);
    seed.fill(0);
    SignedFeed signed_feed;
    signed_feed.bytes.assign(json.begin(), json.end());
    const signing::Signature signature = signing::sign(secret, signed_feed.bytes);
    secret.fill(0);
    const std::string text =
        catalogue::signature_file_text(catalogue::SignatureLine{std::string(key_id), signature});
    signed_feed.signature.assign(text.begin(), text.end());
    signed_feed.key_id = std::string(key_id);
    signed_feed.key_text = std::string(signing::public_key_text(public_key).view());
    return signed_feed;
}

/// One release of the stable feed, with one package.
std::string feed_json(int64_t release, std::string_view package) {
    std::string json = R"({"feed":1,"channel":"stable","releases":[{"release":)";
    json += std::to_string(release);
    json += R"(,"version":"9.9.9","date":"2026-10-25","from":"0.0.1","packages":{")";
    json += package;
    json += R"(":{"file":"engine.zip","size":1,"sha256":")";
    json += kSha;
    json += R"(","mirrors":["http://127.0.0.1/v1/engine/"]}}}]})";
    return json;
}

/// Reads a file whole.
std::vector<uint8_t> read_file(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return {};
    in.seekg(0, std::ios::end);
    const auto length = in.tellg();
    if (length < 0)
        return {};
    std::vector<uint8_t> bytes(static_cast<std::size_t>(length));
    in.seekg(0);
    if (!bytes.empty())
        in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in)
        return {};
    return bytes;
}

/// Waits until `ready` is true of the snapshot, or the time runs out.
bool wait_until(
    update::Checker& checker, bool (*ready)(const update::CheckSnapshot&), int milliseconds
) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (ready(checker.snapshot()))
            return true;
        oa::base::threads::sleep_ms(20);
    }
    return ready(checker.snapshot());
}

bool is_offered(const update::CheckSnapshot& shot) {
    return shot.state == update::CheckState::offered;
}

bool is_current(const update::CheckSnapshot& shot) {
    return shot.state == update::CheckState::up_to_date;
}

bool is_failed(const update::CheckSnapshot& shot) {
    return shot.state == update::CheckState::failed;
}

bool is_cancelled(const update::CheckSnapshot& shot) {
    return shot.state == update::CheckState::cancelled;
}

void due_dates() {
    constexpr int64_t now = 1'700'000'000;
    OA_CHECK(update::check_due(update::CheckMode::automatically, std::nullopt, now));
    OA_CHECK(!update::check_due(update::CheckMode::automatically, now, now));
    OA_CHECK(!update::check_due(update::CheckMode::automatically, now - 86399, now));
    OA_CHECK(update::check_due(update::CheckMode::automatically, now - 86400, now));
    OA_CHECK(update::check_due(update::CheckMode::automatically, now + 1, now));
    OA_CHECK(!update::check_due(update::CheckMode::when_asked, std::nullopt, now));
    OA_CHECK(!update::check_due(update::CheckMode::never, std::nullopt, now));
    OA_CHECK(!update::check_due(update::CheckMode::when_asked, now - 90000, now));
    OA_CHECK(!update::check_due(update::CheckMode::never, now - 90000, now));
}

void modes_round_trip() {
    oa::platform::preferences::Values values;
    OA_CHECK(update::read_check_mode(values) == update::CheckMode::automatically);
    OA_CHECK(!update::read_checked(values).has_value());
    for (const update::CheckMode mode :
         {update::CheckMode::automatically,
          update::CheckMode::when_asked,
          update::CheckMode::never}) {
        update::write_check_mode(values, mode);
        OA_CHECK(update::read_check_mode(values) == mode);
    }
    values["open-annihilation.update-check"] = "sometimes";
    OA_CHECK(update::read_check_mode(values) == update::CheckMode::automatically);
    update::write_checked(values, 42);
    OA_CHECK(update::read_checked(values) == 42);
    values["open-annihilation.update-checked"] = "nope";
    OA_CHECK(!update::read_checked(values).has_value());
    values["open-annihilation.update-checked"] = "42abc";
    OA_CHECK(!update::read_checked(values).has_value());
    values["open-annihilation.update-checked"] = "";
    OA_CHECK(!update::read_checked(values).has_value());
}

update::Source test_source(const fixture::Server& server, const SignedFeed& signed_feed) {
    update::Source source;
    source.feed_url = server.url("/v1/engine/stable.json");
    source.test = true;
    update::FeedKey key;
    key.id = signed_feed.key_id;
    key.text = signed_feed.key_text;
    source.keys.push_back(std::move(key));
    return source;
}

void serve(fixture::Server& server, const SignedFeed& signed_feed) {
    server.serve_bytes("/v1/engine/stable.json", signed_feed.bytes, "application/json");
    server.serve_bytes("/v1/engine/stable.json.sig", signed_feed.signature, "text/plain");
}

bool agent_and_no_cookie(const fixture::LoggedRequest& request) {
    const std::string* agent = request.header("User-Agent");
    if (agent == nullptr || !agent->starts_with("OpenAnnihilation/"))
        return false;
    if (request.header("Cookie") != nullptr)
        return false;
    return request.target.find('?') == std::string::npos;
}

void offered_check(fixture::Server& server) {
    const update::ThisBuild built = update::this_build({});
    const SignedFeed signed_feed =
        sign_text(feed_json(built.release + 1, built.package), "test-pin", 0x21);
    serve(server, signed_feed);
    server.clear_log();
    Scratch scratch("offered");
    update::Checker checker(scratch.path);
    checker.start(test_source(server, signed_feed), built, false);
    OA_CHECK(wait_until(checker, is_offered, 5000));
    const update::CheckSnapshot shot = checker.snapshot();
    OA_CHECK(shot.state == update::CheckState::offered);
    OA_CHECK(shot.checked_at.has_value());
    OA_CHECK(shot.offer.release != nullptr);
    if (shot.offer.release != nullptr) {
        OA_CHECK(shot.offer.release->release == built.release + 1);
        OA_CHECK(shot.offer.release->version_text == "9.9.9");
    }
    OA_CHECK(read_file(scratch.path / "feed.json") == signed_feed.bytes);
    OA_CHECK(read_file(scratch.path / "feed.json.sig") == signed_feed.signature);
    const std::vector<fixture::LoggedRequest> requests = server.requests();
    OA_CHECK(requests.size() == 2);
    if (requests.size() == 2) {
        OA_CHECK(requests[0].method == "GET");
        OA_CHECK(requests[0].target == "/v1/engine/stable.json");
        OA_CHECK(requests[1].target == "/v1/engine/stable.json.sig");
        OA_CHECK(agent_and_no_cookie(requests[0]));
        OA_CHECK(agent_and_no_cookie(requests[1]));
    }
}

void current_check(fixture::Server& server) {
    const update::ThisBuild built = update::this_build({});
    const SignedFeed signed_feed =
        sign_text(feed_json(built.release, built.package), "test-pin", 0x21);
    serve(server, signed_feed);
    Scratch scratch("current");
    update::Checker checker(scratch.path);
    checker.start(test_source(server, signed_feed), built, false);
    OA_CHECK(wait_until(checker, is_current, 5000));
    const update::CheckSnapshot shot = checker.snapshot();
    OA_CHECK(shot.state == update::CheckState::up_to_date);
    OA_CHECK(shot.checked_at.has_value());
    OA_CHECK(shot.offer.way == oa::data::engine_feed::Way::none);
    OA_CHECK(read_file(scratch.path / "feed.json") == signed_feed.bytes);
}

void missing_check(fixture::Server& server) {
    server.clear_log();
    Scratch scratch("missing");
    update::Checker checker(scratch.path);
    update::Source source;
    source.feed_url = server.url("/v1/engine/absent.json");
    source.test = true;
    checker.start(source, update::this_build({}), false);
    OA_CHECK(wait_until(checker, is_failed, 5000));
    const update::CheckSnapshot shot = checker.snapshot();
    OA_CHECK(shot.state == update::CheckState::failed);
    OA_CHECK(shot.failure == "the feed answered 404");
    OA_CHECK(shot.checked_at.has_value());
    std::error_code error;
    OA_CHECK(!fs::exists(scratch.path / "feed.json", error));
}

void refused_check(fixture::Server& server) {
    const update::ThisBuild built = update::this_build({});
    const SignedFeed signed_feed =
        sign_text(feed_json(built.release + 1, built.package), "other-pin", 0x44);
    serve(server, signed_feed);
    Scratch scratch("refused");
    update::Checker checker(scratch.path);
    update::Source source = test_source(server, signed_feed);
    source.keys[0].id = "test-pin";
    source.keys[0].text = sign_text("{}", "test-pin", 0x21).key_text;
    checker.start(source, built, true);
    OA_CHECK(wait_until(checker, is_failed, 5000));
    const update::CheckSnapshot shot = checker.snapshot();
    OA_CHECK(shot.state == update::CheckState::failed);
    OA_CHECK(shot.failure == "unknown_key");
    OA_CHECK(shot.checked_at.has_value());
    std::error_code error;
    OA_CHECK(!fs::exists(scratch.path / "feed.json", error));
    OA_CHECK(!fs::exists(scratch.path / "feed.json.sig", error));
}

void cancelled_check(fixture::Server& server) {
    const update::ThisBuild built = update::this_build({});
    const SignedFeed signed_feed =
        sign_text(feed_json(built.release + 1, built.package), "test-pin", 0x21);
    fixture::Reply delayed;
    delayed.status = 200;
    delayed.head_delay_ms = 30000;
    delayed.body = signed_feed.bytes;
    server.override_next("/v1/engine/held.json", delayed, 1);
    Scratch scratch("cancelled");
    update::Checker checker(scratch.path);
    update::Source source = test_source(server, signed_feed);
    source.feed_url = server.url("/v1/engine/held.json");
    checker.start(source, built, false);
    oa::base::threads::sleep_ms(200);
    checker.cancel();
    OA_CHECK(wait_until(checker, is_cancelled, 2000));
    const update::CheckSnapshot shot = checker.snapshot();
    OA_CHECK(shot.state == update::CheckState::cancelled);
    OA_CHECK(!shot.checked_at.has_value());
    std::error_code error;
    OA_CHECK(!fs::exists(scratch.path / "feed.json", error));
}

void timeout_check(fixture::Server& server) {
    fixture::Reply delayed;
    delayed.status = 200;
    delayed.head_delay_ms = 60000;
    delayed.body = {1, 2, 3};
    server.override_next("/v1/engine/slow.json", delayed, 1);
    Scratch scratch("timeout");
    update::Checker checker(scratch.path);
    update::Source source;
    source.feed_url = server.url("/v1/engine/slow.json");
    source.test = true;
    checker.start(source, update::this_build({}), false);
    OA_CHECK(wait_until(checker, is_failed, 25000));
    const update::CheckSnapshot shot = checker.snapshot();
    OA_CHECK(shot.state == update::CheckState::failed);
    OA_CHECK(shot.failure == http::failure_text(http::Failure::timed_out));
    OA_CHECK(!shot.checked_at.has_value());
    std::error_code error;
    OA_CHECK(!fs::exists(scratch.path / "feed.json", error));
}

} // namespace

int main() {
    due_dates();
    modes_round_trip();
    fixture::Server server;
    server.set_idle_close_ms(120000);
    std::string listen_error;
    OA_CHECK(server.start(&listen_error));
    if (!listen_error.empty())
        return oa::test::check_exit_status();
    offered_check(server);
    current_check(server);
    missing_check(server);
    refused_check(server);
    cancelled_check(server);
    timeout_check(server);
    return oa::test::check_exit_status();
}

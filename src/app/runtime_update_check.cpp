// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-engine-feed: the background check for a new release, against a
// feed this check serves on 127.0.0.1. The key is made from a fixed seed.
// The published feed is not requested.

#include "oa/app/runtime.hpp"

#include "oa/app/game_directory.hpp"
#include "update_state.hpp"
#include "oa/app/update/build.hpp"
#include "oa/app/update/check.hpp"
#include "oa/base/signing/ed25519.hpp"
#include "oa/data/catalogue/check.hpp"
#include "oa/netgame/http_fixture/server.hpp"
#include "oa/ui/first_run/first_run.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace fs = std::filesystem;
namespace update = oa::app::update;
namespace signing = oa::base::signing;
namespace catalogue = oa::data::catalogue;
namespace fixture = oa::netgame::http_fixture;

constexpr std::string_view kSha =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr std::string_view kKeyId = "engine-feed-check";

/// Throws when a step of the check fails.
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("engine-feed: " + std::string(what));
}

/// Seconds since 1970.
int64_t wall_seconds() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(now).count();
}

/// One release of the stable feed, with one package. The mirror is not fetched.
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

/// Signs `json` with a key derived from `fill`. The seed and the secret are wiped.
struct SignedFeed {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> signature;
    std::string key_text;
};

SignedFeed sign_feed(std::string_view json, std::string_view key_id, uint8_t fill) {
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
    signed_feed.key_text = std::string(signing::public_key_text(public_key).view());
    return signed_feed;
}

/// Reads a file whole. Empty when it cannot be read.
std::vector<uint8_t> read_bytes(const fs::path& file) {
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

int count_target(const fixture::Server& server, std::string_view target) {
    int count = 0;
    for (const fixture::LoggedRequest& request : server.requests())
        if (request.target == target)
            ++count;
    return count;
}

/// This program's build, using the same folder the check will.
update::ThisBuild running_build() {
    const char* const base = SDL_GetBasePath();
    if (base == nullptr)
        return update::this_build({});
    return update::this_build(path_from_utf8(base));
}

} // namespace

void Runtime::check_engine_feed() {
    require(options_.data_dir.has_value(), "the run has no --data-dir");
    // A feed an earlier run of this check stored would be read again at the
    // first menu; this run starts with none.
    {
        std::error_code cleared;
        fs::remove(*options_.data_dir / "updates" / "feed.json", cleared);
        fs::remove(*options_.data_dir / "updates" / "feed.json.sig", cleared);
    }
    preference_values_[std::string(oa::ui::first_run::preference_key)] =
        std::string(oa::ui::first_run::preference_done);
    preferences_dirty_ = true;
    flush_preferences();

    const update::ThisBuild built = running_build();
    const SignedFeed offered_feed =
        sign_feed(feed_json(built.release + 1, built.package), kKeyId, 0x51);
    fixture::Server server;
    server.set_idle_close_ms(120000);
    std::string listen_error;
    require(server.start(&listen_error), "the server did not listen: " + listen_error);
    server.serve_bytes("/v1/engine/stable.json", offered_feed.bytes, "application/json");
    server.serve_bytes("/v1/engine/stable.json.sig", offered_feed.signature, "text/plain");
    options_.update_feed = server.url("/v1/engine/stable.json");
    options_.update_keys = {std::string(kKeyId) + "=" + offered_feed.key_text};

    const fs::path updates = *options_.data_dir / "updates";
    const auto deadline_from = [](std::chrono::seconds limit) {
        return std::chrono::steady_clock::now() + limit;
    };
    const auto feed_target = std::string_view{"/v1/engine/stable.json"};
    const auto signature_target = std::string_view{"/v1/engine/stable.json.sig"};
    // Moves the schedule's clock an hour past the last automatic check of
    // this run, so the next one is due without waiting out the hour.
    const auto pass_an_hour = [&]() {
        require(
            update_ != nullptr && update_->automatic_began.has_value(),
            "no automatic check has begun"
        );
        update_->schedule_now = *update_->automatic_began + update::check_interval_seconds;
    };

    // 1. Automatically, even when the server was reached a minute ago: one
    // offer from the settled menu.
    {
        update::write_checked(preference_values_, wall_seconds() - 60);
        preferences_dirty_ = true;
        flush_preferences();
        const auto deadline = deadline_from(std::chrono::seconds{30});
        while (update_snapshot().state != update::CheckState::offered) {
            require(
                std::chrono::steady_clock::now() < deadline,
                "the first check did not offer a release (" + update_snapshot().failure + ")"
            );
            idle_tick();
        }
        require(count_target(server, feed_target) == 1, "the feed was not requested once");
        require(
            count_target(server, signature_target) == 1, "the signature was not requested once"
        );
        require(server.requests().size() == 2, "the check requested something other than the feed");
        for (const fixture::LoggedRequest& request : server.requests()) {
            require(request.method == "GET", "the check did not use GET");
            require(request.target.find('?') == std::string::npos, "the check sent a query");
            const std::string* agent = request.header("User-Agent");
            require(
                agent != nullptr && agent->starts_with("OpenAnnihilation/"),
                "the check did not use the game's user agent"
            );
            require(request.header("Cookie") == nullptr, "the check sent a cookie");
        }
        require(
            update::read_checked(preference_values_).has_value(), "the check was not remembered"
        );
        require(read_bytes(updates / "feed.json") == offered_feed.bytes, "the stored feed differs");
        std::cout << "engine-feed: 1\n";
        std::cout.flush();
    }

    // 2. Again, unasked: no request. With the check marked as just done: no request.
    {
        const int feeds = count_target(server, feed_target);
        start_update_check(false);
        for (int frame = 0; frame < 5; ++frame)
            idle_tick();
        require(count_target(server, feed_target) == feeds, "a second automatic check ran");
        const int64_t now = wall_seconds();
        update::write_checked(preference_values_, now);
        preferences_dirty_ = true;
        flush_preferences();
        if (update_ != nullptr)
            update_->ran = false;
        start_update_check(false);
        for (int frame = 0; frame < 5; ++frame)
            idle_tick();
        require(count_target(server, feed_target) == feeds, "a check ran inside the hour");
        std::cout << "engine-feed: 2\n";
        std::cout.flush();
    }

    // 3. An hour after this run's automatic check began, another runs. A
    // second short of the hour does not.
    {
        const int feeds = count_target(server, feed_target);
        const int signatures = count_target(server, signature_target);
        require(
            update_ != nullptr && update_->automatic_began.has_value(), "the hour has no start"
        );
        update_->schedule_now = *update_->automatic_began + update::check_interval_seconds - 1;
        for (int frame = 0; frame < 8; ++frame)
            idle_tick();
        require(count_target(server, feed_target) == feeds, "a check ran inside the hour");
        pass_an_hour();
        const auto deadline = deadline_from(std::chrono::seconds{30});
        while (count_target(server, feed_target) == feeds) {
            require(
                std::chrono::steady_clock::now() < deadline, "the hour did not run another check"
            );
            idle_tick();
        }
        require(count_target(server, feed_target) == feeds + 1, "the hour ran more than one check");
        require(
            count_target(server, signature_target) == signatures + 1,
            "the hour did not fetch the signature"
        );
        while (update_snapshot().state == update::CheckState::checking) {
            require(std::chrono::steady_clock::now() < deadline, "the hour's check did not finish");
            idle_tick();
        }
        std::cout << "engine-feed: 3\n";
        std::cout.flush();
    }

    // 4. Never refuses even an asked check. Only when I ask waits to be asked.
    {
        const int feeds = count_target(server, feed_target);
        update::write_check_mode(preference_values_, update::CheckMode::never);
        update::write_checked(preference_values_, wall_seconds() - 90000);
        preferences_dirty_ = true;
        flush_preferences();
        if (update_ != nullptr)
            update_->ran = false;
        for (int frame = 0; frame < 8; ++frame)
            idle_tick();
        start_update_check(true);
        for (int frame = 0; frame < 4; ++frame)
            idle_tick();
        require(count_target(server, feed_target) == feeds, "Never made a request");
        update::write_check_mode(preference_values_, update::CheckMode::when_asked);
        preferences_dirty_ = true;
        flush_preferences();
        if (update_ != nullptr)
            update_->ran = false;
        for (int frame = 0; frame < 8; ++frame)
            idle_tick();
        start_update_check(false);
        for (int frame = 0; frame < 4; ++frame)
            idle_tick();
        require(count_target(server, feed_target) == feeds, "Only when I ask checked on its own");
        start_update_check(true);
        const auto deadline = deadline_from(std::chrono::seconds{30});
        while (update_snapshot().state == update::CheckState::checking ||
               count_target(server, feed_target) == feeds) {
            require(std::chrono::steady_clock::now() < deadline, "an asked check did not finish");
            idle_tick();
        }
        require(count_target(server, feed_target) == feeds + 1, "an asked check made no request");
        std::cout << "engine-feed: 4\n";
        std::cout.flush();
    }

    // 5. A slow answer leaves the menu drawing.
    {
        fixture::Reply delayed;
        delayed.status = 200;
        // The feed's own idle limit is 15 s, so the status line must start
        // before that. Twelve seconds still holds the answer out long enough
        // for a sanitizer build, where one menu frame can take the better
        // part of a second, to draw 20 frames before the answer arrives. A
        // fast machine draws them at once and then waits out the rest.
        delayed.head_delay_ms = 12000;
        delayed.body = offered_feed.bytes;
        server.override_next("/v1/engine/stable.json", std::move(delayed), 1);
        update::write_check_mode(preference_values_, update::CheckMode::automatically);
        update::write_checked(preference_values_, wall_seconds() - 90000);
        preferences_dirty_ = true;
        flush_preferences();
        if (update_ != nullptr)
            update_->ran = false;
        pass_an_hour();
        int waiting_frames = 0;
        const auto deadline = deadline_from(std::chrono::seconds{25});
        while (std::chrono::steady_clock::now() < deadline && waiting_frames < 20) {
            idle_tick();
            if (update_snapshot().state == update::CheckState::checking)
                ++waiting_frames;
        }
        require(waiting_frames >= 20, "the menu drew fewer than 20 frames while the check waited");
        const auto finish = deadline_from(std::chrono::seconds{25});
        while (update_snapshot().state == update::CheckState::checking) {
            require(std::chrono::steady_clock::now() < finish, "the slow check did not finish");
            idle_tick();
        }
        std::cout << "engine-feed: 5\n";
        std::cout.flush();
    }

    // 6. A match started while the answer is delayed cancels the check.
    {
        fixture::Reply delayed;
        delayed.status = 200;
        delayed.head_delay_ms = 60000;
        delayed.body = offered_feed.bytes;
        server.override_next("/v1/engine/stable.json", std::move(delayed), 1);
        update::write_checked(preference_values_, 1000);
        preferences_dirty_ = true;
        flush_preferences();
        if (update_ != nullptr)
            update_->ran = false;
        pass_an_hour();
        const auto deadline = deadline_from(std::chrono::seconds{20});
        while (update_snapshot().state != update::CheckState::checking) {
            require(std::chrono::steady_clock::now() < deadline, "the check did not start");
            idle_tick();
        }
        start_benchmark_skirmish();
        const auto cancel_deadline = deadline_from(std::chrono::seconds{20});
        while (update_snapshot().state != update::CheckState::cancelled) {
            require(
                std::chrono::steady_clock::now() < cancel_deadline,
                "the match did not cancel the check (" + update_snapshot().failure + ")"
            );
            idle_tick();
        }
        const std::optional<int64_t> remembered = update::read_checked(preference_values_);
        require(remembered.has_value() && *remembered == 1000, "a cancelled check was remembered");
        leave_match();
        load(Screen::main_menu);
        for (int frame = 0; frame < 4; ++frame)
            idle_tick();
        require(screen_ != Screen::match, "the match was still showing");
        require(
            update_snapshot().state != update::CheckState::checking,
            "the cancelled check was still running"
        );
        std::cout << "engine-feed: 6\n";
        std::cout.flush();
    }

    // 7. A feed signed by another key is refused and not stored.
    {
        std::error_code io;
        fs::remove(updates / "feed.json", io);
        fs::remove(updates / "feed.json.sig", io);
        const SignedFeed other =
            sign_feed(feed_json(built.release + 2, built.package), "other-key", 0x52);
        server.serve_bytes("/v1/engine/stable.json", other.bytes, "application/json");
        server.serve_bytes("/v1/engine/stable.json.sig", other.signature, "text/plain");
        update::write_check_mode(preference_values_, update::CheckMode::automatically);
        preferences_dirty_ = true;
        flush_preferences();
        start_update_check(true);
        const auto deadline = deadline_from(std::chrono::seconds{30});
        while (update_snapshot().state != update::CheckState::failed) {
            require(
                std::chrono::steady_clock::now() < deadline,
                "the refused feed did not fail (" + update_snapshot().failure + ")"
            );
            idle_tick();
        }
        require(update_snapshot().failure == "unknown_key", "the refusal was not unknown_key");
        require(!fs::exists(updates / "feed.json", io), "a refused feed was stored");
        require(!fs::exists(updates / "feed.json.sig", io), "a refused signature was stored");
        std::cout << "engine-feed: 7\n";
        std::cout.flush();
    }

    // 8. An unattended run with no named feed makes no request.
    {
        while (update_snapshot().state == update::CheckState::checking)
            idle_tick();
        const uint64_t generation = update_snapshot().generation;
        server.clear_log();
        options_.update_feed.reset();
        options_.update_keys.clear();
        options_.unattended = true;
        update::write_check_mode(preference_values_, update::CheckMode::automatically);
        update::write_checked(preference_values_, wall_seconds() - 90000);
        preferences_dirty_ = true;
        flush_preferences();
        if (update_ != nullptr)
            update_->ran = false;
        for (int frame = 0; frame < 40; ++frame)
            idle_tick();
        start_update_check(false);
        start_update_check(true);
        for (int frame = 0; frame < 4; ++frame)
            idle_tick();
        require(server.requests().empty(), "an unattended run with no feed made a request");
        require(update_snapshot().generation == generation, "an unattended run started a check");
        require(
            update_snapshot().state != update::CheckState::checking,
            "an unattended run left a check running"
        );
        std::cout << "engine-feed: 8\n";
        std::cout.flush();
    }
}

} // namespace oa::app

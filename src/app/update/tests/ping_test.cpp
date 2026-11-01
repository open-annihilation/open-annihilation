// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The report of a check: its body, which addresses count as this machine,
// and one report posted to a server on 127.0.0.1. Nothing is sent anywhere
// else.

#include "oa/app/update/ping.hpp"

#include "oa/base/threads.hpp"
#include "oa/netgame/http_fixture/server.hpp"
#include "oa/test/check.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace update = oa::app::update;
namespace fixture = oa::netgame::http_fixture;

/// Waits until the report is no longer on its way, or the time runs out.
update::PingSnapshot settled(const update::Pinger& pinger, int milliseconds) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    update::PingSnapshot shot = pinger.snapshot();
    while (shot.state == update::PingState::sending &&
           std::chrono::steady_clock::now() < deadline) {
        oa::base::threads::sleep_ms(20);
        shot = pinger.snapshot();
    }
    return shot;
}

/// A report of 0.8.12 on a Mac, on the stable channel.
update::PingReport mac_report() {
    update::PingReport report;
    report.version = "0.8.12";
    report.platform = "macos-universal";
    report.channel = "stable";
    report.install_id = "7f3a90d2-1b2c-4d5e-8f90-a1b2c3d4c91e";
    return report;
}

void bodies() {
    update::PingReport report = mac_report();
    OA_CHECK(
        update::ping_body(report) ==
        R"({"version":"0.8.12","platform":"macos-universal","channel":"stable",)"
        R"("install_id":"7f3a90d2-1b2c-4d5e-8f90-a1b2c3d4c91e","trigger":"automatic"})"
    );
    report.asked = true;
    report.channel = "testing";
    report.install_id.clear();
    OA_CHECK(
        update::ping_body(report) ==
        R"({"version":"0.8.12","platform":"macos-universal","channel":"testing",)"
        R"("install_id":"","trigger":"manual"})"
    );
}

/// A release build of each package the releases are built for reports; a
/// labelled build and a build of another package, such as iOS, do not.
void builds_that_report() {
    update::ThisBuild build;
    build.version_text = "0.8.12";
    for (const std::string_view platform : update::reported_platforms) {
        build.package = std::string(platform);
        OA_CHECK(update::build_reports(build));
    }
    build.package = "macos-universal";
    build.development = true;
    OA_CHECK(!update::build_reports(build));
    build.development = false;
    for (const char* other : {"ios", "android", "", "windows-x64-setup"}) {
        build.package = other;
        OA_CHECK(!update::build_reports(build));
    }
    build.package = "windows-x64";
    build.version_text.clear();
    OA_CHECK(!update::build_reports(build));
}

void local_addresses() {
    OA_CHECK(update::local_address("http://127.0.0.1:8123/v1/engine/pings"));
    OA_CHECK(update::local_address("http://127.1.2.3/pings"));
    OA_CHECK(update::local_address("https://localhost/pings"));
    OA_CHECK(!update::local_address("https://telemetry.example.net/v1/engine/pings"));
    OA_CHECK(!update::local_address("http://128.0.0.1/pings"));
    OA_CHECK(!update::local_address("http://127.0.0.1.example.net/pings"));
    OA_CHECK(!update::local_address("ftp://127.0.0.1/pings"));
    OA_CHECK(!update::local_address(""));
}

/// One report is one POST of the body, with the game's user agent, the JSON
/// type and no cookie or query. The answer's status is kept.
void one_report(fixture::Server& server) {
    server.handle("POST", "/v1/engine/pings", [](const fixture::LoggedRequest&) {
        fixture::Reply reply;
        reply.status = 204;
        return reply;
    });
    server.clear_log();
    update::Pinger pinger;
    OA_CHECK(pinger.snapshot().state == update::PingState::idle);
    OA_CHECK(pinger.send(server.url("/v1/engine/pings"), mac_report()));
    const update::PingSnapshot shot = settled(pinger, 10000);
    OA_CHECK(shot.state == update::PingState::sent);
    OA_CHECK(shot.status == 204);
    const std::vector<fixture::LoggedRequest> requests = server.requests();
    OA_CHECK(requests.size() == 1);
    if (requests.size() != 1)
        return;
    const fixture::LoggedRequest& posted = requests[0];
    OA_CHECK(posted.method == "POST");
    OA_CHECK(posted.target == "/v1/engine/pings");
    const std::string body(posted.body.begin(), posted.body.end());
    OA_CHECK(body == update::ping_body(mac_report()));
    const std::string* type = posted.header("Content-Type");
    OA_CHECK(type != nullptr && *type == "application/json");
    const std::string* agent = posted.header("User-Agent");
    OA_CHECK(agent != nullptr && agent->starts_with("OpenAnnihilation/"));
    OA_CHECK(posted.header("Cookie") == nullptr);
}

/// A refused report, and one no server answers, fail with the reason and
/// leave the pinger ready for the next.
void failed_reports(fixture::Server& server) {
    server.handle("POST", "/v1/engine/refused", [](const fixture::LoggedRequest&) {
        fixture::Reply reply;
        reply.status = 400;
        return reply;
    });
    update::Pinger pinger;
    OA_CHECK(pinger.send(server.url("/v1/engine/refused"), mac_report()));
    const update::PingSnapshot refused = settled(pinger, 10000);
    OA_CHECK(refused.state == update::PingState::failed);
    OA_CHECK(refused.status == 400);
    OA_CHECK(refused.failure == "the server answered 400");

    // Port 9 on this machine takes no connection.
    OA_CHECK(pinger.send("http://127.0.0.1:9/v1/engine/pings", mac_report()));
    const update::PingSnapshot unanswered = settled(pinger, 20000);
    OA_CHECK(unanswered.state == update::PingState::failed);
    OA_CHECK(unanswered.status == 0);
    OA_CHECK(!unanswered.failure.empty());
    OA_CHECK(unanswered.generation > refused.generation);
}

/// A report still on its way keeps the next one out, and the pinger can be
/// destroyed while it waits.
void one_at_a_time(fixture::Server& server) {
    fixture::Reply slow;
    slow.status = 204;
    slow.head_delay_ms = 30000;
    server.override_next("/v1/engine/slow-pings", slow, 1);
    server.clear_log();
    const auto began = std::chrono::steady_clock::now();
    {
        update::Pinger pinger;
        OA_CHECK(pinger.send(server.url("/v1/engine/slow-pings"), mac_report()));
        OA_CHECK(!pinger.send(server.url("/v1/engine/slow-pings"), mac_report()));
        OA_CHECK(pinger.snapshot().state == update::PingState::sending);
        oa::base::threads::sleep_ms(200);
    }
    // Leaving did not wait out the server.
    OA_CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(10));
}

} // namespace

int main() {
    bodies();
    builds_that_report();
    local_addresses();
    fixture::Server server;
    server.set_idle_close_ms(120000);
    std::string listen_error;
    OA_CHECK(server.start(&listen_error));
    if (!listen_error.empty())
        return oa::test::check_exit_status();
    one_report(server);
    failed_reports(server);
    one_at_a_time(server);
    return oa::test::check_exit_status();
}

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Core Prime's replay feed: a page, one replay with its needs, the last
// page's empty cursor, a member this version does not know, a damaged feed,
// an empty feed, an error, and an id that is not accepted.

#include "oa/data/replays/feed.hpp"
#include "oa/test/check.hpp"

#include <string>
#include <string_view>

namespace {

namespace replays = oa::data::replays;

constexpr std::string_view kId = "r_0123456789abcdefghijAB";
constexpr std::string_view kOther = "r_0123456789abcdefghijAC";
constexpr std::string_view kRidgeSha =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kPackSha =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr std::string_view kDemoSha =
    "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";

std::string one_entry(std::string_view id, bool needs, std::string_view extra) {
    std::string text;
    text += '{';
    text += "\"id\":\"";
    text += id;
    text += "\",\"revision\":3,\"title\":\"Show Down\",\"source\":\"capture\","
            "\"visibility\":\"public\",\"recorded_at\":\"2026-11-04T14:32:10Z\","
            "\"length_ms\":1432960,\"game\":{\"kind\":\"outpost\",\"game_id\":\"5f2e9a0c41b7d3\","
            "\"planet\":\"Ashmere\",\"sector\":\"North Span\",\"spare\":true},"
            "\"map\":{\"name\":\"Show Down\"},"
            "\"players\":[{\"name\":\"NJ_THUG\",\"side\":\"arm\",\"colour\":0,\"team\":1,"
            "\"result\":\"won\",\"kills\":41}],"
            "\"mod\":{\"id\":\"ridge\",\"name\":\"Ridge\",\"version\":\"2.8\",\"revision\":1},"
            "\"engine_needs\":\">=0.9.1\",\"uploader\":\"\",\"downloads\":12,";
    text += extra;
    text += "\"files\":{\"oademo\":{\"sha256\":\"";
    text += kDemoSha;
    text += "\",\"size\":1048576},\"tad\":{\"sha256\":\"";
    text += kRidgeSha;
    text += "\",\"size\":1313354}}";
    if (needs) {
        text += ",\"needs\":[{\"kind\":\"oamod\",\"id\":\"ridge\",\"version\":\"2.8\","
                "\"revision\":1,\"sha256\":\"";
        text += kRidgeSha;
        text += "\"},{\"kind\":\"oamap\",\"id\":\"ridge-reach\",\"version\":\"1\","
                "\"revision\":2,\"sha256\":\"";
        text += kPackSha;
        text += "\",\"maps\":[{\"name\":\"Show Down\",\"sha256\":\"";
        text += kDemoSha;
        text += "\"},{\"name\":\"Twin Rivers\",\"sha256\":\"";
        text += kRidgeSha;
        text += "\"}]}]";
    }
    text += '}';
    return text;
}

void a_page_keeps_its_replays_and_ignores_an_unknown_member() {
    const std::string body = std::string(
                                 "{\"replays\":1,\"total\":50,\"next\":\"page-2\","
                                 "\"beacon\":true,\"items\":["
                             ) +
                             one_entry(kId, false, "\"note\":\"kept\",") + "," +
                             one_entry("not-an-id", false, "") + "," +
                             one_entry(kOther, false, "") + "]}";
    const replays::FeedPageResult page = replays::parse_replay_page(body);
    OA_CHECK(page.status == replays::FeedStatus::page);
    OA_CHECK(page.page.total == 50);
    OA_CHECK(page.page.next == "page-2");
    OA_CHECK(page.page.items.size() == 2);
    if (page.page.items.size() == 2) {
        const replays::FeedEntry& entry = page.page.items[0];
        OA_CHECK(entry.id == kId);
        OA_CHECK(entry.revision == 3);
        OA_CHECK(entry.title == "Show Down");
        OA_CHECK(entry.source == "capture");
        OA_CHECK(entry.recorded == 1793802730);
        OA_CHECK(entry.length_ms == 1432960);
        OA_CHECK(entry.game_kind == "outpost");
        OA_CHECK(entry.planet == "Ashmere");
        OA_CHECK(entry.sector == "North Span");
        OA_CHECK(entry.map_name == "Show Down");
        OA_CHECK(entry.players.size() == 1);
        OA_CHECK(entry.players.size() == 1 && entry.players[0].colour == 0);
        OA_CHECK(entry.players.size() == 1 && entry.players[0].kills == 41);
        OA_CHECK(entry.mod.has_value() && entry.mod->id == "ridge");
        OA_CHECK(entry.mod.has_value() && entry.mod->name == "Ridge");
        OA_CHECK(entry.downloads == 12);
        OA_CHECK(entry.oademo.sha256 == kDemoSha);
        OA_CHECK(entry.oademo.size == 1048576);
        OA_CHECK(entry.tad.size == 1313354);
        OA_CHECK(entry.replay.kind == "outpost");
        OA_CHECK(entry.replay.planet == "Ashmere");
        OA_CHECK(page.page.items[1].id == kOther);
    }
}

void the_last_page_has_an_empty_cursor() {
    const std::string body = std::string("{\"replays\":1,\"total\":1,\"next\":\"\",\"items\":[") +
                             one_entry(kId, false, "") + "]}";
    const replays::FeedPageResult page = replays::parse_replay_page(body);
    OA_CHECK(page.status == replays::FeedStatus::page);
    OA_CHECK(page.page.next.empty());
    OA_CHECK(page.page.items.size() == 1);
}

void one_replay_carries_its_needs() {
    const replays::FeedEntryResult entry = replays::parse_replay_entry(one_entry(kId, true, ""));
    OA_CHECK(entry.status == replays::FeedStatus::entry);
    OA_CHECK(entry.entry.needs_present);
    OA_CHECK(entry.entry.replay.engine_needs == ">=0.9.1");
    OA_CHECK(entry.entry.replay.needs.size() == 3);
    if (entry.entry.replay.needs.size() == 3) {
        OA_CHECK(entry.entry.replay.needs[0].kind == "oamod");
        OA_CHECK(entry.entry.replay.needs[0].id == "ridge");
        OA_CHECK(entry.entry.replay.needs[0].name == "Ridge");
        OA_CHECK(entry.entry.replay.needs[0].version == "2.8");
        OA_CHECK(entry.entry.replay.needs[0].revision == 1);
        OA_CHECK(entry.entry.replay.needs[1].kind == "oamap");
        OA_CHECK(entry.entry.replay.needs[1].map_name == "Show Down");
        OA_CHECK(entry.entry.replay.needs[1].id == "ridge-reach");
        OA_CHECK(entry.entry.replay.needs[2].map_name == "Twin Rivers");
    }
    OA_CHECK(entry.entry.mod.has_value());
}

void a_null_mod_is_the_base_game() {
    std::string body = one_entry(kId, false, "");
    const auto at = body.find("\"mod\":{");
    OA_CHECK(at != std::string::npos);
    if (at != std::string::npos) {
        const auto end = body.find('}', at);
        body.replace(at, end - at + 1, "\"mod\":null");
    }
    const replays::FeedEntryResult entry = replays::parse_replay_entry(body);
    OA_CHECK(entry.status == replays::FeedStatus::entry);
    OA_CHECK(!entry.entry.mod.has_value());
    OA_CHECK(entry.entry.replay.mod_id.empty());
}

void a_damaged_feed_and_an_empty_feed_cannot_be_read() {
    OA_CHECK(replays::parse_replay_page("").status == replays::FeedStatus::unreadable);
    OA_CHECK(replays::parse_replay_page("   ").status == replays::FeedStatus::unreadable);
    OA_CHECK(replays::parse_replay_page("{").status == replays::FeedStatus::unreadable);
    OA_CHECK(
        replays::parse_replay_page("{\"replays\":2,\"items\":[]}").status ==
        replays::FeedStatus::unreadable
    );
    OA_CHECK(replays::parse_replay_entry("").status == replays::FeedStatus::unreadable);
    OA_CHECK(replays::parse_replay_entry("[]").status == replays::FeedStatus::unreadable);
}

void an_error_answer_is_an_error() {
    const replays::FeedPageResult page = replays::parse_replay_page(
        "{\"error\":\"unknown_replay\",\"message\":\"That replay is gone.\"}"
    );
    OA_CHECK(page.status == replays::FeedStatus::error);
    OA_CHECK(page.failure.error == "unknown_replay");
    OA_CHECK(page.failure.message == "That replay is gone.");
    const replays::FeedEntryResult entry = replays::parse_replay_entry(
        "{\"error\":\"replays_unavailable\",\"message\":\"Core Prime's replays are not available "
        "just now.\"}"
    );
    OA_CHECK(entry.status == replays::FeedStatus::error);
    OA_CHECK(entry.failure.error == "replays_unavailable");
}

void an_id_that_is_not_the_form_is_refused() {
    OA_CHECK(!replays::replay_id_ok(""));
    OA_CHECK(!replays::replay_id_ok("r_short"));
    OA_CHECK(!replays::replay_id_ok("x_0123456789abcdefghijAB"));
    OA_CHECK(!replays::replay_id_ok("r_0123456789abcdefghijA!"));
    OA_CHECK(replays::replay_id_ok(kId));
    const replays::FeedEntryResult entry =
        replays::parse_replay_entry(one_entry("r_short", false, ""));
    OA_CHECK(entry.status == replays::FeedStatus::refused);
    OA_CHECK(replays::feed_entry_url("http://127.0.0.1:9", "r_short").empty());
    const std::string address = replays::feed_entry_url("http://127.0.0.1:9/", kId);
    OA_CHECK(address == "http://127.0.0.1:9/content/v1/replays/" + std::string(kId));
}

void a_query_names_the_planet_and_the_page() {
    replays::FeedQuery query;
    query.sort = "downloads";
    query.planet = "Ashmere Reach";
    query.after = "page-2";
    query.first = 48;
    const std::string text = replays::feed_query_text(query);
    OA_CHECK(text.find("sort=downloads") != std::string::npos);
    OA_CHECK(text.find("planet=Ashmere%20Reach") != std::string::npos);
    OA_CHECK(text.find("after=page-2") != std::string::npos);
    OA_CHECK(text.find("first=48") != std::string::npos);
    OA_CHECK(text.find("kind=") == std::string::npos);
    query.first = 0;
    OA_CHECK(replays::feed_query_text(query).find("first=48") != std::string::npos);
    query.first = 500;
    OA_CHECK(replays::feed_query_text(query).find("first=100") != std::string::npos);
    const std::string url = replays::feed_list_url(replays::core_prime_feed_origin, query);
    OA_CHECK(url.starts_with("https://api.coreprime.net/content/v1/replays?"));
}

} // namespace

void test_replay_feed() {
    a_page_keeps_its_replays_and_ignores_an_unknown_member();
    the_last_page_has_an_empty_cursor();
    one_replay_carries_its_needs();
    a_null_mod_is_the_base_game();
    a_damaged_feed_and_an_empty_feed_cannot_be_read();
    an_error_answer_is_an_error();
    an_id_that_is_not_the_form_is_refused();
    a_query_names_the_planet_and_the_page();
}

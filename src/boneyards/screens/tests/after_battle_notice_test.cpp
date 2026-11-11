// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// After a battle: a Duel says it goes on Core Prime by itself, a For Fun
// offers a share, and the Library card follows a loopback feed.

#include "oa/boneyards/screens/after_battle.hpp"
#include "oa/boneyards/screens/locator.hpp"
#include "oa/boneyards/screens/matchmaking.hpp"
#include "oa/boneyards/screens/quickplay.hpp"
#include "oa/data/replays/feed.hpp"
#include "oa/netgame/http/client.hpp"
#include "oa/netgame/http_fixture/server.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/library/replays.hpp"
#include "oa/ui/screen_registry.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace screens = oa::boneyards::screens;
namespace http = oa::netgame::http;
namespace feed = oa::data::replays;
namespace library = oa::ui::library;

constexpr std::string_view kRanked = screens::kAfterBattleRanked;
constexpr std::string_view kShare = screens::kAfterBattleShare;
constexpr std::string_view kListedId = "r_0123456789abcdefghijAB";
constexpr std::string_view kGameId = "5f2e9a0c41b7d3";

struct Heard {
    int calls{};
    int notices{};
    int prompts{};
    std::string title;
    std::string body;
    std::vector<std::string> buttons;
    int32_t answered{-2};
};

uint32_t hear_notice(void* host, const oa::app::NoticeRequest* request) {
    auto* heard = static_cast<Heard*>(host);
    ++heard->calls;
    ++heard->notices;
    if (request == nullptr)
        return 1;
    if (request->title != nullptr)
        heard->title = request->title;
    if (request->paragraph_count > 0 && request->paragraphs != nullptr &&
        request->paragraphs[0] != nullptr)
        heard->body = request->paragraphs[0];
    return 1;
}

uint32_t hear_prompt(void* host, const oa::app::PromptRequest* request) {
    auto* heard = static_cast<Heard*>(host);
    ++heard->calls;
    ++heard->prompts;
    if (request == nullptr)
        return 1;
    if (request->title != nullptr)
        heard->title = request->title;
    if (request->paragraph_count > 0 && request->paragraphs != nullptr &&
        request->paragraphs[0] != nullptr)
        heard->body = request->paragraphs[0];
    heard->buttons.clear();
    if (request->buttons != nullptr) {
        for (std::size_t index = 0; index < request->button_count; ++index) {
            if (request->buttons[index] != nullptr)
                heard->buttons.emplace_back(request->buttons[index]);
        }
    }
    if (request->answered != nullptr)
        request->answered(request->user, 1, 0);
    heard->answered = 0;
    return 1;
}

oa::app::ScreenServices services_for(Heard* heard) {
    oa::app::ScreenServices services{};
    services.show_notice = hear_notice;
    services.ask_prompt = hear_prompt;
    (void)heard;
    return services;
}

void reach_the_game() {
    screens::begin_after_battle();
    screens::mark_after_battle_played();
}

void show(Heard& heard) {
    oa::app::ScreenServices services = services_for(&heard);
    screens::show_after_battle_notice(&services, &heard);
}

void copy_text(char* out, std::size_t capacity, const char* text) {
    std::size_t written = 0;
    if (text != nullptr) {
        while (text[written] != '\0' && written + 1 < capacity) {
            out[written] = text[written];
            ++written;
        }
    }
    out[written] = '\0';
}

void the_wire_types_pick_a_sentence() {
    OA_CHECK(screens::after_battle_sentence(0) == kShare);
    OA_CHECK(screens::after_battle_sentence(1) == kRanked);
    OA_CHECK(screens::after_battle_sentence(2) == kRanked);
    OA_CHECK(screens::after_battle_sentence(4) == kRanked);
    OA_CHECK(screens::after_battle_sentence(7) == kRanked);
    OA_CHECK(screens::after_battle_sentence(-1).empty());
    OA_CHECK(screens::after_battle_sentence(3).empty());
}

void an_unknown_type_shows_nothing() {
    Heard heard;
    screens::arm_after_battle(-1);
    reach_the_game();
    show(heard);
    OA_CHECK(heard.calls == 0);
}

void a_launch_that_never_reached_the_game_shows_nothing() {
    screens::arm_after_battle(0);
    screens::begin_after_battle();
    Heard heard;
    show(heard);
    OA_CHECK(heard.calls == 0);
    OA_CHECK(screens::after_battle_shown() == 0 || heard.calls == 0);
}

void an_arm_without_a_launch_shows_nothing() {
    screens::arm_after_battle(1);
    screens::mark_after_battle_played();
    Heard heard;
    show(heard);
    OA_CHECK(heard.calls == 0);
}

void a_for_fun_launch_offers_a_share() {
    screens::QuickPlay play{};
    play.offer_open = true;
    play.offer_role = 0;
    play.offer_game_known = true;
    play.offer_game_type = 0;
    const screens::QuickPlayEffect effect = screens::quickplay_decide(&play, 1);
    OA_CHECK(effect.launch_join);
    reach_the_game();
    const int shown = screens::after_battle_shown();
    Heard heard;
    show(heard);
    OA_CHECK(heard.prompts == 1);
    OA_CHECK(heard.notices == 0);
    OA_CHECK(heard.title == screens::kAfterBattleTitle);
    OA_CHECK(heard.body == kShare);
    OA_CHECK(heard.buttons.size() == 2);
    OA_CHECK(heard.buttons[0] == screens::kAfterBattleShareButton);
    OA_CHECK(heard.buttons[1] == screens::kAfterBattleLater);
    OA_CHECK(screens::take_after_battle_share());
    OA_CHECK(!screens::take_after_battle_share());
    OA_CHECK(screens::after_battle_shown() == shown + 1);
    OA_CHECK(std::string_view(screens::after_battle_picture_name()) == "notice-share");
}

void an_unrecorded_for_fun_battle_is_a_statement() {
    screens::arm_after_battle(0);
    screens::begin_after_battle();
    screens::mark_after_battle_played();
    screens::mark_after_battle_recorded(false);
    const int shown = screens::after_battle_shown();
    Heard heard;
    show(heard);
    OA_CHECK(heard.notices == 1);
    OA_CHECK(heard.prompts == 0);
    OA_CHECK(heard.body == screens::kAfterBattleUnrecorded);
    OA_CHECK(heard.body.find('?') == std::string::npos);
    OA_CHECK(!screens::take_after_battle_share());
    OA_CHECK(screens::after_battle_shown() == shown + 1);
    OA_CHECK(std::string_view(screens::after_battle_picture_name()) == "notice-ranked");
}

void a_duel_host_says_it_goes_by_itself() {
    screens::QuickPlay play{};
    play.offer_role = 1;
    play.offer_game_type = 1;
    oa::boneyards::HostGameRequest request{};
    OA_CHECK(screens::quickplay_host_request(&play, &request));
    OA_CHECK(request.game_mode == 1);
    reach_the_game();
    Heard heard;
    show(heard);
    OA_CHECK(heard.notices == 1);
    OA_CHECK(heard.prompts == 0);
    OA_CHECK(heard.body == kRanked);
    OA_CHECK(heard.body.find('?') == std::string::npos);
    OA_CHECK(std::string_view(screens::after_battle_picture_name()) == "notice-ranked");
}

void a_winner_takes_all_host_says_the_same() {
    oa::boneyards::MatchmakingGoNotice go{};
    go.game_type = 2;
    go.result = 0;
    go.role = 1;
    oa::boneyards::HostGameRequest request{};
    OA_CHECK(screens::matchmaking_host_request(&go, &request));
    reach_the_game();
    Heard heard;
    show(heard);
    OA_CHECK(heard.body == kRanked);
    OA_CHECK(heard.notices == 1);
}

oa::boneyards::ServiceResult join_game(
    void* user, uint32_t /*area*/, const char* /*game*/, const char* /*password*/
) {
    auto* calls = static_cast<int*>(user);
    ++*calls;
    return oa::boneyards::ServiceResult::ok;
}

void a_joined_battle_uses_its_type() {
    screens::Locator locator{};
    locator.kind = screens::locator_kind::battle;
    locator.result_count = 1;
    locator.selected = 0;
    copy_text(locator.results[0].game_id, sizeof locator.results[0].game_id, "abc");
    locator.results[0].battle_type = 4;
    locator.results[0].password = false;
    int joins = 0;
    oa::boneyards::BoneyardsService service{};
    service.join_game = join_game;
    service.user = &joins;
    locator.service = &service;
    screens::locator_extra(&locator);
    OA_CHECK(joins == 1);
    reach_the_game();
    Heard heard;
    show(heard);
    OA_CHECK(heard.body == kRanked);
    OA_CHECK(heard.notices == 1);
}

void a_password_battle_arms_nothing() {
    screens::Locator locator{};
    locator.kind = screens::locator_kind::battle;
    locator.result_count = 1;
    locator.selected = 0;
    copy_text(locator.results[0].game_id, sizeof locator.results[0].game_id, "abc");
    locator.results[0].battle_type = 0;
    locator.results[0].password = true;
    int joins = 0;
    oa::boneyards::BoneyardsService service{};
    service.join_game = join_game;
    service.user = &joins;
    locator.service = &service;
    screens::locator_extra(&locator);
    OA_CHECK(joins == 0);
    reach_the_game();
    Heard heard;
    show(heard);
    OA_CHECK(heard.calls == 0);
}

// Sequences a stale type could reach: none shows the wrong question.
void a_stale_for_fun_arm_never_follows_a_ladder_launch() {
    screens::forget_after_battle();
    // A For Fun join was sent and nothing came back.
    screens::arm_after_battle(0);
    // The next press hosts a Duel.
    screens::QuickPlay play{};
    play.offer_role = 1;
    play.offer_game_type = 1;
    oa::boneyards::HostGameRequest request{};
    OA_CHECK(screens::quickplay_host_request(&play, &request));
    reach_the_game();
    Heard heard;
    show(heard);
    OA_CHECK(heard.prompts == 0);
    OA_CHECK(heard.notices == 1);
    OA_CHECK(heard.body == kRanked);
}

void a_refused_launch_leaves_no_type() {
    screens::forget_after_battle();
    screens::arm_after_battle(0);
    screens::forget_after_battle();
    // A launch that nothing armed: the module cannot tell the type.
    reach_the_game();
    Heard heard;
    show(heard);
    OA_CHECK(heard.calls == 0);
}

void a_return_without_a_match_leaves_nothing_behind() {
    screens::forget_after_battle();
    screens::arm_after_battle(0);
    screens::begin_after_battle();
    Heard first;
    show(first);
    OA_CHECK(first.calls == 0);
    // A skirmish later starts a match; a second return shows nothing.
    screens::mark_after_battle_played();
    Heard second;
    show(second);
    OA_CHECK(second.calls == 0);
}

void a_ladder_after_a_for_fun_is_a_statement() {
    screens::forget_after_battle();
    screens::arm_after_battle(0);
    reach_the_game();
    Heard fun;
    show(fun);
    OA_CHECK(fun.prompts == 1);
    (void)screens::take_after_battle_share();
    screens::arm_after_battle(4);
    reach_the_game();
    Heard war;
    show(war);
    OA_CHECK(war.prompts == 0);
    OA_CHECK(war.notices == 1);
    OA_CHECK(war.body == kRanked);
    OA_CHECK(std::string_view(screens::after_battle_picture_name()) == "notice-ranked");
}

std::string one_entry(std::string_view id, std::string_view game_id) {
    std::string text = "{\"id\":\"";
    text += id;
    text += "\",\"revision\":1,\"title\":\"Show Down\",\"source\":\"capture\","
            "\"visibility\":\"public\",\"recorded_at\":\"2026-11-04T14:32:10Z\","
            "\"length_ms\":1,\"game\":{\"kind\":\"boneyards\",\"game_id\":\"";
    text += game_id;
    text += "\"},\"map\":{\"name\":\"Show Down\"},"
            "\"players\":[{\"name\":\"NJ_THUG\",\"side\":\"arm\",\"colour\":0,"
            "\"result\":\"won\",\"kills\":1}],\"downloads\":0,"
            "\"files\":{\"oademo\":{\"sha256\":"
            "\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\","
            "\"size\":1},\"tad\":{\"sha256\":"
            "\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\","
            "\"size\":1}}}";
    return text;
}

std::string page_of(std::string_view game_id) {
    return std::string("{\"replays\":1,\"total\":1,\"items\":[") + one_entry(kListedId, game_id) +
           "]}";
}

std::vector<uint8_t> bytes_of(std::string_view text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

bool button_is(
    const std::vector<library::ReplayButton>& buttons, library::Action action, std::string_view text
) {
    for (const library::ReplayButton& button : buttons) {
        if (button.action == action && button.text == text)
            return true;
    }
    return false;
}

library::Library card_for(library::CoreListing listing, std::string_view feed_id) {
    library::Library model;
    library::ReplayRow row;
    row.replay.path = "battle.oademo";
    row.replay.kind = "boneyards";
    row.replay.game_id = std::string(kGameId);
    row.replay.map = "Show Down";
    row.day = 20000;
    model.inputs.replays.rows.push_back(std::move(row));
    model.inputs.replays.today = 20000;
    library::update_replays(model);
    model.inputs.replays.core_signed_in = true;
    model.inputs.replays.core_sign_in_offered = true;
    model.inputs.replays.core_handle = "NJ_THUG";
    model.inputs.replays.core_battle = true;
    model.inputs.replays.core_listing_path = "battle.oademo";
    model.inputs.replays.core_listing = listing;
    model.inputs.replays.core_listing_id = std::string(feed_id);
    return model;
}

void the_card_follows_the_feed() {
    oa::netgame::http_fixture::Server server;
    OA_CHECK(server.start(nullptr));
    server.serve_bytes("/content/v1/replays", bytes_of(page_of(kGameId)), "application/json");
    feed::FeedQuery query;
    query.sort = "newest";
    query.kind = "boneyards";
    query.player = "NJ_THUG";
    query.first = feed::replay_page_most;
    const std::string url = feed::feed_list_url(server.url(""), query);
    OA_CHECK(url.find("127.0.0.1") != std::string::npos);
    OA_CHECK(url.find("coreprime.net") == std::string::npos);
    http::Request request;
    request.url = url;
    request.timeouts.connect_ms = 2000;
    request.timeouts.idle_ms = 2000;
    request.timeouts.total_ms = 10000;
    std::vector<uint8_t> body;
    const http::Response response = http::Client{}.fetch(request, body);
    OA_CHECK(response.failure == http::Failure::none);
    OA_CHECK(response.status == 200);
    const std::string text(body.begin(), body.end());
    const feed::FeedPageResult page = feed::parse_replay_page(text);
    OA_CHECK(page.status == feed::FeedStatus::page);
    bool listed = false;
    std::string listed_id;
    for (const feed::FeedEntry& item : page.page.items) {
        if (item.game_id == kGameId && !item.game_id.empty()) {
            listed = true;
            listed_id = item.id;
        }
    }
    OA_CHECK(listed);
    OA_CHECK(listed_id == kListedId);
    const std::vector<oa::netgame::http_fixture::LoggedRequest> seen = server.requests();
    OA_CHECK(!seen.empty());
    OA_CHECK(seen.back().target.find("kind=boneyards") != std::string::npos);
    OA_CHECK(seen.back().target.find("player=NJ_THUG") != std::string::npos);

    const library::Library on_prime = card_for(library::CoreListing::listed, listed_id);
    const library::ReplayRow* row = library::selected_replay(on_prime);
    OA_CHECK(row != nullptr);
    const std::string note = library::replay_note(on_prime, *row);
    OA_CHECK(note.find("On Core Prime") != std::string::npos);
    const std::vector<library::ReplayButton> listed_buttons = library::replay_buttons(on_prime);
    OA_CHECK(button_is(listed_buttons, library::Action::open_shared_replay, "Open on Core Prime"));
    OA_CHECK(button_is(listed_buttons, library::Action::share_replay, "Share my own recording"));
    OA_CHECK(!button_is(listed_buttons, library::Action::share_replay, "Share on Core Prime…"));
    library::Library signed_out = card_for(library::CoreListing::listed, listed_id);
    signed_out.inputs.replays.core_signed_in = false;
    signed_out.inputs.replays.core_handle.clear();
    const std::vector<library::ReplayButton> out_buttons = library::replay_buttons(signed_out);
    OA_CHECK(button_is(out_buttons, library::Action::open_shared_replay, "Open on Core Prime"));
    OA_CHECK(!button_is(out_buttons, library::Action::share_replay, "Share my own recording"));

    server.serve_bytes(
        "/content/v1/replays", bytes_of(page_of("other-battle")), "application/json"
    );
    body.clear();
    const http::Response again = http::Client{}.fetch(request, body);
    OA_CHECK(again.failure == http::Failure::none);
    const feed::FeedPageResult missed =
        feed::parse_replay_page(std::string(body.begin(), body.end()));
    OA_CHECK(missed.status == feed::FeedStatus::page);
    bool found = false;
    for (const feed::FeedEntry& item : missed.page.items) {
        if (item.game_id == kGameId)
            found = true;
    }
    OA_CHECK(!found);
    const library::Library absent = card_for(library::CoreListing::absent, "");
    const library::ReplayRow* missed_row = library::selected_replay(absent);
    OA_CHECK(missed_row != nullptr);
    OA_CHECK(library::replay_note(absent, *missed_row).find("On Core Prime") == std::string::npos);
    const std::vector<library::ReplayButton> share_buttons = library::replay_buttons(absent);
    OA_CHECK(button_is(share_buttons, library::Action::share_replay, "Share on Core Prime…"));
    OA_CHECK(!button_is(share_buttons, library::Action::open_shared_replay, "Open on Core Prime"));
}

} // namespace

int main() {
    screens::forget_after_battle();
    the_wire_types_pick_a_sentence();
    an_unknown_type_shows_nothing();
    a_launch_that_never_reached_the_game_shows_nothing();
    an_arm_without_a_launch_shows_nothing();
    a_for_fun_launch_offers_a_share();
    an_unrecorded_for_fun_battle_is_a_statement();
    a_duel_host_says_it_goes_by_itself();
    a_winner_takes_all_host_says_the_same();
    a_joined_battle_uses_its_type();
    a_password_battle_arms_nothing();
    a_stale_for_fun_arm_never_follows_a_ladder_launch();
    a_refused_launch_leaves_no_type();
    a_return_without_a_match_leaves_nothing_behind();
    a_ladder_after_a_for_fun_is_a_statement();
    the_card_follows_the_feed();
    return oa::test::check_exit_status();
}

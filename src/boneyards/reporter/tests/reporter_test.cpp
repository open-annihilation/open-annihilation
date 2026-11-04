// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/reporter.hpp"
#include "oa/data/campaign/campaign_file.hpp"
#include "oa/ui/frontend_multiplayer/lobby_net.hpp"

#include <cstdio>
#include <cstring>

using namespace oa;
using namespace oa::boneyards::reporter;

namespace {
int failures = 0;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

// The session's game options: the OTA's timemul and killmul.
const oa::data::campaign::CampaignFile* score_options() {
    static oa::data::campaign::CampaignFile options{};
    options.time_multiplier = 0.3f;
    options.kill_multiplier = 33.3f;
    return &options;
}

bool same(const char* a, const char* b) {
    return a != nullptr && b != nullptr && std::strcmp(a, b) == 0;
}

const char* text(Reporter& r, Setting s) {
    return r.setting_text[static_cast<uint32_t>(s)];
}

int32_t value(Reporter& r, Setting s) {
    return r.setting_values[static_cast<uint32_t>(s)];
}

struct Recorder {
    bool online = true;
    const OnlineOptions* options = nullptr;
    int settings = 0;
    SettingsUpload last_settings{};
    int players = 0;
    PlayerUpload player_uploads[OA_PLAYER_COUNT]{};
    int scores = 0;
    ScoreUpload score_uploads[OA_PLAYER_COUNT]{};
    int chats = 0;
    uint32_t chat_id = 0;
    char chat_line[256]{};
    // Fake reporting library.
    bool library = false;
    uint32_t version = 0x00000102;
    int32_t initialize_result = 0;
    bool permit = true;
    int reports = 0;
    int32_t report_count = 0;
    int terminated = 0;
    int closed = 0;
    int sessions_closed = 0;
    int timers = 0;
};

Hooks hooks_for(Recorder& r) {
    Hooks h{};
    h.context = &r;
    h.online_options = [](void* c) { return static_cast<Recorder*>(c)->options; };
    h.uploads_online = [](void* c) { return static_cast<Recorder*>(c)->online; };
    h.upload_settings = [](void* c, const SettingsUpload& u) {
        auto& r = *static_cast<Recorder*>(c);
        ++r.settings;
        r.last_settings = u;
    };
    h.upload_player = [](void* c, const PlayerUpload& u) {
        auto& r = *static_cast<Recorder*>(c);
        if (r.players < OA_PLAYER_COUNT)
            r.player_uploads[r.players] = u;
        ++r.players;
    };
    h.upload_score = [](void* c, const ScoreUpload& u) {
        auto& r = *static_cast<Recorder*>(c);
        if (r.scores < OA_PLAYER_COUNT)
            r.score_uploads[r.scores] = u;
        ++r.scores;
    };
    h.upload_chat = [](void* c, uint32_t id, const char* line) {
        auto& r = *static_cast<Recorder*>(c);
        ++r.chats;
        r.chat_id = id;
        r.chat_line[0] = '\0';
        if (line != nullptr) {
            std::size_t length = std::strlen(line);
            if (length >= sizeof r.chat_line)
                length = sizeof r.chat_line - 1;
            std::memcpy(r.chat_line, line, length);
            r.chat_line[length] = '\0';
        }
    };
    h.open_library = [](void* c, Library* library) {
        auto& r = *static_cast<Recorder*>(c);
        if (!r.library)
            return false;
        library->context = c;
        library->get_version = [](void* c2, const char* product) {
            return same(product, "Total Annihilation") ? static_cast<Recorder*>(c2)->version
                                                       : 0xffffffffu;
        };
        library->initialize = [](void* c2, int32_t* status) {
            *status = 3;
            return static_cast<Recorder*>(c2)->initialize_result;
        };
        library->report = [](void* c2, const LibraryReport& report) {
            auto& r2 = *static_cast<Recorder*>(c2);
            ++r2.reports;
            r2.report_count = report.player_count;
            return 0;
        };
        library->report_chat = [](void*, const ReportPlayer&, const char*) { return 9; };
        library->interval_timer = [](void* c2) { ++static_cast<Recorder*>(c2)->timers; };
        library->terminate = [](void* c2) { ++static_cast<Recorder*>(c2)->terminated; };
        return true;
    };
    h.close_library = [](void* c) { ++static_cast<Recorder*>(c)->closed; };
    h.close_session = [](void* c) { ++static_cast<Recorder*>(c)->sessions_closed; };
    return h;
}

void set_player(
    World& w,
    uint32_t index,
    uint32_t net_id,
    uint8_t status,
    uint8_t side,
    uint16_t options,
    uint8_t role
) {
    Player& p = w.game.players[index];
    p.in_use = 1;
    p.player_id = net_id;
    p.status = status;
    p.index = static_cast<uint8_t>(index);
    p.info = oa_ref_from_index(index);
    PlayerSetupInfo& info = w.player_info[index];
    info.side = side;
    info.options = options;
    info.role = role;
}

// Four report rows: the local host, a computer ally, a defeated remote
// watcher and a departed slot that built units.
void build_world(World& w) {
    Game& g = w.game;
    g.tick = 5432; // 90 steps of 60 ticks
    g.local_player_index = 0;
    g.setup_options = 1;
    std::memcpy(g.provider_guid, oa::ui::frontend_multiplayer::kProviderGuidTcpip, 16);
    set_session_name(g, "My Game   ");

    const uint16_t host_options = 0x0800 | OA_SETUP_OPTION_LOS_LIMITED | OA_SETUP_OPTION_LOS_TRUE |
                                  OA_SETUP_OPTION_CHEATS_ALLOWED | OA_SETUP_OPTION_UNMAPPED |
                                  OA_SETUP_OPTION_WATCHING_ALLOWED;
    set_player(w, 0, 0x101, OA_PLAYER_STATUS_LOCAL, 0, host_options, 1);
    Player& p0 = g.players[0];
    std::snprintf(p0.name, sizeof p0.name, "%s", "Alpha");
    p0.team = 0;
    p0.alliance[1] = 1;
    p0.kills = 3;
    p0.losses = 2;
    p0.energy_produced_total = 1234.99;
    p0.metal_produced_total = 456.5;
    p0.energy_wasted_total = -5.7;
    p0.metal_wasted_total = 3e10;
    p0.commanders_killed = 1;
    p0.unit_count = 5;
    p0.units_created = 9;
    PlayerSetupInfo& i0 = w.player_info[0];
    std::snprintf(i0.map_name, sizeof i0.map_name, "%s", "Coast To Coast");
    std::snprintf(i0.password, sizeof i0.password, "%s", "pw");
    i0.max_units = 250;
    i0.energy_hundreds = 10;
    i0.metal_hundreds = 5;

    set_player(w, 1, 0x202, OA_PLAYER_STATUS_COMPUTER, 1, 0, 0);
    Player& p1 = g.players[1];
    std::snprintf(p1.name, sizeof p1.name, "%s", "Beta");
    p1.team = 0;
    p1.alliance[0] = 1;
    p1.kills = 10;
    p1.losses = 1;
    p1.units_created = 4;

    set_player(w, 2, 0xffffffffu, OA_PLAYER_STATUS_MIRRORED, 5, OA_SETUP_OPTION_WATCHER, 0);
    Player& p2 = g.players[2];
    std::snprintf(p2.name, sizeof p2.name, "%s", "Gamma");
    p2.team = 7;
    w.player_info[2].state = 2;

    Player& p3 = g.players[3];
    p3.units_created = 5;
    p3.index = 10;
}

void test_tables() {
    Reporter r{};
    CHECK(alloc_report_tables(r) == result::ok);
    CHECK(r.buffers && r.setting_total == 0x17);
    CHECK(
        same(r.settings[0].name, "connect") && same(r.settings[12].name, "lockOptions") &&
        same(r.settings[22].name, "tournament")
    );
    CHECK(r.settings[3].text == r.setting_text[3] && r.settings[3].value == &r.setting_values[3]);
    CHECK(r.settings[static_cast<uint32_t>(Setting::mission)].text == r.mission);
    for (int32_t v : r.setting_values)
        CHECK(v == setting_unset);
    Recorder rec;
    shutdown_reporter(r, hooks_for(rec));
    CHECK(!r.buffers && r.setting_total == 0 && r.active == 0);
}

void test_init() {
    World* w = world_create();
    Recorder rec;
    const Hooks h = hooks_for(rec);
    Reporter r{};
    int32_t status = 7;

    std::memcpy(w->game.provider_guid, oa::ui::frontend_multiplayer::kProviderGuidIpx, 16);
    CHECK(init_game_reporter(r, w->game, score_options(), &status, h) == result::unavailable);
    CHECK(r.active == 0 && !r.buffers && status == 7);

    OnlineOptions online{};
    online.provider = "Ladder";
    rec.options = &online;
    CHECK(init_game_reporter(r, w->game, score_options(), &status, h) == result::unavailable);
    CHECK(
        r.active == 1 && status == 0 && same(text(r, Setting::connect), "ipx") &&
        value(r, Setting::connect) == 2
    );
    rec.options = nullptr;

    std::memset(w->game.provider_guid, 0, 16);
    CHECK(init_game_reporter(r, w->game, score_options(), &status, h) == result::unavailable);
    CHECK(same(text(r, Setting::connect), "unknown provider") && value(r, Setting::connect) == 5);

    std::memcpy(w->game.provider_guid, oa::ui::frontend_multiplayer::kProviderGuidTcpip, 16);
    CHECK(init_game_reporter(r, w->game, score_options(), &status, h) == result::unavailable);
    CHECK(
        r.active == 1 && same(text(r, Setting::connect), "tcpip") && value(r, Setting::connect) == 1
    );
    CHECK(!reporter_library_loaded(r));

    // A library whose major version is not zero is turned away.
    rec.library = true;
    rec.version = 0x00010000;
    CHECK(init_game_reporter(r, w->game, score_options(), &status, h) == result::version_mismatch);
    CHECK(!reporter_library_loaded(r) && rec.terminated == 1 && rec.closed == 1);
    rec.version = 0x00000102;
    rec.initialize_result = 4;
    CHECK(init_game_reporter(r, w->game, score_options(), &status, h) == result::unavailable);
    CHECK(!reporter_library_loaded(r) && rec.terminated == 2 && status == 3);
    rec.initialize_result = 0;
    CHECK(init_game_reporter(r, w->game, score_options(), &status, h) == result::ok);
    CHECK(reporter_library_loaded(r));
    // Re-initialising first shuts the loaded library and its session down.
    CHECK(init_game_reporter(r, w->game, score_options(), &status, h) == result::ok);
    CHECK(rec.sessions_closed == 1 && rec.terminated == 3);
    shutdown_reporter(r, h);
    CHECK(!reporter_library_loaded(r) && rec.sessions_closed == 2 && rec.terminated == 4);
    world_destroy(w);
}

void test_score_report() {
    World* w = world_create();
    build_world(*w);
    Recorder rec;
    rec.online = false;
    const Hooks h = hooks_for(rec);
    Reporter r{};
    alloc_report_tables(r);
    r.options = score_options();

    CHECK(build_score_report(r, *w, true, false, h) == 4);
    const ReportPlayer& a = r.players[0];
    CHECK(a.name == w->game.players[0].name && a.net_id == 0x101);
    CHECK(a.flags == (player_flag::playing | player_flag::host));
    CHECK(same(a.side, "Arm") && same(a.team, "diamond"));
    CHECK(a.ally_count == 1 && a.allies[0] == &r.players[1] && a.allies[1] == nullptr);
    CHECK(a.team_member_count == 1 && a.team_members[0] == &r.players[1]);
    const ReportPlayer& b = r.players[1];
    CHECK(b.flags == player_flag::computer_or_out && same(b.side, "Core"));
    CHECK(b.ally_count == 1 && b.allies[0] == &r.players[0]);
    const ReportPlayer& c = r.players[2];
    CHECK(c.net_id == 0 && c.flags == (player_flag::computer_or_out | player_flag::watching));
    CHECK(same(c.side, "triangle") && same(c.team, "") && c.team_member_count == 0);
    const ReportPlayer& d = r.players[3];
    CHECK(d.flags == 0 && same(d.side, "Arm") && same(d.team, "diamond"));
    CHECK(
        d.team_member_count == 2 && d.team_members[0] == &r.players[0] &&
        d.team_members[1] == &r.players[1]
    );
    CHECK(r.players[4].name == nullptr);

    // Score = trunc(tick / 60 * timemul) + trunc(kills * killmul):
    // 90 * 0.3f = 27.0000011 -> 27, 3 * 33.3f = 99.8999977 -> 99.
    const ScoreBoard& sa = r.boards[0];
    CHECK(sa.score == 126 && sa.line_count == 9);
    CHECK(same(sa.lines[0].label, "Kills") && same(sa.lines[8].label, "I am Winner"));
    CHECK(sa.lines[score_line::kills].value == 3 && sa.lines[score_line::losses].value == 2);
    CHECK(
        sa.lines[score_line::energy_produced].value == 1234 &&
        sa.lines[score_line::metal_produced].value == 456
    );
    CHECK(sa.lines[score_line::excess_energy].value == -5);
    // 3e10 keeps the low 32 bits of its 64-bit truncation.
    CHECK(sa.lines[score_line::excess_metal].value == -64771072);
    CHECK(
        sa.lines[score_line::commanders_killed].value == 1 &&
        sa.lines[score_line::commanders_lost].value == 0
    );
    // 90 * 0.3f -> 27, 10 * 33.3f = 332.9999924 -> 332.
    CHECK(r.boards[1].score == 359);
    CHECK(r.boards[2].score == 27);
    // Without a game options record nothing weights the score.
    r.options = nullptr;
    build_score_report(r, *w, false, false, h);
    CHECK(sa.score == 0 && sa.lines[score_line::kills].value == 3);
    r.options = score_options();
    build_score_report(r, *w, false, false, h);
    CHECK(sa.score == 126);

    // The winner line is set for the local player only at the end of a won
    // game, and cleared by every other report.
    w->game.outcome_flags = 0x10;
    build_score_report(r, *w, false, true, h);
    CHECK(
        sa.lines[score_line::winner].value == 1 && r.boards[1].lines[score_line::winner].value == 0
    );
    build_score_report(r, *w, false, false, h);
    CHECK(sa.lines[score_line::winner].value == 0);

    CHECK(same(text(r, Setting::creator), "creator") && value(r, Setting::creator) == 1);
    CHECK(
        same(text(r, Setting::registered_only), "registered only") &&
        value(r, Setting::registered_only) == 1
    );
    CHECK(same(text(r, Setting::player_limit), "10") && value(r, Setting::player_limit) == 10);
    CHECK(same(text(r, Setting::time_limit), "0") && value(r, Setting::time_limit) == 0);
    CHECK(same(text(r, Setting::provider), "") && value(r, Setting::provider) == setting_unset);
    CHECK(same(text(r, Setting::session), "My Game"));
    CHECK(same(r.mission, "Coast To Coast"));
    CHECK(same(text(r, Setting::password), "pw") && same(text(r, Setting::player), "Alpha"));
    CHECK(same(text(r, Setting::pfs), "5") && value(r, Setting::pfs) == 5);
    CHECK(same(text(r, Setting::address), ""));
    CHECK(same(text(r, Setting::lock_options), "locked") && value(r, Setting::lock_options) == 1);
    CHECK(
        same(text(r, Setting::max_units), "250") && same(text(r, Setting::energy), "1000") &&
        same(text(r, Setting::metal), "500")
    );
    CHECK(
        same(text(r, Setting::death_option), "game ends") && value(r, Setting::death_option) == 2
    );
    CHECK(same(text(r, Setting::los), "true") && value(r, Setting::los) == 1);
    CHECK(same(text(r, Setting::cheats), "allowed") && value(r, Setting::cheats) == 2);
    CHECK(same(text(r, Setting::location), "random") && value(r, Setting::location) == 2);
    CHECK(same(text(r, Setting::map_mode), "mapped") && value(r, Setting::map_mode) == 1);
    CHECK(same(text(r, Setting::watching), "allowed") && value(r, Setting::watching) == 2);
    CHECK(same(text(r, Setting::tournament), "0") && value(r, Setting::tournament) == 0);

    // A joiner in a limited-LOS, fixed-location game with the options off.
    w->player_info[0].role = 0;
    w->player_info[0].options =
        OA_SETUP_OPTION_LOS_LIMITED | OA_SETUP_OPTION_FIXED_LOCATIONS | 0x1800;
    w->game.setup_options = 0;
    OnlineOptions online{4, 30, 3, "Ladder", 7, "10.0.0.1"};
    rec.options = &online;
    build_score_report(r, *w, true, false, h);
    CHECK(same(text(r, Setting::creator), "joiner") && value(r, Setting::creator) == 0);
    CHECK(
        same(text(r, Setting::lock_options), "not locked") && value(r, Setting::lock_options) == 0
    );
    CHECK(same(text(r, Setting::death_option), "unknown") && value(r, Setting::death_option) == 4);
    CHECK(same(text(r, Setting::los), "circular") && value(r, Setting::los) == 2);
    CHECK(same(text(r, Setting::cheats), "disallowed") && value(r, Setting::cheats) == 1);
    CHECK(same(text(r, Setting::location), "fixed") && value(r, Setting::location) == 1);
    CHECK(same(text(r, Setting::map_mode), "unmapped") && value(r, Setting::map_mode) == 2);
    CHECK(same(text(r, Setting::watching), "disallowed") && value(r, Setting::watching) == 1);
    CHECK(same(text(r, Setting::player_limit), "4") && same(text(r, Setting::time_limit), "30"));
    CHECK(same(text(r, Setting::provider), "Ladder") && same(text(r, Setting::pfs), "3"));
    CHECK(same(text(r, Setting::address), "10.0.0.1") && same(text(r, Setting::tournament), "7"));
    online.player_limit = 10;
    build_score_report(r, *w, true, false, h);
    CHECK(value(r, Setting::player_limit) == 10);
    w->player_info[0].options = 0;
    build_score_report(r, *w, true, false, h);
    CHECK(same(text(r, Setting::los), "permanent") && value(r, Setting::los) == 3);
    CHECK(
        same(text(r, Setting::death_option), "game continues") &&
        value(r, Setting::death_option) == 1
    );
    world_destroy(w);
}

void test_events() {
    World* w = world_create();
    build_world(*w);
    Recorder rec;
    const Hooks h = hooks_for(rec);
    Reporter r{};
    CHECK(report_event(r, *w, event::game_started, route_all, h) == result::unavailable);
    int32_t status = 0;
    CHECK(init_game_reporter(r, w->game, score_options(), &status, h) == result::unavailable);

    // The first event copies the session name, trimming the spaces that pad
    // it to 16 characters; a shorter name keeps its spaces and index 0 is
    // never trimmed.
    set_session_name(w->game, "Duel            ");
    CHECK(report_event(r, *w, event::battleroom_opened, route_all, h) == result::ok);
    CHECK(same(r.session_name, "Duel"));
    set_session_name(w->game, "Duel  ");
    report_event(r, *w, event::battleroom_opened, route_all, h);
    CHECK(same(r.session_name, "Duel  "));
    set_session_name(w->game, "                ");
    report_event(r, *w, event::battleroom_opened, route_all, h);
    CHECK(same(r.session_name, " "));
    CHECK(rec.settings == 0 && rec.scores == 0);

    // Launch: the host uploads the settings and every counted row.
    CHECK(report_event(r, *w, event::game_launched, route_all, h) == result::ok);
    CHECK(rec.settings == 1 && rec.players == 4 && r.upload_countdown == upload_interval);
    const SettingsUpload& s = rec.last_settings;
    CHECK(s.lock_options == 1 && same(s.mission, "Coast To Coast") && s.player_limit == 10);
    CHECK(
        s.death_option == 2 && s.energy == 1000 && s.metal == 500 && s.location == 2 && s.los == 1
    );
    CHECK(
        s.map_mode == 1 && s.max_units == 250 && s.watching == 2 && s.cheats == 2 &&
        s.tournament == 0
    );
    CHECK(rec.player_uploads[0].net_id == 0x101 && same(rec.player_uploads[0].name, "Alpha"));
    CHECK(same(rec.player_uploads[0].side, "Arm") && rec.player_uploads[0].team == 1);
    CHECK(rec.player_uploads[2].team == 0 && rec.player_uploads[3].team == 1);

    // The interval upload comes when the countdown has run out.
    for (int frame = 0; frame < upload_interval; ++frame)
        step_reporter(r, *w, h);
    CHECK(rec.scores == 0 && r.upload_countdown == 0);
    step_reporter(r, *w, h);
    CHECK(rec.scores == 4 && r.upload_countdown == upload_interval);
    const ScoreUpload& up = rec.score_uploads[0];
    CHECK(up.net_id == 0x101 && up.lines[0] == 3 && up.lines[1] == 2 && up.lines[2] == 1234);
    CHECK(up.score == 126 && !up.winner && !up.final_report);

    // A computer host does not upload scores on the interval.
    w->player_info[0].role = 0;
    rec.scores = 0;
    report_event(r, *w, event::interval, route_all, h);
    CHECK(rec.scores == 0);
    w->player_info[0].role = 1;

    // Chat goes to the upload hooks with the local player's id.
    CHECK(report_chat(r, *w, "<Alpha> gg", h) == result::ok);
    CHECK(rec.chats == 1 && rec.chat_id == 0x101);

    // Characters the Western European page cannot hold stay UTF-8 on the upload.
    const char tokyo[] = "\xe6\x9d\xb1\xe4\xba\xac";
    CHECK(report_chat(r, *w, tokyo, h) == result::ok);
    CHECK(rec.chats == 2 && std::strcmp(rec.chat_line, tokyo) == 0);
    CHECK(static_cast<unsigned char>(rec.chat_line[0]) == 0xe6);

    // The end of a won game uploads the final scores and stops the uploads.
    std::snprintf(w->game.password, sizeof w->game.password, "%s", "secret");
    w->game.outcome_flags = 0x10;
    rec.scores = 0;
    report_event(r, *w, event::game_ended, route_all, h);
    CHECK(rec.scores == 4 && rec.score_uploads[0].winner && rec.score_uploads[0].final_report);
    CHECK(!rec.score_uploads[1].winner);
    CHECK(r.upload_countdown == upload_off && w->game.password[0] == '\0');

    // A lost game uploads nothing but still stops the uploads.
    w->game.outcome_flags = 0;
    r.upload_countdown = 5;
    rec.scores = 0;
    report_event(r, *w, event::game_ended, route_all, h);
    CHECK(rec.scores == 0 && r.upload_countdown == upload_off);

    // Without the uploads' destination up nothing is sent.
    rec.online = false;
    rec.chats = 0;
    report_chat(r, *w, "hi", h);
    CHECK(rec.chats == 0);

    // The library route alone.
    rec.online = true;
    rec.library = true;
    CHECK(init_game_reporter(r, w->game, score_options(), &status, h) == result::ok);
    rec.settings = 0;
    CHECK(report_event(r, *w, event::game_launched, route_library, h) == result::ok);
    CHECK(rec.reports == 1 && rec.report_count == 4 && rec.settings == 0);
    CHECK(report_chat(r, *w, "x", h) == 9);
    step_reporter(r, *w, h);
    CHECK(rec.timers == 1);

    shutdown_reporter(r, h);
    CHECK(report_event(r, *w, event::game_ended, route_all, h) == result::unavailable);
    world_destroy(w);
}

// A simulation the Boneyards does not report is not started, uploaded or handed to the library.
void test_refused_simulation() {
    World* w = world_create();
    build_world(*w);
    Recorder rec;
    rec.permit = false;
    rec.library = true;
    Hooks h = hooks_for(rec);
    h.simulation_permitted = [](void* context) { return static_cast<Recorder*>(context)->permit; };
    Reporter r{};
    int32_t status = 7;
    CHECK(init_game_reporter(r, w->game, score_options(), &status, h) == result::refused);
    CHECK(r.active == 0 && !r.library_loaded && rec.reports == 0 && status == 7);

    rec.permit = true;
    CHECK(init_game_reporter(r, w->game, score_options(), &status, h) == result::ok);
    CHECK(r.active == 1 && r.library_loaded);
    rec.settings = 0;
    rec.reports = 0;
    CHECK(report_event(r, *w, event::game_launched, route_all, h) == result::ok);
    CHECK(rec.settings == 1 && rec.reports == 1);

    rec.permit = false;
    const int settings = rec.settings;
    const int reports = rec.reports;
    const int chats = rec.chats;
    const int timers = rec.timers;
    CHECK(report_event(r, *w, event::interval, route_all, h) == result::refused);
    CHECK(report_chat(r, *w, "hi", h) == result::refused);
    step_reporter(r, *w, h);
    CHECK(
        rec.settings == settings && rec.reports == reports && rec.chats == chats &&
        rec.timers == timers
    );
    world_destroy(w);
}

} // namespace

int main() {
    test_tables();
    test_init();
    test_score_report();
    test_events();
    test_refused_simulation();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("reporter: ok");
    return 0;
}

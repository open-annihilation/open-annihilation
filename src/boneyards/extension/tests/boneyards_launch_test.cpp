// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A battle the Boneyards launches, through network play's API
// (oa/app/netgame/extension_api.hpp): how its launch reply fills the launch block,
// the launch it applies and ends, the frontend's questions about it, the
// answers to network play's hooks (the match report and its uploads to
// Rover, the console's Page, a failed launched join), the label after a Boneyards
// launch and the lines it sends to the battle. The test reaches network play
// through its public headers alone and binds no running game, so the API's
// block stands in for the game's and network play's launch, bound to a
// record, keeps the leave the Boneyards' YES asks for; network play's own
// side of each call (the nickname, the pending flag, the close handler, the
// leave itself, the lines posted to a running match) is checked by the
// engine's netgame-launch-binding.
#include "boneyards_data.hpp"
#include "boneyards_launch.hpp"
#include "boneyards_mod_gate.hpp"
#include "boneyards_report.hpp"
#include "launch_binding.hpp"

#include "oa/boneyards/overlay_launch.hpp"
#include "oa/boneyards/rover_calls.hpp"
#include "oa/netgame/overlay_route.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/app/netgame/extension_api.hpp"
#include "oa/netgame/frontend/multiplayer_states.hpp"
#include "oa/boneyards/reporter.hpp"
#include "oa/formats/oademo/oademo.hpp"
#include "oa/session/demo/recording.hpp"
#include "oa/session/demo/replay_record.hpp"
#include "oa/test/netgame_launch_record.hpp"
#include "oa/rover/interface.h"
#include "oa/ui/frontend_multiplayer/launch_block.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"

#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = oa::app::netgame::extension_api;
namespace mp = oa::ui::frontend_multiplayer;
namespace nl = oa::ui::frontend_multiplayer::launch;
namespace reporter = oa::boneyards::reporter;
namespace frontend = oa::netgame::frontend;
using namespace oa::app;

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

// ---------------------------------------------------------------------------
// A Rover table that records the uploads

struct Uploads {
    std::vector<std::string> calls;
    uint32_t area_id = 0;
    std::string game_id;
    int32_t settings_tournament = -1;
    std::string settings_mission;
    uint32_t player_dpid = 0;
    std::string player_name;
    std::string player_side;
    int32_t player_team = -1;
    int32_t score_kills = -1;
    int32_t score_commanders_lost = -1;
    int32_t score = -1;
    bool score_victory = false;
    bool score_last = false;
    std::string message;
    std::string page;
};

Uploads g_uploads;
RoverInterface g_table;

int32_t ROVER_CALL fake_check_version(int32_t, int32_t) {
    return 1;
}

bool ROVER_CALL fake_left_game(uint32_t area_id, const char* game_id) {
    g_uploads.calls.emplace_back("left_game");
    g_uploads.area_id = area_id;
    g_uploads.game_id = game_id;
    return true;
}

bool ROVER_CALL fake_game_settings(
    uint32_t area_id,
    const char* game_id,
    int32_t,
    const char* mission,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t tournament
) {
    g_uploads.calls.emplace_back("game_settings");
    g_uploads.area_id = area_id;
    g_uploads.game_id = game_id;
    g_uploads.settings_mission = mission != nullptr ? mission : "";
    g_uploads.settings_tournament = tournament;
    return true;
}

bool ROVER_CALL fake_player_info(
    uint32_t area_id,
    const char* game_id,
    uint32_t dpid,
    const char* name,
    const char* side,
    int32_t team
) {
    g_uploads.calls.emplace_back("player_info");
    g_uploads.area_id = area_id;
    g_uploads.game_id = game_id;
    g_uploads.player_dpid = dpid;
    g_uploads.player_name = name;
    g_uploads.player_side = side;
    g_uploads.player_team = team;
    return true;
}

bool ROVER_CALL fake_score_report(
    uint32_t area_id,
    const char* game_id,
    uint32_t,
    int32_t kills,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t commanders_lost,
    int32_t score,
    bool victory,
    bool last
) {
    g_uploads.calls.emplace_back("score_report");
    g_uploads.area_id = area_id;
    g_uploads.game_id = game_id;
    g_uploads.score_kills = kills;
    g_uploads.score_commanders_lost = commanders_lost;
    g_uploads.score = score;
    g_uploads.score_victory = victory;
    g_uploads.score_last = last;
    return true;
}

bool ROVER_CALL
fake_score_message(uint32_t area_id, const char* game_id, uint32_t, const char* message) {
    g_uploads.calls.emplace_back("score_report_message");
    g_uploads.area_id = area_id;
    g_uploads.game_id = game_id;
    g_uploads.message = message;
    return true;
}

void ROVER_CALL fake_page(const char* user_name, const char* message) {
    g_uploads.calls.emplace_back("page");
    g_uploads.page = std::string(user_name) + ": " + message;
}

void ROVER_CALL fake_get_interface(const RoverInterface** out) {
    *out = &g_table;
}

void bind_fake_rover() {
    oa::boneyards::rover_module_unload();
    const RoverInterface* real = nullptr;
    rover_get_interface(&real);
    g_table = *real;
    g_table.check_version = fake_check_version;
    g_table.send_left_game = fake_left_game;
    g_table.send_game_settings = fake_game_settings;
    g_table.send_player_info = fake_player_info;
    g_table.send_score_report = fake_score_report;
    g_table.send_score_report_message = fake_score_message;
    g_table.send_page_by_name = fake_page;
    oa::boneyards::rover_module_set_source(fake_get_interface);
    check(oa::boneyards::rover_module_load(), "the fake Rover table binds");
}

// ---------------------------------------------------------------------------
// The launch

bool test_battle_ids(uint32_t* area_id, const char** game_id) {
    *area_id = 709;
    *game_id = "9bfaa0d0f7b93f12";
    return true;
}

ExitPrompt g_prompt{};

void bind_test_launch() {
    BoneyardsLaunch boneyards{};
    boneyards.battle_ids = test_battle_ids;
    boneyards.exit_prompt = &g_prompt;
    bind_boneyards_launch(boneyards);
}

// A launch reply fills the block field for field over TCP/IP, with the
// Boneyards' provider label, the faction its allegiance names and texts
// bounded to their fields.
void test_fill() {
    nl::LaunchBlock block{};
    block.connection_type = nl::connection_type::modem;
    block.time_limit = 9;
    block.pfs = 7;
    oa::boneyards::LaunchReport launch{};
    launch.game_id = "9bfaa0d0f7b93f12";
    launch.session = "Acid Pools Battle";
    launch.address = "10.66.5.217";
    launch.password = "YFCPDVPKETZYKXA1";
    launch.creator = false;
    launch.lock_options = true;
    launch.mission = "Acid Pools";
    launch.playerlimit = 4;
    launch.deathopt = 2;
    launch.energy = 1000;
    launch.metal = 2000;
    launch.location = 1;
    launch.los = 3;
    launch.mapping = 2;
    launch.maxunits = 200;
    launch.watching = 2;
    launch.cheats = 1;
    launch.tournament = 5;
    launch.allegiance = "Core";
    launch.team = 3;
    online_launch_fill_boneyards(block, launch, "Ghost_Commander");
    check(
        block.connection_type == nl::connection_type::tcpip &&
            block.launched_connection_type == nl::connection_type::tcpip,
        "both connection type fields become TCP/IP"
    );
    check(block.hosting == 0 && block.lock_options == 1, "creator and lock flags");
    check(block.bar_mutators == 1, "mutators are barred when the room's options are locked");
    launch.lock_options = false;
    online_launch_fill_boneyards(block, launch, "Ghost_Commander");
    check(
        block.lock_options == 0 && block.bar_mutators == 1,
        "mutators stay barred when the room's options are open"
    );
    launch.lock_options = true;
    check(block.time_limit == 0 && block.pfs == 0, "time limit and pfs are cleared");
    check(
        std::strcmp(block.provider_label, "Boneyards") == 0 &&
            std::strcmp(kBoneyardsProviderLabel, "Boneyards") == 0,
        "provider label"
    );
    check(
        std::strcmp(block.session_name, "Acid Pools Battle") == 0 &&
            std::strcmp(block.mission, "Acid Pools") == 0 &&
            std::strcmp(block.password, "YFCPDVPKETZYKXA1") == 0 &&
            std::strcmp(block.user_name, "Ghost_Commander") == 0 &&
            std::strcmp(block.address, "10.66.5.217") == 0,
        "texts copied"
    );
    check(
        block.player_limit == 4 && block.death_option == 2 && block.energy == 1000 &&
            block.metal == 2000 && block.location == 1 && block.los == 3 && block.mapping == 2 &&
            block.max_units == 200 && block.watching == 2 && block.cheats == 1 &&
            block.tournament == 5 && block.team == 3,
        "options copied"
    );
    check(block.faction == nl::faction::core, "'C' is Core");
    launch.allegiance = "Arm";
    launch.creator = true;
    online_launch_fill_boneyards(block, launch, "Ghost_Commander");
    check(block.faction == nl::faction::arm && block.hosting == 1, "'A' is Arm; creator hosts");
    launch.allegiance = "U";
    online_launch_fill_boneyards(block, launch, "Ghost_Commander");
    check(block.faction == nl::faction::none, "any other allegiance is none");
    launch.allegiance = nullptr;
    online_launch_fill_boneyards(block, launch, "Ghost_Commander");
    check(block.faction == nl::faction::none, "no allegiance is none");
    const std::string long_text(80, 'y');
    launch.session = long_text.c_str();
    launch.password = long_text.c_str();
    launch.mission = nullptr;
    online_launch_fill_boneyards(block, launch, long_text.c_str());
    check(
        std::strlen(block.session_name) == 63 && std::strlen(block.password) == 31 &&
            std::strlen(block.user_name) == 31 && block.mission[0] == '\0',
        "texts are bounded and null stores empty"
    );
    online_launch_fill_boneyards(block, launch, nullptr);
    check(block.user_name[0] == '\0', "no name stores an empty one");
}

// A launch reply fills network play's launch block and applies the launch,
// which removes the Boneyards box; the end of the launched battle ends it.
void test_launch() {
    nl::LaunchBlock& block = api::launch_block();
    block = nl::LaunchBlock{};
    oa::boneyards::LaunchReport launch{};
    launch.game_id = "9bfaa0d0f7b93f12";
    launch.session = "Acid Pools Battle";
    launch.address = "10.66.5.217";
    launch.password = "YFCPDVPKETZYKXA1";
    launch.creator = false;
    launch.lock_options = true;
    launch.mission = "Acid Pools";
    launch.playerlimit = 4;
    launch.tournament = 2;
    launch.allegiance = "Arm";
    launch.team = 2;
    g_prompt.installed = true;
    on_boneyards_launch(nullptr, &launch, "Ghost_Commander_And_More");
    check(
        block.connection_type == nl::connection_type::tcpip && block.hosting == 0 &&
            std::strcmp(block.address, "10.66.5.217") == 0 &&
            std::strcmp(block.user_name, "Ghost_Commander_And_More") == 0 &&
            std::strcmp(block.provider_label, kBoneyardsProviderLabel) == 0 &&
            block.faction == nl::faction::arm && block.tournament == 2,
        "the launch reply fills network play's launch block"
    );
    check(api::launch_active(), "the launch is active");
    check(!g_prompt.installed, "the launch removes the Boneyards box");
    check(
        boneyards_return_label() == nullptr && !boneyards_launched_game(),
        "no label for the match's return without the Boneyards' launched battle"
    );
    const auto bridge = boneyards_launch_bridge();
    bridge.launch_ended(bridge.user);
    check(!api::launch_active(), "the end of the launched battle ends the launch");
    on_boneyards_launch(nullptr, nullptr, "Ghost");
    check(
        api::launch_active() && std::strcmp(block.user_name, "Ghost_Commander_And_More") == 0,
        "a launch without a reply applies the block as it is"
    );
    check(block.bar_mutators == 1, "a launch without a reply bars mutators");
    api::end_launch();
    block = nl::LaunchBlock{};
}

// A profile head that changes nothing, then the body under test.
constexpr std::string_view kProfileHead =
    "oamod: 1\n"
    "id: boneyards\n"
    "name: t\n"
    "version: \"1\"\n"
    "requires: {base: ta-3.1c, catalogue: 1}\n"
    "author: {name: unknown}\n"
    "packaging: {revision: 1, date: 2026-10-04, packager: t}\n";

std::string profile_text(std::string_view body) {
    std::string text{kProfileHead};
    text.append(body);
    return text;
}

DataFacts facts_of(std::string text) {
    DataFacts facts{};
    facts.profile_text = std::move(text);
    facts.profile_name = "oamod.yaml";
    return facts;
}

std::optional<oa::base::sha256::Digest> hash_of(std::string_view text) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(text.data());
    const auto result = oa::data::mod_profile::resolve_profile(
        std::span<const uint8_t>(bytes, text.size()), "oamod.yaml"
    );
    if (!result.resolution)
        return std::nullopt;
    return result.resolution->profile.sim_hash;
}

struct HeldFacts {
    DataFacts facts{};
};

DataFacts hold_facts(void* context) {
    return static_cast<const HeldFacts*>(context)->facts;
}

// Plain 3.1c data may be launched and reported. Another mod, and a Developer
// Mode change to the simulation, may not. Core Contingency and Battle
// Tactics add no profile, so they are the plain case.
void test_data_gate() {
    namespace profiles = oa::data::mod_profile;
    const DataVerdict plain = boneyards_data_verdict(DataFacts{});
    check(
        plain.allowed && plain.message == nullptr,
        "plain 3.1c, Core Contingency and Battle Tactics may be launched"
    );
    DataFacts developer_only{};
    developer_only.developer_mode = true;
    check(
        boneyards_data_verdict(developer_only).allowed,
        "Developer Mode with no change still plays 3.1c"
    );

    const std::string boneyards = profile_text("");
    const DataVerdict own = boneyards_data_verdict(facts_of(boneyards));
    check(
        own.allowed && own.message == nullptr, "the Boneyards' own profile changes no simulation"
    );
    const DataVerdict display =
        boneyards_data_verdict(facts_of(profile_text("hacks:\n  ui.megamap: true\n")));
    check(
        display.allowed && display.message == nullptr,
        "a display change in the profile stays allowed"
    );

    const std::optional<oa::base::sha256::Digest> own_hash = hash_of(boneyards);
    DataFacts shipped{};
    shipped.shipped_sim_hash = own_hash;
    check(
        own_hash.has_value() && boneyards_data_verdict(shipped).allowed,
        "a profile the game already resolved to 3.1c is allowed"
    );

    const DataVerdict other = boneyards_data_verdict(
        facts_of(profile_text("layout:\n  installation-archives: [extra.ccx]\n"))
    );
    check(
        !other.allowed && other.message != nullptr &&
            std::strcmp(other.message, kBoneyardsDataRefusal) == 0,
        "another mod is refused with the Boneyards' sentence"
    );
    DataFacts other_shipped{};
    other_shipped.shipped_sim_hash =
        hash_of(profile_text("layout:\n  installation-archives: [extra.ccx]\n"));
    check(
        other_shipped.shipped_sim_hash.has_value() &&
            !boneyards_data_verdict(other_shipped).allowed,
        "a resolved profile of another mod is refused"
    );
    DataFacts unread{};
    unread.profile_text = "not a profile";
    unread.profile_name = "oamod.yaml";
    check(!boneyards_data_verdict(unread).allowed, "a profile that cannot be read is refused");
    DataFacts broken{};
    broken.unreadable = true;
    check(!boneyards_data_verdict(broken).allowed, "data that cannot be confirmed is refused");

    DataFacts changed{};
    changed.developer_mode = true;
    changed.options.overrides.push_back(
        profiles::HackOverride{
            "units.id-reuse-delay", true, {{"ticks", profiles::make_integer(120)}}
        }
    );
    const DataVerdict changed_verdict = boneyards_data_verdict(changed);
    check(
        !changed_verdict.allowed && changed_verdict.message != nullptr &&
            std::strcmp(changed_verdict.message, kBoneyardsDataRefusal) == 0,
        "a Developer Mode change to the simulation is refused"
    );
    DataFacts changed_profile = facts_of(boneyards);
    changed_profile.developer_mode = true;
    changed_profile.options.overrides = changed.options.overrides;
    check(
        !boneyards_data_verdict(changed_profile).allowed,
        "the same change over the Boneyards' profile is refused"
    );
    DataFacts view_change{};
    view_change.developer_mode = true;
    view_change.options.overrides.push_back(profiles::HackOverride{"ui.megamap", true, {}});
    check(
        boneyards_data_verdict(view_change).allowed, "a Developer Mode display change stays allowed"
    );
    DataFacts stored_off = changed;
    stored_off.developer_mode = false;
    check(
        boneyards_data_verdict(stored_off).allowed,
        "a stored simulation change applies only while Developer Mode is on"
    );

    const std::string base_text = profiles::base_game_profile_text();
    const auto* base_bytes = reinterpret_cast<const uint8_t*>(base_text.data());
    const auto base_resolved = profiles::resolve_profile(
        std::span<const uint8_t>(base_bytes, base_text.size()), profiles::base_game_id
    );
    DataFacts engine_plain{};
    if (base_resolved.resolution)
        engine_plain.run_sim_hash =
            profiles::digest_text(base_resolved.resolution->profile.sim_hash);
    check(
        base_resolved.resolution && boneyards_data_verdict(engine_plain).allowed,
        "the engine's plain 3.1c hash is allowed"
    );
    DataFacts engine_changed{};
    engine_changed.run_sim_hash = profiles::digest_text(*other_shipped.shipped_sim_hash);
    const DataVerdict engine_changed_verdict = boneyards_data_verdict(engine_changed);
    check(
        other_shipped.shipped_sim_hash.has_value() && !engine_changed_verdict.allowed &&
            engine_changed_verdict.message != nullptr &&
            std::strcmp(engine_changed_verdict.message, kBoneyardsDataRefusal) == 0,
        "the engine's changed simulation hash is refused"
    );
    DataFacts engine_over_profile =
        facts_of(profile_text("layout:\n  installation-archives: [extra.ccx]\n"));
    engine_over_profile.run_sim_hash = engine_plain.run_sim_hash;
    check(
        boneyards_data_verdict(engine_over_profile).allowed,
        "the engine's hash decides, ahead of a profile supplied beside it"
    );
    DataFacts own_id{};
    own_id.profile_id = "boneyards";
    own_id.run_sim_hash = engine_plain.run_sim_hash;
    check(boneyards_data_verdict(own_id).allowed, "the Boneyards' own profile id is allowed");
    DataFacts ridge{};
    ridge.profile_text = "id: ridge\n";
    ridge.run_sim_hash = engine_plain.run_sim_hash;
    const DataVerdict ridge_verdict = boneyards_data_verdict(ridge);
    check(
        !ridge_verdict.allowed && ridge_verdict.message != nullptr &&
            std::strcmp(ridge_verdict.message, kBoneyardsDataRefusal) == 0,
        "a data-only mod profile is refused"
    );
    DataFacts folder_only{};
    folder_only.mod_folder = true;
    folder_only.run_sim_hash = engine_plain.run_sim_hash;
    check(!boneyards_data_verdict(folder_only).allowed, "a mod folder with no profile is refused");
    DataFacts one_mutator{};
    one_mutator.mutator_count = 1;
    one_mutator.run_sim_hash = engine_plain.run_sim_hash;
    check(!boneyards_data_verdict(one_mutator).allowed, "a match with one mutator is refused");

    nl::LaunchBlock& block = api::launch_block();
    block = nl::LaunchBlock{};
    std::snprintf(block.session_name, sizeof block.session_name, "%s", "keep");
    api::end_launch();
    HeldFacts held{};
    held.facts = facts_of(profile_text("layout:\n  installation-archives: [extra.ccx]\n"));
    boneyards_set_data_facts_source(hold_facts, &held);
    oa::boneyards::LaunchReport launch{};
    launch.session = "Nope";
    launch.game_id = "9bfaa0d0f7b93f12";
    on_boneyards_launch(nullptr, &launch, "Ghost");
    check(
        !api::launch_active() && std::strcmp(block.session_name, "keep") == 0,
        "another mod does not launch"
    );
    const auto bridge = boneyards_launch_bridge();
    const char* refusal = nullptr;
    check(
        bridge.launch_permitted != nullptr && !bridge.launch_permitted(bridge.user, &refusal) &&
            refusal != nullptr && std::strcmp(refusal, kBoneyardsDataRefusal) == 0,
        "the console is told to refuse the launch"
    );
    reporter::Hooks hooks = boneyards_reporter_hooks();
    check(
        hooks.simulation_permitted != nullptr && !hooks.simulation_permitted(hooks.context) &&
            !hooks.uploads_online(hooks.context),
        "another mod is not reported"
    );

    boneyards_set_data_facts_source(nullptr, nullptr);
    on_boneyards_launch(nullptr, &launch, "Ghost");
    check(
        api::launch_active() && std::strcmp(block.session_name, "Nope") == 0,
        "plain data still launches"
    );
    hooks = boneyards_reporter_hooks();
    check(
        hooks.simulation_permitted(hooks.context) && hooks.uploads_online(hooks.context),
        "plain data is reported"
    );
    boneyards_set_data_facts_source(hold_facts, &held);
    check(
        boneyards_report_simulation_allowed(),
        "a launch already applied keeps its report after the simulation changes"
    );
    api::end_launch();
    check(
        !boneyards_report_simulation_allowed(),
        "after that launch the changed simulation is not reported"
    );
    boneyards_set_data_facts_source(nullptr, nullptr);
    block = nl::LaunchBlock{};
}

// An allegiance names its faction by its first letter.
void test_factions() {
    check(boneyards_faction_of_allegiance("Arm") == nl::faction::arm, "'A' is Arm");
    check(boneyards_faction_of_allegiance("Core") == nl::faction::core, "'C' is Core");
    check(
        boneyards_faction_of_allegiance("U") == nl::faction::none, "any other allegiance is none"
    );
    check(boneyards_faction_of_allegiance(nullptr) == nl::faction::none, "no allegiance is none");
}

// The match report's hooks: the launch block's options and online uploads
// only while a launch is active, the uploads to the Boneyards, no reporting
// library, and every report hook of network play's API set.
void test_report_hooks() {
    nl::LaunchBlock& block = api::launch_block();
    block = nl::LaunchBlock{};
    api::end_launch();
    reporter::Hooks hooks = boneyards_reporter_hooks();
    check(
        hooks.online_options(hooks.context) == nullptr && !hooks.uploads_online(hooks.context),
        "no launch: no launch options, the uploads are offline"
    );
    check(
        hooks.upload_settings != nullptr && hooks.upload_player != nullptr &&
            hooks.upload_score != nullptr && hooks.upload_chat != nullptr,
        "the uploads go to the Boneyards"
    );
    check(
        hooks.open_library == nullptr && hooks.close_session == nullptr,
        "no reporting library is opened"
    );
    block.player_limit = 4;
    block.time_limit = 30;
    block.tournament = 2;
    std::snprintf(block.provider_label, sizeof block.provider_label, "%s", kBoneyardsProviderLabel);
    std::snprintf(block.address, sizeof block.address, "%s", "10.66.5.217");
    api::apply_launch();
    const reporter::OnlineOptions* options = hooks.online_options(hooks.context);
    check(
        options != nullptr && options->player_limit == 4 && options->time_limit == 30 &&
            options->tournament == 2 && std::strcmp(options->provider, "Boneyards") == 0 &&
            std::strcmp(options->address, "10.66.5.217") == 0 &&
            hooks.uploads_online(hooks.context),
        "an active launch gives the block's options and online uploads"
    );
    api::end_launch();
    block = nl::LaunchBlock{};
    api::Hooks api_hooks{};
    fill_boneyards_report_hooks(api_hooks);
    check(
        api_hooks.report_start != nullptr && api_hooks.report_event != nullptr &&
            api_hooks.report_chat != nullptr && api_hooks.report_step != nullptr &&
            api_hooks.report_close != nullptr && api_hooks.report_release != nullptr &&
            api_hooks.page == nullptr && api_hooks.join_failed == nullptr,
        "every report hook is set, and only those"
    );
    api_hooks.report_release(api_hooks.context);
    check(
        boneyards_report().active == 0 && !boneyards_report().library_loaded,
        "a release with no report started leaves it inactive"
    );
}

/// Runs the handler registered for a query, as the dispatcher asks it.
///
/// @param registry registry the queries were registered in
/// @param query query asked
/// @param[out] answer the handler's answer
/// @return false when no handler takes the query
bool ask(const ScreenRegistry& registry, oa::ui::frontend_state::Query query, uint32_t* answer) {
    const QueryDesc* desc = query_find(&registry, query);
    if (desc == nullptr)
        return false;
    *answer = desc->run(nullptr, desc->state);
    return true;
}

void test_frontend_answers() {
    ScreenRegistry registry{};
    register_boneyards_queries(&registry);
    check(
        registry.rejected == nullptr && registry.query_count == 3,
        "three queries answer through the dispatcher's registry"
    );
    uint32_t answer = 7;
    api::end_launch();
    check(
        ask(registry, frontend::query::context_flags, &answer) && answer == 0,
        "context flags without a launch"
    );
    api::apply_launch();
    check(
        ask(registry, frontend::query::context_flags, &answer) && answer == 1,
        "context flags after a launch"
    );
    check(
        ask(registry, frontend::query::generate_default_game_name, &answer) && answer == 0,
        "no lobby program launched the game"
    );
    check(
        ask(registry, frontend::query::adapter_lookup_needed, &answer) && answer == 0,
        "the network check is answered; no overlay, no network"
    );
    check(
        !ask(registry, frontend::query::transport_kind, &answer),
        "the questions about joining and leaving are not the Boneyards'"
    );
    const auto bridge = boneyards_launch_bridge();
    check(
        bridge.connection_type(bridge.user) == api::connection_type(),
        "the console reads network play's connection type"
    );
    // The Boneyards' YES reaches network play's leave with the reason.
    oa::test::netgame::LaunchRecord record{};
    oa::test::netgame::bind_launch_record(&record);
    bridge.leave_game(bridge.user);
    check(
        record.leaves_with_reason == 1 && record.leaves_without_reason == 0,
        "the Boneyards' YES leaves with the reason"
    );
    oa::test::netgame::bind_launch_record(nullptr);
    api::end_launch();
}

// The match report's uploads go to Rover with the Boneyards' area and the
// launch's game id; the other entries of the hooks are left as they are. A failed launched join leaves the battle, and the console's Page goes
// out by name.
void test_report_uploads() {
    bind_fake_rover();
    g_uploads = Uploads{};
    reporter::Hooks hooks{};
    boneyards_report_hooks(nullptr, hooks);
    check(
        hooks.uploads_online == nullptr && hooks.online_options == nullptr,
        "the other entries are left as they are"
    );
    reporter::SettingsUpload settings{};
    settings.mission = "Acid Pools";
    settings.tournament = 2;
    hooks.upload_settings(hooks.context, settings);
    check(
        g_uploads.calls.back() == "game_settings" && g_uploads.area_id == 709 &&
            g_uploads.game_id == "9bfaa0d0f7b93f12" && g_uploads.settings_mission == "Acid Pools" &&
            g_uploads.settings_tournament == 2,
        "settings go out as slot 26 with the area and game ids"
    );
    hooks.upload_player(hooks.context, reporter::PlayerUpload{0x1234, "Ghost", "ARM", 3});
    check(
        g_uploads.calls.back() == "player_info" && g_uploads.player_dpid == 0x1234 &&
            g_uploads.player_name == "Ghost" && g_uploads.player_side == "ARM" &&
            g_uploads.player_team == 3,
        "a player goes out as slot 28"
    );
    reporter::ScoreUpload score{};
    score.net_id = 0x1234;
    score.lines[reporter::score_line::kills] = 12;
    score.lines[reporter::score_line::commanders_lost] = 1;
    score.score = 345;
    score.winner = true;
    score.final_report = true;
    hooks.upload_score(hooks.context, score);
    check(
        g_uploads.calls.back() == "score_report" && g_uploads.score_kills == 12 &&
            g_uploads.score_commanders_lost == 1 && g_uploads.score == 345 &&
            g_uploads.score_victory && g_uploads.score_last,
        "a score goes out as slot 30"
    );
    hooks.upload_chat(hooks.context, 0x1234, "gg");
    check(
        g_uploads.calls.back() == "score_report_message" && g_uploads.message == "gg",
        "a chat line goes out as slot 31"
    );
    check(
        hooks.open_library == nullptr && hooks.session_guid == nullptr &&
            hooks.session_value == nullptr,
        "the reporting library stays unbound"
    );
    // A join that failed is still on the game list. The leave waits for the
    // return to the Boneyards, so nothing goes out from the failure itself.
    const auto calls_before_failure = g_uploads.calls.size();
    boneyards_join_failed(nullptr, "Host not found");
    check(
        g_uploads.calls.size() == calls_before_failure,
        "a failed join sends no leave while the player is still joining"
    );
    // The console's Page goes to Rover (slot 41).
    boneyards_page(nullptr, "Ghost", "hello");
    check(
        g_uploads.calls.back() == "page" && g_uploads.page == "Ghost: hello",
        "a page goes out by name"
    );

    // The same uploads for a battle whose planet is not the one the earlier
    // stub named. The functions send whatever planet the battle ids name.
    const auto other_planet = [](uint32_t* area_id, const char** game_id) {
        *area_id = 1800;
        *game_id = "c0ffee";
        return true;
    };
    BoneyardsLaunch other{};
    other.battle_ids = other_planet;
    bind_boneyards_launch(other);
    g_uploads = Uploads{};
    hooks.upload_settings(hooks.context, settings);
    check(
        g_uploads.calls.back() == "game_settings" && g_uploads.area_id == 1800 &&
            g_uploads.game_id == "c0ffee",
        "settings go out for the battle's planet"
    );
    hooks.upload_player(hooks.context, reporter::PlayerUpload{0x1234, "Ghost", "ARM", 3});
    check(
        g_uploads.calls.back() == "player_info" && g_uploads.area_id == 1800 &&
            g_uploads.game_id == "c0ffee",
        "a player goes out for the battle's planet"
    );
    hooks.upload_score(hooks.context, score);
    check(
        g_uploads.calls.back() == "score_report" && g_uploads.area_id == 1800 &&
            g_uploads.game_id == "c0ffee",
        "a score goes out for the battle's planet"
    );
    const auto calls_on_other = g_uploads.calls.size();
    boneyards_join_failed(nullptr, "Host not found");
    check(
        g_uploads.calls.size() == calls_on_other,
        "a failed join on another planet sends no leave yet"
    );
    bind_test_launch();
    oa::boneyards::rover_module_unload();
}

// The bridge hands the Boneyards' lines to network play, which, with no
// match to take them, puts them into the chat ring and raises the message
// flag; the bridge also empties the chat ring.
/// Installs a relay route against a loopback port nothing is listening on.
///
/// connect returns once the attempt is started, including when the port
/// refuses it, and the route is installed either way.
///
/// @return true when a route is installed
bool install_refused_route() {
    oa::boneyards::OverlayLaunch overlay{};
    overlay.overlay_ip[0] = 10;
    overlay.overlay_ip[1] = 66;
    overlay.overlay_ip[2] = 0;
    overlay.overlay_ip[3] = 2;
    overlay.key[0] = 1;
    overlay.key_len = 1;
    overlay.key_id = 1;
    overlay.relay_host = "127.0.0.1";
    overlay.relay_port = 1;
    boneyards_install_overlay_route(overlay);
    return oa::netgame::sock::overlay_route() != nullptr;
}

// Each way out of a battle drops the relay route. A battle still being set
// up, or still being played, does not: tearing a match down is how a new
// battle starts, and losing the Boneyards while the battle is on leaves the
// route installed.
void test_tunnel_exits() {
    check(install_refused_route(), "a battle installs a relay route");
    const auto bridge = boneyards_launch_bridge();
    bridge.launch_ended(bridge.user);
    check(
        oa::netgame::sock::overlay_route() == nullptr,
        "the battle room's back button drops the route"
    );

    check(install_refused_route(), "the route installs again after the battle room");
    // MatchEvent: finished 0, torn_down 1, left 2, watching_kept 5.
    check(
        !boneyards_match_event_leaves(1) && !boneyards_match_event_leaves(5) &&
            boneyards_match_event_leaves(0) && boneyards_match_event_leaves(2) &&
            oa::netgame::sock::overlay_route() != nullptr,
        "setting the battle up keeps the route; finishing or leaving drops it"
    );
    boneyards_match_ended();
    check(oa::netgame::sock::overlay_route() == nullptr, "the match ending drops the route");

    check(install_refused_route(), "the route installs again after the match");
    boneyards_join_failed(nullptr, "Host not found");
    check(oa::netgame::sock::overlay_route() == nullptr, "a failed join drops the route");

    check(install_refused_route(), "the route installs again after the failed join");
    boneyards_overlay_lost(true);
    check(
        oa::netgame::sock::overlay_route() != nullptr,
        "losing the Boneyards during a match keeps the route"
    );
    boneyards_overlay_lost(false);
    check(
        oa::netgame::sock::overlay_route() == nullptr,
        "losing the Boneyards outside a match drops the route"
    );

    check(install_refused_route(), "the route installs again after the disconnect");
    oa::test::netgame::LaunchRecord record{};
    oa::test::netgame::bind_launch_record(&record);
    bridge.leave_game(bridge.user);
    check(
        oa::netgame::sock::overlay_route() == nullptr && record.leaves_with_reason == 1,
        "quitting drops the route and leaves with the reason"
    );
    oa::test::netgame::bind_launch_record(nullptr);

    check(install_refused_route(), "the route installs again after quitting");
    boneyards_runtime_released();
    check(oa::netgame::sock::overlay_route() == nullptr, "releasing the runtime drops the route");
}

// No mod, and the Boneyards' own profile, go on. Another mod asks a player
// and is switched off with no question when nobody can answer. A mod the
// command line named is not switched off. A simulation change with no chosen
// mod is not a mod. Cancel keeps the mod. A quiet switch does not save the
// one-shot. The one-shot is taken once. Leaving asks once, and either answer
// forgets the mod. Only the mutators a match carries are counted.
void test_mod_gate() {
    check(
        decide_mod_entry(PlayedMod::none, EntryWay::player) == EntryDecision::proceed,
        "no mod asks nothing"
    );
    check(
        decide_mod_entry(PlayedMod::own, EntryWay::player) == EntryDecision::proceed,
        "the Boneyards' own profile goes on"
    );
    check(
        decide_mod_entry(PlayedMod::foreign, EntryWay::player) == EntryDecision::ask,
        "another mod asks"
    );
    check(
        decide_mod_entry(PlayedMod::foreign, EntryWay::quiet) == EntryDecision::switch_quietly,
        "a quiet run switches another mod off"
    );
    check(
        decide_mod_entry(PlayedMod::none, EntryWay::quiet) == EntryDecision::proceed,
        "a quiet run with no mod goes on"
    );
    check(
        decide_mod_entry(PlayedMod::foreign, EntryWay::player, true) ==
            EntryDecision::command_notice,
        "a command-line mod tells the player"
    );
    check(
        decide_mod_entry(PlayedMod::foreign, EntryWay::quiet, true) == EntryDecision::command_stay,
        "a quiet run stays when the command line named the mod"
    );
    check(
        decide_mod_entry(PlayedMod::own, EntryWay::player, true) == EntryDecision::proceed,
        "the Boneyards' own profile goes on from the command line"
    );
    check(
        decide_mod_entry(PlayedMod::none, EntryWay::quiet, true) == EntryDecision::proceed,
        "no chosen mod goes on when the command line is set"
    );
    check(!chosen_mod(false, false), "a simulation change with no chosen mod is not a mod");
    check(chosen_mod(true, false), "a chosen profile is a mod");
    check(chosen_mod(false, true), "a chosen folder is a mod");

    EntryCarry carry{};
    check(
        apply_mod_entry_answer(ModAnswer::cancel, carry) == ModEffect::stay && !carry.pending &&
            !carry.remembered,
        "Cancel keeps the mod"
    );
    check(
        apply_mod_entry_answer(ModAnswer::switch_off, carry) == ModEffect::switch_off &&
            carry.pending && carry.remembered,
        "Switch off remembers the mod and the entry"
    );
    EntryCarry quiet{};
    check(
        apply_mod_entry_answer(ModAnswer::switch_off, quiet, false) == ModEffect::switch_off &&
            !quiet.pending && quiet.remembered,
        "a quiet switch remembers the mod and does not save the one-shot"
    );
    check(
        take_entry_request(carry) && !carry.pending && carry.remembered,
        "the one-shot is taken once"
    );
    check(!take_entry_request(carry) && carry.remembered, "a second take finds no entry request");

    check(
        decide_mod_leave(false, false) == LeaveDecision::skip, "leaving asks nothing without a mod"
    );
    check(decide_mod_leave(true, false) == LeaveDecision::ask, "leaving asks once");
    check(decide_mod_leave(true, true) == LeaveDecision::skip, "leaving does not ask again");
    const LeaveEffect switched = apply_mod_leave(LeaveAnswer::switch_on);
    check(switched.switch_on && switched.forget, "Switch on switches back and forgets");
    const LeaveEffect later = apply_mod_leave(LeaveAnswer::not_now);
    check(!later.switch_on && later.forget, "Not now forgets");

    check(carried_mutator_count(0, 0) == 0, "a match with no mutators carries none");
    check(carried_mutator_count(0, 1) == 1, "an applied loadout counts");
    check(carried_mutator_count(2, 0) == 2, "the room's list counts");
    check(
        carried_mutator_count(static_cast<std::size_t>(0xffffffffu), 1) == 0xffffffffu,
        "the count saturates"
    );

    check(!mp::mutators_bar_locks_picker(false, 0), "an open room leaves the picker open");
    check(mp::mutators_bar_locks_picker(true, 0), "locked options lock the picker");
    check(mp::mutators_bar_locks_picker(false, 1), "a barred battle locks the picker");
    check(mp::mutators_bar_locks_picker(true, 1), "both locks lock the picker");
}

void test_battle_lines() {
    oa::Game& frontend = mp::multiplayer_game();
    frontend.gui_flags = 0;
    const auto bridge = boneyards_launch_bridge();
    bridge.battle_message(bridge.user, "Boneyards: WARNING! DISCONNECTED FROM THE BONEYARDS!");
    check((frontend.gui_flags & 1U) != 0, "network play takes the line");
    mp::lobby_chat_head(frontend) = 3;
    mp::lobby_chat_tail(frontend) = 5;
    bridge.empty_chat_ring(bridge.user);
    check(
        mp::lobby_chat_head(frontend) == 0 && mp::lobby_chat_tail(frontend) == 0,
        "the chat ring empties"
    );
}

} // namespace

bool test_battle_place(const char** planet, const char** sector) {
    *planet = "Rosewood";
    *sector = "Cygnus Reach";
    return true;
}

bool unknown_battle_place(const char** planet, const char** sector) {
    *planet = "";
    *sector = "";
    return true;
}

// A battle the Boneyards launched says what it knows of the game its replay is made of: its
// kind, its game id, its planet and its sector. Network play carries that to the replay's
// description, under `game`. A game no one launched, and a launch that is over, say nothing.
void test_recorded_game() {
    nl::LaunchBlock& block = api::launch_block();
    block = nl::LaunchBlock{};
    api::end_launch();
    api::Hooks hooks{};
    hooks.recorded_game = boneyards_recorded_game;
    api::set_hooks(hooks);
    BoneyardsLaunch boneyards{};
    boneyards.battle_ids = test_battle_ids;
    boneyards.battle_place = test_battle_place;
    boneyards.exit_prompt = &g_prompt;
    bind_boneyards_launch(boneyards);

    check(!recorded_game_facts().has_value(), "no launch: no account of the game");
    api::apply_launch();
    const auto facts = recorded_game_facts();
    check(
        facts.has_value() && facts->kind == "boneyards" &&
            facts->object_json ==
                R"({"game_id":"9bfaa0d0f7b93f12","planet":"Rosewood","sector":"Cygnus Reach"})",
        "the hook gives the game id, the planet and the sector"
    );

    // The account reaches the replay's description, under `game`.
    oa::session::demo::RecordingSetup setup{};
    setup.max_units = 20;
    setup.map_name = "Acid Pools";
    oa::session::demo::RecordingPlayer player{};
    player.player_id = 0x100;
    player.name = "Ghost_Commander";
    setup.players.push_back(player);
    oa::session::demo::ReplayFacts replay_facts{};
    if (facts.has_value()) {
        replay_facts.game_kind = facts->kind;
        replay_facts.game_kind_object = facts->object_json;
    }
    const auto replay = oa::session::demo::replay_begin(setup, replay_facts);
    const std::string described = oa::formats::oademo::meta_text(replay.meta);
    check(
        described.find(R"("game":{"kind":"boneyards")") != std::string::npos &&
            described.find(
                R"("boneyards":{"game_id":"9bfaa0d0f7b93f12","planet":"Rosewood","sector":"Cygnus Reach"})"
            ) != std::string::npos,
        "the game id, planet and sector reach meta.json's game member"
    );
    const auto written = oa::formats::oademo::write(replay);
    const auto read = oa::formats::oademo::read(written.bytes);
    check(
        written.ok() && read.ok() && read.replay->meta.game.kind == "boneyards" &&
            read.replay->meta.game.kind_object == facts->object_json,
        "a replay written with them reads back with them"
    );

    // A planet or sector the map does not name leaves its member out.
    boneyards.battle_place = unknown_battle_place;
    bind_boneyards_launch(boneyards);
    const auto unplaced = recorded_game_facts();
    check(
        unplaced.has_value() && unplaced->object_json == R"({"game_id":"9bfaa0d0f7b93f12"})",
        "an unnamed planet and sector are left out"
    );

    // A battle that is over is not recorded as one.
    api::end_launch();
    check(!recorded_game_facts().has_value(), "after the launch ends there is no account");
    api::apply_launch();
    boneyards.battle_ids = nullptr;
    bind_boneyards_launch(boneyards);
    check(
        !recorded_game_facts().has_value(), "a launch with no battle in the console has no account"
    );
    api::end_launch();
    api::set_hooks(api::Hooks{});
    block = nl::LaunchBlock{};
}

int main() {
    bind_test_launch();
    test_fill();
    test_launch();
    test_data_gate();
    test_mod_gate();
    test_factions();
    test_report_hooks();
    test_frontend_answers();
    test_report_uploads();
    test_battle_lines();
    test_tunnel_exits();
    test_recorded_game();
    bind_boneyards_launch({});
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("boneyards launch tests passed\n");
    return 0;
}

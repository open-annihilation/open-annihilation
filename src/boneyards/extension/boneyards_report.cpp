// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The match report the Boneyards keeps of a network game (boneyards_report.hpp).
#include "boneyards_report.hpp"

#include "boneyards_data.hpp"
#include "boneyards_launch.hpp"

namespace oa::app {

void boneyards_console_show_notice(const char* text);

namespace {

namespace api = oa::app::netgame::extension_api;
namespace reporter = oa::boneyards::reporter;

// The report, kept from report_start until the process exits.
reporter::Reporter g_report{};

// The refusal has already been shown for the data playing now.
bool g_report_notice_shown = false;

/// Shows the refusal once while the simulation may not be reported.
void note_report_refusal() noexcept {
    if (boneyards_report_simulation_allowed()) {
        g_report_notice_shown = false;
        return;
    }
    if (g_report_notice_shown)
        return;
    g_report_notice_shown = true;
    boneyards_console_show_notice(boneyards_data_refusal_message());
}

/// Returns the launch block's options while a launch is active (reporter::Hooks::online_options).
///
/// @param context unused
/// @return the options, or null without an active launch
const reporter::OnlineOptions* launch_options(void*) noexcept {
    static reporter::OnlineOptions options{};
    if (!api::launch_active())
        return nullptr;
    const auto& block = api::launch_block();
    options.player_limit = block.player_limit;
    options.time_limit = block.time_limit;
    options.pfs = block.pfs;
    options.provider = block.provider_label;
    options.tournament = block.tournament;
    options.address = block.address;
    return &options;
}

/// Tells whether a launch is active on data the Boneyards reports (reporter::Hooks::uploads_online).
///
/// @param context unused
/// @return whether the uploads may be sent
bool uploads_online(void*) noexcept {
    return api::launch_active() && boneyards_report_simulation_allowed();
}

/// Tells the reporter whether this simulation may be reported (reporter::Hooks::simulation_permitted).
///
/// @param context unused
/// @return whether a report may be sent
bool report_simulation_permitted(void*) noexcept {
    return boneyards_report_simulation_allowed();
}

/// Starts the report for the match (Hooks::report_start).
///
/// @param context unused
/// @param[in,out] world the match's world
/// @param options the game's options file
void report_start(
    void*, oa::World& world, const oa::data::campaign::CampaignFile* options
) noexcept {
    note_report_refusal();
    int32_t status = 0;
    const reporter::Hooks hooks = boneyards_reporter_hooks();
    (void)reporter::init_game_reporter(g_report, world.game, options, &status, hooks);
}

/// Reports an event of the match to every destination (Hooks::report_event).
///
/// @param context unused
/// @param[in,out] world the world the report reads
/// @param event a report_event value
void report_event(void*, oa::World& world, int32_t event) noexcept {
    note_report_refusal();
    (void)reporter::report_event(
        g_report, world, event, reporter::route_all, boneyards_reporter_hooks()
    );
}

/// Reports a chat line of the match (Hooks::report_chat).
///
/// @param context unused
/// @param[in,out] world the world the report reads
/// @param line the line
void report_chat(void*, oa::World& world, const char* line) noexcept {
    note_report_refusal();
    (void)reporter::report_chat(g_report, world, line, boneyards_reporter_hooks());
}

/// Steps the report once a frame of the running match (Hooks::report_step).
///
/// @param context unused
/// @param[in,out] world the running match's world
void report_step(void*, oa::World& world) noexcept {
    note_report_refusal();
    reporter::step_reporter(g_report, world, boneyards_reporter_hooks());
}

/// Shuts the report down after the closed session's event (Hooks::report_close).
///
/// @param context unused
void report_close(void*) noexcept {
    reporter::shutdown_reporter(g_report, boneyards_reporter_hooks());
}

/// Shuts the report down when it holds the reporting library, as the connection selection opens (Hooks::report_release).
///
/// @param context unused
void report_release(void*) noexcept {
    if (reporter::reporter_library_loaded(g_report))
        reporter::shutdown_reporter(g_report, boneyards_reporter_hooks());
}

} // namespace

reporter::Hooks boneyards_reporter_hooks() noexcept {
    reporter::Hooks hooks{};
    hooks.online_options = launch_options;
    hooks.uploads_online = uploads_online;
    hooks.simulation_permitted = report_simulation_permitted;
    boneyards_report_hooks(nullptr, hooks);
    return hooks;
}

const reporter::Reporter& boneyards_report() noexcept {
    return g_report;
}

void fill_boneyards_report_hooks(api::Hooks& hooks) noexcept {
    hooks.report_start = report_start;
    hooks.report_event = report_event;
    hooks.report_chat = report_chat;
    hooks.report_step = report_step;
    hooks.report_close = report_close;
    hooks.report_release = report_release;
}

} // namespace oa::app

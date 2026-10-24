// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// .tad demo playback: the recording's map and players become a match in
// which every recorded player is remote and the local slot only watches; the
// recorded packets arrive through the network receive path.
#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "network_play.hpp"
#include "oa/app/check_host.hpp"
#include "demo_state.hpp"
#include "net_options.hpp"
#include "wire_rules_binding.hpp"

#include "oa/netgame/match/net_match.hpp"
#include "oa/session/demo.hpp"
#include "oa/data/brain_library/seats.hpp"
#include "oa/data/mutators/choice.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace demo = oa::session::demo;

constexpr std::size_t kReportedExtraDefinitions = 8;
constexpr int32_t kFixedOne = 1 << 16;
constexpr uint64_t kMillisecondsPerSecond = 1000;

// FBI hash of every definition of the match's table in definition-index
// order, as the 0x1a handshake keys it (UnitDef.fbi_hash).
std::vector<uint32_t> unit_table_keys(const oa::data::defs::UnitDefTables& table) {
    std::vector<uint32_t> keys;
    for (uint32_t index = 1; index < table.count; ++index)
        keys.push_back(table.records[index].fbi_hash);
    return keys;
}

int32_t whole(int32_t fixed) {
    return fixed / kFixedOne;
}

// The recording's verdicts mark the unit types the watcher keeps.
void mark_recorded_units(void* context, oa::UnitDef* headers, uint32_t count) {
    oa::ui::frontend_multiplayer::unit_sync_mark_units(
        *static_cast<const oa::ui::frontend_multiplayer::UnitSync*>(context), headers, count
    );
}

} // namespace

void NetworkPlay::destroy_demo_session(DemoState* session) noexcept {
    demo::demo_session_end(session);
    delete session;
}

void NetworkPlay::start_demo_playback() {
    std::unique_ptr<DemoState, void (*)(DemoState*) noexcept> session{
        new DemoState(), destroy_demo_session
    };
    std::string error;
    // A recording the engine handed over comes first, the --play-demo file
    // otherwise.
    auto staged = std::exchange(netgame_context().staged_recording, std::nullopt);
    const bool loaded = staged
                            ? demo::demo_load(&session->playback, std::move(staged->bytes), &error)
                            : demo::demo_open(&session->playback, net_options().play_demo, &error);
    if (!loaded)
        throw std::runtime_error(error);
    const fs::path recorded = staged ? path_from_utf8(staged->name) : net_options().play_demo;
    const std::string needs = runtime_.content_switch_sentence(recorded);
    if (!needs.empty())
        throw std::runtime_error(needs);
    std::vector<oa::data::mutators::ResolvedMutator> playback_mutators;
    if (const oa::formats::tad::Sector* sector = oa::formats::tad::find_sector(
            session->playback.demo, oa::formats::tad::SectorType::mutators
        )) {
        const std::string_view mutator_text = oa::formats::tad::sector_text(*sector);
        if (!mutator_text.empty()) {
            const std::string refusal =
                runtime_.recorded_mutators_refusal(mutator_text, &playback_mutators);
            if (!refusal.empty())
                throw std::runtime_error(refusal);
        }
    }
    // The sector only names the brains. Playback mirrors every player, so
    // nothing is seated and a sector that cannot be read is not a refusal.
    if (const oa::formats::tad::Sector* brains = oa::formats::tad::find_sector(
            session->playback.demo, oa::formats::tad::SectorType::brains
        )) {
        const std::string_view brains_view = oa::formats::tad::sector_text(*brains);
        const std::optional<oa::data::brain_library::Seats> recorded =
            oa::data::brain_library::parse_seats(brains_view);
        if (recorded) {
            std::vector<const oa::data::brain_library::Seat*> order;
            order.reserve(recorded->seats.size());
            for (const oa::data::brain_library::Seat& recorded_seat : recorded->seats)
                order.push_back(&recorded_seat);
            std::stable_sort(
                order.begin(),
                order.end(),
                [](const oa::data::brain_library::Seat* left,
                   const oa::data::brain_library::Seat* right) {
                    return left->place < right->place;
                }
            );
            for (const oa::data::brain_library::Seat* recorded_seat : order) {
                if (recorded_seat->brain.empty())
                    continue;
                std::cerr << "demo brains: player " << recorded_seat->place << " played "
                          << recorded_seat->brain << ' ' << recorded_seat->version << " at "
                          << recorded_seat->level << '\n';
            }
        }
    }
    const bool ignore_unit_table = staged ? !staged->strict : net_options().demo_ignore_unit_table;
    const auto& recording = session->playback.demo;
    const auto& players = recording.players;
    session->playback.ten_player_replay =
        oa::app::netgame::wire_rules_of(runtime_.mod_profile()).ten_player_replay;
    const bool slotless = demo::demo_watches_without_slot(session->playback);
    if (players.size() >= OA_PLAYER_COUNT && !slotless)
        throw std::runtime_error("demo has no free slot for the watcher");
    if (!select_map_named(recording.map_name))
        throw std::runtime_error("demo map '" + recording.map_name + "' is not installed");
    const auto watcher = players.size();
    runtime_.state_.player_count = static_cast<uint16_t>(slotless ? watcher : watcher + 1);
    // The match is built for the view of the first recorded player, the
    // others as computer slots; demo_session_begin then makes every recorded
    // player remote and the watcher after them local.
    runtime_.keep_player_skirmish_settings();
    for (std::size_t slot = 0; slot < runtime_.skirmish_settings_.slots.size(); ++slot) {
        auto& target = runtime_.skirmish_settings_.slots[slot];
        target = {};
        target.alliance = entry::unassigned_alliance;
        target.controller = slot == 0        ? entry::controller::human
                            : slot < watcher ? entry::controller::computer
                                             : entry::controller::disabled;
        if (slot < watcher) {
            target.side = session->playback.players[slot].info.side;
            target.color = session->playback.players[slot].info.color;
        }
    }
    // The watcher leaves the battleroom with the recorded verdicts.
    // A recording without the sector plays with no mutators. The player's
    // own list comes back when this match ends, or at once when the start
    // throws.
    const auto* verdicts = demo::demo_unit_sync(session->playback);
    runtime_.play_loadout_once(std::move(playback_mutators));

    struct LoadoutOnceGuard {
        Runtime& runtime;
        int exceptions = std::uncaught_exceptions();

        ~LoadoutOnceGuard() {
            if (std::uncaught_exceptions() > exceptions)
                runtime.cancel_loadout_once();
        }
    } loadout_once{runtime_};

    runtime_.bootstrap_match(
        {.units_per_player = recording.max_units,
         .place_commanders = false,
         .defeat_allowed = false,
         .unit_filter =
             {const_cast<oa::ui::frontend_multiplayer::UnitSync*>(verdicts),
              verdicts != nullptr ? mark_recorded_units : nullptr},
         .replay = true}
    );
    if (!runtime_.match_ || runtime_.altitude_sight_blocked_)
        throw std::runtime_error("demo playback: " + runtime_.status_);

    const auto keys = unit_table_keys(runtime_.unit_table_.tables);
    std::vector<std::size_t> extra;
    const auto check = demo::demo_check_unit_table(session->playback, keys, &extra);
    std::string table_line = "demo unit table: " + std::to_string(check.recorded) + " recorded, " +
                             std::to_string(check.matched) + " matched, " +
                             std::to_string(check.missing) + " missing, " +
                             std::to_string(check.extra) + " extra here";
    for (std::size_t i = 0; i < extra.size() && i < kReportedExtraDefinitions; ++i)
        table_line += (i == 0 ? " (" : ", ") + runtime_.spawn_type_names_[extra[i] + 1];
    if (!extra.empty())
        table_line += extra.size() > kReportedExtraDefinitions ? ", ...)" : ")";
    std::cerr << table_line << '\n';
    const oa::data::mutators::Loadout* applied = runtime_.match_loadout();
    const std::size_t applied_count = applied == nullptr ? 0 : applied->entries.size();
    std::cerr << "demo mutators: " << applied_count << " applied\n";
    if (!check.identical && !ignore_unit_table)
        throw std::runtime_error(
            "demo playback needs the recording's unit definitions: " + table_line
        );
    demo_unit_table_differs_ = !check.identical;
    session->unicode_chat = unicode_chat();
    // The recorded game's notices read in the language shown.
    session->game_text_context = this;
    session->translate_game_text = translate_text;
    if (!demo::demo_session_begin(session.get(), runtime_.match_.get(), &error))
        throw std::runtime_error("demo playback: " + error);
    // Mission start rebuilds the sight grids once the players are seated.
    runtime_.reset_match_sight(true);
    // The recorded game is loaded, as a network game is; its players' pass
    // waits for the first tick.
    session->match->finish_load();
    // The view is the watcher's: it owns no units, so nothing can be
    // ordered and no unit is announced as its own, and with mapping and
    // line of sight off the whole map shows.
    const uint8_t view = slotless ? 0 : session->watcher_slot;
    runtime_.match_local_player_ = view;
    demo_speed_ = 0;
    demo_ = std::move(session);
    runtime_.enter_match_view();
    runtime_.status_ =
        "Demo playback: " + recording.map_name + ", " + std::to_string(players.size()) +
        " players, " + std::to_string(demo_->playback.frames.size()) + " frames over " +
        std::to_string(demo::demo_duration_ms(demo_->playback) / kMillisecondsPerSecond) + " s";
    std::cerr << runtime_.status_ << '\n';
}

// Headless snapshots look at --camera, else at the followed unit.
void NetworkPlay::place_demo_camera() {
    if (const auto& camera = runtime_options(runtime_).camera) {
        runtime_.match_camera_x_ = camera->first;
        runtime_.match_camera_z_ = camera->second;
    } else if (
        const auto* unit = world_unit_at(&runtime_.match_->state(), demo_->tracked_unit);
        unit != nullptr && unit->type_index != 0
    ) {
        runtime_.center_camera_on_unit(demo_->tracked_unit);
    }
}

// Recorded 0x19 speed changes pace playback as they paced the recorded game.
void NetworkPlay::demo_frame() {
    if (!demo_ || !runtime_.match_)
        return;
    const auto speed = runtime_.match_->state().game.requested_speed;
    const auto range = runtime_.game_speed_range();
    if (speed == demo_speed_ || speed < range.slowest || speed > range.fastest)
        return;
    demo_speed_ = speed;
    runtime_.match_timing_.requested_rate = speed;
    runtime_.match_timing_.actual_rate = speed;
}

void NetworkPlay::step_demo_frame() {
    if (!demo_ || !runtime_.match_ || demo_->match != runtime_.match_.get()) {
        demo_.reset();
        return;
    }
    const auto errors_before = demo_->tick_errors;
    demo::demo_session_frame(demo_.get());
    runtime_.match_timing_.tick = runtime_.match_->state().game.tick;
    if (demo_->tick_errors != errors_before)
        runtime_.report_match_tick_error(demo_->last_error);
    for (const auto& line : demo_->lines)
        runtime_.console_post_message(line);
    demo_->lines.clear();
}

// Replays up to ticks networked ticks and reports the full unit records the
// recording carried for the first recorded player's first unit, and how far
// every unit had drifted from each of its full records.
int NetworkPlay::run_headless_demo(std::size_t ticks) {
    start_demo_playback();
    const Options& options = runtime_options(runtime_);
    const CheckHost host = check_host(runtime_);
    // The viewer watches, so Enter opens no chat line and no
    // console command runs during playback.
    if (options.headless_check) {
        bool running = true;
        SDL_Event enter{};
        enter.type = SDL_EVENT_KEY_DOWN;
        enter.key.key = SDLK_RETURN;
        enter.key.scancode = SDL_SCANCODE_RETURN;
        runtime_.handle_sdl_event(enter, running);
        if (!runtime_.local_player_watches() || runtime_.chat_composing_)
            throw std::runtime_error("demo playback opened the viewer's chat line");
    }
    runtime_.match_layout_ = runtime_.lay_out_match(options.match_width, options.match_height);
    const auto& world = runtime_.match_->state();
    const auto& playback = demo_->playback;
    std::printf(
        "demo %s: %zu players, %zu frames, %llu ms, unit limit %u\n",
        playback.demo.map_name.c_str(),
        playback.demo.players.size(),
        playback.frames.size(),
        static_cast<unsigned long long>(demo::demo_duration_ms(playback)),
        playback.demo.max_units
    );
    // A snapshot needs every frame drawn: mapped terrain accumulates as drawn.
    const bool drawing = !options.snapshot.empty();
    if (drawing) {
        runtime_.match_zoom_ =
            std::clamp(options.match_zoom, kMinBattlefieldZoom, kMaxBattlefieldZoom);
        runtime_.match_zoom_target_ = runtime_.match_zoom_;
    }
    bool finished = false;
    for (std::size_t tick = 1; tick <= ticks && !finished && demo_; ++tick) {
        step_demo_frame();
        if (!demo_)
            break;
        if (drawing) {
            place_demo_camera();
            host.compose(host.context);
        }
        finished = demo::demo_session_finished(*demo_);
    }
    if (!demo_)
        return 1;
    std::printf(
        "demo tick %u: digest %016llx, %u live units, frames %u/%zu%s\n",
        world.game.tick,
        static_cast<unsigned long long>(demo::demo_world_digest(&world)),
        demo::demo_live_units(&world),
        playback.stats.frames_delivered,
        playback.frames.size(),
        finished ? ", finished" : ""
    );
    for (const auto& sample : demo_->tracked)
        std::printf(
            "unit %u full record at tick %u: recorded (%d, %d, %d), replay before (%d, %d, %d)\n",
            demo_->tracked_unit,
            sample.tick,
            whole(sample.recorded.x),
            whole(sample.recorded.y),
            whole(sample.recorded.z),
            whole(sample.before.x),
            whole(sample.before.y),
            whole(sample.before.z)
        );
    const auto& records = demo_->full_records;
    std::printf("full records: %u placed\n", records.placements);
    for (const auto& [kind, drift] :
         {std::pair{"ground", &records.ground}, std::pair{"air", &records.air}})
        std::printf(
            "full records %s: %u measured, %u moved, drift mean %.1f max %u\n",
            kind,
            drift->measured,
            drift->moved,
            drift->measured != 0 ? static_cast<double>(drift->drift_sum) / drift->measured : 0.0,
            drift->drift_max
        );
    const auto& stats = playback.stats;
    std::printf(
        "demo summary: records applied %u refused %u, created %u refused %u past table %u, record "
        "errors %u, "
        "tick errors %u, dropped frames %u, oversized %u, bad packets %u, untimed unit states "
        "%u%s%s\n",
        demo_->net.records_applied,
        demo_->net.records_refused,
        demo_->binding.created_remote,
        demo_->binding.refused_creates,
        demo_->binding.creates_past_table,
        demo_->net.record_errors,
        demo_->tick_errors,
        demo_->connection.packets != nullptr ? demo_->connection.packets->dropped_frames : 0u,
        stats.frames_oversized,
        stats.bad_packets,
        stats.untimed_unit_states,
        demo_->last_error.empty() ? "" : "; last error: ",
        demo_->last_error.c_str()
    );
    if (stats.unknown_senders != 0)
        std::printf("demo packets from unknown senders dropped: %u\n", stats.unknown_senders);
    std::fflush(stdout);
    if (drawing)
        write_ppm(options.snapshot, *host.surface(host.context));
    // The followed unit's full records ride on every units_per_player-th
    // sender tick. Over a different unit table, record errors and creates
    // past the table are only reported (demo_session_verdict).
    const auto period = static_cast<int64_t>(runtime_.match_->state().game.units_per_player);
    const auto verdict = demo::demo_session_verdict(*demo_, demo_unit_table_differs_);
    const bool clean = verdict.clean;
    const bool paced = verdict.paced;
    if (demo_unit_table_differs_)
        std::printf("demo unit table differs: unit types and unit-state deltas are not verified\n");
    if (!clean)
        std::printf("demo check failed: the replay was not clean\n");
    if (!paced)
        std::printf(
            "demo check failed: unit %u's full records were not %lld ticks apart\n",
            demo_->tracked_unit,
            static_cast<long long>(period)
        );
    return clean && paced ? 0 : 1;
}

} // namespace oa::app

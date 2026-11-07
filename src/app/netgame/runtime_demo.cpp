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
#include "oa/core/unit_def.h"
#include "oa/data/brain_library/seats.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/mutators/choice.hpp"

#include <SDL3/SDL.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace demo = oa::session::demo;

constexpr std::size_t kReportedExtraDefinitions = 8;
constexpr int32_t kFixedOne = 1 << 16;
constexpr uint64_t kMillisecondsPerSecond = 1000;
constexpr std::string_view kNeedsMod =
    "This recording needs a mod with {n} weapon types and {m} sides.";
constexpr std::string_view kBaseLimits = "This recording was made with 256 weapon types and five "
                                         "sides. The mod you are playing has more.";

/// The catalogue sentence with the sector's two counts in place of {n} and {m}.
///
/// @param english the catalogue key
/// @param weapon_types weapon types the recording names
/// @param sides sides the recording names
/// @return the sentence
std::string limits_sentence(std::string_view english, uint32_t weapon_types, uint32_t sides) {
    std::string sentence{oa::data::languages::interface_text(english)};
    const auto put = [&](std::string_view token, uint32_t value) {
        const std::size_t at = sentence.find(token);
        if (at != std::string::npos)
            sentence.replace(at, token.size(), std::to_string(value));
    };
    put("{n}", weapon_types);
    put("{m}", sides);
    return sentence;
}

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

// A campaign mission's table is the definitions it kept. Playback keeps those
// and drops the rest, then the catalog sorts them as the mission did, so a
// created unit's type index is the mission's.
std::unordered_set<uint32_t> mission_unit_hashes(const demo::DemoPlayback& playback) {
    std::unordered_set<uint32_t> hashes;
    const auto records = oa::formats::tad::unit_check_records(playback.demo);
    for (const auto bytes : records) {
        oa::netgame::UnitDefHandshakeRecord record{};
        if (oa::netgame::decode_record(bytes.data(), bytes.size(), &record) !=
            oa::netgame::WireError::ok)
            continue;
        if (record.subtype == static_cast<uint8_t>(oa::netgame::HandshakeSubtype::def_checksum))
            hashes.insert(record.key);
    }
    return hashes;
}

void mark_mission_units(void* context, oa::UnitDef* headers, uint32_t count) {
    const auto* kept = static_cast<const std::unordered_set<uint32_t>*>(context);
    for (uint32_t type = 1; type < count; ++type) {
        auto& unit = headers[type];
        unit.flags &= ~OA_UNIT_DEF_FLAG_AVAILABLE;
        if (kept != nullptr && kept->contains(unit.fbi_hash))
            unit.flags |= OA_UNIT_DEF_FLAG_AVAILABLE;
    }
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
    // A staged recording's name is the file name alone. The full path was
    // stored by whoever handed the bytes over. A file named on the command
    // line is the path itself.
    runtime_.remember_replay_file(static_cast<bool>(staged), recorded);
    const std::string needs = runtime_.content_switch_sentence(recorded);
    if (!needs.empty())
        throw std::runtime_error(needs);
    const auto rules = oa::app::netgame::wire_rules_of(runtime_.mod_profile());
    switch (demo::demo_limits_verdict(session->playback, rules.weapon_types, rules.sides)) {
    case demo::DemoLimitsVerdict::plays:
        break;
    case demo::DemoLimitsVerdict::other_limits: {
        const auto recorded_limits =
            session->playback.limits.value_or(oa::formats::tad::RecordedLimits{});
        throw std::runtime_error(
            limits_sentence(kNeedsMod, recorded_limits.weapon_types, recorded_limits.sides)
        );
    }
    case demo::DemoLimitsVerdict::base_limits:
        throw std::runtime_error(std::string{oa::data::languages::interface_text(kBaseLimits)});
    case demo::DemoLimitsVerdict::unreadable:
        throw std::runtime_error("demo playback: the recording's limits cannot be read");
    }
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
        const std::optional<oa::data::brain_library::Seats> parsed =
            oa::data::brain_library::parse_seats(brains_view);
        if (parsed) {
            std::vector<const oa::data::brain_library::Seat*> order;
            order.reserve(parsed->seats.size());
            for (const oa::data::brain_library::Seat& recorded_seat : parsed->seats)
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
    // The stream names the map. A map only the campaign loader accepts is a
    // mission, whatever the container's description says, and playback keeps
    // the definitions the recording's unit checks name.
    session->playback.mission_table = mission_map_taken_;
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
    // Named before the match is built: the marker reads it while bootstrap runs.
    std::unordered_set<uint32_t> mission_hashes;
    void* filter_context = const_cast<oa::ui::frontend_multiplayer::UnitSync*>(verdicts);
    void (*filter_mark)(void*, oa::UnitDef*, uint32_t) = nullptr;
    if (session->playback.mission_table) {
        mission_hashes = mission_unit_hashes(session->playback);
        filter_context = &mission_hashes;
        filter_mark = mark_mission_units;
    } else if (verdicts != nullptr) {
        filter_mark = mark_recorded_units;
    }
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
         .unit_filter = {filter_context, filter_mark},
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
    // The meteor storm's state is the runtime's, and the match's next tick reads it.
    session->keyframe_extension.context = &runtime_;
    session->keyframe_extension.save_sections = [](void* context,
                                                   sim::match_runtime::keyframe::Writer* out) {
        const auto bytes = static_cast<Runtime*>(context)->meteor_state_bytes();
        out->begin("app.meteors");
        out->bytes(bytes.data(), bytes.size());
    };
    session->keyframe_extension.load_sections = [](void* context,
                                                   sim::match_runtime::keyframe::Reader* in) {
        const auto bytes = static_cast<Runtime*>(context)->meteor_state_bytes();
        if (in->begin("app.meteors"))
            return in->bytes(bytes.data(), bytes.size());
        return in->ok();
    };
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
    demo_ = std::move(session);
    runtime_.enter_match_view();
    arm_replay_controls();
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

// A recorded speed change paces playback, multiplied by the viewer's speed. It does not replace
// the speed the viewer chose.
void NetworkPlay::demo_frame() {
    if (!demo_ || !runtime_.match_)
        return;
    note_recorded_speed();
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
    // A replay that reached a keyframe's tick with another digest missed state the keyframe lacked.
    auto& faults = demo_->keyframe_faults;
    if (keyframe_faults_logged_ > faults.size())
        keyframe_faults_logged_ = 0;
    for (; keyframe_faults_logged_ < faults.size(); ++keyframe_faults_logged_) {
        const auto& fault = faults[keyframe_faults_logged_];
        std::fprintf(
            stderr,
            "open-annihilation: keyframe fault at tick %u: the replay reached digest %016llx, the "
            "keyframe holds %016llx\n",
            fault.tick,
            static_cast<unsigned long long>(fault.found),
            static_cast<unsigned long long>(fault.expected)
        );
    }
}

namespace {

// How many ticks a keyframe check plays between its keyframe and the comparison.
constexpr uint32_t kKeyframeCheckTicks = 3000;

double milliseconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

} // namespace

bool NetworkPlay::run_keyframe_check(uint32_t at, bool* finished) {
    using clock = std::chrono::steady_clock;
    const auto& world = *demo_->net.world;
    const auto digest_hex = [](uint64_t digest) {
        char hex[17];
        std::snprintf(hex, sizeof hex, "%016llx", static_cast<unsigned long long>(digest));
        return std::string(hex);
    };
    const auto play_one = [&] {
        step_demo_frame();
        if (demo_)
            *finished = demo::demo_session_finished(*demo_);
        return demo_ != nullptr;
    };
    const auto no_keyframe = [&](uint32_t tick, const std::string& why) {
        std::printf("keyframe check at %u: no keyframe at tick %u: %s\n", at, tick, why.c_str());
        std::fflush(stdout);
        return false;
    };
    std::string refusal;
    // The start of the replay is the base every later keyframe is stored against.
    demo::DemoKeyframe base;
    if (!demo::demo_keyframe_take(demo_.get(), &base, &refusal))
        return no_keyframe(world.game.tick, refusal);
    while (world.game.tick < at && !*finished)
        if (!play_one())
            return false;
    if (world.game.tick < at) {
        std::printf("keyframe check at %u: the replay ends at tick %u\n", at, world.game.tick);
        std::fflush(stdout);
        return true;
    }
    // The keyframe, and what it costs. The timed take is the one a viewer makes, with the default
    // options: the measurement of what the image compresses to without the base copies and
    // compresses the image once more, so it is a take of its own, after the clock stops.
    demo::DemoKeyframe key;
    auto started = clock::now();
    if (!demo::demo_keyframe_take(demo_.get(), &key, &refusal))
        return no_keyframe(at, refusal);
    const double take_ms = milliseconds_since(started);
    demo::DemoKeyframeOptions options;
    options.measure_plain = true;
    demo::DemoKeyframe measured;
    if (!demo::demo_keyframe_take(demo_.get(), &measured, &refusal, options))
        return no_keyframe(at, refusal);
    key.plain_frame_bytes = measured.plain_frame_bytes;
    // The first run: the ticks after the keyframe, each tick's digest kept.
    std::vector<uint64_t> first_run;
    while (first_run.size() < kKeyframeCheckTicks && !*finished) {
        if (!play_one())
            return false;
        first_run.push_back(demo::demo_world_digest(&world));
    }
    const uint64_t digest_a = demo::demo_world_digest(&world);
    const uint32_t end_tick = world.game.tick;
    // What the match holds where the run ended: the second run must end holding the same.
    std::vector<uint8_t> image_a_fixed;
    std::vector<uint8_t> image_a_varying;
    demo::DemoKeyframe end_key;
    options = {};
    options.image_fixed = &image_a_fixed;
    options.image_varying = &image_a_varying;
    if (!demo::demo_keyframe_take(demo_.get(), &end_key, &refusal, options))
        return no_keyframe(end_tick, refusal);
    // The second run: restored, and the same ticks again.
    std::string error;
    started = clock::now();
    if (!demo::demo_keyframe_restore(demo_.get(), key, &end_key, &error)) {
        std::printf("keyframe check at %u: restore failed: %s\n", at, error.c_str());
        std::fflush(stdout);
        return false;
    }
    const double restore_ms = milliseconds_since(started);
    *finished = demo::demo_session_finished(*demo_);
    uint32_t first_difference = 0;
    for (std::size_t i = 0; i < first_run.size() && !*finished; ++i) {
        if (!play_one())
            return false;
        if (first_difference == 0 && demo::demo_world_digest(&world) != first_run[i])
            first_difference = world.game.tick;
    }
    const uint64_t digest_b = demo::demo_world_digest(&world);
    const bool same = digest_a == digest_b && world.game.tick == end_tick && first_difference == 0;
    const bool faulted = !demo_->keyframe_faults.empty();
    demo_->keyframe_faults.clear();
    keyframe_faults_logged_ = 0;
    // The match as the second run left it, against the match as the first did.
    std::vector<uint8_t> image_b_fixed;
    std::vector<uint8_t> image_b_varying;
    demo::DemoKeyframe check_key;
    options = {};
    options.image_only = true;
    options.image_fixed = &image_b_fixed;
    options.image_varying = &image_b_varying;
    std::string state = "same";
    if (!demo::demo_keyframe_take(demo_.get(), &check_key, &refusal, options)) {
        state = "not compared: " + refusal;
    } else if (
        const std::string sections = demo::demo_keyframe_difference(
            image_a_fixed, image_a_varying, image_b_fixed, image_b_varying
        );
        !sections.empty()
    ) {
        state = "differs in " + sections;
    }
    std::printf(
        "keyframe at %u: raw %zu bytes (fixed %zu, varying %zu), compressed %zu bytes, without the "
        "base %zu bytes, held %zu bytes, taken in %.2f ms, restored in %.2f ms\n",
        key.tick,
        key.raw_bytes(),
        key.fixed_bytes,
        key.varying_bytes,
        key.frame.size(),
        key.plain_frame_bytes,
        key.memory_bytes(),
        take_ms,
        restore_ms
    );
    // The image's sections by size: the inventory docs/replays.md keeps is made from this line.
    std::string sizes;
    for (const auto& [name, bytes] : demo::demo_keyframe_sections(image_a_varying))
        sizes += (sizes.empty() ? "" : ", ") + name + " " + std::to_string(bytes);
    std::printf("keyframe sections at %u: %s\n", end_tick, sizes.c_str());
    std::printf(
        "keyframe check at %u: digest %s, restored %s, %s\n",
        at,
        digest_hex(digest_a).c_str(),
        digest_hex(digest_b).c_str(),
        same ? "same" : "differs"
    );
    if (first_difference != 0)
        std::printf("keyframe check at %u: first differing tick %u\n", at, first_difference);
    std::printf("keyframe check at %u: state at tick %u: %s\n", at, end_tick, state.c_str());
    std::fflush(stdout);
    return same && !faulted;
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
    // The map's own features, before any recorded change is applied. A replay
    // of a saved game starts here, not from the save.
    print_feature_cells("demo features at start", world);
    bool finished = false;
    bool keyframes_same = true;
    if (const auto check_at = net_options().replay_keyframe_check)
        keyframes_same = run_keyframe_check(*check_at, &finished);
    if (!demo_)
        return 1;
    // The replay goes on from where the check left it (the start, without one). A counter would
    // stop short after a jump back: the loop runs while the play head is still before the limit,
    // and a seek runs to its target even when that is past the limit.
    if (const std::size_t base = demo::demo_keyframe_base_bytes(*demo_); base != 0)
        demo_->clock.reel().set_base_bytes(base);
    if (demo_->keyframe_scratch_bytes != 0 || demo_->keyframe_image_bytes != 0)
        demo_->clock.reel().set_working_bytes(
            demo_->keyframe_scratch_bytes + demo_->keyframe_image_bytes
        );
    follow_play_head(world.game.tick);
    perform_due_seeks(false, &finished, {}, false, nullptr);
    while (demo_ && !finished && world.game.tick < ticks) {
        step_demo_frame();
        if (!demo_)
            break;
        if (drawing) {
            place_demo_camera();
            host.compose(host.context);
        }
        follow_play_head(world.game.tick);
        finished = demo::demo_session_finished(*demo_);
        perform_due_seeks(false, &finished, {}, false, nullptr);
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
    // Each seated player's units, kills, losses, metal and energy, so a
    // skirmish recording can be compared with the skirmish that wrote it.
    // A watcher is included.
    {
        std::array<uint32_t, OA_PLAYER_COUNT> live_by_owner{};
        for (uint32_t unit_slot = 1; unit_slot < world.unit_slot_count; ++unit_slot) {
            const auto& unit = world.units[unit_slot];
            if (unit.type_index == 0 || unit.owner_index >= OA_PLAYER_COUNT)
                continue;
            ++live_by_owner[unit.owner_index];
        }
        for (uint8_t player_slot = 0; player_slot < OA_PLAYER_COUNT; ++player_slot) {
            const auto& player = world.game.players[player_slot];
            if (player.in_use == 0 || player.status == OA_PLAYER_STATUS_FREE)
                continue;
            std::printf(
                "demo player: slot %u units %u kills %d losses %d metal %.9g energy %.9g\n",
                static_cast<unsigned>(player_slot),
                live_by_owner[player_slot],
                static_cast<int>(player.kills),
                static_cast<int>(player.losses),
                static_cast<double>(player.metal),
                static_cast<double>(player.energy)
            );
        }
    }
    print_feature_cells("demo features", world);
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
    std::printf("%s\n", demo::demo_summary_line(demo::demo_summary_of(*demo_)).c_str());
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
    return clean && paced && keyframes_same ? 0 : 1;
}

namespace {

using clock = std::chrono::steady_clock;

/// Whether a frame's time budget is spent.
///
/// @param started when the frame's ticks began
/// @param budget_ms the budget, in milliseconds
/// @return true when the budget is spent
bool budget_spent(clock::time_point started, uint32_t budget_ms) {
    return clock::now() - started >= std::chrono::milliseconds(budget_ms);
}

/// Whether more than half of a frame's time budget is spent.
///
/// @param started when the frame's ticks began
/// @param budget_ms the budget, in milliseconds
/// @return true when more than half is spent
bool budget_half_spent(clock::time_point started, uint32_t budget_ms) {
    return (clock::now() - started) * 2 >= std::chrono::milliseconds(budget_ms);
}

constexpr char kJumpUnavailable[] = "Jumping back isn't available in this replay.";
constexpr char kJumpNeedsMemory[] = "Jumping back needs more memory than this device has.";
constexpr char kJumpStopped[] = "This replay could not jump to {time} and has stopped.";

void write_pause_bit(oa::Game& game, bool paused) {
    const uint16_t bit = oa::netgame::match::run_flag_paused;
    if (paused) {
        game.sim_run_flags = static_cast<uint16_t>(game.sim_run_flags | bit);
        return;
    }
    game.sim_run_flags = static_cast<uint16_t>(game.sim_run_flags & static_cast<uint16_t>(~bit));
}

bool pause_bit_set(const oa::Game& game) {
    return (game.sim_run_flags & oa::netgame::match::run_flag_paused) != 0;
}

} // namespace

void NetworkPlay::arm_replay_controls() {
    if (!demo_)
        return;
    // The frame clock lives on this play, which outlives a replay. The next
    // replay's first frame plans nothing, not the gap since the last one.
    replay_have_ms_ = false;
    replay_last_ms_ = 0;
    std::size_t budget = demo::keyframe_budget_bytes;
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    budget = demo::keyframe_budget_bytes_ios;
#endif
    demo_->clock.reel().set_budget(budget);
    if (const auto speed = net_options().replay_speed)
        demo_->clock.set_speed(*speed);
    demo_->seeks = net_options().replay_seeks;
    note_recorded_speed();
    wrap_replay_sound();
    // The window takes the first keyframe while the replay loads. A headless run takes one only
    // when a jump was asked for; a plain headless replay takes none.
    const bool headless = runtime_options(runtime_).headless_check;
    demo_->watch_keyframes = !headless || !demo_->seeks.empty();
    if (demo_->watch_keyframes && demo_->match != nullptr)
        follow_play_head(demo_->match->state().game.tick);
}

void NetworkPlay::wrap_replay_sound() {
    auto* playing = demo_ != nullptr ? demo_->match : nullptr;
    if (playing == nullptr)
        return;
    auto& hook = playing->point_sound;
    // Already wrapped: a later match start replaces the hook, and this saves that one.
    if (hook.context == this)
        return;
    replay_saved_sound_ = hook;
    hook = {
        this,
        [](void* context) {
            const auto& saved = static_cast<NetworkPlay*>(context)->replay_saved_sound_;
            return saved.spatial != nullptr && saved.spatial(saved.context);
        },
        [](void* context,
           const char* name,
           const oa::sim::match_runtime::Match::PointSound& sound) {
            auto& play = *static_cast<NetworkPlay*>(context);
            if (play.demo_ != nullptr) {
                if (play.replay_hide_tick_)
                    return;
                if (!play.demo_->clock.allows_unit_sound(
                        name != nullptr ? name : "", play.replay_sound_ms_
                    ))
                    return;
            }
            const auto& saved = play.replay_saved_sound_;
            if (saved.play != nullptr)
                saved.play(saved.context, name, sound);
        }
    };
}

void NetworkPlay::note_recorded_speed() {
    auto* playing = demo_ != nullptr ? demo_->match : nullptr;
    if (!demo_ || playing == nullptr)
        return;
    const auto speed = static_cast<int32_t>(playing->state().game.requested_speed);
    const auto range = runtime_.game_speed_range();
    if (speed > 0 && speed >= range.slowest && speed <= range.fastest)
        demo_->clock.set_recorded_speed(static_cast<uint32_t>(speed));
}

void NetworkPlay::apply_replay_speech(const ReplayFrameHost& host) {
    if (!demo_ || host.unit_speech == nullptr || host.set_unit_speech == nullptr)
        return;
    if (demo_->clock.thins_sound()) {
        if (!demo_->speech_forced_off) {
            demo_->speech_was_on = host.unit_speech(host.context);
            demo_->speech_forced_off = true;
        }
        host.set_unit_speech(host.context, false);
        return;
    }
    if (!demo_->speech_forced_off)
        return;
    host.set_unit_speech(host.context, demo_->speech_was_on);
    demo_->speech_forced_off = false;
}

void NetworkPlay::follow_play_head(uint32_t play_tick) {
    if (!demo_)
        return;
    demo_->clock.note_tick(play_tick);
    note_replay_length(play_tick);
    // A catch-up keeps only keyframes the schedule still keeps when it lands: minute ones, and
    // ten-second ones within two minutes of the target. Thinning uses that same point, so a
    // keyframe taken for the landing is not dropped because the head has not reached it yet.
    const bool catching = demo_->seek.has_value();
    const uint32_t kept_near = catching ? demo_->seek->to : play_tick;
    if (demo_->watch_keyframes && demo::keyframe_due(play_tick) &&
        (!catching || demo::keyframe_kept(play_tick, kept_near)))
        take_replay_keyframe(play_tick);
    drop_replay_keyframes(demo_->clock.reel().thin(kept_near));
    drop_replay_keyframes(demo_->clock.reel().evict(kept_near));
}

void NetworkPlay::take_replay_keyframe(uint32_t play_tick) {
    if (!demo_ || demo_->keyframe_refused || demo_->keyframes.contains(play_tick))
        return;
    auto& reel = demo_->clock.reel();
    demo::DemoKeyframeOptions options;
    options.budget_bytes = reel.budget();
    const std::size_t aside = reel.base_bytes() + reel.working_bytes();
    const std::size_t held = reel.total_bytes();
    options.committed_bytes = held > aside ? held - aside : 0;
    std::string refusal;
    demo::DemoKeyframe key;
    if (!demo::demo_keyframe_take(demo_.get(), &key, &refusal, options)) {
        reel.set_working_bytes(0);
        if (!demo_->keyframe_refused) {
            std::fprintf(
                stderr,
                "open-annihilation: no keyframe at tick %u: %s\n",
                play_tick,
                refusal.c_str()
            );
            demo_->keyframe_refused = true;
        }
        const char* sentence =
            refusal == demo::keyframe_over_budget ? kJumpNeedsMemory : kJumpUnavailable;
        demo_->clock.set_unavailable(replay_player_line(sentence, 0));
        return;
    }
    reel.set_base_bytes(demo::demo_keyframe_base_bytes(*demo_));
    // The compress buffer and the writer's image stay with the session, and
    // the next take reuses them. The image is counted as the room the writer holds.
    reel.set_working_bytes(demo_->keyframe_scratch_bytes + demo_->keyframe_image_bytes);
    reel.add(play_tick, key.memory_bytes());
    demo_->keyframes.emplace(play_tick, std::move(key));
    // follow_play_head thins and evicts after this returns, against the landing of a catch-up.
    if (reel.base_bytes() + reel.working_bytes() > reel.budget()) {
        reel.forget(play_tick);
        demo_->keyframes.erase(play_tick);
        demo_->keyframe_scratch.reset();
        demo_->keyframe_scratch_bytes = 0;
        demo_->keyframe_image_bytes = 0;
        demo_->keyframe_image = sim::match_runtime::keyframe::Writer{};
        reel.set_working_bytes(0);
        if (!demo_->keyframe_refused) {
            std::fprintf(
                stderr,
                "open-annihilation: no keyframe at tick %u: %s\n",
                play_tick,
                demo::keyframe_over_budget
            );
            demo_->keyframe_refused = true;
        }
        demo_->clock.set_unavailable(replay_player_line(kJumpNeedsMemory, 0));
        return;
    }
    // A keyframe the reel dropped for room is not a refusal. One it kept clears an older sentence.
    if (demo_->keyframes.contains(play_tick))
        demo_->clock.set_unavailable({});
}

void NetworkPlay::drop_replay_keyframes(const std::vector<uint32_t>& ticks) {
    if (!demo_)
        return;
    for (const uint32_t play_tick : ticks)
        demo_->keyframes.erase(play_tick);
}

void NetworkPlay::note_replay_length(uint32_t play_tick) {
    if (!demo_)
        return;
    uint32_t known = demo_->clock.furthest();
    if (!demo_->playback.frames.empty())
        known = std::max(known, demo_->playback.frames.back().due_tick);
    if (demo::demo_session_finished(*demo_))
        known = std::max(known, play_tick);
    demo_->clock.note_length(known);
}

bool NetworkPlay::perform_due_seeks(
    bool windowed,
    bool* finished,
    clock::time_point deadline,
    bool bound,
    const ReplayFrameHost* host
) {
    if (!demo_)
        return false;
    bool sought = false;
    for (;;) {
        if (!demo_)
            return sought;
        if (demo_->seek) {
            continue_seek(windowed, finished, deadline, bound, host);
            sought = true;
            if (!demo_ || demo_->seek)
                return true;
            continue;
        }
        auto* playing = demo_->match;
        if (playing == nullptr || demo_->seeks.empty())
            return sought;
        const demo::ReplayJump waiting = demo_->seeks.front();
        const uint32_t play_tick = playing->state().game.tick;
        // Only the first jump is armed. A FROM already reached, or passed, fires now.
        if (!waiting.immediate && play_tick < waiting.from)
            return sought;
        const demo::ReplayJump jump = waiting;
        demo_->seeks.erase(demo_->seeks.begin());
        begin_seek(jump, host);
        if (!demo_)
            return true;
        // A jump back from the end restores a keyframe the recording goes on
        // from. Without this the catch-up would still see the ended recording
        // and stop on the keyframe, short of the tick asked for.
        if (finished != nullptr)
            *finished = demo::demo_session_finished(*demo_);
        if (demo_->seek)
            sought = true;
    }
}

void NetworkPlay::begin_seek(const demo::ReplayJump& jump, const ReplayFrameHost* host) {
    auto* playing = demo_ != nullptr ? demo_->match : nullptr;
    if (!demo_ || playing == nullptr)
        return;
    // Before the restore, so the printed time includes it and every frame of the skip.
    const auto started = clock::now();
    const auto stop_playback = [&](const char* detail) {
        std::fprintf(stderr, "open-annihilation: replay seek failed: %s\n", detail);
        if (host != nullptr) {
            replay_stop_sentence_ = replay_player_line(kJumpStopped, jump.to);
            if (host->tell != nullptr)
                host->tell(host->context, replay_stop_sentence_.c_str());
            if (host->stop != nullptr)
                host->stop(host->context);
        }
        demo_.reset();
    };
    const uint32_t current = playing->state().game.tick;
    const demo::SeekPlan plan =
        demo_->clock.reel().plan_seek(current, jump.to, demo_->clock.furthest());
    if (plan.kind == demo::SeekPlan::Kind::restore) {
        if (!plan.restore_tick || !demo_->keyframes.contains(*plan.restore_tick)) {
            const std::string detail =
                "no keyframe at tick " + std::to_string(plan.restore_tick.value_or(jump.to));
            stop_playback(detail.c_str());
            return;
        }
        const uint32_t restore_at = *plan.restore_tick;
        const demo::DemoKeyframe& key = demo_->keyframes.at(restore_at);
        const demo::DemoKeyframe* following = nullptr;
        if (const auto later = demo_->clock.reel().next_after(restore_at)) {
            const auto found = demo_->keyframes.find(*later);
            if (found != demo_->keyframes.end())
                following = &found->second;
        }
        // The picture is the viewer's. The recording's ticks may change the camera after this;
        // only the pause bit is put back when the jump lands, because each tick clears it.
        demo::ViewerPicture saved_viewer;
        demo::take_viewer_picture(playing->state(), &saved_viewer);
        std::string restore_error;
        if (!demo::demo_keyframe_restore(demo_.get(), key, following, &restore_error)) {
            stop_playback(restore_error.c_str());
            return;
        }
        playing = demo_->match;
        if (playing == nullptr) {
            stop_playback("the playback is not bound to a match");
            return;
        }
        demo::give_viewer_picture(&playing->state(), saved_viewer);
        if (host != nullptr && host->set_clock_tick != nullptr)
            host->set_clock_tick(host->context, playing->state().game.tick);
    } else if (plan.kind == demo::SeekPlan::Kind::stay && current != jump.to) {
        std::fprintf(
            stderr,
            "open-annihilation: replay seek failed: no keyframe at or before tick %u\n",
            jump.to
        );
        // A refusal already said why. A jump back that finds nothing keeps that
        // sentence, and says the device is short of memory when none was set.
        if (!demo_->keyframe_refused)
            demo_->clock.set_unavailable(replay_player_line(kJumpUnavailable, 0));
        else if (demo_->clock.state(current).unavailable.empty())
            demo_->clock.set_unavailable(replay_player_line(kJumpNeedsMemory, 0));
        return;
    }
    if (!demo_->keyframe_refused)
        demo_->clock.set_unavailable({});
    ReplaySeekRun run;
    run.from = jump.from;
    run.to = jump.to;
    run.origin = playing->state().game.tick;
    run.past_furthest = plan.past_furthest;
    run.started = started;
    run.viewer_paused = pause_bit_set(playing->state().game);
    demo_->seek = run;
    demo_->clock.set_skipping(true);
    if (run.past_furthest)
        demo_->clock.set_catch_up(demo::catch_up_percent(run.origin, run.origin, run.to));
}

void NetworkPlay::continue_seek(
    bool windowed,
    bool* finished,
    clock::time_point deadline,
    bool bound,
    const ReplayFrameHost* host
) {
    auto* playing = demo_ != nullptr ? demo_->match : nullptr;
    if (!demo_ || !demo_->seek || playing == nullptr)
        return;
    const uint32_t target = demo_->seek->to;
    bool stepped = false;
    for (;;) {
        playing = demo_->match;
        if (!demo_ || playing == nullptr || playing->state().game.tick >= target)
            break;
        if (finished != nullptr && *finished)
            break;
        if (bound && stepped && clock::now() >= deadline)
            break;
        const uint32_t before = playing->state().game.tick;
        const bool landing = before + 1 >= target;
        if (windowed) {
            if (host == nullptr || !run_replay_tick(*host, landing))
                break;
        } else {
            step_demo_frame();
        }
        playing = demo_ != nullptr ? demo_->match : nullptr;
        if (!demo_ || playing == nullptr)
            break;
        const uint32_t after = playing->state().game.tick;
        if (after == before)
            break;
        stepped = true;
        follow_play_head(after);
        if (demo_ && demo_->seek && demo_->seek->past_furthest)
            demo_->clock.set_catch_up(demo::catch_up_percent(demo_->seek->origin, after, target));
        if (demo_ && finished != nullptr)
            *finished = demo::demo_session_finished(*demo_);
    }
    replay_hide_tick_ = false;
    playing = demo_ != nullptr ? demo_->match : nullptr;
    if (!demo_ || !demo_->seek || playing == nullptr)
        return;
    const bool arrived = playing->state().game.tick >= demo_->seek->to;
    const bool ended = finished != nullptr && *finished;
    if (!arrived && !ended) {
        if (!bound) {
            std::fprintf(
                stderr,
                "open-annihilation: replay seek stopped at tick %u\n",
                playing->state().game.tick
            );
            demo_->clock.set_skipping(false);
            demo_->clock.set_catch_up(std::nullopt);
            demo_->seek.reset();
        }
        return;
    }
    const ReplaySeekRun run = *demo_->seek;
    const uint64_t digest = demo::demo_world_digest(&playing->state());
    const auto taken =
        std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - run.started);
    std::printf(
        "demo seek %u -> %u: digest %016llx in %lld ms\n",
        run.from,
        run.to,
        static_cast<unsigned long long>(digest),
        static_cast<long long>(taken.count())
    );
    std::fflush(stdout);
    write_pause_bit(playing->state().game, run.viewer_paused);
    if (run.viewer_paused)
        demo_->clock.pause();
    else
        demo_->clock.play();
    demo_->clock.set_skipping(false);
    demo_->clock.set_catch_up(std::nullopt);
    demo_->seek.reset();
}

void NetworkPlay::run_replay_frame(uint32_t now_ms, const ReplayFrameHost& host) {
    auto* playing = demo_ != nullptr ? demo_->match : nullptr;
    if (!demo_ || playing == nullptr || host.match == nullptr ||
        host.match(host.context) != playing)
        return;
    runtime_.ensure_replay_viewer();
    // A seek's ticks clear the world's pause bit. A Pause pressed during the jump sets that
    // bit before this frame's ticks. Fold it into the pause the landing puts back, then clear
    // it so the skip does not see it. The next press sets the bit again and takes the pause off.
    // With no jump running, the clock follows the bit.
    if (demo_->seek) {
        bool clear_bit = false;
        const bool paused = demo::fold_jump_pause(
            demo_->seek->viewer_paused, pause_bit_set(playing->state().game), &clear_bit
        );
        demo_->seek->viewer_paused = paused;
        if (clear_bit)
            write_pause_bit(playing->state().game, false);
        if (paused)
            demo_->clock.pause();
        else
            demo_->clock.play();
    } else if (pause_bit_set(playing->state().game)) {
        demo_->clock.pause();
    } else {
        demo_->clock.play();
    }
    note_recorded_speed();
    apply_replay_speech(host);
    const uint32_t elapsed = replay_elapsed(now_ms);
    const uint32_t tick_before = host.clock_tick != nullptr ? host.clock_tick(host.context) : 0;
    const uint32_t capped = std::min(elapsed, demo::playback_stall_cap_ms);
    const uint32_t budget_ms = std::max<uint32_t>(1, capped);
    const auto frame_started = clock::now();
    const auto deadline = frame_started + std::chrono::milliseconds(budget_ms);
    bool finished = demo::demo_session_finished(*demo_);
    replay_hide_tick_ = false;
    try {
        const bool sought = perform_due_seeks(true, &finished, deadline, true, &host);
        if (demo_ && !sought && !finished && !demo_->seek) {
            const bool hold = pause_bit_set(playing->state().game);
            const demo::FramePlan plan = demo_->clock.begin_frame(elapsed, hold);
            uint32_t ran = 0;
            bool noted = false;
            while (demo_ && ran < plan.ticks && !finished && !demo_->seek) {
                if (ran > 0 && budget_spent(frame_started, budget_ms))
                    break;
                const bool last = ran + 1 == plan.ticks;
                const bool draw = last || (ran > 0 && budget_half_spent(frame_started, budget_ms));
                if (!run_replay_tick(host, draw))
                    break;
                ++ran;
                playing = demo_->match;
                if (!demo_ || playing == nullptr)
                    break;
                follow_play_head(playing->state().game.tick);
                finished = demo::demo_session_finished(*demo_);
                if (demo_ && perform_due_seeks(true, &finished, deadline, true, &host)) {
                    (void)demo_->clock.note_frame(ran);
                    noted = true;
                    break;
                }
                if (draw && !last)
                    break;
            }
            if (demo_ && !noted && !demo_->seek)
                (void)demo_->clock.note_frame(ran);
        }
    } catch (const std::exception& error) {
        replay_hide_tick_ = false;
        if (host.note_error != nullptr)
            host.note_error(host.context, error.what());
    }
    replay_hide_tick_ = false;
    if (!demo_ || demo_->match == nullptr)
        return;
    if (host.present != nullptr)
        host.present(host.context, tick_before);
    apply_replay_speech(host);
}

void NetworkPlay::replay_hold(uint32_t now_ms) {
    if (!demo_)
        return;
    (void)replay_elapsed(now_ms);
}

void NetworkPlay::replay_play() {
    if (!demo_ || demo_->match == nullptr)
        return;
    write_pause_bit(demo_->match->state().game, false);
    if (demo_->seek)
        demo_->seek->viewer_paused = false;
    demo_->clock.play();
}

void NetworkPlay::replay_pause() {
    if (!demo_ || demo_->match == nullptr)
        return;
    write_pause_bit(demo_->match->state().game, true);
    if (demo_->seek)
        demo_->seek->viewer_paused = true;
    demo_->clock.pause();
}

void NetworkPlay::replay_set_speed(demo::PlaybackSpeed speed) {
    if (demo_)
        demo_->clock.set_speed(speed);
}

void NetworkPlay::replay_set_speed_milli(int32_t milli) {
    demo::PlaybackSpeed speed = demo::PlaybackSpeed::normal;
    switch (milli) {
    case 500:
        speed = demo::PlaybackSpeed::half;
        break;
    case 1000:
        speed = demo::PlaybackSpeed::normal;
        break;
    case 2000:
        speed = demo::PlaybackSpeed::twice;
        break;
    case 4000:
        speed = demo::PlaybackSpeed::quadruple;
        break;
    case 8000:
        speed = demo::PlaybackSpeed::octuple;
        break;
    case 16000:
        speed = demo::PlaybackSpeed::sixteen;
        break;
    default:
        return;
    }
    replay_set_speed(speed);
}

void NetworkPlay::replay_step_speed(int delta) {
    if (demo_)
        demo_->clock.step_speed(delta);
}

void NetworkPlay::replay_seek_to(uint32_t play_tick) {
    if (!demo_ || demo_->match == nullptr)
        return;
    demo_->seeks.clear();
    if (demo_->seek) {
        const bool paused = demo_->seek->viewer_paused;
        demo_->seek.reset();
        demo_->clock.set_skipping(false);
        demo_->clock.set_catch_up(std::nullopt);
        write_pause_bit(demo_->match->state().game, paused);
        if (paused)
            demo_->clock.pause();
        else
            demo_->clock.play();
    }
    demo::ReplayJump jump;
    jump.from = demo_->match->state().game.tick;
    jump.to = play_tick;
    jump.immediate = true;
    demo_->seeks.push_back(jump);
}

demo::PlaybackState NetworkPlay::replay_playback_state() const {
    if (!demo_ || demo_->match == nullptr)
        return {};
    return demo_->clock.state(demo_->match->state().game.tick);
}

bool NetworkPlay::replay_fill_state(ReplayPlaybackView* out) {
    if (out == nullptr || !demo_ || demo_->match == nullptr)
        return false;
    const demo::PlaybackState clock_state = demo_->clock.state(demo_->match->state().game.tick);
    demo_->state_ready.clear();
    demo_->state_ready.reserve(clock_state.ready.size());
    for (const demo::ReadySpan& span : clock_state.ready)
        demo_->state_ready.push_back(ReplayReadySpan{span.begin, span.end});
    demo_->state_unavailable = clock_state.unavailable;
    out->tick = clock_state.tick;
    out->length_known = clock_state.length_known;
    out->furthest = clock_state.furthest;
    out->asked_milli = clock_state.asked_milli;
    out->reached_milli = clock_state.reached_milli;
    out->reached_lower = clock_state.reached_lower;
    out->paused = clock_state.paused;
    out->skipping = clock_state.skipping;
    out->catch_up_percent =
        clock_state.catch_up_percent ? static_cast<int32_t>(*clock_state.catch_up_percent) : -1;
    out->unavailable =
        demo_->state_unavailable.empty() ? nullptr : demo_->state_unavailable.c_str();
    out->ready = demo_->state_ready.empty() ? nullptr : demo_->state_ready.data();
    out->ready_count = demo_->state_ready.size();
    return true;
}

std::string NetworkPlay::take_replay_stop_notice() {
    std::string line = std::move(replay_stop_sentence_);
    replay_stop_sentence_.clear();
    return line;
}

uint32_t NetworkPlay::replay_elapsed(uint32_t now_ms) {
    if (!replay_have_ms_) {
        replay_have_ms_ = true;
        replay_last_ms_ = now_ms;
        return 0;
    }
    const uint32_t elapsed = now_ms - replay_last_ms_;
    replay_last_ms_ = now_ms;
    return elapsed;
}

bool NetworkPlay::run_replay_tick(const ReplayFrameHost& host, bool draw) {
    replay_hide_tick_ = !draw;
    replay_sound_ms_ = host.sound_millis != nullptr ? host.sound_millis(host.context) : 0;
    const bool ran = host.run_tick != nullptr && host.run_tick(host.context, draw);
    replay_hide_tick_ = false;
    return ran;
}

std::string NetworkPlay::replay_player_line(const char* english, uint32_t tick) const {
    if (english == nullptr)
        return {};
    std::string line(oa::data::languages::interface_text(english));
    const auto mark = line.find("{time}");
    if (mark == std::string::npos)
        return line;
    constexpr uint32_t kTicksPerSecond = 30;
    const uint32_t seconds_total = tick / kTicksPerSecond;
    const uint32_t minutes = seconds_total / 60;
    const uint32_t seconds = seconds_total % 60;
    std::string shown = std::to_string(minutes);
    shown.push_back(':');
    if (seconds < 10)
        shown.push_back('0');
    shown += std::to_string(seconds);
    line.replace(mark, 6, shown);
    return line;
}

} // namespace oa::app

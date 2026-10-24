// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The scripted showcases --showcase plays: pointer and key events sent
// through dispatch_event() as SDL delivers the player's, on the application
// loop's frames and clock, so that a video capture shows them as a player
// sees them (docs/capture.md).
#include "oa/app/runtime.hpp"
#include "renderer_effect_sources.hpp"
#include "oa/present/world_renderer/world_camera.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

namespace {

// Seconds the showcase gives each step, so that a viewer can take it in.
constexpr double kMenuSeconds = 3.0;         // the main menu before SINGLE
constexpr double kScreenSeconds = 1.5;       // a menu screen before its next click
constexpr double kBriefingSeconds = 9.0;     // the briefing and its narration
constexpr double kMissionStartSeconds = 3.0; // the mission's opening view
constexpr double kOrderSeconds = 1.0;        // between the orders
constexpr double kGlamourSeconds = 5.0;      // the victory picture before a click
constexpr double kScoreSeconds = 6.0;        // the score screen at the end
constexpr double kGlideSeconds = 0.6;        // the pointer's move to what it clicks
constexpr double kPressSeconds = 0.15;       // a button held down, drawn pressed
// Longest waits: for a screen to open, and for the march to the gate.
constexpr double kScreenTimeoutSeconds = 60.0;
constexpr double kMarchTimeoutSeconds = 900.0;
constexpr double kNanosecondsPerSecond = 1e9;
// The camera changes to a unit nearer the goal only when it is this many map
// pixels nearer, so that it does not jump between neighbours, and looks for
// the nearest this often, in seconds.
constexpr double kFollowMarginPixels = 160.0;
constexpr double kFollowSeconds = 1.0;
// The standing move order Hold Position, and how many settings MOVE ORDERS
// cycles through.
constexpr uint32_t kHoldPosition = 0;
constexpr int kMoveOrderSettings = 3;
// 16.16 world units to whole map pixels.
constexpr int32_t kFixedShift = 16;
// The campaign the first Arm mission belongs to.
constexpr std::string_view kArmCampaign = "Arm Campaign";
// --showcase skirmish-battle: the seconds the battle plays once its armies
// stand, and the units each side brings without --combat.
constexpr double kBattleSeconds = 60.0;
constexpr std::size_t kBattleUnitsPerSide = 50;
// A battle frame longer than five ticks at normal speed, the most a frame
// catches up, costs the match ticks; the showcase lists the first few.
constexpr double kSlowFrameSeconds = 5.0 / 30.0;
constexpr std::size_t kSlowFramesShown = 12;
// The skirmish's commander death rule in the game's preferences, and its
// value for a game that continues after a commander's death.
constexpr std::string_view kCommanderDeathKey = "SkirmishCommanderDeath";
constexpr const char* kGameContinues = "0";

// Throws the showcase's error.
[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("showcase: " + what);
}

// Prints one "showcase:" line; the terminal follows the run.
void report(const std::string& what) {
    std::printf("showcase: %s\n", what.c_str());
    std::fflush(stdout);
}

// The unit's standing move order.
uint32_t move_order(const oa::Unit& unit) {
    return (unit.flags & OA_UNIT_FLAG_MOVE_ORDER_MASK) >> OA_UNIT_FLAG_MOVE_ORDER_SHIFT;
}

} // namespace

void Runtime::run_battle_showcase() {
    bool running = true;
    const auto now = [] { return static_cast<double>(SDL_GetTicksNS()) / kNanosecondsPerSecond; };
    // One pass of the application loop, paced as run() paces it; the time it
    // worked, without the wait, is added to `work`.
    double work = 0.0;
    const auto frame = [&] {
        const double start = now();
        run_frame(running);
        work += now() - start;
        if (!running || exit_requested_)
            fail("the game was closed");
        pace_next_frame(running);
    };
    const double menu_end = now() + kMenuSeconds;
    while (now() < menu_end)
        frame();
    // The battle plays on when a commander falls: the skirmish screen reads
    // the game continuing after a commander's death, and the player's own
    // choice is put back once the skirmish has started.
    const auto commander_key =
        preference_key(oa::ui::frontend_state::initialization::general_section, kCommanderDeathKey);
    const auto kept_value = preference_values_.find(commander_key);
    const std::optional<std::string> kept =
        kept_value != preference_values_.end() ? std::optional{kept_value->second} : std::nullopt;
    const auto kept_rule = preferences_.skirmish.commander_death;
    preference_values_[commander_key] = kGameContinues;
    start_benchmark_skirmish();
    if (kept)
        preference_values_[commander_key] = *kept;
    else
        preference_values_.erase(commander_key);
    preferences_.skirmish.commander_death = kept_rule;
    preferences_dirty_ = true;
    flush_preferences();
    const auto units = options_.combat_units != 0 ? options_.combat_units : kBattleUnitsPerSide;
    // The busy combat's units, away from the armies, keep the player in the
    // game when the army beside the commander falls.
    options_.busy_combat = true;
    spawn_combat_armies(units);
    const uint32_t drawing_threads = draw_pool_ ? draw_pool_->threads() : 1U;
    report(
        "the battle started at " + std::to_string(match_layout_.width) + "x" +
        std::to_string(match_layout_.height) + " with " + std::to_string(units) +
        " units a side and the busy combat's, at most " +
        std::to_string(options_.max_frames_per_second) + " frames a second, on " +
        std::to_string(drawing_threads) + " drawing thread" + (drawing_threads > 1 ? "s" : "")
    );
    phase_times_ = {};
    work = 0.0;
    const uint32_t first_tick = match_timing_.tick;
    const double start = now();
    double previous = start;
    double longest = 0.0;
    // The longest frame's ticks and the time they, its drawing and its
    // presenting took.
    uint32_t longest_ticks = 0;
    PhaseTimes longest_phases{};
    std::size_t frames = 0;
    // Frames too long for the clock to catch up within the most ticks a
    // frame runs, which the match loses.
    std::size_t slow_frames = 0;
    while (previous - start < kBattleSeconds) {
        const auto phases = phase_times_;
        const uint32_t tick = match_timing_.tick;
        frame();
        const double finished = now();
        const PhaseTimes spent{
            phase_times_.simulation - phases.simulation,
            phase_times_.compose - phases.compose,
            0,
            0,
            phase_times_.upload - phases.upload,
            phase_times_.present - phases.present,
        };
        if (finished - previous > kSlowFrameSeconds) {
            ++slow_frames;
            if (slow_frames <= kSlowFramesShown) {
                char slow[160];
                std::snprintf(
                    slow,
                    sizeof slow,
                    "slow frame at tick %u: %.1f ms (%u ticks in %.1f ms, draw %.1f ms, "
                    "present %.1f ms)",
                    tick,
                    (finished - previous) * 1000.0,
                    match_timing_.tick - tick,
                    static_cast<double>(spent.simulation) / 1.0e6,
                    static_cast<double>(spent.compose) / 1.0e6,
                    static_cast<double>(spent.upload + spent.present) / 1.0e6
                );
                report(slow);
            }
        }
        if (finished - previous > longest) {
            longest = finished - previous;
            longest_ticks = match_timing_.tick - tick;
            longest_phases = spent;
        }
        previous = finished;
        ++frames;
        if (screen_ != Screen::match) {
            char ended[64];
            std::snprintf(
                ended, sizeof ended, "the match ended %.1f s into the battle", previous - start
            );
            report(ended);
            break;
        }
    }
    const double seconds = previous - start;
    const uint32_t ticks = match_timing_.tick - first_tick;
    const auto per_frame_ms = [&](double total_seconds) {
        return frames == 0 ? 0.0 : total_seconds * 1000.0 / static_cast<double>(frames);
    };
    const auto ns_to_seconds = [](int64_t ns) {
        return static_cast<double>(ns) / kNanosecondsPerSecond;
    };
    char line[640];
    std::snprintf(
        line,
        sizeof line,
        "battle %.1f s: %u ticks (%.1f a second), %zu frames (%.1f a second), game speed %d "
        "of %d; frame %.1f ms mean, %.1f longest (%u ticks in %.1f ms, draw %.1f ms, present "
        "%.1f ms); %zu frames slower than %.0f ms; work %.1f ms a frame; tick %.2f ms mean; "
        "draw %.1f ms and present %.1f ms a frame",
        seconds,
        ticks,
        seconds > 0.0 ? ticks / seconds : 0.0,
        frames,
        seconds > 0.0 ? static_cast<double>(frames) / seconds : 0.0,
        static_cast<int>(match_timing_.actual_rate),
        static_cast<int>(match_timing_.requested_rate),
        per_frame_ms(seconds),
        longest * 1000.0,
        longest_ticks,
        ns_to_seconds(longest_phases.simulation) * 1000.0,
        ns_to_seconds(longest_phases.compose) * 1000.0,
        ns_to_seconds(longest_phases.upload + longest_phases.present) * 1000.0,
        slow_frames,
        kSlowFrameSeconds * 1000.0,
        per_frame_ms(work),
        ticks == 0 ? 0.0 : ns_to_seconds(phase_times_.simulation) * 1000.0 / ticks,
        per_frame_ms(ns_to_seconds(phase_times_.compose)),
        per_frame_ms(ns_to_seconds(phase_times_.upload + phase_times_.present))
    );
    report(line);
    print_memory_status();
}

void Runtime::run_showcase() {
    if (sdl_.window == nullptr || sdl_.renderer == nullptr)
        fail("needs the game's window");
    if (options_.showcase == Showcase::skirmish_battle) {
        run_battle_showcase();
        return;
    }
    bool running = true;
    const auto now = [] { return static_cast<double>(SDL_GetTicksNS()) / kNanosecondsPerSecond; };
    // One pass of the application loop, paced as run() paces it.
    const auto frame = [&] {
        run_frame(running);
        if (!running || exit_requested_)
            fail("the game was closed");
        pace_next_frame(running);
    };
    const auto hold = [&](double seconds) {
        const auto end = now() + seconds;
        while (now() < end)
            frame();
    };
    if (options_.showcase == Showcase::base_construction) {
        constexpr std::string_view map_name = "Town & Country";
        constexpr double stage_timeout = 300.0;
        constexpr float orbit_pixel_scale = 4.0F;
        hold(kMenuSeconds);
        exercise_click(menu::resource_name(menu::Button::single_player));
        exercise_click(entry::resource_name(entry::Button::skirmish));
        skirmish_settings_.map_name = std::string(map_name);
        if (select_map(map_name) == 0)
            fail("cannot select Town & Country; enable Core Contingency content");
        state_.player_count = 2;
        skirmish_settings_.slots[0].side = 0;
        skirmish_settings_.slots[1].side = 1;
        exercise_click(skirmish::resource_name(skirmish::Button::start));
        if (screen_ != Screen::match || !match_)
            fail("Town & Country did not start");
        // This is a construction demonstration: leave the opponent idle and
        // replenish stores, but retain ordinary build work and unit scripts.
        for (auto& player : match_->state().game.players)
            if (player.status == OA_PLAYER_STATUS_COMPUTER)
                player.status = OA_PLAYER_STATUS_LOCAL;
        select_local_commander();
        const uint16_t commander = selected_match_unit_;
        if (!commander)
            fail("no local commander");
        const auto original = match_->world().slots[commander].record.position;
        const int base_x = (original.x >> kFixedShift) + 160;
        const int base_z = original.z >> kFixedShift;
        center_camera_on_unit(commander);
        match_zoom_ = match_zoom_target_ = 0.8F;
        report("Town & Country: staged construction, replenished resources, idle opponent");
        hold(4.0);
        for (int faster = 0; faster < 10; ++faster)
            adjust_game_speed(1);
        options_.experimental_3d = true;
        const double orbit_start = now();
        float previous_yaw = 0.0F;
        float previous_pitch = 0.0F;
        bool orbit_enabled = true;
        double focus_x = base_x;
        double focus_z = base_z;
        double camera_time = now();
        const auto select = [&](uint16_t id) {
            clear_local_selection();
            adopt_selection(id);
            apply_match_hud_for_selection();
        };
        const auto gesture = [&](float dx, float dy, bool orbit) {
            float x{}, y{};
            if (!frame_to_window(
                    sdl_.renderer,
                    static_cast<float>(match_layout_.left + match_layout_.battlefield_width() - 96),
                    static_cast<float>(match_layout_.top + 96),
                    &x,
                    &y
                ))
                fail("cannot map the camera gesture to the window");
            SDL_SetModState(orbit ? SDL_KMOD_ALT : SDL_KMOD_NONE);
            SDL_Event event{};
            event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.button = SDL_BUTTON_MIDDLE;
            event.button.x = x;
            event.button.y = y;
            live_renderer_take_event(event);
            event = {};
            event.type = SDL_EVENT_MOUSE_MOTION;
            event.motion.x = x;
            event.motion.y = y;
            event.motion.xrel = dx;
            event.motion.yrel = dy;
            live_renderer_take_event(event);
            event = {};
            event.type = SDL_EVENT_MOUSE_BUTTON_UP;
            event.button.button = SDL_BUTTON_MIDDLE;
            event.button.x = x;
            event.button.y = y;
            live_renderer_take_event(event);
            SDL_SetModState(SDL_KMOD_NONE);
        };
        const auto construction_frame = [&] {
            auto& stores = match_->world().players[match_local_player_];
            stores.metal = stores.metal_cap;
            stores.energy = stores.energy_cap;
            const double current_time = now();
            const double camera_step = 1.0 - std::exp(-2.0 * (current_time - camera_time));
            camera_time = current_time;
            const auto place = match_view_place();
            const double target_x = focus_x - visible_map_width() / 2.0;
            const double target_z = focus_z - visible_map_height() / 2.0;
            place_match_view(
                place[0] + (target_x - place[0]) * camera_step,
                place[1] + (target_z - place[1]) * camera_step
            );
            const double elapsed = current_time - orbit_start;
            const auto yaw = static_cast<float>(32.0 * std::sin(elapsed / 18.0));
            const auto pitch = static_cast<float>(12.0 * std::sin(elapsed / 27.0));
            if (orbit_enabled)
                gesture(
                    (yaw - previous_yaw) * orbit_pixel_scale,
                    (pitch - previous_pitch) * orbit_pixel_scale,
                    true
                );
            previous_yaw = yaw;
            previous_pitch = pitch;
            frame();
            if (!match_ || screen_ != Screen::match || match_->fault())
                fail("construction match stopped or raised a simulation fault");
            if (live_renderer_failed())
                fail("the 3D renderer failed");
        };
        const auto type_of = [&](std::string_view name) {
            const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
            if (type == 0 || type >= spawn_types_.size())
                fail("missing build type " + std::string(name));
            return type;
        };
        const auto snapshot = [&](const std::string& name) {
            if (options_.snapshot.empty())
                return;
            fs::create_directories(options_.snapshot);
            render_match_surface();
            renderer::Surface picture;
            compose_match_frame(picture);
            write_ppm(options_.snapshot / (name + ".ppm"), picture);
        };
        const auto wait_complete = [&](uint16_t type,
                                       const std::optional<PendingBuildSite>& site,
                                       uint16_t builder = 0) {
            const auto deadline = now() + stage_timeout;
            auto next_report = now() + 30.0;
            bool saved_nanoframe = false;
            uint16_t construction = 0;
            uint32_t last_motion = match_timing_.tick;
            uint32_t last_progress = match_timing_.tick;
            const uint32_t started = match_timing_.tick;
            float remaining = 1.0F;
            int builder_x = builder ? match_->world().slots[builder].record.position.x >> 16 : 0;
            int builder_z = builder ? match_->world().slots[builder].record.position.z >> 16 : 0;
            while (now() < deadline) {
                if (now() >= next_report) {
                    report("construction advancing at tick " + std::to_string(match_timing_.tick));
                    next_report = now() + 30.0;
                }
                if (builder) {
                    const auto position = match_->world().slots[builder].record.position;
                    const int x = position.x >> 16;
                    const int z = position.z >> 16;
                    if (std::abs(x - builder_x) + std::abs(z - builder_z) >= 4) {
                        builder_x = x;
                        builder_z = z;
                        last_motion = match_timing_.tick;
                    }
                    constexpr uint32_t approach_grace_ticks = 300;
                    constexpr uint32_t stalled_motion_ticks = 180;
                    if (!construction && match_timing_.tick - started > approach_grace_ticks &&
                        match_timing_.tick - last_motion > stalled_motion_ticks) {
                        report(
                            "builder stalled before placing a nanoframe at " + std::to_string(x) +
                            "," + std::to_string(z) + "; trying another site"
                        );
                        snapshot(
                            "stalled-builder-" + std::to_string(builder) + "-" +
                            std::to_string(match_timing_.tick)
                        );
                        return uint16_t{0};
                    }
                }
                for (const auto& slot : match_->world().slots) {
                    if (!slot.unit || slot.record.type_index != type ||
                        slot.record.owner_index != match_local_player_)
                        continue;
                    if (site &&
                        (std::abs((slot.record.position.x - site->world[0]) >> kFixedShift) > 24 ||
                         std::abs((slot.record.position.z - site->world[2]) >> kFixedShift) > 24))
                        continue;
                    if (!construction) {
                        construction = slot.unit_index;
                        last_progress = match_timing_.tick;
                        if (builder && builder != commander)
                            match_->issue_help_build(commander, construction, false);
                    }
                    if (slot.record.build_remaining < remaining) {
                        remaining = slot.record.build_remaining;
                        last_progress = match_timing_.tick;
                    }
                    constexpr uint32_t stalled_build_ticks = 300;
                    if (builder && match_timing_.tick - last_progress > stalled_build_ticks) {
                        match_->issue_help_build(builder, construction, false);
                        if (builder != commander)
                            match_->issue_help_build(commander, construction, false);
                        report(
                            "renewed construction assistance on unfinished unit " +
                            std::to_string(construction)
                        );
                        last_progress = match_timing_.tick;
                    }
                    if (!saved_nanoframe && slot.record.build_remaining > 0.0F &&
                        slot.record.build_remaining < 0.95F) {
                        snapshot("nanoframe-" + std::to_string(slot.unit_index));
                        saved_nanoframe = true;
                    }
                    if (slot.record.build_remaining <= 0.0F)
                        return slot.unit_index;
                }
                construction_frame();
            }
            if (builder && !construction)
                return uint16_t{0};
            fail("construction did not finish within five minutes");
        };
        const auto build = [&](uint16_t builder,
                               std::string_view name,
                               int desired_x,
                               int desired_z,
                               bool extractor) {
            const uint16_t type = type_of(name);
            std::vector<PendingBuildSite> refused_sites;
            constexpr int maximum_site_attempts = 8;
            for (int attempt = 0; attempt < maximum_site_attempts; ++attempt) {
                pending_build_type_ = type;
                std::optional<PendingBuildSite> chosen;
                double best = -std::numeric_limits<double>::infinity();
                const int radius = extractor ? 32 : 12;
                const auto& world = match_->state();
                for (int dz = -radius; dz <= radius; ++dz)
                    for (int dx = -radius; dx <= radius; ++dx) {
                        const int cell_x = desired_x / 16 + dx;
                        const int cell_z = desired_z / 16 + dz;
                        const auto& definition = spawn_types_[type];
                        const oa::sim::ground_orders::Point point{
                            (cell_x * 16 + definition.footprint_x * 8) * 65536,
                            0,
                            (cell_z * 16 + definition.footprint_z * 8) * 65536
                        };
                        const auto site = pending_build_site(point);
                        if (!site || !site->legal || site->over_own_units)
                            continue;
                        const bool refused_nearby = std::any_of(
                            refused_sites.begin(),
                            refused_sites.end(),
                            [&](const PendingBuildSite& refused) {
                                return std::abs(refused.cell_x - site->cell_x) < 4 &&
                                       std::abs(refused.cell_z - site->cell_z) < 4;
                            }
                        );
                        if (refused_nearby)
                            continue;
                        int metal = 0;
                        for (int z = 0; z < site->footprint_z; ++z)
                            for (int x = 0; x < site->footprint_x; ++x)
                                if (const auto* plot =
                                        oa::world_plot(&world, site->cell_x + x, site->cell_z + z))
                                    metal += plot->metal;
                        const double distance = static_cast<double>(dx * dx + dz * dz);
                        const double score = (extractor ? metal * 1000.0 : 0.0) - distance;
                        if (score > best) {
                            chosen = site;
                            best = score;
                        }
                    }
                pending_build_type_ = 0;
                if (!chosen)
                    fail("no legal site for " + std::string(name));
                select(builder);
                const auto& builder_position = match_->world().slots[builder].record.position;
                focus_x =
                    ((chosen->world[0] >> kFixedShift) + (builder_position.x >> kFixedShift)) / 2.0;
                focus_z =
                    ((chosen->world[2] >> kFixedShift) + (builder_position.z >> kFixedShift)) / 2.0;
                match_zoom_target_ = 1.0F;
                if (builder != commander) {
                    // Do not let a guarding commander occupy the Kbot's approach.
                    // He resumes assistance only after the nanoframe is placed.
                    const int dx = (chosen->world[0] >> 16) - (builder_position.x >> 16);
                    const int dz = (chosen->world[2] >> 16) - (builder_position.z >> 16);
                    const oa::sim::ground_orders::Point aside{
                        builder_position.x + (dz >= 0 ? -96 : 96) * 65536,
                        builder_position.y,
                        builder_position.z + (dx >= 0 ? 96 : -96) * 65536
                    };
                    match_->issue_ground_move(commander, aside, false);
                }
                match_->issue_mobile_build(builder, type, chosen->world, false);
                report(
                    "building " + std::string(name) + " at " + std::to_string(chosen->cell_x * 16) +
                    "," + std::to_string(chosen->cell_z * 16)
                );
                const auto id = wait_complete(type, chosen, builder);
                if (!id) {
                    refused_sites.push_back(*chosen);
                    match_->stop_orders(builder);
                    continue;
                }
                snapshot(std::string(name) + "-" + std::to_string(id));
                report("completed " + std::string(name) + " unit " + std::to_string(id));
                return id;
            }
            fail("no reachable build site for " + std::string(name));
        };
        build(commander, "ARMSOLAR", base_x, base_z - 112, false);
        for (int extractor = 0; extractor < 3; ++extractor)
            build(commander, "ARMMEX", base_x, base_z, true);
        const auto factory = build(commander, "ARMLAB", base_x + 80, base_z + 96, false);
        select(factory);
        const auto& factory_position = match_->world().slots[factory].record.position;
        focus_x = factory_position.x >> kFixedShift;
        focus_z = factory_position.z >> kFixedShift;
        match_->queue_factory_build(factory, type_of("ARMCK"), 1);
        match_->issue_guard(commander, factory, false);
        report("Kbot plant producing construction Kbot; commander assisting");
        const auto builder = wait_complete(type_of("ARMCK"), std::nullopt);
        snapshot("ARMCK-" + std::to_string(builder));
        select(builder);
        match_->stop_orders(commander);
        report("construction Kbot building the perimeter; commander assists each nanoframe");
        const std::array<std::array<int, 2>, 5> perimeter{
            {{0, -288}, {304, -160}, {304, 160}, {112, -240}, {112, 256}}
        };
        std::array<uint16_t, 5> defenses{};
        for (std::size_t index = 0; index < perimeter.size(); ++index)
            defenses[index] = build(
                builder,
                index < 3 ? "ARMRL" : "ARMGUARD",
                base_x + perimeter[index][0],
                base_z + perimeter[index][1],
                false
            );
        report(
            "base complete: solar, three extractors, Kbot plant, construction Kbot, three "
            "Defenders and two Guardians"
        );
        focus_x = base_x + 96;
        focus_z = base_z;
        match_zoom_target_ = 0.8F;
        const auto finish = now() + 20.0;
        while (now() < finish) {
            focus_x = base_x + 96.0 + 64.0 * std::sin((finish - now()) / 6.0);
            construction_frame();
        }
        snapshot("base-complete");
        // Attackers are staged; weapon fire, damage, deaths and wrecks are
        // produced by the ordinary simulation against the completed base.
        while (match_timing_.requested_rate > 10)
            adjust_game_speed(-1);
        const uint8_t enemy = match_local_player_ == 0 ? 1 : 0;
        std::vector<uint16_t> attackers;
        std::vector<uint16_t> surviving_attackers;
        std::size_t destroyed = 0;
        uint32_t last_death_tick = match_timing_.tick;
        std::size_t current_weapon_lights = 0;
        std::size_t current_hot_wrecks = 0;
        std::size_t peak_projectiles = 0;
        std::size_t peak_weapon_lights = 0;
        std::size_t peak_hot_wrecks = 0;
        std::size_t peak_heat_sources = 0;
        const auto observe_combat = [&] {
            std::size_t projectiles = 0, lights = 0, wrecks = 0, heat = 0;
            for (const auto& source : match_effect_sources()) {
                using renderer_effect_sources::Kind;
                using renderer_effect_sources::Origin;
                projectiles += source.origin == Origin::projectile;
                lights += source.kind == Kind::weapon && source.intensity > 0.0F;
                wrecks += source.kind == Kind::wreck && source.glow > 0.0F;
                heat += source.heat > 0.0F;
            }
            current_weapon_lights = lights;
            current_hot_wrecks = wrecks;
            std::erase_if(surviving_attackers, [&](uint16_t id) {
                if (match_unit_present(id))
                    return false;
                ++destroyed;
                last_death_tick = match_timing_.tick;
                return true;
            });
            peak_projectiles = std::max(peak_projectiles, projectiles);
            peak_weapon_lights = std::max(peak_weapon_lights, lights);
            peak_hot_wrecks = std::max(peak_hot_wrecks, wrecks);
            peak_heat_sources = std::max(peak_heat_sources, heat);
        };
        const auto live_defense = [&](uint16_t id) {
            return match_unit_present(id) &&
                   match_->world().slots[id].record.owner_index == match_local_player_ &&
                   match_->world().slots[id].record.health > 0;
        };
        std::array<uint16_t, 5> defense_targets{};
        const auto order_defenses = [&] {
            for (std::size_t index = 0; index < defenses.size(); ++index) {
                const auto defender = defenses[index];
                if (!live_defense(defender))
                    continue;
                const auto previous = defense_targets[index];
                if (previous &&
                    std::find(surviving_attackers.begin(), surviving_attackers.end(), previous) !=
                        surviving_attackers.end())
                    continue;
                uint16_t nearest = 0;
                double best_distance = std::numeric_limits<double>::infinity();
                const auto position = match_->world().slots[defender].record.position;
                for (const auto attacker : surviving_attackers) {
                    if (!match_unit_present(attacker))
                        continue;
                    const auto enemy_position = match_->world().slots[attacker].record.position;
                    const double dx = (enemy_position.x >> 16) - (position.x >> 16);
                    const double dz = (enemy_position.z >> 16) - (position.z >> 16);
                    const double distance = dx * dx + dz * dz;
                    if (distance < best_distance) {
                        best_distance = distance;
                        nearest = attacker;
                    }
                }
                defense_targets[index] = 0;
                if (nearest && match_->issue_attack_command(defender, nearest, false, nullptr)) {
                    defense_targets[index] = nearest;
                    report(
                        "defense " + std::to_string(defender) + " engaging attacker " +
                        std::to_string(nearest)
                    );
                }
            }
        };
        const double combat_start = now();
        const auto combat_frame = [&] {
            const double elapsed = now() - combat_start;
            match_zoom_target_ = static_cast<float>(0.95 + 0.20 * std::sin(elapsed / 9.0));
            focus_x = base_x + 370.0 + 80.0 * std::sin(elapsed / 12.0);
            focus_z = base_z + 60.0 * std::sin(elapsed / 16.0);
            order_defenses();
            construction_frame();
            observe_combat();
        };
        select(defenses[3]);
        for (int wave = 0; wave < 2; ++wave) {
            defense_targets.fill(0);
            const auto first_attacker = attackers.size();
            auto target = defenses[wave == 0 ? 1 : 2];
            if (!live_defense(target)) {
                const auto surviving = std::find_if(defenses.begin(), defenses.end(), live_defense);
                if (surviving == defenses.end()) {
                    report("no surviving perimeter for another wave; ending staged attacks");
                    break;
                }
                target = *surviving;
            }
            report(
                "combat wave " + std::to_string(wave + 1) + " begins at tick " +
                std::to_string(match_timing_.tick)
            );
            // Raiders withstand more than a light Kbot and can leave wrecks
            // after a missile or artillery hit instead of being vaporised.
            for (int index = 0; index < 2; ++index) {
                const auto type = type_of("CORRAID");
                const auto& target_position = match_->world().slots[target].record.position;
                const int desired_x = (target_position.x >> 16) + 300 + index * 40;
                const int desired_z =
                    (target_position.z >> 16) + (wave == 0 ? -64 : 64) + index * 32;
                std::optional<PendingBuildSite> site;
                for (int ring = 0; ring < 10 && !site; ++ring)
                    for (int dz = -ring; dz <= ring && !site; ++dz)
                        for (int dx = -ring; dx <= ring && !site; ++dx) {
                            if (std::abs(dx) != ring && std::abs(dz) != ring)
                                continue;
                            const oa::sim::ground_orders::Point point{
                                (desired_x + dx * 16) * 65536, 0, (desired_z + dz * 16) * 65536
                            };
                            const auto candidate = build_site_for(type, 0, point);
                            if (candidate && candidate->legal && !candidate->over_own_units)
                                site = candidate;
                        }
                if (!site)
                    fail("no clear ground for the staged Raider wave");
                oa::sim::unit_spawn::Request request;
                request.player = enemy;
                request.type = type;
                request.finished = true;
                request.state = kGroundOccupancyState;
                request.position = {
                    static_cast<uint32_t>(site->world[0]),
                    static_cast<uint32_t>(site->world[1]),
                    static_cast<uint32_t>(site->world[2])
                };
                const auto* placed = match_->create(request);
                if (!placed || !placed->unit)
                    fail("could not stage the Raider attack");
                attackers.push_back(placed->unit_index);
                surviving_attackers.push_back(placed->unit_index);
                if (!match_->issue_attack_command(placed->unit_index, target, false, nullptr))
                    fail("the Raider refused its attack order");
            }
            order_defenses();
            const double wave_start = now();
            bool saved_fire = false, saved_wreck = false;
            while (now() - wave_start < 70.0) {
                combat_frame();
                if (!saved_fire && current_weapon_lights > 0) {
                    snapshot("combat-wave-" + std::to_string(wave + 1) + "-fire");
                    saved_fire = true;
                }
                if (!saved_wreck && current_hot_wrecks > 0) {
                    snapshot("combat-wave-" + std::to_string(wave + 1) + "-wrecks");
                    saved_wreck = true;
                }
                const bool all_destroyed = std::all_of(
                    attackers.begin() + static_cast<std::ptrdiff_t>(first_attacker),
                    attackers.end(),
                    [&](uint16_t id) { return !match_unit_present(id); }
                );
                if (all_destroyed && now() - wave_start > 12.0)
                    break;
            }
            report(
                "combat wave " + std::to_string(wave + 1) + " ends at tick " +
                std::to_string(match_timing_.tick)
            );
        }
        const auto aftermath_start = now();
        constexpr uint32_t cooling_ticks = renderer_effect_sources::wreck_cooling_ticks;
        while (now() - aftermath_start < 90.0 &&
               (now() - aftermath_start < 15.0 ||
                match_timing_.tick - last_death_tick < cooling_ticks))
            combat_frame();
        snapshot("combat-aftermath");
        report(
            "combat evidence: " + std::to_string(destroyed) + " enemy attackers destroyed; " +
            std::to_string(std::count_if(defenses.begin(), defenses.end(), live_defense)) +
            " surviving defenses; peaks " + std::to_string(peak_projectiles) +
            " visible projectiles, " + std::to_string(peak_weapon_lights) + " weapon lights, " +
            std::to_string(peak_hot_wrecks) + " hot wrecks, " + std::to_string(peak_heat_sources) +
            " heat sources"
        );
        if (!destroyed || !peak_projectiles || !peak_weapon_lights || !peak_hot_wrecks ||
            !peak_heat_sources)
            fail("combat did not demonstrate all requested battlefield effects");
        orbit_enabled = false;
        reset_live_renderer();
        report("default top-down view: the same base and construction effects");
        build(commander, "ARMSOLAR", base_x - 96, base_z - 160, false);
        focus_x = base_x + 96;
        focus_z = base_z;
        match_zoom_target_ = 0.8F;
        const auto classic_finish = now() + 10.0;
        while (now() < classic_finish)
            construction_frame();
        snapshot("base-complete-default-view");
        report("base-construction done; camera remained constrained throughout");
        return;
    }
    const auto wait_for = [&](const std::function<bool()>& done, const std::string& what) {
        const auto end = now() + kScreenTimeoutSeconds;
        while (!done()) {
            if (now() > end)
                fail(
                    "no " + what + " after " + std::to_string(std::lround(kScreenTimeoutSeconds)) +
                    " s"
                );
            frame();
        }
    };

    // The pointer, in window coordinates, and the events that move and press
    // it, as SDL delivers them.
    int window_width = 0;
    int window_height = 0;
    if (!SDL_GetWindowSize(sdl_.window, &window_width, &window_height))
        fail(std::string("cannot read the window's size: ") + SDL_GetError());
    float pointer_x = static_cast<float>(window_width) / 2.0F;
    float pointer_y = static_cast<float>(window_height) / 2.0F;
    const auto send = [&](SDL_EventType type, uint8_t button) {
        SDL_Event event{};
        event.type = type;
        if (type == SDL_EVENT_MOUSE_MOTION) {
            event.motion.windowID = SDL_GetWindowID(sdl_.window);
            event.motion.x = pointer_x;
            event.motion.y = pointer_y;
        } else {
            event.button.windowID = SDL_GetWindowID(sdl_.window);
            event.button.button = button;
            event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.clicks = 1;
            event.button.x = pointer_x;
            event.button.y = pointer_y;
        }
        dispatch_event(event, running);
    };
    // Glides the pointer to a point of the current screen's canvas, easing in
    // and out, one step a frame.
    const auto glide = [&](float canvas_x, float canvas_y) {
        float to_x = 0.0F;
        float to_y = 0.0F;
        if (!frame_to_window(sdl_.renderer, canvas_x, canvas_y, &to_x, &to_y))
            fail(std::string("SDL_RenderCoordinatesToWindow: ") + SDL_GetError());
        const float from_x = pointer_x;
        const float from_y = pointer_y;
        const auto start = now();
        for (;;) {
            const double t = std::min(1.0, (now() - start) / kGlideSeconds);
            const auto eased = static_cast<float>(t * t * (3.0 - 2.0 * t));
            pointer_x = from_x + (to_x - from_x) * eased;
            pointer_y = from_y + (to_y - from_y) * eased;
            send(SDL_EVENT_MOUSE_MOTION, 0);
            if (t >= 1.0)
                return;
            frame();
        }
    };
    // A left click where the pointer is, held long enough to show pressed.
    const auto press_here = [&] {
        send(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
        hold(kPressSeconds);
        send(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
    };
    const auto click = [&](float canvas_x, float canvas_y) {
        glide(canvas_x, canvas_y);
        press_here();
    };
    // A key pressed and released with the modifiers held.
    const auto press_key = [&](SDL_Keycode key, SDL_Scancode scancode, SDL_Keymod modifiers) {
        SDL_SetModState(modifiers);
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.windowID = SDL_GetWindowID(sdl_.window);
        event.key.key = key;
        event.key.scancode = scancode;
        event.key.mod = modifiers;
        event.key.down = true;
        dispatch_event(event, running);
        event.type = SDL_EVENT_KEY_UP;
        event.key.down = false;
        dispatch_event(event, running);
        SDL_SetModState(SDL_KMOD_NONE);
    };
    // A gadget of the frontend screen, clicked at its centre.
    const auto click_gadget = [&](std::string_view name) {
        const auto* gadget = widget(name);
        if (gadget == nullptr)
            fail("the screen has no " + std::string(name));
        const auto origin = panel_origin();
        click(
            static_cast<float>(origin.x + gadget->common.x) +
                static_cast<float>(gadget->common.width) / 2.0F,
            static_cast<float>(origin.y + gadget->common.y) +
                static_cast<float>(gadget->common.height) / 2.0F
        );
        report("clicked " + std::string(name));
    };
    const auto shows = [this](Screen screen) {
        return [this, screen] { return screen_ == screen; };
    };
    // A button of the match's side panel, by its action (ARMMOVEORD's is MOVEORD).
    const auto hud_button = [&](std::string_view action) -> const oa::ui::gui_layout::Gadget* {
        if (!match_hud_)
            return nullptr;
        for (const auto& gadget : match_hud_->layout.gadgets)
            if (match_hud_action(gadget.common.name) == action)
                return &gadget;
        return nullptr;
    };
    const auto click_hud = [&](std::string_view action) {
        const auto* gadget = hud_button(action);
        if (gadget == nullptr)
            fail("the side panel has no " + std::string(action));
        const auto rect = oa::ui::display_layout::source_rect_to_canvas(
            match_layout_,
            gadget->common.x,
            gadget->common.y,
            gadget->common.width,
            gadget->common.height
        );
        click(
            static_cast<float>(rect.x) + static_cast<float>(rect.width) / 2.0F,
            static_cast<float>(rect.y) + static_cast<float>(rect.height) / 2.0F
        );
        report("clicked " + std::string(action));
    };

    // The main menu, SINGLE, and New Campaign on the Arm side.
    send(SDL_EVENT_MOUSE_MOTION, 0);
    hold(kMenuSeconds);
    click_gadget(menu::resource_name(menu::Button::single_player));
    wait_for(shows(Screen::single_player), "SINGLE.GUI");
    hold(kScreenSeconds);
    click_gadget(entry::resource_name(entry::Button::new_campaign));
    wait_for(shows(Screen::new_campaign), "NEWGAME.GUI");
    hold(kScreenSeconds);
    click_gadget("Arm");
    if (preferences_.side != 0)
        fail("Arm did not choose the Arm side");
    if (selected_campaign_index_ >= campaign_labels_.size() ||
        campaign_labels_[selected_campaign_index_] != kArmCampaign)
        fail("NEWGAME.GUI does not offer the " + std::string(kArmCampaign) + " first");
    report("NEWGAME.GUI: " + std::string(kArmCampaign) + ", mission 1");
    hold(kScreenSeconds);

    // The first mission's briefing, and its Start.
    click_gadget("Start");
    wait_for(shows(Screen::briefing), "briefing");
    hold(kBriefingSeconds);
    click_gadget("Start");
    wait_for([this] { return screen_ == Screen::match && match_ != nullptr; }, "mission");
    report("the mission started");
    hold(kMissionStartSeconds);

    // Every unit selected, holding position, and sent to the goal on the radar.
    const auto goal = mission_move_goal();
    if (!goal)
        fail("the mission has no MoveUnitToRadius victory condition");
    // The goal's map pixel on the screen plane, where the radar shows it.
    const oa::FixedVec3 goal_position{goal->point[0], goal->point[1], goal->point[2]};
    const auto goal_x = oa::present::world_renderer::world_screen_x(goal_position);
    const auto goal_y = oa::present::world_renderer::world_screen_y(goal_position);
    report(
        "the goal is at map pixel (" + std::to_string(goal_x) + ", " + std::to_string(goal_y) + ")"
    );
    press_key(SDLK_A, SDL_SCANCODE_A, SDL_KMOD_LCTRL);
    std::vector<uint16_t> group;
    for (const auto id : selected_local_ids())
        if (match_->takes_move_order(id))
            group.push_back(id);
    if (group.empty())
        fail("Ctrl+A selected no unit that moves");
    report("Ctrl+A selected " + std::to_string(group.size()) + " units that move");
    hold(kOrderSeconds);
    const auto present = [this](uint16_t id) { return match_unit_present(id); };
    const auto unit_of = [this](uint16_t id) -> const oa::Unit& {
        return match_->world().slots[id].record;
    };
    const auto holding = [&] {
        return std::all_of(group.begin(), group.end(), [&](uint16_t id) {
            return !present(id) || move_order(unit_of(id)) == kHoldPosition;
        });
    };
    if (hud_button("MOVEORD") == nullptr) {
        click_hud("ORDERS");
        hold(kOrderSeconds);
    }
    for (int clicks = 0; !holding(); ++clicks) {
        if (clicks == kMoveOrderSettings)
            fail("MOVE ORDERS did not give every unit Hold Position");
        click_hud("MOVEORD");
        hold(kOrderSeconds);
    }
    report("MOVE ORDERS: Hold Position");
    if (hud_button("MOVE") != nullptr)
        click_hud("MOVE");
    // radar_world_point() takes radar pixel p to map pixel p * map / radar;
    // the pointer goes to the middle of the pixel nearest the map pixel.
    const auto radar_pixel = [](int32_t map, int32_t picture, int32_t map_size) {
        constexpr float kPixelMiddle = 0.5F;
        return static_cast<float>(
                   std::lround(static_cast<double>(map) * picture / static_cast<double>(map_size))
               ) +
               kPixelMiddle;
    };
    if (radar_map_w_ <= 0 || radar_map_h_ <= 0)
        fail("the radar shows no map");
    click(
        static_cast<float>(radar_picture_.x) +
            radar_pixel(goal_x, radar_picture_.width, radar_map_w_),
        static_cast<float>(radar_picture_.y) +
            radar_pixel(goal_y, radar_picture_.height, radar_map_h_)
    );
    report("clicked the goal on the radar: " + status_);
    // The pointer rests over the top bar, off the battlefield.
    glide(
        static_cast<float>(match_layout_.width) / 2.0F, static_cast<float>(match_layout_.top) / 2.0F
    );

    // The march: the camera follows the unit of the group nearest the goal.
    const auto distance = [&](uint16_t id) {
        const auto& unit = unit_of(id);
        const auto dx =
            static_cast<double>((unit.position.x >> kFixedShift) - (goal->point[0] >> kFixedShift));
        const auto dz =
            static_cast<double>((unit.position.z >> kFixedShift) - (goal->point[2] >> kFixedShift));
        return std::sqrt(dx * dx + dz * dz);
    };
    const auto follow_front = [&] {
        uint16_t front = 0;
        double nearest = std::numeric_limits<double>::max();
        for (const auto id : group)
            if (present(id) && distance(id) < nearest) {
                front = id;
                nearest = distance(id);
            }
        if (front == 0)
            fail("every unit of the group was destroyed");
        const bool following =
            match_tracking_ && present(tracked_match_unit_) &&
            std::find(group.begin(), group.end(), tracked_match_unit_) != group.end();
        if (following && (tracked_match_unit_ == front ||
                          distance(tracked_match_unit_) - nearest < kFollowMarginPixels))
            return;
        // T follows the next selected unit; it is pressed until the front one.
        const auto selected = selected_local_ids().size();
        for (std::size_t presses = 0;
             presses <= selected && !(match_tracking_ && tracked_match_unit_ == front);
             ++presses)
            press_key(SDLK_T, SDL_SCANCODE_T, SDL_KMOD_NONE);
        report(
            "the camera follows " + unit_info_name(front) + " (unit " + std::to_string(front) +
            "), " + std::to_string(std::lround(nearest)) + " pixels from the goal"
        );
    };
    const auto give_up = now() + kMarchTimeoutSeconds;
    auto next_look = now();
    while (screen_ == Screen::match && !match_finished_) {
        if (now() > give_up)
            fail("the units did not reach the goal in time");
        if (now() >= next_look || !match_tracking_) {
            follow_front();
            next_look = now() + kFollowSeconds;
        }
        frame();
    }
    // The finished match's Game block, which the end screen shows, holds
    // its outcome.
    const auto* finished = endgame_world();
    if (finished == nullptr ||
        (finished->game.outcome_flags & sim::scenario::outcome_flag::won) == 0)
        fail("the mission was lost");
    report("victory");

    // The end screen: the darkened last frame, the glamour picture, which a
    // click advances once it has shown a while, and the score screen.
    const auto end_step = [this] {
        const auto* world = endgame_world();
        return world != nullptr ? world->game.endgame_state : 0;
    };
    wait_for(
        [&] {
            const auto* world = endgame_world();
            return world != nullptr && world->game.endgame_state == OA_ENDGAME_GLAMOUR &&
                   world->game.endgame_fade_done != 0;
        },
        "glamour picture"
    );
    report("the glamour picture");
    hold(kGlamourSeconds);
    press_here();
    wait_for([&] { return end_step() == OA_ENDGAME_PANEL; }, "score screen");
    report("the score screen");
    hold(kScoreSeconds);
    report("done");
}

} // namespace oa::app

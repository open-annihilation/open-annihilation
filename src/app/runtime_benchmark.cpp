// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Bounded frame-time benchmark and headless runs over live skirmish and
// campaign matches.
#include "oa/app/runtime.hpp"
#include "engine_settings_state.hpp"
#include "match_models.hpp"
#include "stage_state.hpp"
#include "oa/sim/selection.hpp"
#include "oa/platform/files.hpp"
#include "match_fault.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {
namespace {

using BenchClock = std::chrono::steady_clock;

[[nodiscard]] int64_t elapsed_ns(BenchClock::time_point since) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(BenchClock::now() - since).count();
}

[[nodiscard]] double average_ms(int64_t total_ns, std::size_t frames) {
    return frames == 0 ? 0.0 : static_cast<double>(total_ns) / 1.0e6 / static_cast<double>(frames);
}

constexpr int32_t kScrollStep = 6;
// 16.16 world units: the shift to whole map pixels and one map pixel.
constexpr uint32_t kFixedShift = 16;
constexpr double kFixedOne = 65536.0;
// Map pixels of a map tile.
constexpr uint32_t kTilePixels = 32;
// --march: map pixels each unit is sent south, kept this far inside the
// map's bottom edge.
constexpr int32_t kMarchDistance = 400;
constexpr int32_t kMarchEdge = 64;
// --scroll-camera: map pixels the camera sweeps from where it starts before
// the held scroll turns back, so that the army it starts over stays in view.
constexpr int32_t kScrollSweep = 192;
// Ticks of a mission between the orders --give-orders gives a headless
// campaign run.
constexpr std::size_t kMissionOrderPeriod = 300;
// Ticks of a mission between the unit counts a headless campaign run prints.
constexpr std::size_t kCampaignCensusPeriod = 300;
// --busy-combat: the missile trucks each side, the first one's place south
// of the armies' centre (map pixels, negative is north), the space between
// them and how far east or west of the centre they stand.
constexpr int32_t kBusyCombatTrucks = 4;
constexpr int32_t kBusyCombatTruckTop = -72;
constexpr int32_t kBusyCombatTruckSpacing = 40;
constexpr int32_t kBusyCombatTruckReach = 170;
// The kbot lab's place from the centre, and the peewees it is told to build.
constexpr int32_t kBusyCombatLabX = -120;
constexpr int32_t kBusyCombatLabZ = 140;
constexpr int32_t kBusyCombatLabBuilds = 4;
// The air transport's place from the centre, and how far east of it the
// peewee it loads stands.
constexpr int32_t kBusyCombatTransportX = -40;
constexpr int32_t kBusyCombatTransportZ = 120;
constexpr int32_t kBusyCombatCargoOffset = 24;
// The frames digest of a frame run: its start, and the odd multiplier each
// word of a frame is folded in with.
constexpr uint64_t kFramesDigestBasis = 0xcbf29ce484222325ULL;
constexpr uint64_t kFramesDigestMultiplier = 0x9e3779b97f4a7c15ULL;

/// Folds a drawn layer into a frame run's frames digest: its size, then its
/// bytes eight at a time, little-endian, the last word padded with zeros.
///
/// Each word is xored into the digest, which is then multiplied by
/// kFramesDigestMultiplier and turned left by 29 bits.
///
/// @param digest the digest so far; kFramesDigestBasis before the first frame
/// @param layer the layer drawn for the frame
/// @return the digest with the layer folded in
[[nodiscard]] uint64_t fold_frame(uint64_t digest, const renderer::Surface& layer) {
    const auto fold = [&](uint64_t word) {
        digest = std::rotl((digest ^ word) * kFramesDigestMultiplier, 29);
    };
    fold((static_cast<uint64_t>(layer.width) << 32) | layer.height);
    const uint8_t* bytes = layer.rgb.data();
    const std::size_t size = layer.rgb.size();
    std::size_t at = 0;
    for (; at + sizeof(uint64_t) <= size; at += sizeof(uint64_t)) {
        uint64_t word = 0;
        std::memcpy(&word, bytes + at, sizeof(word));
        fold(word);
    }
    if (at < size) {
        uint64_t word = 0;
        std::memcpy(&word, bytes + at, size - at);
        fold(word);
    }
    return digest;
}

} // namespace

void Runtime::step_match_simulation() {
    ++match_timing_.tick;
    match_->simulation().tick = match_timing_.tick;
    // A stage's lines timed for this tick run before it.
    run_due_stage_lines();
    tick_or_raise(*match_);
}

void Runtime::benchmark_scene(std::string_view label, std::size_t frames, bool scroll) {
    if (screen_ != Screen::match || !match_ || !selected_tnt_)
        throw std::runtime_error("benchmark scene requires an active match");
    phase_times_ = {};
    int32_t direction = kScrollStep;
    const auto map_width = shown_map_size()[0];
    int64_t total = 0;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const auto frame_start = BenchClock::now();
        SDL_Event event{};
        while (SDL_PollEvent(&event)) {
        }
        if (scroll) {
            const auto limit = std::max(0, map_width - visible_map_width());
            if (match_camera_x_ + direction > limit || match_camera_x_ + direction < 0)
                direction = -direction;
            match_camera_x_ = std::clamp(match_camera_x_ + direction, 0, limit);
        }
        if (!match_tick_blocked_ && !match_finished_) {
            const auto sim_start = BenchClock::now();
            try {
                step_match_simulation();
            } catch (const std::exception& error) {
                match_tick_blocked_ = true;
                std::fprintf(stderr, "benchmark tick stopped: %s\n", error.what());
            }
            phase_times_.simulation += elapsed_ns(sim_start);
        }
        render();
        total += elapsed_ns(frame_start);
    }
    const auto frame_ms = average_ms(total, frames);
    const auto upload_present = phase_times_.upload + phase_times_.present;
    std::printf(
        "benchmark %-18.*s %zu frames @ %dx%d: %.2f ms/frame (%.1f fps) | sim %.2f | "
        "world %.2f (fog %.2f) | hud %.2f | upload %.2f | present %.2f\n",
        static_cast<int>(label.size()),
        label.data(),
        frames,
        match_layout_.width,
        match_layout_.height,
        frame_ms,
        frame_ms > 0.0 ? 1000.0 / frame_ms : 0.0,
        average_ms(phase_times_.simulation, frames),
        average_ms(phase_times_.compose - phase_times_.hud, frames),
        average_ms(phase_times_.fog, frames),
        average_ms(phase_times_.hud, frames),
        average_ms(phase_times_.upload, frames),
        average_ms(upload_present - phase_times_.upload, frames)
    );
    std::fflush(stdout);
    if (!options_.snapshot.empty()) {
        std::string tag(label);
        std::replace(tag.begin(), tag.end(), ' ', '-');
        auto base = options_.snapshot;
        base.replace_extension();
        renderer::Surface frame;
        compose_match_frame(frame);
        write_ppm(base.string() + "-" + tag + ".ppm", frame);
    }
}

void Runtime::exercise_click(std::string_view gadget_name) {
    const auto found = std::find_if(
        resources_.layout.gadgets.begin(),
        resources_.layout.gadgets.end(),
        [gadget_name](const auto& gadget) { return gadget.common.name == gadget_name; }
    );
    if (found == resources_.layout.gadgets.end())
        throw std::runtime_error("navigation check lacks button: " + std::string(gadget_name));
    const float modal_x =
        screen_ == Screen::map_selection
            ? static_cast<float>(
                  (kCanvasWidth -
                   static_cast<int>(resources_.layout.gadgets.front().common.width)) /
                  2
              )
            : 0.0F;
    const float modal_y =
        screen_ == Screen::map_selection
            ? static_cast<float>(
                  (kCanvasHeight -
                   static_cast<int>(resources_.layout.gadgets.front().common.height)) /
                  2
              )
            : 0.0F;
    const auto origin = panel_origin();
    const float x = modal_x + static_cast<float>(origin.x + found->common.x) +
                    static_cast<float>(found->common.width) / 2.0F;
    const float y = modal_y + static_cast<float>(origin.y + found->common.y) +
                    static_cast<float>(found->common.height) / 2.0F;
    update_pointer(x, y);
    selected_ =
        hovered_ && frontend_gadget_pressable(*hovered_) ? static_cast<int32_t>(*hovered_) : -1;
    rebuild_surface(); // Preserve a complete rendered frame between press and release.
    update_pointer(x, y);
    const auto released = hovered_;
    if (!released || selected_ != static_cast<int32_t>(*released))
        throw std::runtime_error(
            "navigation click did not retain selection: " + std::string(gadget_name)
        );
    activate();
    selected_ = -1;
}

void Runtime::start_benchmark_skirmish() {
    exercise_click(menu::resource_name(menu::Button::single_player));
    exercise_click(entry::resource_name(entry::Button::skirmish));
    state_.player_count = 2;
    if (map_player_capacity() < 2)
        throw std::runtime_error("benchmark map lacks two start positions");
    exercise_click(skirmish::resource_name(skirmish::Button::start));
    if (screen_ != Screen::match || !match_)
        throw std::runtime_error("benchmark Start did not enter a match");
}

void Runtime::spawn_combat_armies(std::size_t per_side) {
    // Each player's first unit: the commander the skirmish placed.
    const auto first_unit = [this](uint8_t player) -> const sim::unit_spawn::Slot* {
        for (const auto& slot : match_->world().slots)
            if (slot.unit != nullptr && slot.record.type_index != 0 &&
                slot.record.owner_index == player)
                return &slot;
        return nullptr;
    };
    const sim::unit_spawn::Slot* commander = first_unit(match_local_player_);
    if (commander == nullptr)
        throw std::runtime_error("combat benchmark needs the local commander");
    // The Peewee against the A.K.; a side whose game has no such unit fights
    // with its player's commander type.
    const std::array<std::string_view, 2> names{"ARMPW", "CORAK"};
    const auto map_w = static_cast<int32_t>(selected_tnt_->tile_width * 32U);
    const auto map_h = static_cast<int32_t>(selected_tnt_->tile_height * 32U);
    const auto centre_x =
        std::clamp(static_cast<int32_t>(commander->unit->position[0] >> 16), 200, map_w - 200);
    const auto centre_z =
        std::clamp(static_cast<int32_t>(commander->unit->position[2] >> 16), 200, map_h - 200);
    for (uint8_t side = 0; side < 2; ++side) {
        const uint8_t player = side == 0 ? match_local_player_
                                         : static_cast<uint8_t>(match_local_player_ == 0 ? 1 : 0);
        auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, names[side]);
        if (type == 0) {
            const auto* player_commander = first_unit(player);
            if (player_commander == nullptr)
                throw std::runtime_error(
                    "combat benchmark lacks " + std::string(names[side]) +
                    " and a commander of player " + std::to_string(player)
                );
            type = player_commander->record.type_index;
        }
        for (std::size_t index = 0; index < per_side; ++index) {
            const auto x = centre_x + (side == 0 ? -90 : 90) +
                           static_cast<int32_t>(index / 8) * (side == 0 ? -24 : 24);
            const auto z = centre_z - 96 + static_cast<int32_t>(index % 8) * 24;
            oa::sim::unit_spawn::Request request;
            request.player = player;
            request.type = type;
            request.finished = true;
            request.state = kGroundOccupancyState;
            request.position = {
                static_cast<uint32_t>(x) << 16,
                static_cast<uint32_t>(match_->map_height(
                    static_cast<uint32_t>(x) << 16, static_cast<uint32_t>(z) << 16
                )) << 16,
                static_cast<uint32_t>(z) << 16
            };
            const auto* placed = match_->create(request);
            if (placed == nullptr || placed->unit == nullptr)
                throw std::runtime_error(
                    "combat benchmark could not place " + spawn_type_names_.at(type)
                );
        }
    }
    if (options_.busy_combat)
        spawn_busy_combat(centre_x, centre_z);
    match_camera_x_ = centre_x - visible_map_width() / 2;
    match_camera_z_ = centre_z - visible_map_height() / 2;
}

void Runtime::apply_stage() {
    std::ifstream file(options_.stage_file);
    if (!file)
        throw std::runtime_error("cannot read the stage file " + options_.stage_file.string());
    // Each player's first unit, where it stands when the stage begins: the
    // point a placement is measured from.
    begin_stage_state();
    std::vector<StageLine> lines;
    std::string text;
    for (std::size_t number = 1; std::getline(file, text); ++number)
        lines.push_back({number, text});
    run_stage_lines(lines);
}

void Runtime::run_stage_lines(std::span<const StageLine> lines) {
    const auto& commanders = stage_->origins;
    uint16_t& last = stage_->last_unit;
    for (const auto& source : lines) {
        const auto where = options_.stage_file.string() + ":" + std::to_string(source.number);
        std::istringstream line(source.text);
        std::string action;
        if (!(line >> action) || action[0] == '#')
            continue;
        // A line timed for a later tick waits for it.
        if (action == "at" && !take_stage_tick(line, where, source, action))
            continue;
        if (run_stage_direction(action, line, where))
            continue;
        if (action == "unit") {
            int32_t player = -1;
            std::string name;
            int32_t dx = 0, dz = 0;
            if (!(line >> player >> name >> dx >> dz))
                throw std::runtime_error(where + ": unit takes PLAYER TYPE DX DZ [FROM]");
            // The player whose first unit the offsets start from; the owner
            // without one.
            int32_t from = player;
            if (!(line >> from))
                from = player;
            const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
            if (type == 0)
                throw std::runtime_error(where + ": the game has no type " + name);
            if (player < 0 || player >= OA_PLAYER_COUNT ||
                match_->state().game.players[player].in_use == 0)
                throw std::runtime_error(
                    where + ": player " + std::to_string(player) + " is not playing"
                );
            const auto origin = commanders.find(from);
            if (origin == commanders.end())
                throw std::runtime_error(
                    where + ": player " + std::to_string(from) + " has no starting unit"
                );
            const int32_t x = origin->second.first + dx;
            const int32_t z = origin->second.second + dz;
            oa::sim::unit_spawn::Request request;
            request.player = static_cast<uint8_t>(player);
            request.type = type;
            request.finished = true;
            request.state = kGroundOccupancyState;
            request.position = {
                static_cast<uint32_t>(x) << 16,
                static_cast<uint32_t>(match_->map_height(
                    static_cast<uint32_t>(x) << 16, static_cast<uint32_t>(z) << 16
                )) << 16,
                static_cast<uint32_t>(z) << 16
            };
            const auto* placed = match_->create(request);
            if (placed == nullptr || placed->unit == nullptr)
                throw std::runtime_error(where + ": " + name + " could not be placed");
            join_stage_group(placed->unit_index);
            std::printf(
                "stage: unit %u %s of player %d at %d,%d\n",
                static_cast<unsigned>(last),
                name.c_str(),
                player,
                x,
                z
            );
        } else if (action == "stockpile") {
            int32_t rounds = 0;
            if (!(line >> rounds) || rounds <= 0 || last == 0)
                throw std::runtime_error(where + ": stockpile takes ROUNDS after a unit");
            match_->issue_build_weapon(last, 0, rounds);
            std::printf("stage: unit %u stockpiles %d\n", static_cast<unsigned>(last), rounds);
        } else if (action == "build") {
            std::string name;
            int32_t dx = 0, dz = 0;
            if (!(line >> name >> dx >> dz) || last == 0)
                throw std::runtime_error(where + ": build takes TYPE DX DZ after a unit");
            const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
            if (type == 0)
                throw std::runtime_error(where + ": the game has no type " + name);
            const auto* builder = match_->world().slots[last].unit;
            const int32_t x = static_cast<int32_t>(builder->position[0] >> 16) + dx;
            const int32_t z = static_cast<int32_t>(builder->position[2] >> 16) + dz;
            const oa::sim::ground_orders::Point site{
                x * 65536,
                match_->map_height(static_cast<uint32_t>(x) << 16, static_cast<uint32_t>(z) << 16) *
                    65536,
                z * 65536
            };
            match_->issue_mobile_build(last, type, site, true);
            std::printf(
                "stage: unit %u builds %s at %d,%d\n",
                static_cast<unsigned>(last),
                name.c_str(),
                x,
                z
            );
        } else if (action == "console") {
            std::string rest;
            std::getline(line >> std::ws, rest);
            enter_console_check_line(rest.c_str());
            std::printf("stage: console %s\n", rest.c_str());
        } else if (action == "type") {
            // The chat line opened and the text typed into it, left open.
            std::string rest;
            std::getline(line >> std::ws, rest);
            open_chat_line();
            if (!chat_composing_)
                throw std::runtime_error(where + ": the chat line did not open");
            bool running = true;
            SDL_Event event{};
            event.type = SDL_EVENT_TEXT_INPUT;
            event.text.text = rest.c_str();
            handle_sdl_event(event, running);
            std::printf("stage: type %s\n", rest.c_str());
        } else if (action == "settings") {
            // The settings dialog opened beside the in-game menu at a
            // section, named as its list names it.
            namespace settings = oa::ui::engine_settings;
            std::string rest;
            std::getline(line >> std::ws, rest);
            // The sections as the dialog lists them, Touch with touch controls;
            // each entry's control is its section's page_control.
            const bool touch = touch_controls_active();
            settings::Dialog names;
            settings::open_dialog(
                names,
                {},
                {},
                {},
                {},
                settings::Page::common_tweaks,
                {},
                settings::highest_unit_limit,
                {},
                {},
                nullptr,
                touch
            );
            std::optional<settings::Page> page;
            for (const auto& part : settings::dialog_layout(names))
                for (const auto listed :
                     settings::dialog_pages(settings::DialogKind::engine, touch))
                    if (part.text == rest && part.control == settings::page_control(listed))
                        page = listed;
            if (!page)
                throw std::runtime_error(where + ": the settings have no section " + rest);
            engine_settings_state().last_page = *page;
            open_engine_settings_in_match();
            if (engine_settings_dialog() == nullptr)
                throw std::runtime_error(where + ": the settings dialog did not open");
            std::printf("stage: settings %s\n", rest.c_str());
        } else if (action == "pointer") {
            // The pointer moved to a point of the window, whose unit the
            // bottom bar then shows.
            float x = 0.0F;
            float y = 0.0F;
            if (!(line >> x >> y))
                throw std::runtime_error(where + ": pointer takes X Y");
            bool running = true;
            SDL_Event event{};
            event.type = SDL_EVENT_MOUSE_MOTION;
            event.motion.x = x;
            event.motion.y = y;
            handle_sdl_event(event, running);
            std::printf("stage: pointer %g %g\n", static_cast<double>(x), static_cast<double>(y));
        } else if (action == "click") {
            // The pointer pressed and released at a point of the window, as
            // a click of its left button, reaching first whatever dialog or
            // menu shows there, as the player's click does.
            float x = 0.0F;
            float y = 0.0F;
            if (!(line >> x >> y))
                throw std::runtime_error(where + ": click takes X Y");
            bool running = true;
            SDL_Event event{};
            event.type = SDL_EVENT_MOUSE_MOTION;
            event.motion.x = x;
            event.motion.y = y;
            dispatch_event(event, running);
            for (const bool down : {true, false}) {
                SDL_Event press{};
                press.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
                press.button.button = SDL_BUTTON_LEFT;
                press.button.down = down;
                press.button.clicks = 1;
                press.button.x = x;
                press.button.y = y;
                dispatch_event(press, running);
            }
            std::printf("stage: click %g %g\n", static_cast<double>(x), static_cast<double>(y));
        } else if (action == "key") {
            // A key pressed and released, named as SDL names it: "Space",
            // "Down", "Tab"; an open dialog or menu takes it first, as it
            // takes the player's keys.
            std::string name;
            std::getline(line >> std::ws, name);
            const SDL_Keycode code = SDL_GetKeyFromName(name.c_str());
            if (code == SDLK_UNKNOWN)
                throw std::runtime_error(where + ": no key " + name);
            bool running = true;
            for (const bool down : {true, false}) {
                SDL_Event event{};
                event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                event.key.key = code;
                event.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
                event.key.down = down;
                dispatch_event(event, running);
            }
            std::printf("stage: key %s\n", name.c_str());
        } else {
            throw std::runtime_error(where + ": no stage action " + action);
        }
    }
    std::fflush(stdout);
}

void Runtime::spawn_busy_combat(int32_t centre_x, int32_t centre_z) {
    const uint8_t local = match_local_player_;
    const auto enemy = static_cast<uint8_t>(local == 0 ? 1 : 0);
    const auto type_of = [this](std::string_view name) {
        const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
        if (type == 0)
            throw std::runtime_error("busy combat lacks " + std::string(name));
        return type;
    };
    // A finished unit on the ground at a map pixel.
    const auto place = [&](std::string_view name, uint8_t player, int32_t x, int32_t z) {
        oa::sim::unit_spawn::Request request;
        request.player = player;
        request.type = type_of(name);
        request.finished = true;
        request.state = kGroundOccupancyState;
        request.position = {
            static_cast<uint32_t>(x) << 16,
            static_cast<uint32_t>(
                match_->map_height(static_cast<uint32_t>(x) << 16, static_cast<uint32_t>(z) << 16)
            ) << 16,
            static_cast<uint32_t>(z) << 16
        };
        const auto* placed = match_->create(request);
        if (placed == nullptr || placed->unit == nullptr)
            throw std::runtime_error("busy combat could not place " + std::string(name));
        return placed->unit_index;
    };
    // Missile trucks behind each army: their missiles are drawn as 3DO models.
    for (int32_t truck = 0; truck < kBusyCombatTrucks; ++truck) {
        const int32_t z = centre_z + kBusyCombatTruckTop + truck * kBusyCombatTruckSpacing;
        place("ARMSAM", local, centre_x - kBusyCombatTruckReach, z);
        place("CORMIST", enemy, centre_x + kBusyCombatTruckReach, z);
    }
    // A kbot lab building peewees: its nano particles, and each peewee
    // carried on the lab's pad while it is built.
    const auto lab = place("ARMLAB", local, centre_x + kBusyCombatLabX, centre_z + kBusyCombatLabZ);
    match_->queue_factory_build(lab, type_of("ARMPW"), kBusyCombatLabBuilds);
    // An air transport loading the peewee beside it, then carrying it.
    const auto transport = place(
        "ARMATLAS", local, centre_x + kBusyCombatTransportX, centre_z + kBusyCombatTransportZ
    );
    const auto cargo = place(
        "ARMPW",
        local,
        centre_x + kBusyCombatTransportX + kBusyCombatCargoOffset,
        centre_z + kBusyCombatTransportZ
    );
    match_->issue_load(transport, cargo, false);
    // The local army selected, its selection boxes shown.
    auto& world = match_->state();
    world.game.console_flags =
        static_cast<uint16_t>(world.game.console_flags | OA_CONSOLE_FLAG_SELECTION_BOXES);
    oa::sim::selection::select_all(world, selection_hooks());
}

void Runtime::prepare_headless_match() {
    start_benchmark_skirmish();
    match_layout_ = lay_out_match(options_.match_width, options_.match_height);
    // Within the view's limits, which the preferences file's Maximum zoom
    // out and Maximum zoom in decide, and the map and the battlefield.
    match_zoom_ = std::clamp(options_.match_zoom, least_match_zoom(), most_match_zoom());
    match_zoom_target_ = match_zoom_;
    if (options_.combat_units != 0)
        spawn_combat_armies(options_.combat_units);
    if (!options_.stage_file.empty())
        apply_stage();
    if constexpr (self_checks_built)
        if (options_.reclaim_check)
            begin_reclaim_check();
    if (options_.camera) {
        match_camera_x_ = options_.camera->first;
        match_camera_z_ = options_.camera->second;
    }
}

uint64_t Runtime::frame_run_digest() {
    const auto kept_x = match_camera_x_;
    const auto kept_z = match_camera_z_;
    const auto kept_adaptation = match_timing_.adaptation;
    match_camera_x_ = 0;
    match_camera_z_ = 0;
    match_timing_.adaptation = 0;
    const auto digest = match_world_digest();
    match_camera_x_ = kept_x;
    match_camera_z_ = kept_z;
    match_timing_.adaptation = kept_adaptation;
    return digest;
}

void Runtime::run_headless_frames(std::size_t ticks, uint32_t frames_per_second) {
    prepare_headless_match();
    // --march sends the local player's units south; the frame log follows
    // the first of them, or the first local unit without --march.
    uint16_t probe = 0;
    for (const auto& slot : match_->world().slots) {
        if (slot.unit == nullptr || slot.record.type_index == 0 ||
            slot.record.owner_index != match_local_player_ ||
            !match_->takes_move_order(slot.unit_index))
            continue;
        if (probe == 0)
            probe = slot.unit_index;
        if (!options_.march)
            break;
        const auto x = static_cast<int32_t>(slot.unit->position[0] >> kFixedShift);
        const auto map_h = static_cast<int32_t>(selected_tnt_->tile_height * kTilePixels);
        const auto z = std::min(
            static_cast<int32_t>(slot.unit->position[2] >> kFixedShift) + kMarchDistance,
            map_h - kMarchEdge
        );
        const oa::sim::ground_orders::Point point{
            static_cast<int32_t>(static_cast<uint32_t>(x) << kFixedShift),
            static_cast<int32_t>(
                static_cast<uint32_t>(match_->map_height(
                    static_cast<uint32_t>(x) << kFixedShift, static_cast<uint32_t>(z) << kFixedShift
                ))
                << kFixedShift
            ),
            static_cast<int32_t>(static_cast<uint32_t>(z) << kFixedShift)
        };
        match_->issue_ground_move(slot.unit_index, point, false);
    }

    // The run's clock, its held scroll and its log end with it, however it ends.
    struct RunEnd {
        Runtime& runtime;
        std::FILE* log{};

        /// Ends the run's clock, its held scroll and its log.
        ~RunEnd() {
            runtime.frame_run_clock_ns_.reset();
            runtime.frame_run_frames_per_second_ = 0;
            runtime.frame_run_scroll_ = 0;
            if (runtime.match_tracking_)
                runtime.stop_match_tracking();
            if (log != nullptr)
                std::fclose(log);
        }
    } run_end{*this};

    std::FILE* log = nullptr;
    if (!options_.frame_log.empty()) {
        log = run_end.log = oa::platform::open_file(options_.frame_log, "w");
        if (log == nullptr)
            throw std::runtime_error("cannot create the frame log " + options_.frame_log.string());
        std::fprintf(
            log, "frame,time_ms,tick,alpha,camera_x,camera_z,zoom,unit_x,unit_z,drawn_x,drawn_z\n"
        );
    }
    // The run's clock starts at --frame-clock, or 0, where the match clock
    // last stepped; its frames stand for the middle of each
    // 1 / frames_per_second.
    const uint64_t clock_start_ms = options_.frame_clock_ms.value_or(0);
    const uint64_t clock_start_ns = clock_start_ms * frame_pacing::kNanosecondsPerMillisecond;
    frame_run_clock_ns_ = clock_start_ns;
    frame_run_frames_per_second_ = frames_per_second;
    // The low 32 bits of the milliseconds, as the loop's clock keeps them.
    match_timing_.previous_clock = oa::base::game_loop::scaled_clock(
        static_cast<uint32_t>(clock_start_ms), match_clock_scale()
    );
    tick_presentation_ = {};
    scroll_clock_ = 0;
    zoom_clock_valid_ = false;
    frame_draws_ = {};
    frame_draws_.probe_unit = probe;
    // --follow tracks the unit the log follows, as the T key does.
    if (options_.follow && probe != 0)
        begin_match_tracking(probe);
    int32_t scroll = options_.scroll_camera ? 1 : 0;
    // The sweep runs from where the camera starts, or as near it as the
    // map's right edge leaves room for.
    const auto map_width = static_cast<int32_t>(selected_tnt_->tile_width * kTilePixels);
    const int32_t scroll_limit = std::max(0, map_width - visible_map_width());
    const int32_t sweep_left = std::max(0, std::min(match_camera_x_, scroll_limit - kScrollSweep));
    const int32_t sweep_right = std::min(scroll_limit, sweep_left + kScrollSweep);
    // A match whose clock never steps would never end the run.
    const uint64_t frame_limit =
        static_cast<uint64_t>(ticks + 1) * frames_per_second / frame_pacing::kTicksPerSecond * 2 +
        2;
    std::size_t between_ticks = 0;
    uint64_t frame = 0;
    // Every frame's drawn battlefield, which no count of drawing threads changes.
    uint64_t frames_digest = kFramesDigestBasis;
    while (match_timing_.tick < ticks) {
        if (frame >= frame_limit)
            throw std::runtime_error("the frame run's match clock stopped stepping");
        // Each frame stands for the middle of its period, so frames at
        // the tick rate do not fall on the clock's whole milliseconds
        // where its units turn over.
        frame_run_clock_ns_ = clock_start_ns + (2 * frame + 1) *
                                                   frame_pacing::kNanosecondsPerSecond /
                                                   (2 * frames_per_second);
        // The frame's unit announcements, camera and clock step, in the
        // application loop's order: each frame presents at most one
        // announcement, as every frame of the game does.
        take_frame_time();
        camera_moved_ = false;
        present_unit_announcements();
        frame_run_scroll_ = scroll;
        move_match_camera();
        step_match_frame();
        frame_draws_.units_drawn = 0;
        frame_draws_.units_between_ticks = 0;
        frame_draws_.probe_drawn = false;
        rebuild_surface();
        frames_digest = fold_frame(frames_digest, match_world_cpu_);
        between_ticks += presentation_alpha_ < 1.0F ? 1 : 0;
        if (log != nullptr) {
            const auto* unit = probe != 0 ? match_->world().slots[probe].unit : nullptr;
            std::fprintf(
                log,
                "%llu,%.3f,%u,%.4f,%d,%d,%.4f,",
                static_cast<unsigned long long>(frame),
                static_cast<double>(*frame_run_clock_ns_) / 1.0e6,
                match_timing_.tick,
                static_cast<double>(presentation_alpha_),
                match_camera_x_,
                match_camera_z_,
                static_cast<double>(match_zoom_)
            );
            if (unit != nullptr)
                std::fprintf(
                    log,
                    "%.4f,%.4f,",
                    static_cast<double>(static_cast<int32_t>(uint32_t{unit->position[0]})) /
                        kFixedOne,
                    static_cast<double>(static_cast<int32_t>(uint32_t{unit->position[2]})) /
                        kFixedOne
                );
            else
                std::fprintf(log, ",,");
            if (frame_draws_.probe_drawn)
                std::fprintf(
                    log,
                    "%.4f,%.4f\n",
                    static_cast<double>(frame_draws_.probe_x) / kFixedOne,
                    static_cast<double>(frame_draws_.probe_z) / kFixedOne
                );
            else
                std::fprintf(log, ",\n");
        }
        presentation_alpha_ = 1.0F;
        // The held scroll turns back at either end of its sweep.
        if ((scroll > 0 && match_camera_x_ >= sweep_right) ||
            (scroll < 0 && match_camera_x_ <= sweep_left))
            scroll = -scroll;
        ++frame;
    }
    std::printf(
        "frame run: %u frames a second, %llu frames, %u ticks, %zu frames between ticks, "
        "world digest %016llx, frames digest %016llx, drawing threads %u, drawing bands %d\n",
        frames_per_second,
        static_cast<unsigned long long>(frame),
        match_timing_.tick,
        between_ticks,
        static_cast<unsigned long long>(frame_run_digest()),
        static_cast<unsigned long long>(frames_digest),
        draw_pool_ ? draw_pool_->threads() : 1U,
        match_models_ ? match_models_->most_bands : 0
    );
    std::fflush(stdout);
    if (!options_.snapshot.empty())
        write_ppm(options_.snapshot, surface_);
    print_memory_status();
}

void Runtime::run_headless_match(std::size_t ticks) {
    prepare_headless_match();
    int64_t window_ns = 0;
    int64_t worst_ns = 0;
    for (std::size_t tick = 1; tick <= ticks; ++tick) {
        const auto start = BenchClock::now();
        try {
            step_match_simulation();
        } catch (const std::exception& error) {
            report_match_tick_error(error.what());
        }
        if constexpr (self_checks_built)
            if (options_.reclaim_check)
                tick_reclaim_check();
        rebuild_surface();
        const auto spent = elapsed_ns(start);
        window_ns += spent;
        worst_ns = std::max(worst_ns, spent);
        if (tick % 30 == 0 || tick == ticks) {
            std::size_t live = 0;
            for (const auto& slot : match_->world().slots)
                live += slot.unit != nullptr && slot.record.type_index != 0 ? 1 : 0;
            const auto& effects = match_->effects();
            std::size_t fragments = 0;
            for (int32_t record = 0; record < effects.explosion_count; ++record)
                if (effects.explosions[record].fragment != oa::sim::effect_particles::no_fragment)
                    ++fragments;
            std::printf(
                "tick %5zu: %.2f ms avg %.2f ms max | units %zu projectiles %zu wrecks %zu "
                "explosions %zu fragments %zu particles %zu\n",
                tick,
                static_cast<double>(window_ns) / 1.0e6 / 30.0,
                static_cast<double>(worst_ns) / 1.0e6,
                live,
                match_->projectiles().size(),
                match_->wrecks().size(),
                static_cast<std::size_t>(effects.explosion_count),
                fragments,
                match_->particle_count()
            );
            std::fflush(stdout);
            window_ns = 0;
            worst_ns = 0;
        }
    }
    if constexpr (self_checks_built)
        if (options_.reclaim_check)
            finish_reclaim_check();
    if (!options_.snapshot.empty())
        write_ppm(options_.snapshot, surface_);
    print_memory_status();
}

namespace {

[[nodiscard]] const char* outcome_name(sim::scenario::Outcome outcome) {
    switch (outcome) {
    case sim::scenario::Outcome::victory:
        return "victory";
    case sim::scenario::Outcome::defeat:
        return "defeat";
    case sim::scenario::Outcome::respawn:
        return "respawn";
    default:
        return "ongoing";
    }
}

struct TickErrorTally {
    std::string message;
    std::size_t first_tick = 0;
    std::size_t count = 0;
};

} // namespace

void Runtime::write_stage_snapshot(std::string_view stage) {
    if (options_.snapshot.empty())
        return;
    auto path = options_.snapshot;
    path.replace_extension();
    rebuild_surface();
    write_ppm(path.string() + "-" + std::string(stage) + ".ppm", surface_);
}

std::string Runtime::start_headless_campaign_mission() {
    // Game data without the any-mission screen starts its missions from the
    // new-campaign screen, whose lists the check fills the same way.
    load(offers_any_mission() ? Screen::any_mission : Screen::new_campaign);
    bool found = false;
    for (uint8_t side = 0; side < 2 && !found; ++side) {
        preferences_.side = side;
        discover_campaigns();
        for (std::size_t index = 0; index < campaign_labels_.size(); ++index)
            if (tdf_names_equal(campaign_labels_[index], options_.campaign)) {
                selected_campaign_index_ = index;
                found = true;
                break;
            }
    }
    if (!found)
        throw std::runtime_error("no campaign is named '" + options_.campaign + "'");
    load_campaign_missions(selected_campaign_index_);
    const auto mission = *options_.campaign_mission;
    if (mission >= campaign_mission_files_.size())
        throw std::runtime_error(
            options_.campaign + " has " + std::to_string(campaign_mission_files_.size()) +
            " missions"
        );
    selected_mission_index_ = mission;
    show_mission_briefing();
    if (screen_ != Screen::briefing)
        throw std::runtime_error("campaign briefing did not open: " + status_);
    write_stage_snapshot("briefing-" + std::to_string(mission));
    start_campaign_mission();
    if (screen_ != Screen::match || !match_ || !campaign_mission_)
        throw std::runtime_error("campaign Start did not enter the mission: " + status_);
    const auto mission_file = campaign_mission_files_[mission];
    std::printf(
        "campaign start: %s mission %zu (%s) side %u difficulty %u\n",
        options_.campaign.c_str(),
        mission,
        mission_file.c_str(),
        static_cast<unsigned>(preferences_.side),
        static_cast<unsigned>(preferences_.difficulty)
    );
    const auto& session = match_->state();
    const auto schema_features =
        std::count_if(mission_features_.begin(), mission_features_.end(), [](const auto& entry) {
            return entry.name[0] != '\0';
        });
    std::printf(
        "campaign session: unit limit %u, commander rule %d, visibility %u, surface metal %d, "
        "unit types %u, schema features %td, storms %u\n",
        static_cast<unsigned>(session.game.units_per_player),
        static_cast<int>(session.game.session_rules),
        static_cast<unsigned>(session.game.visibility_flags),
        static_cast<int>(configured_map_metal_),
        static_cast<unsigned>(session.unit_def_count - 1U),
        schema_features,
        meteor_enabled() ? 1U : 0U
    );
    return mission_file;
}

std::string Runtime::advance_headless_campaign(std::size_t run_tick) {
    const auto finished = selected_mission_index_;
    const bool won = match_->outcome() == sim::scenario::Outcome::victory;
    present_match_outcome();
    write_stage_snapshot("outcome-" + std::to_string(finished));
    finish_match_outcome();
    if (screen_ != Screen::campaign_end)
        throw std::runtime_error("the finished mission did not open the end screen: " + status_);
    if (!step_end_screen_to_panel().panel) {
        if (screen_ == Screen::campaign_end)
            throw std::runtime_error("the end screen did not reach its panel");
        std::printf(
            "campaign end: the end screen of %s left for frontend state %u\n",
            bound_mission_name().c_str(),
            static_cast<unsigned>(state_.state)
        );
        std::fflush(stdout);
        return {};
    }
    write_stage_snapshot("end-" + std::to_string(finished));
    const auto* start = widget("Start");
    if (!won) {
        // A lost mission's Start plays it again; the run ends here.
        std::printf(
            "campaign end: %s was lost; Start %s mission %zu again\n",
            bound_mission_name().c_str(),
            start != nullptr && start->common.active != 0 ? "offers" : "does not offer",
            selected_mission_index_
        );
        std::fflush(stdout);
        return {};
    }
    if (start == nullptr || start->common.active == 0) {
        std::printf("campaign end: no mission follows %s\n", bound_mission_name().c_str());
        std::fflush(stdout);
        return {};
    }
    exercise_click("Start");
    if (screen_ != Screen::briefing)
        throw std::runtime_error(
            "the end screen's Start did not open the next briefing: " + status_
        );
    write_stage_snapshot("briefing-" + std::to_string(selected_mission_index_));
    exercise_click("Start");
    if (screen_ != Screen::match || !match_ || !campaign_mission_)
        throw std::runtime_error("the next briefing's Start did not enter the mission: " + status_);
    const auto mission_file = campaign_mission_files_.at(selected_mission_index_);
    std::printf(
        "campaign advance: run tick %zu: mission %zu (%s), %s\n",
        run_tick,
        selected_mission_index_,
        mission_file.c_str(),
        bound_mission_name().c_str()
    );
    std::fflush(stdout);
    return mission_file;
}

int Runtime::run_headless_campaign(std::size_t ticks) {
    auto mission = *options_.campaign_mission;
    auto mission_file = start_headless_campaign_mission();
    std::vector<TickErrorTally> errors;
    std::size_t failed_ticks = 0;
    std::size_t ran = 0;
    // The run's tick at which the mission being played started (or last
    // started over); the mission's own ticks count from it.
    std::size_t mission_start = 0;
    std::size_t outcome_tick = 0;
    auto decided = sim::scenario::Outcome::ongoing;

    struct UnitCensus {
        std::size_t live = 0;
        std::size_t owned[2]{};
    };

    const auto census = [&] {
        UnitCensus counted;
        for (const auto& slot : match_->world().slots) {
            if (slot.unit == nullptr || slot.record.type_index == 0)
                continue;
            ++counted.live;
            if (slot.record.owner_index < 2)
                ++counted.owned[slot.record.owner_index];
        }
        return counted;
    };
    const auto report_units = [&](std::size_t tick) {
        const auto counted = census();
        std::printf(
            "campaign tick %5zu: units %zu (player %zu, computer %zu) projectiles %zu\n",
            tick,
            counted.live,
            counted.owned[0],
            counted.owned[1],
            match_->projectiles().size()
        );
        std::fflush(stdout);
    };
    // The units the mission being played started with.
    auto start_units = census();
    const auto hud_has = [this](std::string_view name) {
        return match_hud_ && std::any_of(
                                 match_hud_->layout.gadgets.begin(),
                                 match_hud_->layout.gadgets.end(),
                                 [name](const auto& gadget) { return gadget.common.name == name; }
                             );
    };
    // --restart-at: every cheat typed through the chat line takes effect in
    // the campaign, and the log and the leave question show the profile's
    // texts; then pause, EXIT, EXITMENU's RESTART, then RESTART.GUI's
    // RESTART at the stored difficulty must start the mission over, with
    // cheats still allowed.
    const auto restart_mission = [&](std::size_t tick) {
        check_console_campaign_cheats();
        check_profile_texts();
        show_match_pause_menu();
        activate_pause_gadget("EXIT");
        if (!hud_has("MAINMENU"))
            throw std::runtime_error("campaign restart: EXIT did not open EXITMENU.GUI");
        activate_pause_gadget("RESTART");
        if (!hud_has("MISSIONNAME"))
            throw std::runtime_error("campaign restart: RESTART did not open RESTART.GUI");
        const auto difficulty = preferences_.difficulty;
        activate_pause_gadget("RESTART");
        if (screen_ != Screen::match || !match_ || !campaign_mission_ ||
            match_->state().game.tick != 0 || preferences_.difficulty != difficulty)
            throw std::runtime_error("campaign restart did not start the mission over: " + status_);
        if (!session_cheats_allowed_)
            throw std::runtime_error("campaign restart refused cheats");
        const auto again = census();
        if (again.live != start_units.live || again.owned[0] != start_units.owned[0] ||
            again.owned[1] != start_units.owned[1])
            throw std::runtime_error(
                "campaign restart created " + std::to_string(again.live) +
                " units; the start created " + std::to_string(start_units.live)
            );
        std::printf(
            "campaign restart: at tick %zu the mission started over with %zu units (player %zu, "
            "computer %zu)\n",
            tick,
            again.live,
            again.owned[0],
            again.owned[1]
        );
        std::fflush(stdout);
    };
    report_units(0);
    if (options_.give_orders)
        give_mission_orders();
    for (std::size_t tick = 1; tick <= ticks; ++tick) {
        if (options_.campaign_restart_tick == tick) {
            restart_mission(tick);
            mission_start = tick - 1;
        }
        try {
            step_match_simulation();
        } catch (const std::exception& error) {
            ++failed_ticks;
            report_match_tick_error(error.what());
            const std::string message(error.what());
            auto entry = std::find_if(errors.begin(), errors.end(), [&](const auto& tally) {
                return tally.message == message;
            });
            if (entry == errors.end())
                entry = errors.insert(errors.end(), {message, tick, 0});
            ++entry->count;
        }
        ran = tick;
        const auto mission_tick = tick - mission_start;
        if (options_.give_orders && mission_tick % kMissionOrderPeriod == 0)
            give_mission_orders();
        if (mission_tick % kCampaignCensusPeriod == 0) {
            rebuild_surface();
            report_units(mission_tick);
        }
        const auto outcome = match_->outcome();
        if (outcome_tick == 0 && (outcome == sim::scenario::Outcome::victory ||
                                  outcome == sim::scenario::Outcome::defeat)) {
            outcome_tick = mission_tick;
            decided = outcome;
            std::printf("campaign outcome: %s at tick %zu\n", outcome_name(outcome), mission_tick);
            if (options_.campaign_past_outcome)
                continue;
            // --give-orders goes on through the end screen, and after a
            // victory into the next mission, as a player pressing Start would.
            if (!options_.give_orders)
                break;
            auto next = advance_headless_campaign(tick);
            if (next.empty())
                break;
            mission = selected_mission_index_;
            mission_file = std::move(next);
            mission_start = tick;
            outcome_tick = 0;
            decided = sim::scenario::Outcome::ongoing;
            start_units = census();
            report_units(0);
            give_mission_orders();
        }
    }
    if (!options_.snapshot.empty()) {
        if (match_) {
            renderer::Surface frame;
            compose_match_frame(frame);
            write_ppm(options_.snapshot, frame);
        } else {
            rebuild_surface();
            write_ppm(options_.snapshot, surface_);
        }
    }
    for (const auto& tally : errors)
        std::printf(
            "campaign error: first tick %zu, %zu ticks: %s\n",
            tally.first_tick,
            tally.count,
            tally.message.c_str()
        );
    std::printf(
        "campaign result: %s mission %zu (%s): %zu ticks, %zu failed ticks, %zu distinct errors, "
        "outcome %s at tick %zu\n",
        options_.campaign.c_str(),
        mission,
        mission_file.c_str(),
        ran,
        failed_ticks,
        errors.size(),
        outcome_name(decided),
        outcome_tick
    );
    std::fflush(stdout);
    return failed_ticks == 0 ? 0 : 1;
}

void Runtime::run_benchmark(std::size_t frames) {
    start_benchmark_skirmish();
    benchmark_scene("skirmish static", frames, false);
    benchmark_scene("skirmish scrolling", frames, true);
    leave_match();
    preferences_.side = 0;
    preferences_.difficulty = 0;
    show_mission_briefing();
    start_campaign_mission();
    if (screen_ != Screen::match || !match_)
        throw std::runtime_error("benchmark campaign Start did not enter the mission");
    benchmark_scene("campaign static", frames, false);
    benchmark_scene("campaign scrolling", frames, true);
    print_memory_status();
}

} // namespace oa::app

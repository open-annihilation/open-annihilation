// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A latched Shift queues two moves, a latched Space shows the status strip,
// and sticky Ctrl and Alt select and lock, on a skirmish.
#include "oa/app/runtime.hpp"
#include "touch_state.hpp"
#include "oa/sim/ground_orders/orders.hpp"
#include "oa/sim/simulation_state.hpp"
#include "oa/ui/hud/status_panel.hpp"
#include "oa/ui/key_bindings/chord.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace fs = std::filesystem;

/// The path `--snapshot` writes for one window size.
///
/// @param snapshot the --snapshot path
/// @param size the window, such as 640x480
/// @return the snapshot's path
fs::path size_snapshot(const fs::path& snapshot, std::string_view size) {
    return snapshot.parent_path() / (snapshot.stem().string() + '-' + std::string(size) + ".ppm");
}

} // namespace

void Runtime::check_key_latches() {
    namespace hud = oa::ui::touch_hud;
    namespace keys = oa::ui::key_bindings;
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("key latches: " + what);
    };
    if (sdl_.window == nullptr || sdl_.renderer == nullptr)
        fail("needs the window");

    // A previous run of this check must not leave Sticky modifiers on.
    const auto set_sticky = [&](bool on) {
        auto chosen = engine_settings();
        chosen.sticky_modifiers = on;
        apply_engine_settings(chosen);
    };
    set_sticky(false);

    start_benchmark_skirmish();
    if (!selected_tnt_)
        fail("the match has no map");
    auto& slots = match_->world().slots;
    uint16_t commander = 0;
    for (const auto& slot : slots) {
        if (slot.unit_index == 0 || slot.unit == nullptr || slot.record.type_index == 0 ||
            slot.record.owner_index != match_local_player_)
            continue;
        if (const auto* def = definition_for(slot.unit_index); def != nullptr && def->can_dgun) {
            commander = slot.unit_index;
            break;
        }
    }
    if (commander == 0)
        fail("found no local commander");

    bool running = true;
    const auto key_event = [&](bool down, SDL_Keycode code, SDL_Scancode scan) {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.windowID = SDL_GetWindowID(sdl_.window);
        event.key.key = code;
        event.key.scancode = scan;
        event.key.mod = SDL_KMOD_NONE;
        event.key.down = down;
        dispatch_event(event, running);
        if (!running)
            fail("a key ended the run");
    };
    const auto tap = [&](SDL_Keycode code, SDL_Scancode scan) {
        key_event(true, code, scan);
        key_event(false, code, scan);
    };
    const auto send_pointer = [&](SDL_EventType type, uint8_t button, float x, float y) {
        float window_x = x;
        float window_y = y;
        if (!frame_to_window(sdl_.renderer, x, y, &window_x, &window_y))
            fail(std::string(SDL_GetError()));
        SDL_Event event{};
        event.type = type;
        if (type == SDL_EVENT_MOUSE_MOTION) {
            event.motion.windowID = SDL_GetWindowID(sdl_.window);
            event.motion.x = window_x;
            event.motion.y = window_y;
        } else {
            event.button.windowID = SDL_GetWindowID(sdl_.window);
            event.button.button = button;
            event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.clicks = 1;
            event.button.x = window_x;
            event.button.y = window_y;
        }
        dispatch_event(event, running);
        if (!running)
            fail("a pointer event ended the run");
    };
    const auto click_at = [&](uint8_t button, float x, float y) {
        send_pointer(SDL_EVENT_MOUSE_MOTION, 0, x, y);
        send_pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, button, x, y);
        send_pointer(SDL_EVENT_MOUSE_BUTTON_UP, button, x, y);
    };
    const auto drawn = [&] {
        render_match_surface();
        return key_latch_chips_drawn_;
    };
    const auto has_chip = [](const std::vector<std::string>& texts, std::string_view wanted) {
        for (const std::string& text : texts)
            if (text == wanted)
                return true;
        return false;
    };
    const auto listed = [](const std::vector<std::string>& texts) {
        std::string found;
        for (const std::string& text : texts)
            found += " [" + text + "]";
        return found.empty() ? std::string(" none") : found;
    };
    const auto step = [](const char* name) { std::cout << "key latches: " << name << std::endl; };

    const auto home_x = static_cast<int32_t>(slots[commander].unit->position[0] >> 16);
    const auto home_z = static_cast<int32_t>(slots[commander].unit->position[2] >> 16);
    const int32_t toward = home_x < static_cast<int32_t>(selected_tnt_->tile_width * 16U) ? 1 : -1;
    const auto viewport = [&] { return live_viewport(match_camera_x_, match_camera_z_); };
    const auto ground_at = [&](int32_t along, int32_t across) {
        const int32_t world_x = home_x + toward * along;
        const int32_t world_z = home_z + across;
        if (world_x < 0 || world_z < 0)
            fail("ground is off the map");
        const auto x = static_cast<uint32_t>(world_x) << 16;
        const auto z = static_cast<uint32_t>(world_z) << 16;
        const std::array<uint32_t, 3> position{
            x, static_cast<uint32_t>(match_->map_height(x, z)) << 16, z
        };
        const auto screen = project_match_point(viewport(), position);
        const float px = static_cast<float>(screen.x);
        const float py = static_cast<float>(screen.y);
        if (!battlefield_contains(px, py))
            fail("ground is off the battlefield");
        return std::pair{px, py};
    };

    // A skirmish starts on the left-click interface, where a right press
    // clears the selection. These orders are the right-click interface's.
    match_->state().game.interface_type = oa::sim::gameplay_input::interface_right_click;

    clear_local_selection();
    center_camera_on_unit(commander);
    render_match_surface();
    const auto commander_screen = project_match_point(viewport(), slots[commander].unit->position);
    const float unit_x = static_cast<float>(commander_screen.x);
    const float unit_y = static_cast<float>(commander_screen.y);
    if (!battlefield_contains(unit_x, unit_y))
        fail("the commander is off the battlefield");
    click_at(match_select_button(), unit_x, unit_y);
    if (selected_match_unit_ != commander)
        fail("the click did not select the commander");

    step("tap Shift latches QUEUE");
    tap(SDLK_LSHIFT, SDL_SCANCODE_LSHIFT);
    if (touch_state_if_made() == nullptr || !touch_state().hud.latches.latched(hud::Latch::queue))
        fail("Shift did not latch QUEUE");
    {
        const std::vector<std::string> texts = drawn();
        if (!has_chip(texts, "QUEUE · latched"))
            fail("QUEUE · latched was not drawn:" + listed(texts));
    }

    step("two queued moves");
    // A short step in world pixels still sits under the commander's sprite, and
    // a press there selects instead of moving. These are canvas pixels from
    // the unit, the same reach a pointer check uses for open ground, and the
    // two points stay further apart than the queued-order cancel.
    const auto queue_of = [&] {
        std::vector<oa::sim::match_runtime::Match::QueuedCommandView> queued;
        match_->visit_primary_queue(commander, [&](const auto& entry) { queued.push_back(entry); });
        return queued;
    };
    const auto find_ground = [&](float down) {
        const float reaches[] = {160.0F, -160.0F, 240.0F, -240.0F, 320.0F, -320.0F};
        for (const float reach : reaches) {
            const float x = unit_x + reach;
            const float y = unit_y + down;
            if (!battlefield_contains(x, y))
                continue;
            update_pointer(x, y);
            if (hovered_match_unit_ == 0 && match_world_point(x, y))
                return std::pair{x, y};
        }
        fail("found no open ground beside the commander");
        return std::pair{unit_x, unit_y};
    };
    const auto before = queue_of();
    const auto near_point = find_ground(0.0F);
    const auto far_point = find_ground(96.0F);
    if (near_point == far_point)
        fail("the two order points are the same");
    click_at(match_command_button(), near_point.first, near_point.second);
    click_at(match_command_button(), far_point.first, far_point.second);
    const auto after = queue_of();
    if (after.size() != before.size() + 2)
        fail(
            "the commander has " + std::to_string(after.size()) + " orders after " +
            std::to_string(before.size())
        );
    for (std::size_t index = before.size(); index < after.size(); ++index) {
        if (after[index].kind != oa::sim::ground_orders::move_ground_kind)
            fail("queued order kind " + std::to_string(after[index].kind));
    }
    if ((slots[commander].unit->flags & OA_UNIT_FLAG_SELECTED) == 0)
        fail("the queued moves dropped the commander");
    if (!touch_state().hud.latches.latched(hud::Latch::queue))
        fail("the queued moves let QUEUE go");

    step("tap Shift lets QUEUE go");
    tap(SDLK_LSHIFT, SDL_SCANCODE_LSHIFT);
    if (touch_state().hud.latches.latched(hud::Latch::queue))
        fail("the second Shift tap left QUEUE latched");
    {
        const std::vector<std::string> texts = drawn();
        if (has_chip(texts, "QUEUE · latched"))
            fail("QUEUE · latched was still drawn");
    }

    const auto settle_strip = [&](int32_t goal) {
        int32_t& offset = match_->state().game.status_panel_offset;
        int frames = 0;
        while (offset != goal && frames < 200) {
            SDL_Delay(oa::ui::hud::kStatusPanelStepMs + 1);
            draw_status_panel();
            ++frames;
        }
        if (offset != goal)
            fail("the status strip stopped at offset " + std::to_string(offset));
    };
    step("tap Space shows the status strip");
    tap(SDLK_SPACE, SDL_SCANCODE_SPACE);
    if (!action_latched("game.status-strip"))
        fail("Space did not latch the status strip");
    settle_strip(-oa::ui::hud::kStatusPanelRise);
    step("tap Space hides the status strip");
    tap(SDLK_SPACE, SDL_SCANCODE_SPACE);
    if (action_latched("game.status-strip"))
        fail("the second Space tap left the status strip latched");
    settle_strip(0);

    const auto second_type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "CORAK");
    if (second_type == 0)
        fail("the game lacks CORAK");
    const int32_t spawn_x = home_x + toward * 220;
    const int32_t spawn_z = home_z + 96;
    if (spawn_x < 0 || spawn_z < 0)
        fail("the second unit would be off the map");
    const auto spawn_fixed_x = static_cast<uint32_t>(spawn_x) << 16;
    const auto spawn_fixed_z = static_cast<uint32_t>(spawn_z) << 16;
    oa::sim::unit_spawn::Request request;
    request.player = match_local_player_;
    request.type = second_type;
    request.finished = true;
    request.state = kGroundOccupancyState;
    request.position = {
        spawn_fixed_x,
        static_cast<uint32_t>(match_->map_height(spawn_fixed_x, spawn_fixed_z)) << 16,
        spawn_fixed_z
    };
    auto* spawned = match_->create(request);
    if (spawned == nullptr || spawned->unit == nullptr)
        fail("could not spawn a second unit");
    const uint16_t second = spawned->unit_index;

    step("tap Ctrl then A selects every unit that can be selected");
    set_sticky(true);
    tap(SDLK_LCTRL, SDL_SCANCODE_LCTRL);
    key_event(true, SDLK_A, SDL_SCANCODE_A);
    key_event(false, SDLK_A, SDL_SCANCODE_A);
    // Ctrl+A selects the units that can be selected, as 3.1c does, and leaves
    // the rest of the player's unit list as it is.
    int selectable_units = 0;
    int selected_units = 0;
    const oa::World& world = match_->state();
    for (const auto& slot : slots) {
        if (slot.unit == nullptr || slot.owner_index != match_local_player_)
            continue;
        if (!oa::sim::simulation_state::unit_selectable(world, slot.record))
            continue;
        ++selectable_units;
        if ((slot.unit->flags & OA_UNIT_FLAG_SELECTED) != 0)
            ++selected_units;
    }
    if (selectable_units < 2 || selected_units != selectable_units)
        fail(
            std::to_string(selected_units) + " of " + std::to_string(selectable_units) +
            " selectable units are selected"
        );
    if ((slots[commander].unit->flags & OA_UNIT_FLAG_SELECTED) == 0 ||
        (slots[second].unit->flags & OA_UNIT_FLAG_SELECTED) == 0)
        fail("Ctrl then A left an own unit unselected");
    {
        const std::vector<std::string> texts = drawn();
        if (!texts.empty())
            fail("a chip was left after Ctrl then A:" + listed(texts));
    }
    if (sticky_modifier_bits() != 0)
        fail("sticky Ctrl stayed after A");

    step("tap Alt twice locks");
    tap(SDLK_LALT, SDL_SCANCODE_LALT);
    tap(SDLK_LALT, SDL_SCANCODE_LALT);
    if ((sticky_modifier_bits() & keys::alt) == 0)
        fail("Alt did not lock");
    {
        const std::vector<std::string> texts = drawn();
        if (!has_chip(texts, "ALT · locked"))
            fail("ALT · locked was not drawn:" + listed(texts));
    }
    step("ALT locked across two clicks");
    const auto [lock_x, lock_y] = ground_at(48, 32);
    click_at(match_select_button(), lock_x, lock_y);
    click_at(match_select_button(), lock_x, lock_y);
    if ((sticky_modifier_bits() & keys::alt) == 0)
        fail("the clicks let locked Alt go");
    {
        const std::vector<std::string> texts = drawn();
        if (!has_chip(texts, "ALT · locked"))
            fail("ALT · locked was not drawn after the clicks:" + listed(texts));
    }

    step("focus loss clears latches");
    SDL_Event lost{};
    lost.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    lost.window.windowID = SDL_GetWindowID(sdl_.window);
    dispatch_event(lost, running);
    if (!running)
        fail("focus loss ended the run");
    if (action_latched("game.status-strip"))
        fail("focus loss left the status strip latched");
    if (touch_state_if_made() != nullptr && touch_state().hud.latches.latched(hud::Latch::queue))
        fail("focus loss left QUEUE latched");
    if (sticky_modifier_bits() != 0)
        fail("focus loss left a sticky modifier");
    if (!key_latch_chips_drawn_.empty())
        fail("focus loss left a chip recorded");
    {
        const std::vector<std::string> texts = drawn();
        if (!texts.empty())
            fail("focus loss left a chip drawn:" + listed(texts));
    }
    SDL_Event gained{};
    gained.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    gained.window.windowID = SDL_GetWindowID(sdl_.window);
    dispatch_event(gained, running);
    if (!running)
        fail("focus gain ended the run");

    // The snapshot shows the two latches alone. Sticky modifiers would add
    // a chip for the Shift tap that latches QUEUE.
    set_sticky(false);
    tap(SDLK_LSHIFT, SDL_SCANCODE_LSHIFT);
    // A frame between the taps records QUEUE before the status strip, which
    // is the order the chips stay in.
    render_match_surface();
    tap(SDLK_SPACE, SDL_SCANCODE_SPACE);
    const auto expect_two = [&] {
        const std::vector<std::string> texts = drawn();
        if (texts.size() != 2 || texts[0] != "QUEUE · latched" ||
            texts[1] != "STATUS STRIP · latched")
            fail("the snapshot chips are not QUEUE then STATUS STRIP:" + listed(texts));
    };
    step("snapshot 640x480");
    if (!SDL_SetWindowSize(sdl_.window, 640, 480) || !SDL_SyncWindow(sdl_.window))
        fail(std::string("SDL_SetWindowSize: ") + SDL_GetError());
    apply_output_mode();
    expect_two();
    if (!options_.snapshot.empty()) {
        renderer::Surface frame;
        compose_match_frame(frame);
        write_ppm(size_snapshot(options_.snapshot, "640x480"), frame);
    }
    step("snapshot 1920x1080");
    if (!SDL_SetWindowSize(sdl_.window, 1920, 1080) || !SDL_SyncWindow(sdl_.window))
        fail(std::string("SDL_SetWindowSize: ") + SDL_GetError());
    apply_output_mode();
    expect_two();
    if (!options_.snapshot.empty()) {
        renderer::Surface wide;
        compose_match_frame(wide);
        write_ppm(size_snapshot(options_.snapshot, "1920x1080"), wide);
    }

    set_sticky(false);
}

} // namespace oa::app

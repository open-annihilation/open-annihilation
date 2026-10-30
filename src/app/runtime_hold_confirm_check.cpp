// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Hold to confirm on a skirmish. Self-destruct, the order button and the
// surrender question's Yes wait for the long-press time, and so does Enter
// where it answers Yes, on the question a close request asks. The ring at
// 30% with reduced motion is one quarter.
#include "oa/app/runtime.hpp"

#include "oa/data/defs/layout.hpp"
#include "oa/ui/engine_settings.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
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
    return snapshot.parent_path() / (snapshot.stem().string() + "-" + std::string(size) + ".ppm");
}

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("hold confirm: " + what);
}

/// Reports whether two frames' ring squares are the same pixels.
///
/// @param left one frame
/// @param right the other frame
/// @param width the frame width
/// @param x the square's left
/// @param y the square's top
/// @param side the square's side
/// @return true when every pixel matches
bool same_square(
    const std::vector<uint8_t>& left,
    const std::vector<uint8_t>& right,
    int width,
    int x,
    int y,
    int side
) {
    if (side <= 0 || width <= 0)
        return false;
    for (int row = 0; row < side; ++row) {
        for (int column = 0; column < side; ++column) {
            const int px = x + column;
            const int py = y + row;
            if (px < 0 || py < 0)
                return false;
            const auto at = (static_cast<std::size_t>(py) * static_cast<std::size_t>(width) +
                             static_cast<std::size_t>(px)) *
                            3U;
            if (at + 2 >= left.size() || at + 2 >= right.size())
                return false;
            if (left[at] != right[at] || left[at + 1] != right[at + 1] ||
                left[at + 2] != right[at + 2])
                return false;
        }
    }
    return true;
}

} // namespace

void Runtime::check_hold_confirm() {
    // A local, so it is not the screen's step.
    const auto step = [](std::string_view name) { std::cout << "hold confirm: " << name << '\n'; };
    if (sdl_.window == nullptr || sdl_.renderer == nullptr)
        fail("needs the window");

    const auto set_hold = [&](bool on) {
        auto chosen = engine_settings();
        chosen.hold_to_confirm = on;
        chosen.long_press_ms = 500;
        apply_engine_settings(chosen);
    };
    set_hold(false);

    start_benchmark_skirmish();
    if (match_ == nullptr)
        fail("the match did not start");

    uint16_t commander = 0;
    for (const auto& slot : match_->world().slots) {
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
    clear_local_selection();
    adopt_selection(commander);
    selected_match_unit_ = commander;
    apply_match_hud_for_selection();
    render_match_surface();

    bool running = true;
    const auto key_event = [&](bool down, SDL_Keycode code, SDL_Scancode scan, SDL_Keymod mod) {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.windowID = SDL_GetWindowID(sdl_.window);
        event.key.key = code;
        event.key.scancode = scan;
        event.key.mod = mod;
        event.key.down = down;
        dispatch_event(event, running);
        if (!running)
            fail("a key ended the run");
    };
    const auto ctrl_d = [&](bool down) { key_event(down, SDLK_D, SDL_SCANCODE_D, SDL_KMOD_LCTRL); };
    const auto wait_ms = [](uint32_t milliseconds) {
        const uint64_t began = SDL_GetTicks();
        while (SDL_GetTicks() - began < milliseconds)
            SDL_Delay(1);
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
    const auto area = overlay_area();
    const float pointer_x = static_cast<float>(area.x + area.width / 2);
    const float pointer_y = static_cast<float>(area.y + area.height / 2);
    send_pointer(SDL_EVENT_MOUSE_MOTION, 0, pointer_x, pointer_y);

    const auto remaining = [&] { return match_->self_destruct_remaining(commander); };
    const auto stop_countdown = [&] {
        if (remaining() == 0)
            return;
        ctrl_d(true);
        ctrl_d(false);
        tick_hold_confirm();
        if (remaining() != 0)
            fail("the countdown did not stop");
    };

    step("setting off, Ctrl+D starts the countdown");
    ctrl_d(true);
    ctrl_d(false);
    if (remaining() == 0)
        fail("Ctrl+D with the setting off did not start the countdown");
    render_match_surface();
    if (hold_ring_drawn_)
        fail("the setting off drew a ring");
    stop_countdown();

    set_hold(true);
    step("Ctrl+D held 300 ms leaves no countdown and no ring");
    ctrl_d(true);
    wait_ms(300);
    ctrl_d(false);
    tick_hold_confirm();
    if (remaining() != 0)
        fail("letting go at 300 ms started the countdown");
    render_match_surface();
    if (hold_ring_drawn_)
        fail("letting go at 300 ms left a ring");

    step("ring at the pointer at 250 ms");
    ctrl_d(true);
    wait_ms(250);
    // The fill is read before the frame is drawn. Drawing the world takes
    // longer than the wait, and the ring would otherwise be past half.
    const HoldConfirmDraw half = hold_confirm_draw();
    if (!half.show || half.progress < 400 || half.progress > 700)
        fail("the ring at 250 ms was not about half filled");
    render_match_surface();
    if (!hold_ring_drawn_ || hold_ring_side_ <= 0)
        fail("the ring was not drawn at 250 ms");
    if (remaining() != 0)
        fail("the countdown started before the long press");

    step("held 600 ms starts the countdown while held");
    wait_ms(400);
    tick_hold_confirm();
    if (remaining() == 0)
        fail("holding 600 ms did not start the countdown");
    ctrl_d(false);
    if (remaining() == 0)
        fail("letting go after the countdown started stopped it");

    step("Ctrl+D again stops the countdown at once");
    ctrl_d(true);
    if (remaining() != 0)
        fail("Ctrl+D did not stop the countdown at once");
    ctrl_d(false);
    tick_hold_confirm();

    // Button names carry the side prefix (ARMORDERS, ARMSELFD). The action
    // is the name with that prefix removed.
    const auto find_action = [&](std::string_view action) -> std::optional<std::size_t> {
        if (!match_hud_)
            return std::nullopt;
        const auto& gadgets = match_hud_->layout.gadgets;
        for (std::size_t index = 1; index < gadgets.size(); ++index) {
            if (match_hud_action(gadgets[index].common.name) == action)
                return index;
        }
        return std::nullopt;
    };
    const auto find_selfd = [&]() -> std::optional<std::size_t> {
        if (const auto selfd = find_action("SELFD"))
            return selfd;
        return find_action("DESTRUCT");
    };
    // A commander's page may open on its build menu. ORDERS shows the orders.
    if (!find_selfd()) {
        if (const auto orders = find_action("ORDERS")) {
            const auto orders_at = hud_gadget_centre(*orders);
            send_pointer(
                SDL_EVENT_MOUSE_MOTION,
                0,
                static_cast<float>(orders_at.x),
                static_cast<float>(orders_at.y)
            );
            send_pointer(
                SDL_EVENT_MOUSE_BUTTON_DOWN,
                match_select_button(),
                static_cast<float>(orders_at.x),
                static_cast<float>(orders_at.y)
            );
            send_pointer(
                SDL_EVENT_MOUSE_BUTTON_UP,
                match_select_button(),
                static_cast<float>(orders_at.x),
                static_cast<float>(orders_at.y)
            );
        }
    }
    // Stock ARMGEN.GUI has no self-destruct button. A mod names one SELFD or
    // DESTRUCT, and that name is what the press holds. STOP takes the name
    // here so the click goes through that branch.
    if (!find_selfd()) {
        if (const auto stop = find_action("STOP"))
            match_hud_->layout.gadgets[*stop].common.name = "ARMSELFD";
    }
    const auto selfd = find_selfd();
    if (!selfd)
        fail("the order panel has no self-destruct button");
    const auto centre = hud_gadget_centre(*selfd);
    const auto click_selfd = [&](uint32_t milliseconds) {
        send_pointer(
            SDL_EVENT_MOUSE_MOTION, 0, static_cast<float>(centre.x), static_cast<float>(centre.y)
        );
        send_pointer(
            SDL_EVENT_MOUSE_BUTTON_DOWN,
            match_select_button(),
            static_cast<float>(centre.x),
            static_cast<float>(centre.y)
        );
        wait_ms(milliseconds);
        if (milliseconds >= 500)
            tick_hold_confirm();
        send_pointer(
            SDL_EVENT_MOUSE_BUTTON_UP,
            match_select_button(),
            static_cast<float>(centre.x),
            static_cast<float>(centre.y)
        );
        tick_hold_confirm();
    };

    step("SELFD clicked 100 ms does not start it");
    click_selfd(100);
    if (remaining() != 0)
        fail("a 100 ms click started the countdown");

    step("SELFD held 600 ms starts it");
    click_selfd(600);
    if (remaining() == 0)
        fail("holding SELFD for 600 ms did not start the countdown");
    stop_countdown();

    step("reduced motion ring at 30% draws one quarter");
    // The SELFD click left the pointer on the order panel. The ring is drawn
    // at the pointer, on the battlefield.
    send_pointer(SDL_EVENT_MOUSE_MOTION, 0, pointer_x, pointer_y);
    hold_confirm_reduced_motion_ = false;
    cancel_hold_confirm();
    render_match_surface();
    hold_confirm_reduced_motion_ = true;
    ctrl_d(true);
    replay_hold_confirm(150);
    // The press may have drawn the world again. Both rings blend onto this copy.
    paint_on(PaintLayer::battlefield);
    const std::vector<uint8_t> background = match_world_cpu_.rgb;
    const int frame_width = static_cast<int>(match_world_cpu_.width);
    draw_hold_ring();
    if (!hold_ring_drawn_ || !hold_ring_reduced_ || hold_ring_whole_ <= 0)
        fail("the reduced ring was not drawn");
    if (hold_ring_done_ * 4 / hold_ring_whole_ != 1)
        fail("30% did not snap to one quarter");
    const int ring_x = hold_ring_x_;
    const int ring_y = hold_ring_y_;
    const int ring_side = hold_ring_side_;
    const std::vector<uint8_t> reduced = match_world_cpu_.rgb;
    match_world_cpu_.rgb = background;
    hold_confirm_reduced_motion_ = false;
    replay_hold_confirm(125);
    draw_hold_ring();
    const std::vector<uint8_t> quarter = match_world_cpu_.rgb;
    if (!same_square(reduced, quarter, frame_width, ring_x, ring_y, ring_side))
        fail("the reduced ring at 30% was not the quarter ring");
    match_world_cpu_.rgb = background;
    replay_hold_confirm(150);
    draw_hold_ring();
    if (same_square(reduced, match_world_cpu_.rgb, frame_width, ring_x, ring_y, ring_side))
        fail("30% without reduced motion matched the quarter ring");
    hold_confirm_reduced_motion_ = false;
    ctrl_d(false);
    cancel_hold_confirm();
    if (remaining() != 0)
        fail("the ring check started the countdown");

    step("snapshot 640x480");
    const auto shoot = [&](int width, int height, std::string_view size) {
        if (!SDL_SetWindowSize(sdl_.window, width, height) || !SDL_SyncWindow(sdl_.window))
            fail(std::string("SDL_SetWindowSize: ") + SDL_GetError());
        apply_output_mode();
        // The world is drawn first. The ring is then placed at half, so the
        // time spent drawing the world does not fill it further.
        render_match_surface();
        ctrl_d(true);
        replay_hold_confirm(250);
        paint_on(PaintLayer::battlefield);
        draw_hold_ring();
        if (!hold_ring_drawn_)
            fail(std::string(size) + " did not draw the ring");
        if (hold_ring_done_ < 400 || hold_ring_done_ > 700)
            fail(std::string(size) + " was not about half filled");
        if (!options_.snapshot.empty()) {
            renderer::Surface frame;
            compose_match_frame(frame);
            write_ppm(size_snapshot(options_.snapshot, size), frame);
        }
        ctrl_d(false);
        cancel_hold_confirm();
    };
    shoot(640, 480, "640x480");
    step("snapshot 1280x720");
    shoot(1280, 720, "1280x720");
    step("snapshot 1920x1080");
    shoot(1920, 1080, "1920x1080");

    const auto question = oa::data::defs::gui_path("YESORNO.GUI");
    const auto open_question = [&] {
        if (screen_ != Screen::match)
            fail("the surrender question was not asked over the match");
        show_match_pause_menu();
        activate_pause_gadget("EXIT");
        activate_pause_gadget("MAINMENU");
        if (match_hud_panel_ != question)
            fail("MAINMENU did not ask to surrender");
    };

    step("Enter answers no at once");
    open_question();
    key_event(true, SDLK_RETURN, SDL_SCANCODE_RETURN, SDL_KMOD_NONE);
    key_event(false, SDLK_RETURN, SDL_SCANCODE_RETURN, SDL_KMOD_NONE);
    if (match_ == nullptr || exit_requested_ || match_hud_panel_ == question)
        fail("Enter did not answer No at once");

    step("Y tapped leaves the match running");
    open_question();
    key_event(true, SDLK_Y, SDL_SCANCODE_Y, SDL_KMOD_NONE);
    key_event(false, SDLK_Y, SDL_SCANCODE_Y, SDL_KMOD_NONE);
    tick_hold_confirm();
    if (match_ == nullptr || screen_ != Screen::match || exit_requested_)
        fail("a tapped Y left the match");

    step("Enter tapped on a close request's question leaves the match running");
    request_match_close();
    if (match_hud_panel_ != question)
        fail("the close request did not ask to surrender");
    key_event(true, SDLK_RETURN, SDL_SCANCODE_RETURN, SDL_KMOD_NONE);
    key_event(false, SDLK_RETURN, SDL_SCANCODE_RETURN, SDL_KMOD_NONE);
    tick_hold_confirm();
    if (match_ == nullptr || exit_requested_ || match_hud_panel_ != question)
        fail("a tapped Enter answered the close request");

    step("Y held 600 ms returns to the main menu");
    open_question();
    key_event(true, SDLK_Y, SDL_SCANCODE_Y, SDL_KMOD_NONE);
    wait_ms(600);
    tick_hold_confirm();
    if (match_ != nullptr || screen_ != Screen::main_menu)
        fail("holding Y did not return to the main menu");
    key_event(false, SDLK_Y, SDL_SCANCODE_Y, SDL_KMOD_NONE);

    step("Enter held 600 ms on a close request's question ends the run");
    start_benchmark_skirmish();
    if (match_ == nullptr)
        fail("the second match did not start");
    if (match_paused_)
        resume_match_pause();
    request_match_close();
    if (match_hud_panel_ != question)
        fail("the close request did not ask to surrender in the second match");
    key_event(true, SDLK_RETURN, SDL_SCANCODE_RETURN, SDL_KMOD_NONE);
    wait_ms(600);
    tick_hold_confirm();
    if (match_ != nullptr || !exit_requested_)
        fail("holding Enter did not answer the close request as Yes");
}

} // namespace oa::app

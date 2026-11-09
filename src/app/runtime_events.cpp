// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// SDL event dispatch, menu audio and movie playback.
#include "oa/app/runtime.hpp"
#include "oa_layer.hpp"
#include "gui_actions.hpp"
#include "photo_state.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/app/middle_pan.hpp"
#include "oa/app/mcp_quit.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/media/intro_player.hpp"
#include "oa/present/typed_text.hpp"
#include "oa/ui/gui_input/gadget_panel.hpp"
#include "oa/ui/gui_input/tab_order.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>

namespace oa::app {
namespace {

/// Returns the mouse a pointer event comes from.
///
/// @param event a mouse motion or button event
/// @return its `which`: SDL_TOUCH_MOUSEID for one made from a finger
SDL_MouseID event_mouse_id(const SDL_Event& event) noexcept {
    return event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.which : event.button.which;
}

} // namespace

namespace {

/// Tells whether typed text is the character of the quick key that answered a panel.
///
/// @param text the typed text, UTF-8
/// @param answered the quick key, lowercase, or zero for none
/// @return true for that one character, in either case
bool typed_answered_key(const char* text, int32_t answered) {
    return answered != 0 && text != nullptr && text[0] != '\0' && text[1] == '\0' &&
           std::tolower(static_cast<unsigned char>(text[0])) == answered;
}

} // namespace

void Runtime::handle_sdl_event(SDL_Event& event, bool& running) {
    // The character of a key that ended a middle-click pan types nothing.
    if (middle_pan_drop_text_) {
        if (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_TEXT_EDITING) {
            middle_pan_drop_text_ = false;
            return;
        }
        middle_pan_drop_text_ = false;
    }
    // Each key press forgets the quick key the last one answered a panel
    // with; press_match_panel_key records the press's own again.
    if (event.type == SDL_EVENT_KEY_DOWN)
        answered_key_ = 0;
    if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
        // SIGTERM with nothing the player can answer, or while --mcp-http is
        // served, leaves as game.quit does. The window's close button still
        // asks. A shown window without --mcp-http asks too.
        if (event.type == SDL_EVENT_QUIT) {
            const char* driver = SDL_GetCurrentVideoDriver();
            const bool hidden = sdl_.window != nullptr &&
                                (SDL_GetWindowFlags(sdl_.window) & SDL_WINDOW_HIDDEN) != 0;
            const bool shown = mcp::window_shown_to_player(
                sdl_.window != nullptr,
                options_.headless_check,
                hidden,
                driver != nullptr ? std::string_view(driver) : std::string_view{}
            );
            if (mcp::system_quit_leaves(shown, mcp::http_serving())) {
                quit_requested_ = true;
                quit_reason_.clear();
                running = false;
                return;
            }
        }
        // Closing the window, or the system's quit while there is one, in a
        // running match asks whether to surrender first, as in 3.1c; every
        // other screen ends the run at once.
        const bool asked = event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED || sdl_.window != nullptr;
        // The extension answers first; one that declines leaves the request
        // to the engine.
        if (asked && call_hook_or_raise<&Extension::close_requested>(extension_, *this))
            return;
        // A page opened over the running match (its load and save pages, its
        // briefing) goes back to the match to ask there; the preferences a
        // match opens stay on it.
        if (asked && match_ && !match_finished_ &&
            (screen_ == Screen::match || return_to_match_for_close())) {
            request_match_close();
            return;
        }
        running = false;
        return;
    }
    // Entering or leaving full screen lays the screen out again at the size
    // the window ends at, which some window systems report only then; so
    // does a change of the window's safe area.
    if (event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED ||
        event.type == SDL_EVENT_WINDOW_RESIZED || event.type == SDL_EVENT_WINDOW_ENTER_FULLSCREEN ||
        event.type == SDL_EVENT_WINDOW_LEAVE_FULLSCREEN ||
        event.type == SDL_EVENT_WINDOW_SAFE_AREA_CHANGED) {
        apply_output_mode();
        if (screen_ == Screen::match && match_ && selected_tnt_)
            render_match_surface();
        return;
    }
    // The pointer outside the window has no place on the screen until it
    // comes back. A soft wall puts it back on the edge it crossed, and the
    // view keeps scrolling from that last pixel.
    if (event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE && !(sticky_soft_wall() && sticky_edge_.holding))
        match_pointer_known_ = false;
    // Leaving the window ends a middle-click pan. The focus event itself
    // still reaches the window.
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        if (photo_ != nullptr)
            photo_->dragging = false;
        std::ignore = middle_pan_take(event);
    }
    if (live_renderer_take_event(event))
        return;
    // The character a quick key types after its press answered a panel over
    // the match goes nowhere, though the panel is closed by then; while the
    // surrender confirmation is up, typing reaches neither the chat line nor
    // a marker's text.
    if (event.type == SDL_EVENT_TEXT_INPUT &&
        (typed_answered_key(event.text.text, std::exchange(answered_key_, 0)) ||
         match_question_open()))
        return;
    if (event.type == SDL_EVENT_TEXT_EDITING && match_question_open())
        return;
    if (whiteboard_text(event))
        return;
    if (event.type == SDL_EVENT_TEXT_INPUT && chat_composing_) {
        chat_buffer_ +=
            oa::present::typed_characters(event.text.text, oa::present::TypedCharacters::text);
        chat_composition_.clear();
        return;
    }
    // The input method's composition shows after the chat line until it is
    // committed, as the line will be sent.
    if (event.type == SDL_EVENT_TEXT_EDITING && chat_composing_) {
        chat_composition_ = text_composition_;
        return;
    }
    // The save dialog shows it at the end of the name being typed, and the
    // battle room at the end of its chat line.
    if (event.type == SDL_EVENT_TEXT_EDITING && save_dialog_open()) {
        compose_save_name(text_composition_);
        return;
    }
    if (event.type == SDL_EVENT_TEXT_EDITING &&
        oa::ui::frontend_multiplayer::multiplayer_compose(text_composition_.c_str()))
        return;
    // A key ends a middle-click pan and runs no command.
    if (event.type == SDL_EVENT_KEY_DOWN && middle_pan_take(event))
        return;
    if (event.type == SDL_EVENT_KEY_DOWN && handle_match_hotkey(event.key))
        return;
    if (event.type == SDL_EVENT_KEY_DOWN && typed_key_hook_ != TypedKeyHook::none) {
        const auto key = SDL_GetKeyFromScancode(event.key.scancode, event.key.mod, false);
        if (key >= 0x20 && key < 0x7f)
            record_typed_key(static_cast<uint8_t>(std::toupper(static_cast<int>(key))));
    }
    // A held Enter's repeats press nothing on these screens: the press that
    // opened one may have come from the same key.
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.repeat &&
        (screen_ == Screen::campaign_end || screen_ == Screen::briefing) &&
        event.key.key == SDLK_RETURN)
        return;
    // A check hands a menu key here without the dispatcher's feed. A match key
    // was fed by the hotkey handler just above, and feeding it again would let
    // a latch go.
    if (event.type == SDL_EVENT_KEY_DOWN && screen_ != Screen::match && !match_key_already_fed_)
        feed_input_actions(event);
    if (event.type == SDL_EVENT_KEY_DOWN && answer_menu_keys(event.key))
        return;
    // A turn of the wheel ends a middle-click pan and does not zoom.
    if (event.type == SDL_EVENT_MOUSE_WHEEL && middle_pan_take(event))
        return;
    if (event.type == SDL_EVENT_MOUSE_WHEEL && screen_ == Screen::match) {
        // ui.megamap takes the wheel, which acts only with the Mouse wheel
        // zoom setting off (megamap_on). Photo mode does not open the
        // megamap: the wheel zooms when that setting is on, and does
        // nothing when it is off. The open and close actions name the
        // direction. The wheel is still taken when neither fired, so it
        // does not zoom the battlefield.
        if (megamap_on() && !photo_mode()) {
            if (sdl_.renderer != nullptr && !convert_event_to_frame(sdl_.renderer, event))
                return;
            // One notch is one press. The wheel event opens or closes once.
            const auto wheeled = [&](std::string_view id) {
                for (const oa::ui::key_bindings::ActionEvent& fed : match_action_events()) {
                    if (fed.action == id && fed.phase == oa::ui::key_bindings::Phase::pressed)
                        return true;
                }
                return false;
            };
            if (wheeled("megamap.open-wheel"))
                std::ignore = megamap_wheel(-1.0F, event.wheel.mouse_x, event.wheel.mouse_y);
            else if (wheeled("megamap.close-wheel"))
                std::ignore = megamap_wheel(1.0F, event.wheel.mouse_x, event.wheel.mouse_y);
            return;
        }
        // With the Mouse wheel zoom setting off the wheel does nothing here.
        if (!engine_settings().wheel_zoom || !convert_event_to_frame(sdl_.renderer, event))
            return;
        handle_match_zoom(
            pointer_zoom_amount(event.wheel.y), event.wheel.mouse_x, event.wheel.mouse_y, true
        );
        return;
    }
    if (event.type == SDL_EVENT_MOUSE_WHEEL && !match_key_already_fed_)
        feed_input_actions(event);
    if (event.type == SDL_EVENT_MOUSE_WHEEL && answer_menu_list_wheel(event.wheel.y))
        return;
    if (event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
        event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        // Headless checks have no renderer and send canvas coordinates.
        if (sdl_.renderer != nullptr && !convert_event_to_frame(sdl_.renderer, event)) {
            if (options_.trace_input)
                std::cerr << "input coordinate conversion failed: " << SDL_GetError() << '\n';
            return;
        }
        // Over a scaled frame the pointer in the black bars around it rests
        // on the frame's edge, where it scrolls the view as at the screen's.
        if (screen_ == Screen::match && scaled_frame_width_ > 0 && match_layout_.width > 0 &&
            match_layout_.height > 0) {
            float& pointer_x =
                event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.x : event.button.x;
            float& pointer_y =
                event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.y : event.button.y;
            pointer_x = std::clamp(pointer_x, 0.0F, static_cast<float>(match_layout_.width - 1));
            pointer_y = std::clamp(pointer_y, 0.0F, static_cast<float>(match_layout_.height - 1));
        }
        const float x = event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.x : event.button.x;
        const float y = event.type == SDL_EVENT_MOUSE_MOTION ? event.motion.y : event.button.y;
        // A move to another pixel hides the ring. A press on its own does not.
        if (event.type == SDL_EVENT_MOUSE_MOTION && oa_layer_ != nullptr)
            oa_layer_->note_family_pointer(x, y);
        // Photo mode keeps the pointer off the match. A motion must not
        // pick a hover, and a press must not select, order, drag a box,
        // scroll by the minimap or press the panel. The wheel zoomed
        // above and returned. The place stays unknown, so the edges do
        // not scroll either. A drag on photo.pan grabs the view, and the
        // middle-click pan still answers its own button when the picture
        // is not held.
        if (photo_mode() && screen_ == Screen::match) {
            match_pointer_known_ = false;
            match_pointer_x_ = x;
            match_pointer_y_ = y;
            if (photo_ != nullptr && !photo_picture_held()) {
                const uint8_t pan_button = photo_pan_button();
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && pan_button != 0 &&
                    event.button.button == pan_button) {
                    photo_->dragging = true;
                    photo_->drag_moved = false;
                    photo_->drag_x = x;
                    photo_->drag_y = y;
                } else if (event.type == SDL_EVENT_MOUSE_MOTION && photo_->dragging) {
                    const float dx = x - photo_->drag_x;
                    const float dy = y - photo_->drag_y;
                    const float slop = middle_pan::kClickSlop;
                    if (!photo_->drag_moved && dx * dx + dy * dy > slop * slop)
                        photo_->drag_moved = true;
                    if (photo_->drag_moved) {
                        scroll_match_view_by(-static_cast<double>(dx), -static_cast<double>(dy));
                        photo_->drag_x = x;
                        photo_->drag_y = y;
                    }
                } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && photo_->dragging) {
                    photo_->dragging = false;
                    photo_->drag_moved = false;
                }
                std::ignore = middle_pan_take(event);
            } else if (photo_ != nullptr) {
                photo_->dragging = false;
            }
            return;
        }
        // Where the pointer is on the match's screen is known once SDL
        // reports it there (the screen's edges scroll the camera only then);
        // the events the touch controls make for a finger leave it unknown,
        // since a finger is no pointer resting at an edge.
        match_pointer_known_ =
            screen_ == Screen::match && event_mouse_id(event) != SDL_TOUCH_MOUSEID;
        update_pointer(x, y);
        // A hold's button came up. Remember it before any path returns, so the
        // release does not activate and a later click is not swallowed.
        const bool held_release =
            event.type == SDL_EVENT_MOUSE_BUTTON_UP && take_hold_confirm_release();
        // Select boxes and orders; command looks, clears and orders as the
        // right button does. The interface mode decides what each press means.
        const uint8_t select_button = match_select_button();
        const uint8_t command_button = match_command_button();
        // The release of a press that ended a middle-click pan, before a
        // panel or the whiteboard can take it. A left order is given on
        // the release.
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && middle_pan_swallow_button_ != 0 &&
            event.button.button == middle_pan_swallow_button_) {
            middle_pan_swallow_button_ = 0;
            return;
        }
        // A zoom the wheel eases goes on about the pointer as it moves.
        if (match_pointer_known_ && zoom_focus_.follows_pointer)
            zoom_focus_ = {true, x, y};
        // Only a left press the radar acted on (below) leaves its release
        // nothing to do; any other left press clears that mark.
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == select_button)
            radar_left_press_held_ = false;
        // An open unit info panel takes a press on it, and the release that
        // ends it, before anything under it; the side column takes none.
        if (unit_info_pointer(event, x, y))
            return;
        // The commander placement (setup.commander-warp) takes the pointer
        // first; ui.resource-panel's panel floats over the battlefield and
        // takes it next; ui.build-tools' drag with the snap override key
        // takes a press on an own unit before the battlefield does.
        if (commander_placement_pointer(event, x, y) || resource_panel_pointer(event, x, y) ||
            megamap_pointer(event, x, y) || whiteboard_pointer(event, x, y) ||
            order_drag_pointer(event, x, y) || build_queue_drag_pointer(event, x, y))
            return;
        // After the whiteboard, whose middle button places a dot. A pan's
        // press is consumed here, so it selects nothing and gives no order.
        if (middle_pan_take(event))
            return;
        // A press on a HUD button holds it until either button comes up, on
        // whatever screen; the release acts on a HUD button only when the
        // press was on it.
        std::optional<std::size_t> released_hud;
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP)
            released_hud = std::exchange(match_hud_held_, std::nullopt);
        else if (
            screen_ == Screen::match && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            (event.button.button == select_button || event.button.button == command_button)
        )
            match_hud_held_ = hovered_;
        // A left press on self-destruct, or on the surrender question's Yes,
        // starts a hold. The release then activates nothing. Stopping a
        // countdown stays the release's toggle.
        if (screen_ == Screen::match && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            event.button.button == select_button && hovered_ && match_hud_ &&
            *hovered_ < match_hud_->layout.gadgets.size()) {
            const auto& pressed_gadget = match_hud_->layout.gadgets[*hovered_];
            const std::string_view pressed_name = pressed_gadget.common.name;
            const std::string_view pressed_action = match_hud_action(pressed_name);
            if (!match_paused_ && (pressed_action == "SELFD" || pressed_action == "DESTRUCT"))
                (void)start_self_destruct_hold(0, true, select_button, *hovered_);
            else if (match_question_open() && pressed_name == "CHOICE1")
                (void)start_yes_hold(0, true, select_button, *hovered_);
        }
        // A press on a scroll bar or its arrow, and the release of a held
        // one, belong to the bar alone.
        if (route_scroll_pointer(event, x, y))
            return;
        // Beside the open in-game menu the right button is still the
        // battlefield's, as in 3.1c, and a radar scroll or mouse look its
        // press starts follows the pointer until the button comes up.
        const bool beside_menu = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                                 event.button.button == command_button &&
                                 right_press_beside_menu(x, y);
        const bool pointer_mode =
            match_ && (match_->state().game.mouse_look_active != 0 ||
                       (oa::sim::gameplay_input::pointer_flags(match_->state().game) &
                        oa::sim::gameplay_input::pointer_radar_scroll) != 0);
        if (screen_ == Screen::match && !match_finished_ &&
            (!match_paused_ || beside_menu || pointer_mode)) {
            record_pointer_event(event);
            if (follow_pointer_modes(event))
                return;
        }
        // A press on a placed HUD region or a touch control is not the
        // battlefield's.
        if (screen_ == Screen::match && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            (!match_paused_ || beside_menu) && !match_finished_ && !hovered_ && match_ &&
            !placed_hud_covers(x, y)) {
            if (event.button.button == command_button) {
                // ui.selection-shortcuts takes a right double-click too.
                if (!selection_shortcut_double_click(x, y, event.button.clicks))
                    handle_match_right_press(x, y);
                return;
            }
            // In the right-click interface a left press over the radar
            // moves the view to the point under it at once, then scrolls
            // the view with the pointer until the button comes up, as the
            // left-click interface's right press does.
            namespace input = oa::sim::gameplay_input;
            if (event.button.button == select_button) {
                // The pick is wanted for what it writes into the Game block
                // (the pointer's unit, armed order and ground), which the
                // radar scroll reads; the cursor it returns is not needed.
                std::ignore = pick_match_cursor();
                if (input::start_left_radar_scroll(match_->state().game)) {
                    select_game_cursor(static_cast<uint8_t>(input::OrderCursor::normal));
                    center_camera_on_radar_point(x, y);
                    return;
                }
                // Any other left press on the radar acts as it goes down, as
                // 3.1c's does; its release then does nothing.
                if (radar_contains(x, y)) {
                    radar_left_press_held_ = true;
                    std::ignore = issue_radar_orders(x, y);
                    return;
                }
            }
        }
        if (screen_ == Screen::match && event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            event.button.button == select_button && !match_paused_ && !match_finished_ &&
            match_command_ == MatchCommand::none && !hovered_ && !radar_contains(x, y) &&
            !placed_hud_covers(x, y) && x >= static_cast<float>(match_layout_.left) &&
            y >= static_cast<float>(match_layout_.top) &&
            x < static_cast<float>(match_layout_.left + match_layout_.battlefield_width()) &&
            y < static_cast<float>(match_layout_.top + match_layout_.battlefield_height())) {
            // The press starts the box on the terrain under the
            // pointer, and it is drawn from this frame on.
            if (const auto ground = match_pointer_ground(x, y))
                match_drag_ = MatchDragBox{*ground, *ground, frontend_tick()};
        }
        if (screen_ == Screen::match && event.type == SDL_EVENT_MOUSE_MOTION && match_drag_)
            track_match_drag();
        if (screen_ == Screen::match && event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == select_button) {
            if (match_finished_ || std::exchange(radar_left_press_held_, false)) {
                match_drag_.reset();
                return;
            }
            if (match_drag_ && !match_drag_is_click()) {
                const bool area_order = match_command_ == MatchCommand::attack ||
                                        match_command_ == MatchCommand::dgun ||
                                        match_command_ == MatchCommand::reclaim ||
                                        match_command_ == MatchCommand::repair;
                note_shared_latch_used(area_order ? 1 : 0);
                consume_sticky_modifiers();
                const bool add = (input_modifiers(ModifierUse::selection) & SDL_KMOD_SHIFT) != 0;
                const auto [from, to] =
                    match_drag_corners(live_viewport(match_camera_x_, match_camera_z_));
                match_drag_.reset();
                if (match_command_ == MatchCommand::attack || match_command_ == MatchCommand::dgun)
                    area_order_units(from.x, from.y, to.x, to.y, "attack");
                else if (match_command_ == MatchCommand::reclaim)
                    area_order_units(from.x, from.y, to.x, to.y, "reclaim");
                else if (match_command_ == MatchCommand::repair)
                    area_order_units(from.x, from.y, to.x, to.y, "repair");
                else
                    box_select_units(from.x, from.y, to.x, to.y, add);
                return;
            }
            match_drag_.reset();
            if (hovered_ && match_hud_) {
                if (released_hud == hovered_ && !held_release)
                    activate_match_hud(*hovered_);
                return;
            }
            if (match_paused_)
                return;
            handle_match_left_click(x, y, event.button.clicks);
            return;
        }
        if (screen_ == Screen::match && event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            event.button.button == command_button) {
            if (match_paused_ || match_finished_)
                return;
            // The right button on a unit or weapon build button is the same
            // click with the last message 2: it takes one off
            // the queue.
            if (hovered_ && released_hud == hovered_ && match_hud_ &&
                *hovered_ < match_hud_->layout.gadgets.size()) {
                constexpr auto build_buttons =
                    oa::ui::hud::kCommonUnitButton | oa::ui::hud::kCommonWeaponButton;
                const auto attributes = static_cast<uint8_t>(
                    match_hud_->layout.gadgets[*hovered_].common.common_attributes
                );
                if ((attributes & build_buttons) != 0) {
                    activate_match_hud(*hovered_, false);
                    return;
                }
            }
            return;
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
            (event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_RIGHT)) {
            event_button_ = event.button.button == SDL_BUTTON_LEFT ? 1 : 2;
            selected_ = hovered_ && frontend_gadget_pressable(*hovered_)
                            ? static_cast<int32_t>(*hovered_)
                            : -1;
            // A press on a frontend list gives it the keyboard focus.
            if (screen_ != Screen::match && hovered_ &&
                *hovered_ < resources_.layout.gadgets.size() &&
                resources_.layout.gadgets[*hovered_].common.type ==
                    oa::ui::gui_layout::GadgetType::list_box)
                frontend_focus_ = static_cast<int32_t>(*hovered_);
            if (options_.trace_input)
                std::cerr << "input down button=" << static_cast<int>(event.button.button)
                          << " selected=" << selected_ << '\n';
        }
        if (event.type == SDL_EVENT_MOUSE_BUTTON_UP &&
            (event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_RIGHT)) {
            const auto released = hovered_;
            if (options_.trace_input)
                std::cerr << "input up button=" << static_cast<int>(event.button.button)
                          << " released=" << (released ? std::to_string(*released) : "none")
                          << " selected=" << selected_ << '\n';
            if (released && selected_ == static_cast<int32_t>(*released)) {
                const auto& released_name = resources_.layout.gadgets[*released].common.name;
                if (screen_ == Screen::map_selection && released_name == "MAPNAMES") {
                    select_map_row_at(y);
                    if (event.button.clicks >= 2) {
                        activate();
                        close_map_modal();
                    }
                } else if (
                    (screen_ == Screen::new_campaign || screen_ == Screen::any_mission) &&
                    (released_name == "Campaign" || released_name == "Missions")
                ) {
                    // A press on a NEWGAME.GUI list selects the row under it
                    // and gives the list the focus. A double-click on a row
                    // of the list that stands for Start, Campaign for a new
                    // campaign and Missions for any mission, starts it.
                    campaign_setup_focus_ = released_name;
                    const bool picked = select_campaign_list_row(released_name, y);
                    const auto* start_list =
                        screen_ == Screen::new_campaign ? "Campaign" : "Missions";
                    if (picked && released_name == start_list &&
                        event.button.button == SDL_BUTTON_LEFT && event.button.clicks >= 2)
                        start_campaign_setup();
                } else if (screen_ == Screen::campaign_end && released_name == "Missions") {
                    // ENDMSN starts the clicked mission, as Start does.
                    select_campaign_list_row(released_name, y);
                    activate();
                } else {
                    click_selected_frontend_gadget();
                }
            }
            selected_ = -1;
        }
    }
    if (exit_requested_)
        running = false;
}

void Runtime::click_selected_frontend_gadget() {
    const auto& name = resources_.layout.gadgets[static_cast<std::size_t>(selected_)].common.name;
    const bool close_after =
        screen_ == Screen::map_selection && (name == "LOAD" || name == "PREVMENU");
    activate();
    if (close_after)
        close_map_modal();
}

bool Runtime::answer_menu_keys(const SDL_KeyboardEvent& key) {
    namespace gui = oa::ui::gui_input;
    namespace bindings = oa::ui::key_bindings;
    const bindings::ActionEvent* accept = fired_action("ui.menu-accept");
    const bindings::ActionEvent* cancel = fired_action("ui.menu-cancel");
    // While the save dialog's name takes typing only a typed line's keys are
    // searched, so its Escape comes as the line's cancel.
    if (cancel == nullptr && save_dialog_open())
        cancel = fired_action("chat.cancel");
    const bindings::ActionEvent* press = fired_action("ui.menu-press");
    const bool accept_down = accept != nullptr && accept->phase == bindings::Phase::pressed;
    const bool press_down = press != nullptr && press->phase == bindings::Phase::pressed;
    if (screen_ == Screen::campaign_end && accept_down) {
        activate_end_panel_default();
        return true;
    }
    if (screen_ == Screen::briefing) {
        if (accept_down && press_briefing_default(false))
            return true;
        if (cancel != nullptr && press_briefing_default(true))
            return true;
    }
    // Page Up and Page Down turn a briefing's pages, and move the campaign
    // end's mission list, whichever control is focused. The chords are the
    // screen actions, so a player's layout can move them.
    const bool screen_page_down = fired_action("ui.screen-page-down") != nullptr;
    const bool screen_page_up = fired_action("ui.screen-page-up") != nullptr;
    if (frontend_has_keyboard() && (screen_page_up || screen_page_down)) {
        if (screen_ == Screen::briefing) {
            page_briefing_text(screen_page_down);
            return true;
        }
        if (screen_ == Screen::campaign_end) {
            page_end_mission_list(screen_page_down);
            return true;
        }
    }
    // Up and Down move the selection of a new campaign's focused list.
    if ((screen_ == Screen::new_campaign || screen_ == Screen::any_mission) &&
        (fired_action("ui.menu-list-up") != nullptr ||
         fired_action("ui.menu-list-down") != nullptr)) {
        step_campaign_list(fired_action("ui.menu-list-down") != nullptr);
        return true;
    }
    // Up and Down move the map list while it holds the focus; from a button
    // they move the focus, below.
    if (screen_ == Screen::map_selection && frontend_has_keyboard() &&
        (fired_action("ui.menu-list-up") != nullptr ||
         fired_action("ui.menu-list-down") != nullptr) &&
        step_map_list(fired_action("ui.menu-list-down") != nullptr))
        return true;
    if (frontend_has_keyboard()) {
        const bool tab_next = fired_action("ui.menu-focus-next") != nullptr;
        const bool tab_previous = fired_action("ui.menu-focus-previous") != nullptr;
        if (tab_next || tab_previous) {
            // Tab and Shift+Tab show the ring. The main menu keeps the order
            // it has today. Every other screen follows the shared order.
            if (oa_layer_ != nullptr)
                oa_layer_->note_family_tab();
            if (screen_ == Screen::main_menu) {
                move_frontend_focus(
                    tab_previous ? gui::FocusDirection::previous : gui::FocusDirection::next
                );
            } else {
                move_frontend_tab(tab_next);
            }
            rebuild_surface();
            return true;
        }
        int32_t menu_key = 0;
        if (fired_action("ui.menu-focus-left") != nullptr)
            menu_key = gui::key_code::left;
        else if (fired_action("ui.menu-focus-right") != nullptr)
            menu_key = gui::key_code::right;
        else if (
            fired_action("ui.menu-focus-up") != nullptr ||
            fired_action("ui.menu-list-up") != nullptr
        )
            menu_key = gui::key_code::up;
        else if (
            fired_action("ui.menu-focus-down") != nullptr ||
            fired_action("ui.menu-list-down") != nullptr
        )
            menu_key = gui::key_code::down;
        else if (fired_action("ui.screen-page-up") != nullptr)
            menu_key = gui::key_code::page_up;
        else if (fired_action("ui.screen-page-down") != nullptr)
            menu_key = gui::key_code::page_down;
        else if (fired_action("ui.screen-home") != nullptr)
            menu_key = gui::key_code::home;
        else if (fired_action("ui.screen-end") != nullptr)
            menu_key = gui::key_code::end;
        const auto focus = frontend_focus();
        const auto& gadgets = resources_.layout.gadgets;
        const oa::ui::gui_layout::Gadget* focused = nullptr;
        if (focus > 0 && static_cast<std::size_t>(focus) < gadgets.size())
            focused = &gadgets[static_cast<std::size_t>(focus)];
        const bool ring = oa_layer_ != nullptr && oa_layer_->family_ring_shown();
        gui::FocusKeyQuery query;
        query.gadget = focused;
        query.key = menu_key;
        query.ring_shown = ring;
        const gui::FocusKeyEffect effect =
            menu_key != 0 ? gui::focus_key_action(query) : gui::FocusKeyEffect::unhandled;
        switch (effect) {
        case gui::FocusKeyEffect::scroll_back:
            nudge_focused_scroll(0);
            return true;
        case gui::FocusKeyEffect::scroll_forward:
            nudge_focused_scroll(1);
            return true;
        case gui::FocusKeyEffect::scroll_page_back:
            nudge_focused_scroll(2);
            return true;
        case gui::FocusKeyEffect::scroll_page_forward:
            nudge_focused_scroll(3);
            return true;
        case gui::FocusKeyEffect::scroll_home:
            nudge_focused_scroll(4);
            return true;
        case gui::FocusKeyEffect::scroll_end:
            nudge_focused_scroll(5);
            return true;
        case gui::FocusKeyEffect::list_previous:
        case gui::FocusKeyEffect::list_next:
            if (focused != nullptr && tdf_names_equal(focused->common.name, "GAMES")) {
                std::ignore = step_load_game_list(effect == gui::FocusKeyEffect::list_next);
            } else if (focused != nullptr) {
                std::ignore = step_frontend_list_row(
                    focused->common.name, effect == gui::FocusKeyEffect::list_next
                );
                rebuild_surface();
            }
            return true;
        case gui::FocusKeyEffect::colour_forward:
        case gui::FocusKeyEffect::colour_back:
            if (focus > 0) {
                const auto kept = frontend_focus_;
                press_frontend_button(
                    static_cast<std::size_t>(focus),
                    effect == gui::FocusKeyEffect::colour_forward ? 1 : 2
                );
                if (screen_ == Screen::skirmish && frontend_focus_ != kept)
                    set_frontend_focus(kept);
            }
            return true;
        default:
            break;
        }
        const bool from_button = focus < 0 || focused == nullptr ||
                                 focused->common.type == oa::ui::gui_layout::GadgetType::button;
        std::optional<gui::FocusDirection> direction;
        if (from_button && fired_action("ui.menu-focus-left") != nullptr)
            direction = gui::FocusDirection::previous;
        else if (from_button && fired_action("ui.menu-focus-right") != nullptr)
            direction = gui::FocusDirection::next;
        else if (
            from_button && (fired_action("ui.menu-focus-up") != nullptr ||
                            fired_action("ui.menu-list-up") != nullptr)
        )
            direction = gui::FocusDirection::up;
        else if (
            from_button && (fired_action("ui.menu-focus-down") != nullptr ||
                            fired_action("ui.menu-list-down") != nullptr)
        )
            direction = gui::FocusDirection::down;
        if (direction) {
            // An arrow moves the focus and leaves the ring as it is.
            move_frontend_focus(*direction);
            rebuild_surface();
            return true;
        }
        // Return and Space press the focused button; a gamepad's A and Menu
        // send Return. Return on a control that is not a button presses the
        // screen's carriage-return default once the ring is showing.
        if ((accept_down || press_down) && press_frontend_focus_action(accept_down))
            return true;
        if (accept_down && ring && press_panel_enter_default())
            return true;
    }
    if (cancel != nullptr) {
        if (screen_ == Screen::main_menu) {
            // The main menu itself does nothing with Escape, as in 3.1c: EXIT
            // and closing the window end the program. A dialog or notice over
            // the menu takes the key before it gets here.
        } else if (screen_ == Screen::map_selection)
            close_map_modal();
        else if (screen_ == Screen::match) {
            // A finished match leaves for the end screen on its own. Escape
            // closes an open menu; it opens one only with the Escape opens
            // the game menu setting and nothing else to cancel (the match's
            // keys), and a held key's repeats do nothing more.
            if (!match_finished_ && match_paused_ && cancel->phase == bindings::Phase::pressed)
                escape_match_menu();
        } else if (
            screen_ == Screen::options || screen_ == Screen::sound || screen_ == Screen::visuals ||
            screen_ == Screen::speeds || screen_ == Screen::music
        ) {
            leave_options_screen();
        } else if (screen_ == Screen::load_game)
            // As CANCEL does: a dialog opened over a paused match returns to it.
            leave_load_dialog();
        else if (screen_ == Screen::new_campaign || screen_ == Screen::any_mission)
            load(Screen::single_player);
        else if (screen_ == Screen::campaign_end)
            load(Screen::main_menu);
        else
            load(Screen::main_menu);
        return true;
    }
    // Any other key may be the quick key of one of the panel's buttons.
    return frontend_has_keyboard() && press_frontend_gui_action(key);
}

bool Runtime::answer_menu_list_wheel(float wheel_y) {
    if (wheel_y == 0.0F)
        return false;
    const bool up = fired_action("ui.menu-list-up") != nullptr;
    const bool down = fired_action("ui.menu-list-down") != nullptr;
    if (!up && !down)
        return false;
    const int direction = down ? 1 : -1;
    if (screen_ == Screen::map_selection && !bound_map_names_.empty()) {
        const auto next = std::clamp<int32_t>(
            static_cast<int32_t>(modal_map_index_) + direction,
            0,
            static_cast<int32_t>(bound_map_names_.size() - 1U)
        );
        preview_map_index(static_cast<std::size_t>(next));
        return true;
    }
    if (screen_ == Screen::any_mission && !campaign_mission_files_.empty()) {
        const auto next = std::clamp<int32_t>(
            static_cast<int32_t>(selected_mission_index_) + direction,
            0,
            static_cast<int32_t>(campaign_mission_files_.size() - 1U)
        );
        selected_mission_index_ = static_cast<std::size_t>(next);
        if (selected_mission_index_ < campaign_mission_first_visible_)
            campaign_mission_first_visible_ = selected_mission_index_;
        select_frontend_list_row("Missions", selected_mission_index_);
        rebuild_surface();
        return true;
    }
    return false;
}

bool Runtime::press_frontend_gui_action(const SDL_KeyboardEvent& key) {
    namespace bindings = oa::ui::key_bindings;
    const bindings::ActionEvent* gui = nullptr;
    for (const bindings::ActionEvent& fed : match_action_events()) {
        if (fed.phase == bindings::Phase::pressed && fed.action.starts_with("gui.")) {
            gui = &fed;
            break;
        }
    }
    if (gui == nullptr)
        return false;
    const auto note_answered = [&] {
        if (key.key > 0 && key.key < 0x7F)
            answered_key_ = static_cast<int32_t>(std::tolower(static_cast<int>(key.key)));
    };
    const bool plain_letter =
        gui->chord.kind == bindings::TriggerKind::character && gui->chord.modifiers == 0;
    const auto& gadgets = resources_.layout.gadgets;
    if (plain_letter) {
        // A plain letter with Ctrl, Alt or the system key down types no
        // character, and a held key's repeats press nothing more.
        if (key.repeat || (key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0 ||
            key.key == SDLK_UNKNOWN || key.key >= SDLK_DELETE)
            return false;
        const auto typed = std::tolower(static_cast<int>(key.key));
        for (std::size_t index = 1; index < gadgets.size(); ++index) {
            const auto& gadget = gadgets[index];
            const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
            if (gadget.common.active == 0 || button == nullptr || button->grayed_out ||
                button->quick_key == 0 ||
                std::tolower(static_cast<unsigned char>(button->quick_key)) != typed)
                continue;
            note_answered();
            press_frontend_button(index);
            return true;
        }
    }
    // A letter no button shows, and a rebound chord, press the button of the
    // action the key started.
    for (const std::string& stem : menu_gui_stems_) {
        for (std::size_t index = 1; index < gadgets.size(); ++index) {
            const auto& gadget = gadgets[index];
            const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
            if (gadget.common.active == 0 || button == nullptr || button->grayed_out)
                continue;
            if (gui_action_id(stem, gadget.common.name) != gui->action)
                continue;
            note_answered();
            press_frontend_button(index);
            return true;
        }
    }
    return false;
}

bool Runtime::press_frontend_focus_action(bool accept) {
    const auto focus = frontend_focus();
    const bool ring = oa_layer_ != nullptr && oa_layer_->family_ring_shown();
    if (focus < 0) {
        if (!accept)
            return false;
        // While the ring is hidden, Return does what it does today: the
        // panel takes its first focus and nothing is pressed.
        if (!ring) {
            move_frontend_focus(oa::ui::gui_input::FocusDirection::next);
            rebuild_surface();
            return true;
        }
        const int32_t first = announced_frontend_focus();
        if (first < 0)
            return false;
        const auto index = static_cast<std::size_t>(first);
        const auto& gadget = resources_.layout.gadgets[index];
        const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
        if (gadget.common.active == 0 || button == nullptr || button->grayed_out)
            return false;
        // Hide before the press, so a screen the press opens keeps its own rule.
        oa_layer_->note_family_return();
        press_frontend_button(index);
        return true;
    }
    const auto index = static_cast<std::size_t>(focus);
    const auto& gadget = resources_.layout.gadgets[index];
    const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
    if (gadget.common.active == 0 || button == nullptr || button->grayed_out)
        return false;
    // Return hides the ring. Space presses and leaves it shown, as the kit does.
    if (accept && oa_layer_ != nullptr)
        oa_layer_->note_family_return();
    press_frontend_button(index);
    return true;
}

void Runtime::press_frontend_button(std::size_t index, int32_t button) {
    // The screens' handlers read the selected gadget or the one under the
    // pointer; a screen that stays up then has the gadget under the pointer
    // hovered again, with its help.
    const auto pointer_hover = hovered_;
    event_button_ = button;
    selected_ = static_cast<int32_t>(index);
    hovered_ = index;
    click_selected_frontend_gadget();
    selected_ = -1;
    if (hovered_ == index) {
        hovered_ = pointer_hover;
        refresh_help_text();
    }
}

bool Runtime::press_panel_enter_default() {
    const auto& gadgets = resources_.layout.gadgets;
    if (gadgets.empty())
        return false;
    const auto* panel = std::get_if<oa::ui::gui_layout::PanelFields>(&gadgets.front().fields);
    if (panel == nullptr || panel->carriage_return_default.empty())
        return false;
    for (std::size_t index = 1; index < gadgets.size(); ++index) {
        if (!tdf_names_equal(gadgets[index].common.name, panel->carriage_return_default))
            continue;
        if (gadgets[index].common.active == 0)
            return false;
        if (oa_layer_ != nullptr)
            oa_layer_->note_family_return();
        press_frontend_button(index);
        return true;
    }
    return false;
}

bool Runtime::frontend_gadget_pressable(std::size_t index) const {
    if (index >= resources_.layout.gadgets.size())
        return false;
    const auto* button =
        std::get_if<oa::ui::gui_layout::ButtonFields>(&resources_.layout.gadgets[index].fields);
    return button == nullptr || !button->grayed_out;
}

void Runtime::record_typed_key(uint8_t key) {
    std::copy(typed_keys_.begin() + 1, typed_keys_.end(), typed_keys_.begin());
    typed_keys_.back() = key;
    switch (typed_key_hook_) {
    case TypedKeyHook::skirmish_players:
        skirmish::handle_player_count_code(
            state_, skirmish_settings_, preferences_, skirmish_ui_, typed_keys_, *this, *this
        );
        break;
    case TypedKeyHook::single_player_code:
        check_single_player_code();
        break;
    case TypedKeyHook::none:
        return;
    }
    rebuild_surface();
}

void Runtime::show_unsupported(std::string_view message) {
    status_ = std::string(message);
    std::cerr << "unsupported operation: " << message << '\n';
    if (sdl_.window != nullptr) {
        // The box needs the pointer; full screen holds it again once the
        // box has closed and the window has the focus back. Window edge
        // forgets its hold, so a sticky edge holds again only from inside
        // and Capture asks for the grab again.
        forget_sticky_edge();
        release_pointer(sdl_.window);
        SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_INFORMATION, "Open Annihilation", status_.c_str(), sdl_.window
        );
        keep_pointer_on_screen(sdl_.window);
    }
}

bool Runtime::play_alternate_sound(std::string_view sound) {
    if (options_.mute || options_.headless_check)
        return false;
    const auto selection = oa::audio::game_audio::select_alternate(
        audio_registry_, sound, false, sound_playback_state()
    );
    if (selection.status != oa::audio::game_audio::SelectionStatus::selected ||
        selection.sound == nullptr)
        return false;
    // The route's loop stops as the new one starts, whether or not it does.
    std::string error;
    menu_music_playing_ = audio_player_.start_loop_resource(selection.sound->resource, error);
    if (!menu_music_playing_)
        std::cerr << "menu BGM unavailable: " << error << '\n';
    return menu_music_playing_;
}

void Runtime::play_menu_sound(menu::Sound sound) {
    if (options_.mute)
        return;
    const auto selection = oa::audio::game_audio::select(
        audio_registry_, menu::resource_name(sound), false, sound_playback_state()
    );
    std::string error;
    if (selection.status == oa::audio::game_audio::SelectionStatus::selected &&
        !sound_found_missing(selection.sound->resource) && !audio_player_.play(selection, error))
        report_unplayed_sound(selection.sound->resource, error);
}

void Runtime::take_movie_event(void* context, const SDL_Event& event) {
    auto& runtime = *static_cast<Runtime*>(context);
    // A change of focus during the movie is noted, so that the menu's loop
    // stays held after it when the application has gone inactive.
    runtime.note_window_activation(event);
    // The movie player does not hand its events on, so whether Alt+Enter
    // took one does not matter. A render event is noted for the next
    // render(); the movie's own texture may go with a reset device, which
    // ends the movie as any failed upload does.
    if (runtime.take_render_event(event))
        return;
    std::ignore = runtime.take_full_screen_event(event);
}

void Runtime::play_movie_resource(std::string_view filename) {
    // A movie is a plain file name in the Data folder: one that names a
    // folder, a drive or a parent is not opened.
    if (filename.empty() || filename == "." || filename == ".." ||
        filename.find_first_of("/\\:") != std::string_view::npos) {
        status_ = "movie unavailable: " + std::string(filename) + " is not a file name";
        return;
    }
    // A mod folder's movie replaces the game folder's.
    const auto found = game_path("Data/" + std::string(filename));
    const auto path = found ? *found : options_.game_dir / "Data" / filename;
    auto opened = oa::media::IntroPlayer::open(path);
    if (!opened) {
        status_ = "movie unavailable: " + opened.error;
    } else {
        oa::media::PlaybackOptions playback;
        playback.headless_check = options_.headless_check;
        playback.frame_limit = options_.frame_limit.value_or(0);
        playback.play_audio = !options_.mute && !options_.headless_check;
        playback.window = sdl_.window;
        playback.renderer = sdl_.renderer;
        // Alt+Enter switches full screen during the movie as it does in the
        // game.
        playback.hooks.context = this;
        playback.hooks.window_event = take_movie_event;
        // The menu's loop is silent under the movie and plays on from where
        // it was after it, unless the application is inactive by then.
        audio_player_.hold_loop(true);
        const auto result = opened.player->play(playback);
        audio_player_.hold_loop(!application_active_);
        if (!result.ok())
            status_ = "movie playback failed: " + result.error;
    }
    apply_output_mode();
}

} // namespace oa::app

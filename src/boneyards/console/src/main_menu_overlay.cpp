// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Main-menu overlay: the Boneyards panel over the TA main menu, its buttons
// and the activity light.
#include <cstdlib>

#include "console_state.hpp"
#include "oa/boneyards/message_box.hpp"
#include "widgets.hpp"

namespace oa::boneyards::console {

namespace {

// Shade applied to the overlay graphic while a message box is open and to
// the enter button's disabled frame.
constexpr int32_t k_dim_level = -0x18;
constexpr int32_t k_undim_level = 0x18;
// Cycles each activity lamp stays lit.
constexpr int32_t k_lamp_period = 1;

const char* const k_overlay_name = "MainMenuOverlay";
const char* const k_single_name = "SingleBtn";
const char* const k_multi_name = "MultiBtn";
const char* const k_intro_name = "IntroBtn";
const char* const k_exit_name = "ExitBtn";
const char* const k_single_former_name = "SingleFormer";
const char* const k_multi_former_name = "MultiFormer";
const char* const k_intro_former_name = "IntroFormer";
const char* const k_exit_former_name = "ExitFormer";
const char* const k_credits_name = "CreditsBtn";
// The columns before the strip moved them. Each button was 96 wide, so the
// old SINGLE and MULTI ran through x 235 and the old INTRO and EXIT through
// x 505. The strip's own buttons cover 82..178 and 464..560.
constexpr int32_t k_former_left = 139;
constexpr int32_t k_former_right = 409;
constexpr int32_t k_former_span = 96;
const char* const k_enter_name = "EnterBYBtn";
const char* const k_light_name = "MMActivityLight";

const MainMenuHost& host_of(const MainMenuOverlay* overlay) {
    return overlay->owner->config.host;
}

void play_sound(const MainMenuHost& host, const char* name) {
    if (host.play_sound != nullptr)
        host.play_sound(host.user, name);
}

void set_pending_signal(const MainMenuHost& host, uint8_t signal) {
    if (host.set_pending_signal != nullptr)
        host.set_pending_signal(host.user, signal);
}

void update_gadgets(const MainMenuHost& host) {
    if (host.update_gadgets != nullptr)
        host.update_gadgets(host.user);
}

MainMenuOverlay* as_overlay(Window* window) {
    return reinterpret_cast<MainMenuOverlay*>(window);
}

ActivityLight* as_light(Window* window) {
    return reinterpret_cast<ActivityLight*>(window);
}

void set_all_button_states(MainMenuOverlay* overlay, int32_t state) {
    window_set_state(overlay->single_button, state);
    window_set_state(overlay->multi_button, state);
    window_set_state(overlay->intro_button, state);
    window_set_state(overlay->exit_button, state);
    window_set_state(overlay->single_former, state);
    window_set_state(overlay->multi_former, state);
    window_set_state(overlay->intro_former, state);
    window_set_state(overlay->exit_former, state);
    window_set_state(overlay->credits_button, state);
    window_set_state(overlay->enter_button, state);
}

/// Creates the row of eight lamps across the light's width, all dark.
///
/// @param self the activity light
void activity_light_create_lamps(Window* self) {
    ActivityLight* light = as_light(self);
    const int32_t lamp_width = self->width / k_activity_lamp_count;
    int32_t x = self->x;
    for (Window*& lamp : light->lamps) {
        lamp = panel_create();
        window_create(
            lamp,
            resource::activity_light_sequence,
            x,
            self->y,
            lamp_width,
            self->height,
            self,
            true
        );
        console_bind_images(
            lamp,
            resource::main_console_gaf,
            resource::activity_light_sequence,
            0,
            resource::console_key
        );
        window_set_state(lamp, 0);
        x += lamp_width;
    }
    light->running = 0;
}

/// Advances the lit lamp one position every other update while running.
///
/// @param self the activity light
void activity_light_update(Window* self) {
    ActivityLight* light = as_light(self);
    if (light->running == 0)
        return;
    const int32_t remaining = light->countdown--;
    if (remaining != 0)
        return;
    window_set_state(light->lamps[light->lit], 0);
    light->lit = (light->lit + 1) % k_activity_lamp_count;
    window_set_state(light->lamps[light->lit], 1);
    light->countdown = k_lamp_period;
}

} // namespace

void activity_light_start(ActivityLight* light) {
    if (light->running != 0)
        return;
    light->lit = 0;
    window_set_state(light->lamps[0], 1);
    light->running = 1;
    light->countdown = k_lamp_period;
}

void activity_light_stop(ActivityLight* light) {
    if (light->running == 0)
        return;
    window_set_state(light->lamps[light->lit], 0);
    light->running = 0;
}

namespace {

/// Frees the activity light.
///
/// @param self the activity light
void activity_light_destroy(Window* self) {
    console_window_free(self);
}

/// Re-enables the enter button once the online client has gone offline.
///
/// @param self the main-menu overlay
void overlay_update(Window* self) {
    MainMenuOverlay* overlay = as_overlay(self);
    if (overlay->enable_pending == 0 || overlay->owner->system.connection_level != 0)
        return;
    overlay->enable_pending = 0;
    activity_light_stop(overlay->activity_light);
    window_set_state(overlay->enter_button, button_state::unpressed);
}

bool enter_boneyards_requested(MainMenuOverlay* overlay) {
    StatusWidget* widget = overlay->owner;
    const BoneyardsService& service = widget->config.service;
    return service.network_available != nullptr && service.network_available(service.user);
}

// Answers the strip's MULTI before this overlay opens multiplayer. A true
// result means the press was answered (the demo's notice, or a mod that
// cannot start) and the overlay does nothing else. Null leaves MULTI
// opening multiplayer. Read when the button is pressed, so an answer
// installed before the strip exists is the one that runs.
bool (*g_multiplayer_answer)(void* user) = nullptr;
void* g_multiplayer_answer_user = nullptr;

/// Runs the action of the overlay button that reported the notice.
///
/// Single, Multi, Intro and Exit act here. The click does not reach the
/// gadget under the strip, so the sound and the signal happen once. Multi
/// asks the installed answer first: the demo, which has no multiplayer map,
/// shows that notice and stays on the main menu. Intro and Credits do not
/// ask for a disc or for full screen. Credits is the gadget's: a press it
/// takes is only shown.
///
/// @param self the main-menu overlay
/// @param notify control notice
void overlay_on_notify(Window* self, const Notify* notify) {
    MainMenuOverlay* overlay = as_overlay(self);
    const MainMenuHost& host = host_of(overlay);
    const Window* source = notify->source;
    // Credits stays with the gadget under the hotspot.
    if (source != overlay->enter_button && host.press_taken_below != nullptr &&
        host.press_taken_below(host.user))
        return;
    if (source == overlay->single_button || source == overlay->single_former) {
        play_sound(host, menu_sound::big_button);
        set_pending_signal(host, frontend_value::signal_single_player);
        update_gadgets(host);
        return;
    }
    if (source == overlay->multi_button || source == overlay->multi_former) {
        if (g_multiplayer_answer != nullptr && g_multiplayer_answer(g_multiplayer_answer_user))
            return;
        play_sound(host, menu_sound::big_button);
        if (host.prepare_multiplayer != nullptr)
            host.prepare_multiplayer(host.user);
        const bool loaded =
            host.load_multiplayer_document != nullptr && host.load_multiplayer_document(host.user);
        if (!loaded) {
            main_menu_overlay_show_message(overlay, menu_message::multiplayer_disc_required);
            return;
        }
        set_pending_signal(host, frontend_value::signal_multiplayer);
        if (host.after_multiplayer_selection != nullptr)
            host.after_multiplayer_selection(host.user);
        update_gadgets(host);
        return;
    }
    if (source == overlay->intro_button || source == overlay->intro_former) {
        play_sound(host, menu_sound::small_button);
        if (host.set_frontend_state != nullptr)
            host.set_frontend_state(host.user, frontend_value::state_intro);
        update_gadgets(host);
        return;
    }
    if (source == overlay->exit_button || source == overlay->exit_former) {
        play_sound(host, menu_sound::exit);
        set_pending_signal(host, frontend_value::signal_exit_application);
        update_gadgets(host);
        return;
    }
    if (source == overlay->credits_button) {
        play_sound(host, menu_sound::small_button);
        set_pending_signal(host, frontend_value::signal_credits);
        update_gadgets(host);
        return;
    }
    if (source == overlay->enter_button) {
        play_sound(host, menu_sound::big_button);
        // A host that installed an entry flow (the update check, then the
        // expansion, then the open) takes ENTER and the B key. Without one,
        // the network check and the open stay as they are.
        if (host.request_entry != nullptr) {
            host.request_entry(overlay->owner);
            return;
        }
        if (enter_boneyards_requested(overlay)) {
            console_enter_boneyards(overlay->owner);
            return;
        }
        main_menu_overlay_show_message(overlay, menu_message::internet_required);
    }
}

/// Makes B (either case) act as the enter button.
///
/// @param self the main-menu overlay
/// @param msg key message
/// @return true when the key was B
bool overlay_key_down(Window* self, const Message* msg) {
    if (msg->key != 'B' && msg->key != 'b')
        return false;
    MainMenuOverlay* overlay = as_overlay(self);
    Notify notify{};
    notify.code = NotifyCode::activated;
    notify.source = overlay->enter_button;
    self->ops->on_notify(self, &notify);
    return true;
}

/// Releases the overlay graphic, then the window.
///
/// @param self the main-menu overlay
void overlay_destroy(Window* self) {
    image_release(as_overlay(self)->background);
    console_window_free(self);
}

WindowOps make_overlay_ops() {
    WindowOps ops = console_panel_ops();
    ops.destroy = overlay_destroy;
    ops.pulse = overlay_update;
    ops.on_notify = overlay_on_notify;
    ops.key_down = overlay_key_down;
    return ops;
}

WindowOps make_activity_light_ops() {
    WindowOps ops = console_panel_ops();
    ops.destroy = activity_light_destroy;
    ops.on_bounds = activity_light_create_lamps;
    ops.pulse = activity_light_update;
    return ops;
}

const WindowOps k_overlay_ops = make_overlay_ops();
const WindowOps k_activity_light_ops = make_activity_light_ops();

Window* menu_button(
    MainMenuOverlay* overlay, const char* name, int32_t x, int32_t y, const char* sequence
) {
    Window* button = button_create();
    window_create(
        button,
        name,
        x,
        y,
        overlay_layout::menu_button_width,
        overlay_layout::menu_button_height,
        &overlay->base,
        true
    );
    console_bind_images(button, resource::main_console_gaf, sequence, 0, resource::console_key);
    // These sit on the menu's own buttons. A click must not focus them, or
    // the focus frame paints over that art.
    button->focusable = 0;
    return button;
}

// The part of a main-menu button the strip's own button does not cover.
// The columns sat at x 139 and 409; the strip's sit at 82 and 464, and a
// click on the uncovered part is still that button. Nothing is drawn.
Window*
former_button(MainMenuOverlay* overlay, const char* name, int32_t x, int32_t y, int32_t width) {
    Window* button = hotspot_button_create();
    window_create(
        button, name, x, y, width, overlay_layout::menu_button_height, &overlay->base, true
    );
    button->focusable = 0;
    return button;
}

} // namespace

const WindowOps* main_menu_activity_light_ops() {
    return &k_activity_light_ops;
}

MainMenuOverlay* main_menu_overlay_create(StatusWidget* owner) {
    namespace layout = overlay_layout;
    auto* overlay = reinterpret_cast<MainMenuOverlay*>(
        console_window_alloc(sizeof(MainMenuOverlay), &k_overlay_ops)
    );
    overlay->owner = owner;
    overlay->background = k_no_handle;
    window_create(
        &overlay->base,
        k_overlay_name,
        layout::x,
        layout::y,
        layout::width,
        layout::height,
        nullptr,
        false
    );
    overlay->background = console_images_load(
        owner->images,
        resource::main_console_gaf,
        resource::overlay_sequence,
        0,
        resource::console_key
    );
    window_set_state_image(&overlay->base, 1, overlay->background);

    overlay->single_button = menu_button(
        overlay, k_single_name, layout::left_column_x, layout::top_row_y, resource::single_sequence
    );
    overlay->multi_button = menu_button(
        overlay, k_multi_name, layout::left_column_x, layout::bottom_row_y, resource::multi_sequence
    );
    overlay->intro_button = menu_button(
        overlay, k_intro_name, layout::right_column_x, layout::top_row_y, resource::intro_sequence
    );
    overlay->exit_button = menu_button(
        overlay, k_exit_name, layout::right_column_x, layout::bottom_row_y, resource::exit_sequence
    );
    // The part of each old button the strip's own button does not cover. A
    // click there is still that button. The two rectangles do not meet, so
    // the drawn button keeps the press.
    const int32_t left_x = layout::left_column_x + layout::menu_button_width + 1;
    const int32_t left_width = k_former_left + k_former_span - left_x;
    const int32_t right_width = layout::right_column_x - k_former_right - 1;
    overlay->single_former =
        former_button(overlay, k_single_former_name, left_x, layout::top_row_y, left_width);
    overlay->multi_former =
        former_button(overlay, k_multi_former_name, left_x, layout::bottom_row_y, left_width);
    overlay->intro_former =
        former_button(overlay, k_intro_former_name, k_former_right, layout::top_row_y, right_width);
    overlay->exit_former = former_button(
        overlay, k_exit_former_name, k_former_right, layout::bottom_row_y, right_width
    );

    overlay->credits_button = hotspot_button_create();
    window_create(
        overlay->credits_button,
        k_credits_name,
        layout::credits_x,
        layout::credits_y,
        layout::credits_width,
        layout::credits_height,
        &overlay->base,
        true
    );

    overlay->enter_button = button_create();
    window_create(
        overlay->enter_button,
        k_enter_name,
        layout::enter_x,
        layout::enter_y,
        layout::enter_width,
        layout::enter_height,
        &overlay->base,
        true
    );
    console_bind_images(
        overlay->enter_button,
        resource::main_console_gaf,
        resource::enter_sequence,
        0,
        resource::console_key
    );
    const Handle disabled = console_images_clone(
        owner->images,
        overlay->enter_button->states.images[button_state::unpressed],
        resource::console_key
    );
    console_images_shade(owner->images, disabled, k_dim_level, owner->config.shade);
    window_set_state_image(overlay->enter_button, button_state::disabled, disabled);
    image_release(disabled);

    auto* light = reinterpret_cast<ActivityLight*>(
        console_window_alloc(sizeof(ActivityLight), &k_activity_light_ops)
    );
    overlay->activity_light = light;
    window_create(
        &light->base,
        k_light_name,
        layout::light_x,
        layout::light_y,
        layout::light_width,
        layout::light_height,
        &overlay->base,
        true
    );
    overlay->enable_pending = 0;
    return overlay;
}

void main_menu_overlay_start_activity(MainMenuOverlay* overlay) {
    overlay->enable_pending = 1;
    activity_light_start(overlay->activity_light);
    window_set_state(overlay->enter_button, button_state::disabled);
}

namespace {

// As the game's box does on closing, through the system object: the overlay
// that opened the box may have been released while it was up.
void release_after_message_box(void* user, MessageBox*) {
    boneyards_status_widget_dialog_closed(static_cast<StatusWidget*>(user));
}

// The box the overlay shows. Dimming and disabling the buttons is the
// caller's, so a host that opens this box does not do it twice.
void open_overlay_message(MainMenuOverlay* overlay, const char* text) {
    MessageBox* box =
        message_box_create(text, message_box_buttons::ok, nullptr, nullptr, &overlay->base);
    box->on_closed = release_after_message_box;
    box->user = overlay->owner;
    if (Desktop* desktop = services().desktop; desktop != nullptr)
        desktop_set_focus(desktop, &box->base);
}

} // namespace

void boneyards_status_widget_open_message(StatusWidget* widget, const char* text) {
    if (widget == nullptr || widget->system.main_menu == nullptr || text == nullptr)
        return;
    open_overlay_message(widget->system.main_menu, text);
}

void main_menu_overlay_show_message(MainMenuOverlay* overlay, const char* text) {
    cursor_hide();
    const MainMenuHost& host = host_of(overlay);
    if (host.show_message_box != nullptr)
        host.show_message_box(host.user, text);
    else
        open_overlay_message(overlay, text);
    set_all_button_states(overlay, button_state::disabled);
    console_images_shade(
        overlay->owner->images, overlay->background, k_dim_level, overlay->owner->config.shade
    );
}

bool main_menu_overlay_host_connected(const MainMenuHost& host) {
    // The entries overlay_on_notify and main_menu_overlay_show_message call.
    // A disc, a cursor animation, the display mode, the shift key and an
    // input drain are not among them.
    return host.play_sound != nullptr && host.prepare_multiplayer != nullptr &&
           host.load_multiplayer_document != nullptr &&
           host.after_multiplayer_selection != nullptr && host.set_pending_signal != nullptr &&
           host.set_frontend_state != nullptr && host.update_gadgets != nullptr &&
           host.show_message_box != nullptr && host.press_taken_below != nullptr;
}

/// `answer` handles the strip's MULTI before the overlay opens multiplayer.
///
/// A true result means the press was answered and the overlay does nothing
/// else. A null answer leaves the strip opening multiplayer. The answer is
/// read when MULTI is pressed, including on a strip created earlier.
///
/// @param answer the answer, or null
/// @param user passed to `answer`
void boneyards_console_set_multiplayer_answer(bool (*answer)(void* user), void* user) {
    g_multiplayer_answer = answer;
    g_multiplayer_answer_user = user;
}

void main_menu_overlay_release_dialog(MainMenuOverlay* overlay) {
    cursor_show();
    set_all_button_states(overlay, button_state::unpressed);
    console_images_shade(
        overlay->owner->images, overlay->background, k_undim_level, overlay->owner->config.shade
    );
    services_desktop_focus(&overlay->base);
}

} // namespace oa::boneyards::console

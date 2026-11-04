// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Built-in console screen views: the windows the Main Frame shows in its info
// area for the Boneyards screens, driven by the screens' models (entered
// through boneyards_open) and the service behind them.
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "battle_console.hpp"
#include "console_layer.hpp"
#include "console_state.hpp"
#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/console/feature_consoles.hpp"
#include "oa/boneyards/console/registration.hpp"
#include "oa/boneyards/screens/lobby_widgets.hpp"
#include "oa/boneyards/screens/update_pass.hpp"
#include "oa/present/raster.hpp"
#include "oa/present/surface.hpp"
#include "widgets.hpp"

namespace oa::boneyards::console {

namespace {

using screens::ByScreen;

constexpr int32_t k_view_state_normal = 1;
// The planetary map of the server whose yard master answered the login.
constexpr const char* k_map_prefix = "Boneyards\\";
constexpr const char* k_map_suffix = "_map.gwm";
constexpr const char* k_map_unavailable =
    "Unable to load:\n%s\n\nPlanetary functions unavailable at this time";
constexpr std::size_t k_map_path_capacity = 0x40;
constexpr std::size_t k_map_message_capacity = 0x100;
constexpr uint8_t k_login_entry_outline = 0x5a;

// Every view: a panel filling the info area plus its owner.
struct ScreenView {
    Window base;
    StatusWidget* owner;
    ByScreen screen;
};

struct LoginView {
    ScreenView view;
    SingleEdit* name;
    SingleEdit* password;
    Window* login;
    Window* cancel;
    Window* register_button;
    screens::ByListBox* status;
    screens::ByListBox* servers;
    screens::LobbyRadio* member;
    screens::LobbyRadio* guest;
    Handle logo[3];
    RegistrationView* registration;
};

// Update console; its update state is MainFrame::update_console. The pass is
// the placeholder the player sees. The session behind it fetches nothing.
struct UpdateView {
    ScreenView view;
    Window* cancel;
    Window* continue_button;
    screens::ByListBox* status;
    Window* progress_bar;
    int32_t shown_lines;
    int32_t painted_fill;
    Handle bright;
};

namespace update_layout {
constexpr int32_t title_x = 0x6f, title_y = 0x14;
constexpr int32_t continue_x = 0x6f, cancel_x = 0xde, buttons_y = 0x10a;
constexpr int32_t status_dx = 0x6f, status_dy = 0x32, status_width = 0xe5, status_height = 0xbb;
constexpr int32_t status_indent = 2;
constexpr int32_t bar_dx = 0x190, bar_dy = 0x32, bar_width = 0x1a4, bar_height = 0xed;
constexpr int32_t track_width = 0x14, track_height = 0xbb;
constexpr uint8_t track_outline = 0x5a;
// The rising fill and the two-pixel edge at its top. The empty track stays
// the transparent key, so the console shows through until the bar covers it.
constexpr uint8_t bar_fill = 35;
constexpr uint8_t bar_highlight = 33;
constexpr int32_t highlight_rows = 2;
constexpr uint8_t status_bright = 0xe9;
// The label's x counts the console's x twice, as in 3.1c: once added to this
// offset and once through the label's parent.
constexpr int32_t progress_label_dx = 0x16d, progress_label_y = 0x19;
} // namespace update_layout

Handle view_font(const StatusWidget* widget) {
    return widget->toolkit != nullptr ? toolkit_default_font(widget->toolkit) : k_no_handle;
}

/// Drops a view's background colour, so the console art shows through it as in 3.1c.
///
/// @param[in,out] w the view
void clear_background(Window* w) {
    color_release(w->color);
    w->color = k_no_handle;
}

Window* add_function_button(ScreenView* view, int32_t dx, int32_t dy, const char* caption) {
    Window* button = button_create();
    screens::function_button_setup(button, false, dx, dy, caption, &view->base);
    window_set_state(button, button_state::unpressed);
    return button;
}

// ---------------------------------------------------------------------------
// Login

LoginView* as_login(Window* w) {
    return reinterpret_cast<LoginView*>(w);
}

void login_view_sync(LoginView* view) {
    screens::LoginDialog& dialog = view->view.owner->navigation->login;
    copy_text(dialog.name, sizeof dialog.name, single_edit_text(view->name));
    copy_text(dialog.password, sizeof dialog.password, single_edit_text(view->password));
    copy_text(dialog.server, sizeof dialog.server, screens::by_list_selected_text(view->servers));
}

// Runs a login control against the model. LOGIN starts the Main Frame's
// activity light, which the client's reset or the dossier stops; a service
// that refuses the request at once ends the attempt here instead.
void login_view_control(LoginView* view, screens::LoginControl control) {
    StatusWidget* owner = view->view.owner;
    screens::BoneyardsScreens* nav = owner->navigation;
    ActivityLight* light = owner->system.main_frame->activity_light;
    login_view_sync(view);
    if (control == screens::LoginControl::login_button)
        activity_light_start(light);
    screens::login_on_control(&nav->login, control);
    if (control == screens::LoginControl::login_button &&
        nav->login.state != screens::LoginState::connecting)
        activity_light_stop(light);
    if (control == screens::LoginControl::guest_radio)
        view->password->base.ops->set_caption(&view->password->base, "", 0);
    window_set_state(&view->password->base, nav->login.password_field_value);
    window_set_state(&view->member->base, nav->login.member_value);
    window_set_state(&view->guest->base, nav->login.guest_value);
    if (control == screens::LoginControl::cancel_button) {
        list_box_clear(&view->status->list);
    }
}

void login_view_notify(Window* self, const Notify* notify) {
    LoginView* view = as_login(self);
    if (notify->source == view->login)
        login_view_control(view, screens::LoginControl::login_button);
    else if (notify->source == view->cancel)
        login_view_control(view, screens::LoginControl::cancel_button);
    else if (notify->source == &view->member->base)
        login_view_control(view, screens::LoginControl::member_radio);
    else if (notify->source == &view->guest->base)
        login_view_control(view, screens::LoginControl::guest_radio);
    else if (notify->source == view->register_button)
        login_view_control(view, screens::LoginControl::register_button);
}

// Loads the war map of the server whose yard master answered the login
// into the galaxy view; a map that cannot load says so in a message box.
void load_planetary_map(StatusWidget* owner) {
    const BoneyardsService& service = owner->config.service;
    const char* area = service.area_name(service.user);
    char path[k_map_path_capacity];
    std::snprintf(
        path, sizeof path, "%s%s%s", k_map_prefix, area != nullptr ? area : "", k_map_suffix
    );
    GalaxyGraphics* graphics = owner->galaxy_graphics.get();
    GalaxyGraphics bare;
    screens::GalaxyView* galaxy = &owner->navigation->galaxy;
    if (!galaxy_view_load(galaxy, graphics != nullptr ? *graphics : bare, service, path)) {
        char text[k_map_message_capacity];
        std::snprintf(text, sizeof text, k_map_unavailable, path);
        message_box_create(text, message_box_buttons::ok, nullptr, nullptr, nullptr);
    }
    galaxy_panel_sync_hotspots(owner->system.main_frame);
}

/// Runs the login console's pulse while shown.
///
/// Cycles the three logo frames. Once the dossier has loaded (connection
/// level 4), a registered member with no side stays on the side prompt.
/// Choosing a side, once the refreshed dossier names it, loads the planetary
/// map and the Main Frame closes the login console, sliding the green off
/// the map. EXIT leaves for the main menu instead. LOGIN is disabled while
/// the name, or a member's password, is empty and enabled again once they
/// are filled.
///
/// @param self the login console
void login_view_pulse(Window* self) {
    if (!window_is_shown(self))
        return;
    LoginView* view = as_login(self);
    StatusWidget* owner = view->view.owner;
    screens::LoginDialog& dialog = owner->navigation->login;
    login_view_sync(view);
    dialog.logo_frame = (dialog.logo_frame + 1) % screens::kLoginLogoFrames;
    window_set_state_image(self, 1, view->logo[dialog.logo_frame]);
    if (owner->system.connection_level == static_cast<int32_t>(SessionLevel::dossier_loaded)) {
        if (side_prompt_holds_login(owner))
            return;
        load_planetary_map(owner);
        main_frame_close_screen(owner->system.main_frame);
    }
    screens::login_update_button(&dialog);
    window_set_state(
        view->login, dialog.login_value == 0 ? button_state::disabled : button_state::unpressed
    );
}

WindowOps make_field_ops(const WindowOps& base, bool (*key_down)(Window*, const Message*)) {
    WindowOps ops = base;
    ops.key_down = key_down;
    return ops;
}

bool login_field_key_down(Window* self, const Message* msg);
const WindowOps k_login_field_ops =
    make_field_ops(*screens::lobby_edit_ops(), login_field_key_down);

// Enter in the password field presses LOGIN when it is enabled.
bool login_field_key_down(Window* self, const Message* msg) {
    auto* view = reinterpret_cast<LoginView*>(self->parent);
    if (msg->key == key::enter && &view->password->base == self) {
        if (view->view.owner->navigation->login.login_value != 0)
            login_view_control(view, screens::LoginControl::login_button);
        return true;
    }
    return k_single_edit_ops.key_down(self, msg);
}

/// Appends a nonempty Rover location to the login server list.
///
/// @param context the LoginView
/// @param location server location text; null or empty is skipped
void login_server_entry(void* context, const char* location) {
    auto* view = static_cast<LoginView*>(context);
    if (location != nullptr && location[0] != '\0')
        screens::by_text_row_create(
            view->servers, location, &view->view.owner->system.hat_fonts, true
        );
}

screens::ByListBox*
login_list(ScreenView* view, int32_t x, int32_t y, int32_t width, int32_t height) {
    auto* list = screens::by_list_box_create(true, false);
    window_set_bounds(
        &list->list.base, view->base.x + x, view->base.y + y, width, height, &view->base, true
    );
    return list;
}

void login_append_status(void* context, const char* text) {
    auto* view = static_cast<LoginView*>(context);
    screens::by_list_wrap_text(
        view->status, text, &view->view.owner->system.hat_fonts, 3, k_no_handle
    );
}

/// Releases the logo frames and stops being the active login console (the dialog's status target)
/// as the view goes.
///
/// The link is cleared only while it still names this view: a replaced view
/// is destroyed after its successor is built.
///
/// @param self the login console
void login_view_destroy(Window* self) {
    auto* view = as_login(self);
    for (Handle logo : view->logo)
        image_release(logo);
    auto& dialog = view->view.owner->navigation->login;
    if (dialog.status_user == view) {
        dialog.status_user = nullptr;
        dialog.append_status = nullptr;
    }
}

void login_registration_closed(void* user) {
    static_cast<LoginView*>(user)->registration = nullptr;
}

// REGISTER opens the registration console over the login console. BACK hides
// it; the next desktop pass destroys it, so this view does not free it.
void login_show_registration(void* user) {
    auto* view = static_cast<LoginView*>(user);
    if (view->registration != nullptr)
        return;
    StatusWidget* owner = view->view.owner;
    view->registration = registration_open(
        &view->view.base,
        &owner->system.hat_fonts,
        nullptr,
        owner->navigation->login.registration_form
    );
    registration_set_on_closed(view->registration, login_registration_closed, view);
}

/// Builds the login console and makes it the active login console (the dialog's status target), as
/// its constructor does.
///
/// Adds the title, the Username and Password entries, the Registered User
/// and Guest Login radios, REGISTER, LOGIN and CANCEL, the server and status
/// lists and the animated logo. REGISTER opens the registration console. The
/// server list selects the current server
/// (else its first row); a saved user name selects member mode and puts the
/// focus on the password. The controls start zeroed from
/// console_window_alloc.
///
/// @param[in,out] view the login console
void login_view_build(LoginView* view) {
    ScreenView* v = &view->view;
    const auto& f = v->owner->system.hat_fonts;
    screens::lobby_label_create(&v->base, 0x6f, 0x1d, "BONEYARDS LOGIN", f.big, 1, 0);
    screens::lobby_label_create(&v->base, 0x6b, 0x30, "Username", f.medium, 5, 1);
    screens::lobby_label_create(&v->base, 0x6b, 0x49, "Password", f.medium, 5, 1);
    view->name = screens::lobby_edit_create(&v->base, 0x6f, 0x2c, 0x63, f.small, 0xf, false);
    view->password = screens::lobby_edit_create(&v->base, 0x6f, 0x44, 0x63, f.small, 0xf, false);
    window_set_name(&view->name->base, "Username");
    window_set_name(&view->password->base, "Password");
    view->name->scroll = 1;
    view->password->scroll = 1;
    view->password->mask = '*';
    view->name->base.ops = &k_login_field_ops;
    view->password->base.ops = &k_login_field_ops;
    view->member = screens::lobby_radio_create(&v->base, 0x6f, 0x5c, "Registered User", f.medium);
    view->guest = screens::lobby_radio_create(&v->base, 0x6f, 0x6d, "Guest Login", f.medium);
    screens::lobby_radio_add_member(view->member, &view->guest->base);
    screens::lobby_radio_add_member(view->guest, &view->member->base);
    view->register_button = add_function_button(v, 0x6f, 0x7e, "REGISTER");
    view->login = add_function_button(v, 0x6f, 0xf2, "LOGIN");
    view->cancel = add_function_button(v, 0x6f, 0x10a, "CANCEL");
    window_set_name(view->register_button, "REGISTER");
    window_set_name(view->login, "LOGIN");
    window_set_name(view->cancel, "CANCEL");
    screens::lobby_label_create(&v->base, 0x6b, 0x98, "Server List", f.medium, 5, 1);
    screens::lobby_label_create(&v->base, 0xd8, 0x92, "Status", f.medium, 6, 2);
    view->servers = login_list(v, 0x6f, 0x96, 0x63, 0x58);
    view->status = login_list(v, 0xd5, 0x96, 0xe5, 0x89);
    window_set_name(&view->servers->list.base, "Server List");
    window_set_name(&view->status->list.base, "Status");
    const auto& service = v->owner->config.service;
    if (service.list_servers != nullptr)
        service.list_servers(service.user, login_server_entry, view);
    const char* current =
        service.current_server != nullptr ? service.current_server(service.user) : nullptr;
    for (int32_t i = 0; i < view->servers->list.count; ++i) {
        Window* row = list_box_item(&view->servers->list, i);
        if (i == 0)
            view->servers->selected_item = row;
        if (current != nullptr && std::strcmp(row->ops->state_text(row, -1), current) == 0) {
            view->servers->selected_item = row;
            break;
        }
    }
    for (int32_t i = 0; i < 3; ++i)
        view->logo[i] = image_load("Main_Console.gaf", "Login_Logo", i, screens::kWidgetKey);
    window_set_state_image(&v->base, 1, view->logo[0]);
    auto& dialog = v->owner->navigation->login;
    const auto& settings = v->owner->config.settings;
    char saved[0x20]{};
    if (settings.read_string != nullptr &&
        settings.read_string(settings.user, "Username", saved, sizeof saved))
        view->name->base.ops->set_caption(&view->name->base, saved, 0);
    screens::login_on_control(
        &dialog,
        saved[0] != '\0' ? screens::LoginControl::member_radio : screens::LoginControl::guest_radio
    );
    window_set_state(&view->member->base, dialog.member_value);
    window_set_state(&view->guest->base, dialog.guest_value);
    window_set_state(&view->password->base, dialog.password_field_value);
    dialog.status_user = view;
    dialog.append_status = login_append_status;
    dialog.open_registration = login_show_registration;
    dialog.registration_user = view;
    screens::login_set_status(&dialog, "Ready...");
    services_desktop_focus(saved[0] != '\0' ? &view->password->base : &view->name->base);
}

/// Shows the login console (its open slot).
///
/// @param self the login console
void login_view_open(Window* self) {
    self->visible = 1;
}

/// Hides the login console (its close slot).
///
/// @param self the login console
void login_view_close(Window* self) {
    self->visible = 0;
}

/// Outlines the name and password entries, which have no frame of their own, then draws the console
/// and its controls over them. While the registration console is open, draws only it and its form.
///
/// @param self the login console
/// @return true when hidden; otherwise the background draw's result
bool login_view_draw(Window* self) {
    if (!window_is_shown(self))
        return true;
    LoginView* view = as_login(self);
    // The registration console, and its in-game form, cover this console.
    // They draw no field of their own, so the console glass shows through
    // them, and this console's controls are left undrawn while they are up.
    if (Window* page = registration_window(view->registration);
        page != nullptr && window_is_shown(page)) {
        Message draw{};
        draw.code = MessageCode::draw;
        window_dispatch(page, &draw);
        if (Window* form = registration_form_window(view->registration); form != nullptr)
            window_dispatch(form, &draw);
        return true;
    }
    if (Surface* target = console_images_target(view->view.owner->images); target != nullptr) {
        for (const SingleEdit* entry : {view->name, view->password}) {
            const Window& w = entry->base;
            (void)present::draw_rect_outline(
                target,
                Rect32{w.x, w.y, w.x + w.width - 1, w.y + w.height - 1},
                k_login_entry_outline
            );
        }
    }
    return window_draw_background(self);
}

// ---------------------------------------------------------------------------
// Update

/// Wraps a line of update status into the console's status list in the small font, continuation
/// rows one space in (indent 2). The closing line is drawn in the bright colour.
///
/// @param[in,out] view the update console
/// @param text status line
/// @param bright true for the closing line
void update_view_append_status(UpdateView* view, const char* text, bool bright) {
    const int32_t before = view->status->list.count;
    screens::by_list_wrap_text(
        view->status,
        text,
        &view->view.owner->system.hat_fonts,
        update_layout::status_indent,
        k_no_handle
    );
    if (!bright || view->bright < 0)
        return;
    for (int32_t i = before; i < view->status->list.count; ++i) {
        auto* row = reinterpret_cast<screens::ByTextRow*>(list_box_item(&view->status->list, i));
        if (row != nullptr)
            screens::by_text_row_set_font(row, view->bright);
    }
}

/// Paints the vertical track: the outline, then the fill rising from the bottom.
///
/// @param[in,out] view the update console
/// @param fill thousandths of the inner track filled from the bottom
void update_view_paint_bar(UpdateView* view, int32_t fill) {
    namespace layout = update_layout;
    Surface track{};
    if (view->progress_bar == nullptr ||
        !image_surface(view->progress_bar->states.images[k_view_state_normal], &track))
        return;
    const Rect32 bounds = present::surface_clip(track);
    (void)present::fill_rect(track, bounds, screens::kWidgetKey);
    (void)present::draw_rect_outline(&track, bounds, layout::track_outline);
    const int32_t inner_h = track.height - 2;
    const int32_t inner_w = track.width - 2;
    if (inner_h <= 0 || inner_w <= 0 || fill <= 0)
        return;
    int32_t fill_h = inner_h * fill / screens::kUpdatePassFillFull;
    if (fill_h < 1)
        fill_h = 1;
    if (fill_h > inner_h)
        fill_h = inner_h;
    const int32_t top = track.height - 1 - fill_h;
    const Rect32 filled{1, top, inner_w, track.height - 2};
    (void)present::fill_rect(track, filled, layout::bar_fill);
    const int32_t edge = fill_h < layout::highlight_rows ? fill_h : layout::highlight_rows;
    (void)present::fill_rect(track, Rect32{1, top, inner_w, top + edge - 1}, layout::bar_highlight);
}

/// Appends real-check lines that are not on the list yet, and paints the bar.
///
/// Earlier rows stay. A line is added only when the check's picture grew.
///
/// @param[in,out] view the update console
void update_view_apply_real(UpdateView* view) {
    const StatusWidget::UpdateDrive& drive = view->view.owner->update_drive;
    const int32_t count = static_cast<int32_t>(drive.lines.size());
    for (int32_t i = view->shown_lines; i < count; ++i) {
        const UpdateCheckLine& line = drive.lines[static_cast<std::size_t>(i)];
        update_view_append_status(view, line.text.c_str(), line.closing);
    }
    if (count > view->shown_lines)
        view->shown_lines = count;
    if (drive.fill != view->painted_fill) {
        update_view_paint_bar(view, drive.fill);
        view->painted_fill = drive.fill;
    }
}

/// Builds the update console, hidden.
///
/// Adds the "BONEYARDS UPDATE" title, CONTINUE and CANCEL (unpressed,
/// hidden), the framed status list opened with "Checking auto-updates...",
/// the ProgressBar panel showing a 20x187 track outlined in 0x5A, and the
/// "Progress" caption. The placeholder pass has not started.
///
/// @param[in,out] view the update console
void update_view_build(UpdateView* view) {
    namespace layout = update_layout;
    ScreenView* v = &view->view;
    Window* self = &v->base;
    const auto& f = v->owner->system.hat_fonts;
    view->bright = k_no_handle;
    view->shown_lines = 0;
    view->painted_fill = 0;
    if (v->owner->toolkit != nullptr)
        view->bright = toolkit_font_tint(v->owner->toolkit, f.small, layout::status_bright);
    screens::lobby_label_create(
        self, layout::title_x, layout::title_y, "BONEYARDS UPDATE", f.big, 1, 0
    );
    view->continue_button = button_create();
    screens::function_button_setup(
        view->continue_button, false, layout::continue_x, layout::buttons_y, "CONTINUE", self
    );
    window_set_state(view->continue_button, button_state::unpressed);
    view->continue_button->visible = 0;
    view->cancel = button_create();
    screens::function_button_setup(
        view->cancel, false, layout::cancel_x, layout::buttons_y, "CANCEL", self
    );
    window_set_state(view->cancel, button_state::unpressed);
    view->cancel->visible = 0;
    view->status = screens::by_list_box_create(true, false);
    window_set_bounds(
        &view->status->list.base,
        self->x + layout::status_dx,
        self->y + layout::status_dy,
        layout::status_width,
        layout::status_height,
        self,
        true
    );
    const bool real = v->owner->update_drive.real;
    if (!real) {
        update_view_append_status(view, screens::kUpdatePassLines[0], false);
        view->shown_lines = 1;
    }
    view->progress_bar = panel_create();
    window_create(
        view->progress_bar,
        "ProgressBar",
        self->x + layout::bar_dx,
        self->y + layout::bar_dy,
        layout::bar_width,
        layout::bar_height,
        self,
        true
    );
    const Handle track = image_create_blank(
        layout::track_width, layout::track_height, screens::kWidgetKey, screens::kWidgetKey
    );
    window_set_state_image(view->progress_bar, k_view_state_normal, track);
    Surface surface{};
    if (image_surface(track, &surface))
        (void)present::draw_rect_outline(
            &surface, present::surface_clip(surface), layout::track_outline
        );
    image_release(track);
    screens::lobby_label_create(
        self, self->x + layout::progress_label_dx, layout::progress_label_y, "Progress", f.big, 1, 0
    );
    if (real)
        update_view_apply_real(view);
    self->visible = 0;
}

UpdateView* as_update(Window* w) {
    return reinterpret_cast<UpdateView*>(w);
}

/// Shows the frame of the placeholder pass the Main Frame has reached.
///
/// The Main Frame advances the pass on the clock. New status lines are
/// appended in order and the bar is repainted when the fill changes.
///
/// @param self the update console
void update_view_pulse(Window* self) {
    if (!window_is_shown(self))
        return;
    UpdateView* view = as_update(self);
    if (view->view.owner->update_drive.real) {
        update_view_apply_real(view);
        return;
    }
    const MainFrame* frame = view->view.owner->system.main_frame;
    if (frame == nullptr)
        return;
    const screens::UpdatePass& pass = frame->update_pass;
    for (int32_t i = view->shown_lines; i < pass.lines && i < screens::kUpdatePassLineCount; ++i)
        update_view_append_status(
            view, screens::kUpdatePassLines[i], i + 1 == screens::kUpdatePassLineCount
        );
    if (pass.lines > view->shown_lines)
        view->shown_lines = pass.lines;
    if (pass.fill != view->painted_fill) {
        update_view_paint_bar(view, pass.fill);
        view->painted_fill = pass.fill;
    }
}

/// Releases the bright face used for the closing status line.
///
/// @param self the update console
void update_view_destroy(Window* self) {
    UpdateView* view = as_update(self);
    if (view->bright >= 0)
        font_release(view->bright);
    view->bright = k_no_handle;
}

WindowOps make_view_ops() {
    return console_panel_ops();
}

WindowOps make_update_ops() {
    WindowOps ops = console_panel_ops();
    ops.pulse = update_view_pulse;
    ops.on_destroy = update_view_destroy;
    return ops;
}

WindowOps make_login_ops() {
    WindowOps ops = console_panel_ops();
    ops.on_notify = login_view_notify;
    ops.on_destroy = login_view_destroy;
    ops.pulse = login_view_pulse;
    ops.draw = login_view_draw;
    return ops;
}

const WindowOps k_view_ops = make_view_ops();
const WindowOps k_login_ops = make_login_ops();
const WindowOps k_update_ops = make_update_ops();

ByScreen by_screen_for(int32_t screen) {
    return screen == console_screen::login ? ByScreen::login : ByScreen::none;
}

} // namespace

ConsoleScreenSlots console_screen_slots(int32_t screen) {
    switch (screen) {
    case console_screen::help:
    case console_screen::ladders:
    case console_screen::news:
        return {contents_console_open, contents_console_close};
    case console_screen::login:
        return {login_view_open, login_view_close};
    case console_screen::chat:
        return {chat_console_open, chat_console_close};
    case console_screen::recon:
        return {recon_console_open, recon_console_close};
    case console_screen::battle:
        return {battle_console_open, battle_console_close};
    case console_screen::profile:
        return {profile_console_open, profile_console_close};
    default:
        return {};
    }
}

Window* console_create_screen_view(
    StatusWidget* widget,
    int32_t screen,
    Window* frame,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height
) {
    if (screen == console_screen::help || screen == console_screen::ladders ||
        screen == console_screen::news)
        return contents_console_create(widget, screen, frame, x, y, width, height);
    if (screen == console_screen::recon)
        return recon_console_create(widget, frame, x, y, width, height);
    if (screen == console_screen::chat)
        return chat_console_create(widget, frame, x, y, width, height);
    if (screen == console_screen::profile)
        return profile_console_create(widget, frame, x, y, width, height);
    if (screen == console_screen::battle)
        return battle_console_create(widget, frame, x, y, width, height);
    const ByScreen wanted = by_screen_for(screen);
    const ByScreen shown =
        wanted != ByScreen::none ? screens::boneyards_open(widget->navigation, wanted) : wanted;
    std::size_t size = sizeof(ScreenView);
    const WindowOps* ops = &k_view_ops;
    if (shown == ByScreen::login) {
        size = sizeof(LoginView);
        ops = &k_login_ops;
    } else if (screen == console_screen::update) {
        size = sizeof(UpdateView);
        ops = &k_update_ops;
    }
    auto* view = reinterpret_cast<ScreenView*>(console_window_alloc(size, ops));
    view->owner = widget;
    view->screen = shown;
    const char* view_name = "ConsoleScreen";
    if (screen == console_screen::login)
        view_name = "Login";
    else if (screen == console_screen::update)
        view_name = "Update";
    window_create(&view->base, view_name, x, y, width, height, frame, true);
    clear_background(&view->base);
    if (view->base.font < 0 && widget->toolkit != nullptr) {
        view->base.font = view_font(widget);
        font_add_ref(view->base.font);
    }
    switch (shown) {
    case ByScreen::login:
        login_view_build(reinterpret_cast<LoginView*>(view));
        break;
    case ByScreen::none:
        if (screen == console_screen::update)
            update_view_build(reinterpret_cast<UpdateView*>(view));
        break;
    case ByScreen::chat_frequencies:
    case ByScreen::profile_console:
    case ByScreen::battle_list:
    case ByScreen::game_setup:
        break;
    }
    return &view->base;
}

} // namespace oa::boneyards::console

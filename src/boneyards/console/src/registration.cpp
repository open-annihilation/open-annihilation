// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Registration console. The window, its pictures and its captions come from
// gui/registration.gui. This file keeps the route, the address and which
// controls are shown.
#include "oa/boneyards/console/registration.hpp"

#include "oa/boneyards/console/gui_screen.hpp"
#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/shown_text.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace oa::boneyards::console {

struct RegistrationView {
    GuiScreen* screen;
    screens::RegistrationConsole model;
    const screens::HatFonts* fonts;
    void (*on_closed)(void*);
    void* closed_user;
    Window* title;
    Window* web_icon;
    Window* game_icon;
    Window* web_label;
    Window* game_label;
    Window* web_hot;
    Window* game_hot;
    Window* body[3];
    Window* dim[2];
    Window* heading;
    Window* scan[2];
    Window* qr;
    Window* host;
    Window* url;
    Window* open_button;
    Window* deck[3];
    Window* back;
    Handle icon_selected;
    Handle icon_idle;
    GuiScreen* form_screen;
    screens::RegistrationForm form;
    Window* form_root;
    Window* name_edit;
    Window* password_edit;
    Window* confirm_edit;
    Window* email_edit;
    Window* arm_radio;
    Window* core_radio;
    Window* open_radio;
    Window* rules_box;
    Window* register_button;
    Window* cancel_button;
    Window* status_list;
    Window* form_web_hot;
    Window* arm_hot;
    Window* core_hot;
    Window* open_hot;
    Window* rules_hot;
    Window* rules_link;
};

namespace {

constexpr const char* k_web_copy[] = {
    "Enlist on Core Prime's", "web site, then log in", "with your new name."
};
constexpr const char* k_game_copy[] = {"Enlist without leaving", "the game."};
constexpr const char* k_screen_path = "gui/registration.gui";
constexpr const char* k_form_path = "gui/registration_form.gui";
constexpr int32_t k_binding_capacity = 8;
// The status lines sit this many pixels apart, each line's text hanging
// k_status_text_drop below its top, as the design lays them out.
constexpr int32_t k_status_row_height = 14;
constexpr int32_t k_status_text_drop = 3;

struct Binding {
    Window* window;
    RegistrationView* view;
};

Binding g_bindings[k_binding_capacity]{};

void show_window(Window* window, bool shown) {
    if (window != nullptr)
        window->visible = shown ? 1 : 0;
}

void set_icon(Window* icon, Handle image) {
    if (icon == nullptr || image < 0)
        return;
    window_set_state_image(icon, 0, image);
    window_set_state_image(icon, 1, image);
}

void set_label_font(Window* label, Handle font) {
    if (label != nullptr && font >= 0)
        window_set_font_handle(label, font);
}

void set_caption(Window* label, const char* text) {
    if (label == nullptr || label->ops == nullptr || label->ops->set_caption == nullptr)
        return;
    label->ops->set_caption(
        label, shown_text(text != nullptr ? text : ""), text_style::left | text_style::top
    );
}

Window* find_window(GuiScreen* screen, const char* name) {
    const GuiControl* control = gui_screen_find(screen, name);
    return control != nullptr ? control->window : nullptr;
}

void bind_view(RegistrationView* view) {
    Window* window = gui_screen_root(view->screen);
    for (Binding& slot : g_bindings) {
        if (slot.view == nullptr) {
            slot.window = window;
            slot.view = view;
            return;
        }
    }
}

void unbind_view(RegistrationView* view) {
    for (Binding& slot : g_bindings) {
        if (slot.view == view) {
            slot.window = nullptr;
            slot.view = nullptr;
            return;
        }
    }
}

void use_fonts(RegistrationView* view) {
    const screens::HatFonts* fonts = view->fonts;
    if (fonts == nullptr)
        return;
    set_label_font(view->title, fonts->big);
    set_label_font(view->heading, fonts->big);
    set_label_font(view->host, fonts->big);
    for (Window* line : view->body)
        set_label_font(line, fonts->small);
    for (Window* line : view->scan)
        set_label_font(line, fonts->small);
    for (Window* line : view->dim)
        set_label_font(line, fonts->small_dim);
    set_label_font(view->url, fonts->small_dim);
    for (Window* line : view->deck)
        set_label_font(line, fonts->medium);
}

void copy_field(char* out, std::size_t capacity, const char* text) {
    oa::boneyards::utf8::copy(out, capacity, text, 0);
}

void write_edit(Window* window, const char* text) {
    if (window == nullptr || window->ops == nullptr || window->ops->set_caption == nullptr)
        return;
    window->ops->set_caption(
        window, text != nullptr ? text : "", text_style::left | text_style::top
    );
}

const char* read_edit(Window* window) {
    if (window == nullptr || window->ops == nullptr || !window->ops->is_class(class_name::edit_box))
        return "";
    const char* text = reinterpret_cast<const EditBox*>(window)->text;
    return text != nullptr ? text : "";
}

void set_frame(Window* window, const char* sequence, int32_t frame) {
    if (window == nullptr)
        return;
    const Handle image = image_load("Widgets.gaf", sequence, frame, screens::kWidgetKey);
    if (image < 0)
        return;
    window_set_state_image(window, 0, image);
    window_set_state_image(window, 1, image);
    image_release(image);
}

void hide_slider(RegistrationView* view) {
    if (view->status_list == nullptr)
        return;
    auto* list = reinterpret_cast<screens::ByListBox*>(view->status_list);
    if (list->slider != nullptr)
        list->slider->slider.base.visible = 0;
}

void style_one(GuiScreen* screen, const char* name, Handle font) {
    Window* label = find_window(screen, name);
    if (label != nullptr && font >= 0)
        window_set_font_handle(label, font);
}

void style_form(RegistrationView* view) {
    const screens::HatFonts* fonts = view->fonts;
    GuiScreen* screen = view->form_screen;
    if (fonts == nullptr || screen == nullptr)
        return;
    style_one(screen, "Title", fonts->big);
    style_one(screen, "Heading", fonts->big);
    style_one(screen, "WebLabel", fonts->small);
    style_one(screen, "GameLabel", fonts->small_light);
    style_one(screen, "Body0", fonts->small);
    style_one(screen, "Body1", fonts->small);
    style_one(screen, "Dim0", fonts->small_dim);
    style_one(screen, "Dim1", fonts->small_dim);
    const char* medium[] = {
        "UserLabel",
        "PassLabel",
        "ConfirmLabel",
        "EmailLabel",
        "SideLabel",
        "ArmText",
        "CoreText",
        "OpenText",
        "RulesText"
    };
    for (const char* name : medium)
        style_one(screen, name, fonts->medium);
    const char* edits[] = {"User", "Pass", "Confirm", "Email"};
    for (const char* name : edits)
        style_one(screen, name, fonts->small);
}

void take_form(RegistrationView* view) {
    GuiScreen* screen = view->form_screen;
    view->form_root = gui_screen_root(screen);
    view->name_edit = find_window(screen, "User");
    view->password_edit = find_window(screen, "Pass");
    view->confirm_edit = find_window(screen, "Confirm");
    view->email_edit = find_window(screen, "Email");
    view->arm_radio = find_window(screen, "ArmRadio");
    view->core_radio = find_window(screen, "CoreRadio");
    view->open_radio = find_window(screen, "OpenRadio");
    view->rules_box = find_window(screen, "RulesBox");
    view->register_button = find_window(screen, "Register");
    view->cancel_button = find_window(screen, "Cancel");
    view->status_list = find_window(screen, "Status");
    view->form_web_hot = find_window(screen, "WebHot");
    view->arm_hot = find_window(screen, "ArmHot");
    view->core_hot = find_window(screen, "CoreHot");
    view->open_hot = find_window(screen, "OpenHot");
    view->rules_hot = find_window(screen, "RulesHot");
    view->rules_link = find_window(screen, "RulesLink");
}

void paint_status(RegistrationView* view) {
    if (view->status_list == nullptr || view->fonts == nullptr)
        return;
    auto* list = reinterpret_cast<screens::ByListBox*>(view->status_list);
    screens::by_list_box_clear(list);
    for (int32_t i = 0; i < view->form.status_count && i < screens::kRegistrationStatusLines; ++i) {
        screens::ByTextRow* row =
            screens::by_text_row_create(list, view->form.status[i], view->fonts, false, 4);
        const Handle font =
            i == view->form.status_bright ? view->fonts->small_green : view->fonts->small;
        screens::by_text_row_set_font(row, font);
        // The next line is placed by this row's height.
        row->base.height = k_status_row_height;
        if (Window* label = row->label; label != nullptr) {
            label->y = row->base.y + k_status_text_drop;
            label->height = k_status_row_height - k_status_text_drop;
            reinterpret_cast<screens::LobbyLabel*>(label)->style =
                text_style::left | text_style::top;
            screens::lobby_label_set_text(label, view->form.status[i]);
        }
    }
    hide_slider(view);
}

void paint_form(RegistrationView* view) {
    if (view->form_root == nullptr)
        return;
    write_edit(view->name_edit, view->form.name);
    write_edit(view->password_edit, view->form.password);
    write_edit(view->confirm_edit, view->form.confirm);
    write_edit(view->email_edit, view->form.email);
    const int32_t side = view->form.side;
    set_frame(view->arm_radio, "Radio_Button", side == screens::registration_side::arm ? 1 : 2);
    set_frame(view->core_radio, "Radio_Button", side == screens::registration_side::core ? 1 : 2);
    set_frame(
        view->open_radio, "Radio_Button", side == screens::registration_side::undecided ? 1 : 2
    );
    set_frame(view->rules_box, "Check_Box", view->form.rules != 0 ? 1 : 2);
    if (view->register_button != nullptr) {
        window_set_state(
            view->register_button,
            view->form.sending != 0 ? button_state::disabled : button_state::unpressed
        );
    }
    paint_status(view);
}

void capture_fields(RegistrationView* view) {
    copy_field(view->form.name, sizeof view->form.name, read_edit(view->name_edit));
    copy_field(view->form.password, sizeof view->form.password, read_edit(view->password_edit));
    copy_field(view->form.confirm, sizeof view->form.confirm, read_edit(view->confirm_edit));
    copy_field(view->form.email, sizeof view->form.email, read_edit(view->email_edit));
}

void form_destroyed(void* user);
void ensure_form(RegistrationView* view);
void show_reply(RegistrationView* view);
void apply_route(RegistrationView* view);
void close_registration(RegistrationView* view);

void reply_closed(void* user, MessageBox*) {
    auto* view = static_cast<RegistrationView*>(user);
    if (view == nullptr)
        return;
    const int32_t reply = view->form.reply;
    view->form.reply = screens::registration_reply::none;
    view->form.sending = 0;
    if (reply == screens::registration_reply::registered) {
        close_registration(view);
        return;
    }
    if (reply == screens::registration_reply::name_taken) {
        copy_field(
            view->form.status[0],
            screens::kRegistrationStatusBytes,
            "Choose a name of 3 to 15 characters."
        );
        copy_field(
            view->form.status[1],
            screens::kRegistrationStatusBytes,
            "Your password travels as a digest, never in clear."
        );
        copy_field(
            view->form.status[2],
            screens::kRegistrationStatusBytes,
            "That name is taken. Please choose another."
        );
        view->form.status[3][0] = '\0';
        view->form.status_count = 3;
        view->form.status_bright = -1;
        paint_form(view);
        return;
    }
    screens::registration_select(&view->model, 0);
    screens::registration_form_review(&view->form);
    apply_route(view);
}

void show_reply(RegistrationView* view) {
    if (view == nullptr || view->form.reply == screens::registration_reply::none)
        return;
    if (message_box_find_open(services().desktop) != nullptr)
        return;
    char text[160];
    screens::registration_reply_text(view->form.reply, view->form.name, text, sizeof text);
    Window* back =
        view->form.reply == screens::registration_reply::name_taken ? view->name_edit : nullptr;
    MessageBox* box = message_box_create(text, message_box_buttons::ok, nullptr, nullptr, back);
    box->on_closed = reply_closed;
    box->user = view;
}

void form_fired(void* user, Window* source) {
    auto* view = static_cast<RegistrationView*>(user);
    if (view == nullptr || source == nullptr ||
        view->form.reply != screens::registration_reply::none)
        return;
    if (source == view->form_web_hot) {
        screens::registration_select(&view->model, 0);
        apply_route(view);
        return;
    }
    if (source == view->cancel_button) {
        if (view->form.sending != 0) {
            screens::registration_form_abandon(&view->form);
            paint_form(view);
        } else {
            close_registration(view);
        }
        return;
    }
    if (source == view->register_button) {
        capture_fields(view);
        const int32_t check = screens::registration_form_submit(&view->form);
        paint_form(view);
        if (check == screens::registration_check::ok) {
            screens::registration_form_receive(&view->form);
            show_reply(view);
        }
        return;
    }
    if (view->form.sending != 0)
        return;
    if (source == view->arm_hot)
        view->form.side = screens::registration_side::arm;
    else if (source == view->core_hot)
        view->form.side = screens::registration_side::core;
    else if (source == view->open_hot)
        view->form.side = screens::registration_side::undecided;
    else if (source == view->rules_hot)
        view->form.rules = view->form.rules == 0 ? 1 : 0;
    else if (source == view->rules_link)
        view->form.rules_page = 1;
    else
        return;
    capture_fields(view);
    screens::registration_form_review(&view->form);
    paint_form(view);
}

void form_destroyed(void* user) {
    auto* view = static_cast<RegistrationView*>(user);
    if (view == nullptr)
        return;
    view->form_screen = nullptr;
    view->form_root = nullptr;
    view->name_edit = nullptr;
    view->password_edit = nullptr;
    view->confirm_edit = nullptr;
    view->email_edit = nullptr;
    view->arm_radio = nullptr;
    view->core_radio = nullptr;
    view->open_radio = nullptr;
    view->rules_box = nullptr;
    view->register_button = nullptr;
    view->cancel_button = nullptr;
    view->status_list = nullptr;
    view->form_web_hot = nullptr;
    view->arm_hot = nullptr;
    view->core_hot = nullptr;
    view->open_hot = nullptr;
    view->rules_hot = nullptr;
    view->rules_link = nullptr;
}

void say_needs_mod(const char* text) {
    if (message_box_find_open(services().desktop) != nullptr)
        return;
    message_box_create(text, message_box_buttons::ok, nullptr, nullptr, nullptr);
}

void ensure_form(RegistrationView* view) {
    if (view->form_screen != nullptr || view->model.in_game_route == 0)
        return;
    GuiScreen* screen = gui_screen_load(k_form_path, nullptr);
    if (screen == nullptr) {
        say_needs_mod(kRegistrationFormNeedsMod);
        return;
    }
    Window* root = gui_screen_root(screen);
    Window* registration = gui_screen_root(view->screen);
    Window* parent = registration != nullptr ? registration->parent : nullptr;
    window_set_bounds(root, root->x, root->y, root->width, root->height, parent, false);
    root->focusable = 0;
    view->form_screen = screen;
    take_form(view);
    style_form(view);
    hide_slider(view);
    gui_screen_set_user(screen, view, form_fired, form_destroyed);
}

// The first field takes the keyboard when the form is shown, unless the
// player is already typing in one of its fields.
void focus_form_name(RegistrationView* view) {
    if (view->form_root == nullptr || view->form_root->visible == 0 || view->name_edit == nullptr)
        return;
    if (Desktop* desktop = services().desktop; desktop != nullptr) {
        Window* focus = desktop->focus;
        if (focus != nullptr && focus->parent == view->form_root && focus->ops != nullptr &&
            focus->ops->is_class != nullptr &&
            (focus->ops->is_class(class_name::edit_box) ||
             focus->ops->is_class(class_name::single_edit)))
            return;
    }
    services_desktop_focus(view->name_edit);
}

// Selecting the in-game route shows gui/registration_form.gui over this page.
// The web page's code and address are hidden either way.
void apply_route(RegistrationView* view) {
    const bool web = view->model.route == 0;
    const bool browser = web && view->model.browser_available != 0;
    const bool routes = view->model.in_game_route != 0;
    const screens::HatFonts* fonts = view->fonts;
    const Handle light = fonts != nullptr ? fonts->small_light : k_no_handle;
    const Handle plain = fonts != nullptr ? fonts->small : k_no_handle;
    set_caption(view->heading, web ? "ENLIST ON THE WEB" : "ENLIST IN THE GAME");
    if (web) {
        for (int32_t i = 0; i < 3; ++i)
            set_caption(view->body[i], k_web_copy[i]);
        show_window(view->body[2], true);
    } else {
        set_caption(view->body[0], k_game_copy[0]);
        set_caption(view->body[1], k_game_copy[1]);
        show_window(view->body[2], false);
    }
    set_icon(view->web_icon, web ? view->icon_selected : view->icon_idle);
    set_label_font(view->web_label, web ? light : plain);
    set_icon(view->game_icon, web ? view->icon_idle : view->icon_selected);
    set_label_font(view->game_label, web ? plain : light);
    show_window(view->game_icon, routes);
    show_window(view->game_label, routes);
    show_window(view->web_hot, routes);
    show_window(view->game_hot, routes);
    show_window(view->qr, web);
    show_window(view->host, web);
    show_window(view->url, web);
    show_window(view->scan[0], web);
    show_window(view->scan[1], web);
    show_window(view->open_button, browser);
    for (Window* line : view->deck)
        show_window(line, web && !browser);
    if (!web && routes) {
        ensure_form(view);
        if (view->form_root != nullptr) {
            view->form_root->visible = 1;
            paint_form(view);
            focus_form_name(view);
        }
    } else if (view->form_root != nullptr) {
        view->form_root->visible = 0;
    }
    // The form draws no field of its own, so this page shows through it: its
    // BACK TO LOGIN would sit in the form's status box. The form's CANCEL
    // goes back instead.
    show_window(view->back, view->form_root == nullptr || view->form_root->visible == 0);
}

void close_registration(RegistrationView* view) {
    screens::registration_back(&view->model);
    void (*closed)(void*) = view->on_closed;
    void* user = view->closed_user;
    view->on_closed = nullptr;
    view->closed_user = nullptr;
    if (closed != nullptr)
        closed(user);
    if (view->form_root != nullptr)
        view->form_root->visible = 0;
    Window* root = gui_screen_root(view->screen);
    if (root != nullptr) {
        root->visible = 0;
        root->owned = 1;
    }
}

void registration_fired(void* user, Window* source) {
    auto* view = static_cast<RegistrationView*>(user);
    if (view == nullptr || source == nullptr)
        return;
    if (source == view->open_button)
        screens::registration_open_browser(&view->model);
    else if (source == view->back)
        close_registration(view);
    else if (source == view->web_hot) {
        screens::registration_select(&view->model, 0);
        apply_route(view);
    } else if (source == view->game_hot) {
        screens::registration_select(&view->model, 1);
        apply_route(view);
    }
}

void registration_destroyed(void* user) {
    auto* view = static_cast<RegistrationView*>(user);
    if (view == nullptr)
        return;
    unbind_view(view);
    image_release(view->icon_selected);
    image_release(view->icon_idle);
    view->screen = nullptr;
    if (view->form_screen != nullptr) {
        GuiScreen* form = view->form_screen;
        view->form_screen = nullptr;
        gui_screen_destroy(form);
    }
    std::free(view);
}

void take_controls(RegistrationView* view) {
    GuiScreen* screen = view->screen;
    view->title = find_window(screen, "Title");
    view->web_icon = find_window(screen, "WebIcon");
    view->game_icon = find_window(screen, "GameIcon");
    view->web_label = find_window(screen, "WebLabel");
    view->game_label = find_window(screen, "GameLabel");
    view->web_hot = find_window(screen, "WebHot");
    view->game_hot = find_window(screen, "GameHot");
    view->body[0] = find_window(screen, "Body0");
    view->body[1] = find_window(screen, "Body1");
    view->body[2] = find_window(screen, "Body2");
    view->dim[0] = find_window(screen, "Dim0");
    view->dim[1] = find_window(screen, "Dim1");
    view->heading = find_window(screen, "Heading");
    view->scan[0] = find_window(screen, "Scan0");
    view->scan[1] = find_window(screen, "Scan1");
    view->qr = find_window(screen, "Qr");
    view->host = find_window(screen, "Host");
    view->url = find_window(screen, "Url");
    view->open_button = find_window(screen, "Open");
    view->deck[0] = find_window(screen, "Deck0");
    view->deck[1] = find_window(screen, "Deck1");
    view->deck[2] = find_window(screen, "Deck2");
    view->back = find_window(screen, "Back");
}

} // namespace

RegistrationView* registration_open(
    Window* parent, const screens::HatFonts* fonts, const char* address, int32_t in_game_route
) {
    if (parent == nullptr)
        return nullptr;
    if (fonts != nullptr)
        screens::function_button_fonts(fonts);
    GuiScreen* screen = gui_screen_load(k_screen_path, nullptr);
    if (screen == nullptr) {
        if (message_box_find_open(services().desktop) == nullptr) {
            message_box_create(
                kRegistrationNeedsMod, message_box_buttons::ok, nullptr, nullptr, nullptr
            );
        }
        return nullptr;
    }
    Window* root = gui_screen_root(screen);
    window_set_bounds(root, root->x, root->y, root->width, root->height, parent, true);
    root->focusable = 0;

    auto* view = static_cast<RegistrationView*>(std::calloc(1, sizeof(RegistrationView)));
    if (view == nullptr)
        std::abort();
    view->screen = screen;
    view->fonts = fonts;
    view->icon_selected = k_no_handle;
    view->icon_idle = k_no_handle;
    screens::registration_init(
        &view->model, address, screens::registration_browser_available() ? 1 : 0, in_game_route
    );
    screens::registration_form_init(&view->form);
    take_controls(view);
    set_caption(view->host, view->model.host);
    set_caption(view->url, view->model.address);
    view->icon_selected = image_load("Widgets.gaf", "Icon_Circle", 1, screens::kWidgetKey);
    view->icon_idle = image_load("Widgets.gaf", "Icon_Circle", 2, screens::kWidgetKey);
    use_fonts(view);
    apply_route(view);
    gui_screen_set_user(screen, view, registration_fired, registration_destroyed);
    bind_view(view);
    return view;
}

void registration_set_on_closed(RegistrationView* view, void (*closed)(void* user), void* user) {
    if (view == nullptr)
        return;
    view->on_closed = closed;
    view->closed_user = user;
}

Window* registration_window(RegistrationView* view) {
    return view != nullptr ? gui_screen_root(view->screen) : nullptr;
}

RegistrationView* registration_from_window(Window* window) {
    if (window == nullptr)
        return nullptr;
    for (const Binding& slot : g_bindings) {
        if (slot.window == window)
            return slot.view;
    }
    return nullptr;
}

const screens::RegistrationConsole* registration_model(const RegistrationView* view) {
    return view != nullptr ? &view->model : nullptr;
}

screens::RegistrationForm* registration_form(RegistrationView* view) {
    return view != nullptr ? &view->form : nullptr;
}

Window* registration_form_window(RegistrationView* view) {
    return view != nullptr ? view->form_root : nullptr;
}

void registration_form_apply(RegistrationView* view) {
    if (view == nullptr)
        return;
    if (view->form_root == nullptr && view->model.route == 1)
        ensure_form(view);
    paint_form(view);
}

void registration_form_show_reply(RegistrationView* view) {
    show_reply(view);
}

} // namespace oa::boneyards::console

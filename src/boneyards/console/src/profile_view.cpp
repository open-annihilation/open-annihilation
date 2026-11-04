// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Profile console: the player's own profile. The ProfileList down the left
// names the seven Profile.def categories, with the selected category's
// description wrapped below its rows, and APPLY and RESET under the list;
// a status line says while the record is on its way. The selected
// category's tab shows its controls in the rest of the console
// (profile_tabs_view.cpp).

#include <cstring>

#include "console_layer.hpp"
#include "console_state.hpp"
#include "oa/boneyards/screens/lobby_widgets.hpp"
#include "profile_tabs_view.hpp"
#include "oa/present/raster.hpp"
#include "widgets.hpp"

namespace oa::boneyards::console {

namespace {

constexpr const char* k_console_name = "ProfileConsole";
constexpr const char* k_list_sound = "GREEN_BUTTON";
constexpr const char* k_insignia_sequence = "Insignia";
constexpr int32_t k_image_state = 1;

// The console's image: a keyed blank of the green layer's size with a rule
// down the right edge of the ProfileList.
constexpr int32_t k_image_width = 0x1f2;
constexpr int32_t k_image_height = 0x139;
constexpr int32_t k_rule_x = 0x7b;
constexpr int32_t k_rule_top = 3;
constexpr int32_t k_rule_bottom = 0x135;
constexpr uint8_t k_rule_color = 0x5d;

// Build layout: the title centred over the list's width, the ProfileList
// (absolute position), the description lines, APPLY and RESET, and the
// status line, each inside the console.
constexpr int32_t k_title_y = 6;
constexpr int32_t k_title_width = 0x7c;
constexpr int32_t k_title_alignment = 4;
constexpr int32_t k_list_x = 0x17;
constexpr int32_t k_list_y = 0x93;
constexpr int32_t k_list_width = 0x7c;
constexpr int32_t k_list_height = 0xed;
constexpr int32_t k_description_x = 6;
constexpr int32_t k_description_y = 0x9b;
constexpr int32_t k_description_width = 0x72;
constexpr int32_t k_description_height = 0x11;
constexpr int32_t k_description_alignment = 3;
constexpr int32_t k_buttons_x = 0xc;
constexpr int32_t k_apply_y = 0x105;
constexpr int32_t k_reset_y = 0x11d;
constexpr int32_t k_status_x = 0xa0;
constexpr int32_t k_status_y = 0x5a;

// The ProfileList rows in the order the build adds them.
constexpr screens::ProfileTab k_row_tabs[screens::kProfileTabCount] = {
    screens::ProfileTab::account,
    screens::ProfileTab::allegiance,
    screens::ProfileTab::personal,
    screens::ProfileTab::game_preferences,
    screens::ProfileTab::ladder,
    screens::ProfileTab::awards,
    screens::ProfileTab::local_settings,
};

// The console's record: the view and its tabs' windows.
struct ProfileConsoleRecord {
    ProfileView view;
    ProfileTabWindows tabs;
};

ProfileView* as_profile(Window* w) {
    return reinterpret_cast<ProfileView*>(w);
}

ProfileTabWindows* tabs_of(ProfileView* view) {
    return &reinterpret_cast<ProfileConsoleRecord*>(view)->tabs;
}

screens::ProfileConsole* model_of(const ProfileView* view) {
    return &view->owner->navigation->profile;
}

ConsoleNavButton* profile_button_of(const ProfileView* view) {
    return reinterpret_cast<ConsoleNavButton*>(view->profile_button);
}

void play_sound(const ProfileView* view, const char* name) {
    const MainMenuHost& host = view->owner->config.host;
    if (host.play_sound != nullptr)
        host.play_sound(host.user, name);
}

const char* label_text(Window* label) {
    return label->ops->state_text(label, -1);
}

/// Shows a tab's category description and selects its ProfileList row (the controls' part of
/// switching to a tab).
///
/// The description is wrapped into the five lines (a line holds what fits
/// 0x72 pixels in its font, the next one starting at the break; lines past
/// the text are hidden and emptied), then the list's selected row becomes
/// the tab's when it is another.
///
/// @param[in,out] view Profile console view
/// @param tab ProfileTab index
void show_tab_description(ProfileView* view, int32_t tab) {
    const char* text = model_of(view)->tab_description;
    if (text == nullptr)
        text = "";
    for (Window* label : view->description) {
        if (*text == '\0') {
            label->visible = 0;
            screens::lobby_label_set_text(label, "");
            continue;
        }
        label->visible = 1;
        int32_t count = screens::wrapped_line_length(text, label->font, k_description_width);
        char line[screens::kProfileTextCapacity]{};
        std::memcpy(
            line,
            text,
            static_cast<std::size_t>(count) < sizeof line - 1 ? static_cast<std::size_t>(count)
                                                              : sizeof line - 1
        );
        screens::lobby_label_set_text(label, line);
        text += count != 0 ? count : 1;
    }
    view->description_tab = tab;
    screens::ByListBox* list = view->list;
    const Window* selected = list->selected_item;
    if (selected != nullptr && reinterpret_cast<const screens::ByCircleRow*>(selected)->tab == tab)
        return;
    for (int32_t i = 0; i < list->list.count; ++i) {
        Window* row = list_box_item(&list->list, i);
        if (reinterpret_cast<screens::ByCircleRow*>(row)->tab == tab) {
            list->selected_item = row;
            break;
        }
    }
}

// The requests' replies and a submission change the model's tab, status
// line and list value; the console copies them onto its controls here as it
// opens, after each control and on each pulse.
void follow_model(ProfileView* view) {
    screens::ProfileConsole* model = model_of(view);
    profile_tabs_follow(tabs_of(view), view, model);
    window_set_state(&view->list->list.base, model->tab_list_value);
    if (std::strcmp(label_text(view->status), model->status) != 0)
        screens::lobby_label_set_text(view->status, model->status);
    view->status->visible = model->status_visible ? 1 : 0;
    if (model->current_tab != view->description_tab)
        show_tab_description(view, model->current_tab);
}

// A button enabled only from disabled, and disabled from any other state,
// so a pressed or hovered button keeps its face.
void follow_value(Window* button, int32_t value) {
    const bool disabled = button->states.current == button_state::disabled;
    if (value == screens::widget_value::enabled && disabled)
        window_set_state(button, button_state::unpressed);
    else if (value == screens::widget_value::disabled && !disabled)
        window_set_state(button, button_state::disabled);
}

/// Gives the Main Frame's green layer the slide steps APPLY stored, for its next slide.
///
/// @param user the Main Frame
/// @param steps new slide steps
void set_slide_steps(void* user, int32_t steps) {
    auto* frame = static_cast<MainFrame*>(user);
    if (frame->console != nullptr)
        frame->console->slide_steps = steps;
}

/// Stops the playing sounds as APPLY turns the background sound off, and plays it as APPLY turns it on.
///
/// @param user the Main Frame
/// @param on whether the background sound is now on
void set_background_sound(void* user, bool on) {
    const StatusWidget* owner = static_cast<MainFrame*>(user)->owner;
    if (on)
        console_play_background(owner);
    else
        console_stop_sounds(owner);
}

/// Builds the Profile console hidden when its bounds are set.
///
/// Adds the shared Profile console image, reads Profile.def for the category
/// names, the PROFILE title, the ProfileList with a row per category, the
/// five description lines, APPLY and RESET (both disabled) and the hidden
/// status line; then the console's model selects the account tab and asks
/// for the player's own record, which the console's model holds.
///
/// @param self the Profile console
void profile_build(Window* self) {
    ProfileView* view = as_profile(self);
    StatusWidget* owner = view->owner;
    window_set_state_image(self, k_image_state, owner->system.profile_image);
    screens::BoneyardsScreens* nav = owner->navigation;
    const screens::HatFonts& fonts = owner->system.hat_fonts;
    const ProfileTemplates* templates = screens::profile_read_templates(&nav->service);
    screens::lobby_label_create(
        self, 0, k_title_y, "PROFILE", fonts.big, k_title_alignment, 0, k_title_width
    );
    view->list = screens::by_list_box_create(false, false);
    window_create(
        &view->list->list.base,
        "ProfileList",
        k_list_x,
        k_list_y,
        k_list_width,
        k_list_height,
        self,
        true
    );
    for (screens::ProfileTab tab : k_row_tabs) {
        const auto index = static_cast<std::size_t>(tab);
        const char* name = templates != nullptr ? templates->category_names[index] : nullptr;
        screens::by_circle_row_create(
            view->list, name != nullptr ? name : "", static_cast<int32_t>(tab), &fonts
        );
    }
    for (int32_t i = 0; i < k_profile_description_lines; ++i) {
        view->description[i] = screens::lobby_label_create(
            self,
            k_description_x,
            k_description_y + i * k_description_height,
            "",
            fonts.small,
            k_description_alignment,
            0,
            k_description_width,
            k_description_height
        );
    }
    view->apply = button_create();
    screens::function_button_setup(view->apply, false, k_buttons_x, k_apply_y, "APPLY", self);
    window_set_state(view->apply, button_state::disabled);
    view->reset = button_create();
    screens::function_button_setup(view->reset, false, k_buttons_x, k_reset_y, "RESET", self);
    window_set_state(view->reset, button_state::disabled);
    view->status = screens::lobby_label_create(self, k_status_x, k_status_y, "", fonts.big, 0, 0);
    view->status->visible = 0;
    view->description_tab = -1;
    screens::boneyards_open(nav, screens::ByScreen::profile_console);
    nav->profile.slide_user = view->frame;
    nav->profile.set_slide_steps = set_slide_steps;
    nav->profile.sound_user = view->frame;
    nav->profile.background_sound = set_background_sound;
    follow_model(view);
    self->visible = 0;
}

/// Follows the model while shown (the tabs' windows included) and enables APPLY and RESET only while
/// a tab is dirty and nothing is in transit.
///
/// @param self the Profile console
void profile_pulse(Window* self) {
    if (!window_is_shown(self))
        return;
    ProfileView* view = as_profile(self);
    follow_model(view);
    screens::ProfileConsole* model = model_of(view);
    screens::profile_update_buttons(model);
    follow_value(view->apply, model->apply_value);
    follow_value(view->reset, model->reset_value);
}

/// Handles the ProfileList, RESET, APPLY and CHANGE.
///
/// What the tabs' windows hold reaches the model first. A category row
/// picked in the ProfileList sounds GREEN_BUTTON and switches to its tab;
/// RESET refreshes every created tab from the record; APPLY submits the
/// edits and selects the text of an entry it rejects; CHANGE shows the
/// password entries and gives the first the focus.
///
/// @param self the Profile console
/// @param notify control notice
void profile_on_notify(Window* self, const Notify* notify) {
    ProfileView* view = as_profile(self);
    screens::ProfileConsole* model = model_of(view);
    ProfileTabWindows* tabs = tabs_of(view);
    profile_tabs_read(tabs, model);
    if (profile_tabs_on_notify(tabs, model, notify)) {
        follow_model(view);
        return;
    }
    if (notify->source == &view->list->list.base) {
        const Window* row = view->list->selected_item;
        if (row == nullptr || !row->ops->is_class(screens::kByCircleRowClass))
            return;
        play_sound(view, k_list_sound);
        screens::profile_on_control(
            model,
            screens::ProfileControl::tab_list,
            reinterpret_cast<const screens::ByCircleRow*>(row)->tab
        );
    } else if (notify->source == view->reset) {
        screens::profile_on_control(model, screens::ProfileControl::reset_button, 0);
    } else if (notify->source == view->apply) {
        screens::profile_on_control(model, screens::ProfileControl::apply_button, 0);
        follow_model(view);
        profile_tabs_select_rejected(tabs, model);
        return;
    }
    follow_model(view);
}

/// Releases the two Insignia frames, then the window.
///
/// @param self the Profile console
void profile_destroy(Window* self) {
    ProfileView* view = as_profile(self);
    image_release(view->arm_insignia);
    image_release(view->core_insignia);
    console_window_free(self);
}

WindowOps make_profile_ops() {
    WindowOps ops = console_panel_ops();
    ops.destroy = profile_destroy;
    ops.on_bounds = profile_build;
    ops.pulse = profile_pulse;
    ops.on_notify = profile_on_notify;
    return ops;
}

const WindowOps k_profile_ops = make_profile_ops();

} // namespace

ProfileTabWindows* profile_console_tabs(Window* console) {
    return tabs_of(as_profile(console));
}

Handle profile_image_create() {
    const Handle image =
        image_create_blank(k_image_width, k_image_height, screens::kWidgetKey, screens::kWidgetKey);
    Surface surface{};
    if (image_surface(image, &surface))
        (void)present::draw_clipped_line(
            &surface, k_rule_x, k_rule_top, k_rule_x, k_rule_bottom, k_rule_color
        );
    return image;
}

void profile_console_open(Window* self) {
    ProfileView* view = as_profile(self);
    ConsoleNavButton* button = profile_button_of(view);
    window_set_state_image(&button->base, button_state::unpressed, button->lit);
    follow_model(view);
    self->visible = 1;
}

void profile_console_close(Window* self) {
    console_screen_close(self, as_profile(self)->profile_button);
}

Window* profile_console_create(
    StatusWidget* owner, Window* frame, int32_t x, int32_t y, int32_t width, int32_t height
) {
    auto* view = as_profile(console_window_alloc(sizeof(ProfileConsoleRecord), &k_profile_ops));
    auto* main_frame = reinterpret_cast<MainFrame*>(frame);
    view->frame = main_frame;
    view->profile_button =
        main_frame->console_buttons[static_cast<int32_t>(ConsoleButton::profile)];
    view->owner = owner;
    view->arm_insignia = image_load(
        screens::kWidgetsGaf, k_insignia_sequence, screens::insignia_frame::arm, screens::kWidgetKey
    );
    view->core_insignia = image_load(
        screens::kWidgetsGaf,
        k_insignia_sequence,
        screens::insignia_frame::core,
        screens::kWidgetKey
    );
    window_create(&view->base, k_console_name, x, y, width, height, frame, true);
    return &view->base;
}

} // namespace oa::boneyards::console

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battle console's matchmaking panel for accounts with matchmaking: a
// strip along the bottom of the console that shows a minimized, an active
// or a deactivated face. Its buttons switch between the faces. SETTINGS
// flips a flag, passes the press on to the console and opens the Matchmaking
// dialog; ACTIVATE, leaving the active face, opens the dialog's searching face.
#include "battle_console.hpp"
#include "oa/boneyards/console/feature_consoles.hpp"
#include "oa/boneyards/release.hpp"
#include "oa/present/raster.hpp"
#include "oa/present/surface.hpp"
#include "widgets.hpp"

namespace oa::boneyards::console {

namespace {

constexpr const char* k_panel_name = "MatchControlPanel";
constexpr const char* k_green_button = "GREEN_BUTTON";
constexpr const char* k_minimize = "Minimize_Button";
constexpr const char* k_minimize_open = "Minimize_Button_180";
constexpr int32_t k_image_state = 1;

namespace layout {
constexpr int32_t width = 0x7b;
// Each face's top, from the console's top; the panel reaches the console's
// bottom.
constexpr int32_t face_top[3] = {0x123, 0x10b, 0xd6};
constexpr uint8_t rule_color = 0x5d;
constexpr int32_t title_y = 4, title_width = 0x6c;
constexpr int32_t minimize_x = 0x6c, minimize_y = 4, minimize_width = 0xd, minimize_height = 0xc;
constexpr int32_t activate_x = 3, settings_x = 0x3f, buttons_y = 0x48;
constexpr int32_t content_y = 0x12, content_height = 0x33;
} // namespace layout

constexpr int32_t k_align_top_centre = 1;

MatchControlPanel* as_panel(Window* w) {
    return reinterpret_cast<MatchControlPanel*>(w);
}

void play_sound(const MatchControlPanel* panel, const char* name) {
    const MainMenuHost& host = panel->owner->config.host;
    if (host.play_sound != nullptr)
        host.play_sound(host.user, name);
}

/// Handles the panel's buttons, each sounding GREEN_BUTTON first.
///
/// The minimize button shows the active face from the minimized one (the
/// state stays minimized), and the minimized face from the others (a
/// deactivated state becomes active). ACTIVATE and DEACTIVATE move between
/// the active and deactivated faces. ACTIVATE, leaving the active face, opens
/// the Matchmaking dialog on its searching face. SETTINGS, except
/// minimized, flips the state's flag, passes the press on to the console
/// with the flag and opens the dialog on its settings face. An account
/// that does not see Matchmaking keeps the faces and opens no dialog.
///
/// @param self the matchmaking panel
/// @param notify control notice; only presses (activated) count
void match_control_on_notify(Window* self, const Notify* notify) {
    if (notify == nullptr || notify->code != NotifyCode::activated)
        return;
    MatchControlPanel* panel = as_panel(self);
    play_sound(panel, k_green_button);
    screens::MatchmakingState* state = panel->state;
    if (notify->source == panel->minimize) {
        if (state->face == match_face::minimized) {
            match_control_panel_show(panel, match_face::active);
            return;
        }
        if (state->face == match_face::deactivated)
            state->face = match_face::active;
        match_control_panel_show(panel, match_face::minimized);
        return;
    }
    if (notify->source == panel->settings) {
        if (state->face == match_face::minimized)
            return;
        state->settings_flag = !state->settings_flag;
        if (self->parent != nullptr) {
            Notify forwarded{notify->code, self, state->settings_flag ? 1 : 0, nullptr};
            self->parent->ops->on_notify(self->parent, &forwarded);
        }
        if (gated_feature_shown(
                ReleaseFeature::matchmaking, panel->owner->system.matchmaking_enabled
            ))
            matchmaking_dialog_open(panel->owner, k_matchmaking_settings);
        return;
    }
    if (notify->source == panel->activate) {
        if (state->face == match_face::active) {
            state->face = match_face::deactivated;
            match_control_panel_show(panel, match_face::deactivated);
            if (gated_feature_shown(
                    ReleaseFeature::matchmaking, panel->owner->system.matchmaking_enabled
                ))
                matchmaking_dialog_open(panel->owner, k_matchmaking_searching);
        } else if (state->face == match_face::deactivated) {
            state->face = match_face::active;
            match_control_panel_show(panel, match_face::active);
        }
    }
}

/// Builds the panel when its bounds are set: MATCHMAKING centred over all but the minimize button,
/// the minimize button, DEACTIVATE and SETTINGS (small, enabled) and the content list under the
/// title; then the face the state keeps.
///
/// @param self the matchmaking panel
void match_control_build(Window* self) {
    namespace l = layout;
    MatchControlPanel* panel = as_panel(self);
    const screens::HatFonts& f = panel->owner->system.hat_fonts;
    panel->title = screens::lobby_label_create(
        self, 0, l::title_y, "MATCHMAKING", f.big, k_align_top_centre, 0, l::title_width
    );
    panel->minimize = button_create();
    window_set_bounds(
        panel->minimize,
        self->x + l::minimize_x,
        self->y + l::minimize_y,
        l::minimize_width,
        l::minimize_height,
        self,
        true
    );
    panel->activate = button_create();
    screens::function_button_setup(
        panel->activate, true, l::activate_x, l::buttons_y, "DEACTIVATE", self
    );
    window_set_state(panel->activate, button_state::unpressed);
    panel->settings = button_create();
    screens::function_button_setup(
        panel->settings, true, l::settings_x, l::buttons_y, "SETTINGS", self
    );
    window_set_state(panel->settings, button_state::unpressed);
    panel->content = screens::by_list_box_create(false, false);
    window_set_bounds(
        &panel->content->list.base,
        self->x,
        self->y + l::content_y,
        self->width,
        l::content_height,
        self,
        true
    );
    int32_t face = panel->state->face;
    if (face < match_face::minimized || face > match_face::deactivated)
        face = match_face::minimized;
    match_control_panel_show(panel, face);
}

/// Releases the three face images, then the window.
///
/// @param self the matchmaking panel
void match_control_destroy(Window* self) {
    for (Handle image : as_panel(self)->images)
        image_release(image);
    console_window_free(self);
}

WindowOps make_match_control_ops() {
    WindowOps ops = console_panel_ops();
    ops.destroy = match_control_destroy;
    ops.on_bounds = match_control_build;
    ops.on_notify = match_control_on_notify;
    return ops;
}

const WindowOps k_match_control_ops = make_match_control_ops();

} // namespace

void match_control_panel_show(MatchControlPanel* panel, int32_t face) {
    namespace l = layout;
    Window* self = &panel->base;
    if (self->parent != nullptr)
        self->y = self->parent->y + l::face_top[face];
    window_set_height(self, panel->heights[face]);
    window_set_state_image(self, k_image_state, panel->images[face]);
    panel->title->y = self->y + l::title_y;
    panel->minimize->y = self->y + l::minimize_y;
    panel->minimize->ops->bind_images(
        panel->minimize,
        screens::kWidgetsGaf,
        face == match_face::minimized ? k_minimize : k_minimize_open,
        0,
        screens::kWidgetKey
    );
    switch (face) {
    case match_face::minimized:
        panel->activate->visible = 0;
        panel->settings->visible = 0;
        panel->content->list.base.visible = 0;
        break;
    case match_face::active:
        panel->settings->visible = 1;
        panel->activate->visible = 1;
        screens::window_set_four_state_texts(panel->activate, "ACTIVATE");
        panel->content->list.base.visible = 0;
        break;
    default:
        panel->settings->visible = 1;
        panel->activate->visible = 1;
        screens::window_set_four_state_texts(panel->activate, "DEACTIVATE");
        panel->content->list.base.visible = 1;
        break;
    }
}

MatchControlPanel*
match_control_panel_create(StatusWidget* owner, screens::MatchmakingState* state, Window* console) {
    namespace l = layout;
    auto* panel = as_panel(console_window_alloc(sizeof(MatchControlPanel), &k_match_control_ops));
    panel->owner = owner;
    panel->state = state;
    for (int32_t face = 0; face < 3; ++face) {
        panel->heights[face] = console->height - l::face_top[face];
        panel->images[face] = image_create_blank(
            l::width, panel->heights[face], screens::kWidgetKey, screens::kWidgetKey
        );
        Surface surface{};
        if (image_surface(panel->images[face], &surface))
            (void)present::draw_clipped_line(&surface, 0, 0, l::width, 0, l::rule_color);
    }
    const int32_t deactivated = match_face::deactivated;
    window_create(
        &panel->base,
        k_panel_name,
        console->x,
        console->y + l::face_top[deactivated],
        l::width,
        panel->heights[deactivated],
        console,
        true
    );
    return panel;
}

} // namespace oa::boneyards::console

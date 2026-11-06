// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Profile console's category tabs: each tab's windows, made the first
// time the model creates the tab, filled from the model as it refreshes the
// tab, and read back into the model before it checks or applies them.
#include "oa/boneyards/utf8.hpp"

#include <cstring>

#include "profile_tabs_view.hpp"
#include "widgets.hpp"

namespace oa::boneyards::console {

namespace {

constexpr int32_t k_image_state = 1;
constexpr const char* k_user_icons = "User_Icons";

// Label placements (lobby_label_create).
constexpr int32_t k_align_top_left = 0;
constexpr int32_t k_align_left = 3;
constexpr int32_t k_align_right_middle = 5;
constexpr int32_t k_align_bottom_left = 6;
constexpr int32_t k_anchor_top_left = 0;
constexpr int32_t k_anchor_top_right = 1;
constexpr int32_t k_anchor_bottom_left = 2;

// A tab entry's caption ends 6 pixels left of the entry, or sits 6 pixels
// above it with its bottom-left corner there.
constexpr int32_t k_caption_gap = 6;
constexpr int32_t k_caption_height = 0x15;
constexpr int32_t k_value_height = 0x11;
constexpr char k_password_mask = '*';

namespace layout {
// Every tab's controls start this far in.
constexpr int32_t x = 0x9c;
// Account: the values 0xf4 in, their captions ending 6 left of them.
constexpr int32_t values_x = 0xf4, values_width = 0xf6;
constexpr int32_t login_y = 0x54, status_y = 0x65, email_y = 0x76;
constexpr int32_t password_y = 0x8c, confirm_y = 0xab, password_width = 0x6e;
// Personal.
constexpr int32_t tag_y = 0x3e, tag_width = 0x138;
constexpr int32_t core_contingency_y = 0x67, battle_tactics_y = 0x7c;
constexpr int32_t forum_y = 0xd2, icq_y = 0xa4, alias_width = 0x8c;
constexpr int32_t email_alias_y = 0xf3;
// Allegiance.
constexpr int32_t allegiance_x = 0xa8;
constexpr int32_t user_icon_y = 0x20, user_icon_size = 0xb;
constexpr int32_t user_name_x = 0xb5, user_name_width = 0x135, user_name_height = 0xb;
constexpr int32_t insignia_y = 0x30, insignia_size = 0x20;
constexpr int32_t side_x = 0xcc, side_y = 0x30, rank_y = 0x40, side_width = 0x11e;
constexpr int32_t side_height = 0x10;
constexpr int32_t join_y = 0x3a;
constexpr int32_t arm_gap = 4;  // past the "Join Forces of:" caption, from 0xa8
constexpr int32_t icon_gap = 4; // the insignia past its radio
constexpr int32_t core_gap = 5; // the Core radio past the Arm insignia
constexpr int32_t page_y = 0x5a, page_width = 0x142, page_height = 0xd7;
// Game preferences.
constexpr int32_t skill_y = 0x24, players_y = 0x48, death_y = 0x6c;
constexpr int32_t map_y = 0x9e, map_width = 0xaa;
// Ladder and awards pages.
constexpr int32_t report_y = 0x1e, report_width = 0x138, report_height = 0xfd;
// Local settings.
constexpr int32_t unit_limit_y = 0x56, slide_y = 0x8f, number_width = 0x2a;
constexpr int32_t sound_y = 0xba;
} // namespace layout

// Characters the tab entries take.
namespace entry_length {
constexpr int32_t password = 0xf;
constexpr int32_t tag_line = 0xff;
constexpr int32_t forum_alias = 0xf;
constexpr int32_t icq = 0xd;
constexpr int32_t favorite_map = 0x3f;
constexpr int32_t unit_limit = 3;
constexpr int32_t slide_steps = 2;
} // namespace entry_length

// A tab entry: its caption in the medium font, above it or ending left of
// it, and a framed scrolling entry in the small font.
struct TabEntry {
    Window* caption;
    SingleEdit* entry;
};

/// Builds a tab entry.
///
/// @param parent the Profile console
/// @param fonts the Hat fonts
/// @param caption caption text
/// @param x entry's left edge in the console
/// @param y entry's top edge in the console
/// @param width entry width
/// @param max_length characters the entry takes
/// @param masked whether the entry echoes '*'
/// @param above whether the caption sits above the entry rather than left of it
/// @return the caption and the entry
TabEntry tab_entry(
    Window* parent,
    const screens::HatFonts& fonts,
    const char* caption,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t max_length,
    bool masked,
    bool above
) {
    TabEntry made{};
    if (above)
        made.caption = screens::lobby_label_create(
            parent,
            x,
            y - k_caption_gap,
            caption,
            fonts.medium,
            k_align_bottom_left,
            k_anchor_bottom_left,
            0,
            k_caption_height
        );
    else
        made.caption = screens::lobby_label_create(
            parent,
            x - k_caption_gap,
            y,
            caption,
            fonts.medium,
            k_align_right_middle,
            k_anchor_top_right,
            0,
            k_caption_height
        );
    made.entry = screens::lobby_edit_create(parent, x, y, width, fonts.small, max_length, true);
    if (caption != nullptr && caption[0] != '\0')
        window_set_name(&made.entry->base, caption);
    made.entry->scroll = 1;
    if (masked)
        made.entry->mask = k_password_mask;
    return made;
}

/// Adds a caption in the medium font ending 6 pixels left of x and an empty
/// value label of the given width at x, both 0x11 high.
///
/// @param parent the tab window receiving both labels
/// @param fonts the console fonts
/// @param caption the caption's text
/// @param x the value label's left edge, in pixels
/// @param y both labels' top edge, in pixels
/// @param width the value label's width, in pixels
/// @param[out] caption_out receives the caption label
/// @param[out] value_out receives the value label
void value_labels(
    Window* parent,
    const screens::HatFonts& fonts,
    const char* caption,
    int32_t x,
    int32_t y,
    int32_t width,
    Window** caption_out,
    Window** value_out
) {
    *caption_out = screens::lobby_label_create(
        parent,
        x - k_caption_gap,
        y,
        caption,
        fonts.medium,
        k_align_right_middle,
        k_anchor_top_right,
        0,
        k_value_height
    );
    *value_out = screens::lobby_label_create(
        parent, x, y, "", fonts.small, k_align_left, k_anchor_top_left, width, k_value_height
    );
}

Window* icon_create(Window* parent, int32_t x, int32_t y, int32_t size) {
    Window* icon = static_create();
    window_set_bounds(icon, x, y, size, size, parent, true);
    return icon;
}

void play_group_sound(void* user, const char* name) {
    const MainMenuHost& host = static_cast<StatusWidget*>(user)->config.host;
    if (host.play_sound != nullptr)
        host.play_sound(host.user, name);
}

screens::ByRadioGroup* radio_group(
    Window* parent,
    StatusWidget* owner,
    int32_t y,
    const char* caption,
    const screens::RadioGroup& model
) {
    const char* names[screens::kLobbyRadioMembers]{};
    int32_t count =
        model.count < screens::kLobbyRadioMembers ? model.count : screens::kLobbyRadioMembers;
    for (int32_t i = 0; i < count; ++i)
        names[i] = model.members[i].name;
    screens::ByRadioGroup* group = screens::by_radio_group_create(
        parent, layout::x, y, caption, names, count, &owner->system.hat_fonts
    );
    group->play_sound = play_group_sound;
    group->sound_user = owner;
    return group;
}

// ---- building ---------------------------------------------------------------

void build_account(ProfileTabWindows* w, Window* self, const screens::HatFonts& f) {
    namespace l = layout;
    value_labels(
        self,
        f,
        "Login Name:",
        l::values_x,
        l::login_y,
        l::values_width,
        &w->login_caption,
        &w->login_name
    );
    value_labels(
        self,
        f,
        "Status:",
        l::values_x,
        l::status_y,
        l::values_width,
        &w->status_caption,
        &w->status
    );
    value_labels(
        self,
        f,
        "Email Address:",
        l::values_x,
        l::email_y,
        l::values_width,
        &w->email_caption,
        &w->email
    );
    const TabEntry password = tab_entry(
        self,
        f,
        "Password:",
        l::values_x,
        l::password_y,
        l::password_width,
        entry_length::password,
        true,
        false
    );
    w->password_caption = password.caption;
    w->password = password.entry;
    const TabEntry confirm = tab_entry(
        self,
        f,
        "Confirm:",
        l::values_x,
        l::confirm_y,
        l::password_width,
        entry_length::password,
        true,
        false
    );
    w->confirm_caption = confirm.caption;
    w->confirm = confirm.entry;
    w->change = button_create();
    screens::function_button_setup(w->change, false, l::values_x, l::password_y, "CHANGE", self);
}

void build_personal(ProfileTabWindows* w, Window* self, const screens::HatFonts& f) {
    namespace l = layout;
    const TabEntry tag = tab_entry(
        self, f, "Tag Line:", l::x, l::tag_y, l::tag_width, entry_length::tag_line, false, true
    );
    w->tag_caption = tag.caption;
    w->tag_line = tag.entry;
    w->core_contingency = screens::lobby_check_box_create(
        self, l::x, l::core_contingency_y, " Core Contingency Installed", f.small_dim
    );
    w->battle_tactics = screens::lobby_check_box_create(
        self, l::x, l::battle_tactics_y, " Battle Tactics Installed", f.small_dim
    );
    const TabEntry forum = tab_entry(
        self,
        f,
        "TA Forum Alias:",
        l::x,
        l::forum_y,
        l::alias_width,
        entry_length::forum_alias,
        false,
        true
    );
    w->forum_caption = forum.caption;
    w->forum_alias = forum.entry;
    const TabEntry icq = tab_entry(
        self, f, "ICQ Number:", l::x, l::icq_y, l::alias_width, entry_length::icq, false, true
    );
    w->icq_caption = icq.caption;
    w->icq = icq.entry;
    w->email_alias = screens::lobby_check_box_create(
        self, l::x, l::email_alias_y, " Display Email Alias", f.small_dim
    );
}

/// Builds the allegiance tab.
///
/// @quirk The Core radio is placed by a screen position taken as a position
///     in the console, so it and its insignia sit as far right of the Arm
///     insignia again as the console is from the screen's left edge.
void build_allegiance(ProfileTabWindows* w, ProfileView* view, const screens::HatFonts& f) {
    namespace l = layout;
    Window* self = &view->base;
    w->user_icon =
        icon_create(self, self->x + l::allegiance_x, self->y + l::user_icon_y, l::user_icon_size);
    w->user_name = screens::lobby_label_create(
        self,
        l::user_name_x,
        l::user_icon_y,
        "",
        f.small_light,
        k_align_left,
        k_anchor_top_left,
        l::user_name_width,
        l::user_name_height
    );
    w->insignia =
        icon_create(self, self->x + l::allegiance_x, self->y + l::insignia_y, l::insignia_size);
    w->side = screens::lobby_label_create(
        self,
        l::side_x,
        l::side_y,
        "",
        f.small,
        k_align_left,
        k_anchor_top_left,
        l::side_width,
        l::side_height
    );
    w->rank = screens::lobby_label_create(
        self,
        l::side_x,
        l::rank_y,
        "",
        f.small,
        k_align_left,
        k_anchor_top_left,
        l::side_width,
        l::side_height
    );
    w->join_caption = screens::lobby_label_create(
        self,
        l::allegiance_x,
        l::join_y,
        "Join Forces of:  ",
        f.button_hover,
        k_align_top_left,
        k_anchor_top_left
    );
    w->arm = screens::lobby_radio_create(
        self, w->join_caption->width + l::allegiance_x + l::arm_gap, l::join_y, "Arm", f.small_blue
    );
    Window* arm = &w->arm->base;
    w->arm_insignia = icon_create(
        self, arm->x + arm->width + l::icon_gap, self->y + l::insignia_y, l::insignia_size
    );
    window_set_state_image(w->arm_insignia, k_image_state, view->arm_insignia);
    window_set_state_image(w->arm_insignia, 0, view->arm_insignia);
    w->core = screens::lobby_radio_create(
        self,
        w->arm_insignia->x + w->arm_insignia->width + l::core_gap,
        l::join_y,
        "Core",
        f.small_red
    );
    Window* core = &w->core->base;
    w->core_insignia = icon_create(
        self, core->x + core->width + l::icon_gap, self->y + l::insignia_y, l::insignia_size
    );
    window_set_state_image(w->core_insignia, k_image_state, view->core_insignia);
    window_set_state_image(w->core_insignia, 0, view->core_insignia);
    screens::lobby_radio_add_member(w->core, arm);
    screens::lobby_radio_add_member(w->arm, core);
    w->allegiance_page = html_view_create(
        view->owner, self, l::allegiance_x, l::page_y, l::page_width, l::page_height
    );
    w->user_icon_frame = -1;
}

void build_preferences(
    ProfileTabWindows* w, Window* self, StatusWidget* owner, const screens::ProfileTabControls& t
) {
    namespace l = layout;
    w->skill = radio_group(self, owner, l::skill_y, "Player Skill:", t.skill);
    w->players = radio_group(self, owner, l::players_y, "Number of Players:", t.players);
    w->commander_death =
        radio_group(self, owner, l::death_y, "On Commander Death:", t.commander_death);
    const TabEntry map = tab_entry(
        self,
        owner->system.hat_fonts,
        "Favorite Map:",
        l::x,
        l::map_y,
        l::map_width,
        entry_length::favorite_map,
        false,
        true
    );
    w->map_caption = map.caption;
    w->favorite_map = map.entry;
}

void build_local_settings(ProfileTabWindows* w, Window* self, const screens::HatFonts& f) {
    namespace l = layout;
    const TabEntry unit = tab_entry(
        self,
        f,
        "Max Unit Limit: (21 - 500)",
        l::x,
        l::unit_limit_y,
        l::number_width,
        entry_length::unit_limit,
        false,
        true
    );
    w->unit_limit_caption = unit.caption;
    w->unit_limit = unit.entry;
    const TabEntry slide = tab_entry(
        self,
        f,
        "Boneyards Console Slide Steps: (1 - 40)",
        l::x,
        l::slide_y,
        l::number_width,
        entry_length::slide_steps,
        false,
        true
    );
    w->slide_caption = slide.caption;
    w->slide_steps = slide.entry;
    w->sound_off = screens::lobby_check_box_create(
        self, l::x, l::sound_y, " Disable BoneYards Background Sound", f.small_dim
    );
}

/// Builds a tab's windows, the way the tab's controls are first created.
///
/// @param[in,out] w the tab windows
/// @param[in,out] view the Profile console
/// @param model the console's model
/// @param tab ProfileTab index
void build_tab(
    ProfileTabWindows* w, ProfileView* view, const screens::ProfileConsole& model, int32_t tab
) {
    Window* self = &view->base;
    const screens::HatFonts& f = view->owner->system.hat_fonts;
    switch (static_cast<screens::ProfileTab>(tab)) {
    case screens::ProfileTab::account:
        build_account(w, self, f);
        break;
    case screens::ProfileTab::personal:
        build_personal(w, self, f);
        break;
    case screens::ProfileTab::allegiance:
        build_allegiance(w, view, f);
        break;
    case screens::ProfileTab::game_preferences:
        build_preferences(w, self, view->owner, model.tabs);
        break;
    case screens::ProfileTab::ladder:
        w->ladder_page = html_view_create(
            view->owner,
            self,
            layout::x,
            layout::report_y,
            layout::report_width,
            layout::report_height
        );
        break;
    case screens::ProfileTab::awards:
        w->awards_page = html_view_create(
            view->owner,
            self,
            layout::x,
            layout::report_y,
            layout::report_width,
            layout::report_height
        );
        break;
    case screens::ProfileTab::local_settings:
        build_local_settings(w, self, f);
        break;
    }
    w->built[tab] = true;
}

// ---- showing the model ------------------------------------------------------

void set_label(Window* label, const char* text) {
    screens::lobby_label_set_text(label, text != nullptr ? text : "");
}

void set_state(Window* w, int32_t state) {
    if (w->states.current != state)
        window_set_state(w, state);
}

// An entry shows the model's text and value (0 disabled).
void show_entry(SingleEdit* edit, const screens::ProfileEntry& entry) {
    if (std::strcmp(single_edit_text(edit), entry.text) != 0)
        single_edit_set_text(edit, entry.text);
    set_state(&edit->base, entry.value);
}

void show_number(SingleEdit* edit, const char* text) {
    if (std::strcmp(single_edit_text(edit), text) != 0)
        single_edit_set_text(edit, text);
}

Handle side_font(const screens::HatFonts& fonts, screens::SideColor color) {
    switch (color) {
    case screens::SideColor::arm:
        return fonts.small_blue;
    case screens::SideColor::core:
        return fonts.small_red;
    case screens::SideColor::no_side_pane:
        return fonts.small;
    case screens::SideColor::no_side_tab:
        return fonts.small_light;
    }
    return fonts.small;
}

void show_page(HtmlView* page, const char* html) {
    html_view_set_text(page, html != nullptr ? html : screens::kDefaultReportHtml);
}

/// Shows what the model holds for a tab in its windows.
///
/// @param[in,out] w the tab windows
/// @param view the Profile console
/// @param model the console's model
/// @param tab ProfileTab index
void show_tab(
    ProfileTabWindows* w, const ProfileView* view, const screens::ProfileConsole& model, int32_t tab
) {
    const screens::ProfileTabControls& t = model.tabs;
    const screens::HatFonts& f = view->owner->system.hat_fonts;
    switch (static_cast<screens::ProfileTab>(tab)) {
    case screens::ProfileTab::account:
        set_label(w->login_name, t.login_name);
        set_label(w->status, t.status);
        set_label(w->email, t.email);
        show_entry(w->password, t.password);
        show_entry(w->confirm, t.confirm);
        set_state(w->change, t.change_value);
        break;
    case screens::ProfileTab::personal:
        show_entry(w->tag_line, t.tag_line);
        set_state(&w->core_contingency->base, t.core_contingency_value);
        set_state(&w->battle_tactics->base, t.battle_tactics_value);
        show_entry(w->forum_alias, t.forum_alias);
        show_entry(w->icq, t.icq);
        set_state(&w->email_alias->base, t.display_email_alias_value);
        break;
    case screens::ProfileTab::allegiance: {
        if (t.user_icon != w->user_icon_frame) {
            console_bind_images(
                w->user_icon, screens::kWidgetsGaf, k_user_icons, t.user_icon, screens::kWidgetKey
            );
            w->user_icon_frame = t.user_icon;
        }
        set_label(w->user_name, t.user_name);
        const Handle font = side_font(f, t.name_color);
        window_set_font_handle(w->user_name, font);
        Handle logo = k_no_handle;
        if (t.logo_frame == screens::insignia_frame::arm)
            logo = view->arm_insignia;
        else if (t.logo_frame == screens::insignia_frame::core)
            logo = view->core_insignia;
        window_set_state_image(w->insignia, k_image_state, logo);
        if (t.name_color == screens::SideColor::arm || t.name_color == screens::SideColor::core) {
            window_set_font_handle(w->side, font);
            window_set_font_handle(w->rank, font);
        }
        set_label(w->side, t.side_name);
        set_label(w->rank, t.rank);
        set_state(&w->arm->base, t.arm_value);
        set_state(&w->core->base, t.core_value);
        set_state(w->arm_insignia, t.arm_value == screens::widget_value::disabled ? 0 : 1);
        set_state(w->core_insignia, t.core_value == screens::widget_value::disabled ? 0 : 1);
        show_page(w->allegiance_page, t.allegiance_html);
        break;
    }
    case screens::ProfileTab::game_preferences:
        screens::by_radio_group_show(w->skill, t.skill);
        screens::by_radio_group_show(w->players, t.players);
        screens::by_radio_group_show(w->commander_death, t.commander_death);
        show_entry(w->favorite_map, t.favorite_map);
        break;
    case screens::ProfileTab::ladder:
        show_page(w->ladder_page, t.ladder_html);
        break;
    case screens::ProfileTab::awards:
        show_page(w->awards_page, t.awards_html);
        break;
    case screens::ProfileTab::local_settings:
        show_number(w->unit_limit, model.unit_limit_text);
        show_number(w->slide_steps, model.slide_steps_text);
        set_state(
            &w->sound_off->base,
            model.sound_off_checked ? check_state::checked_up : check_state::unchecked_up
        );
        break;
    }
}

void show_window(Window* w, bool shown) {
    if (w != nullptr)
        w->visible = shown ? 1 : 0;
}

/// Shows or hides a built tab's windows as the model shows its tab and controls.
///
/// @param[in,out] w the tab windows
/// @param model the console's model
/// @param tab ProfileTab index
void show_visibility(ProfileTabWindows* w, const screens::ProfileConsole& model, int32_t tab) {
    const bool shown = model.tab_visible[tab];
    const screens::ProfileTabControls& t = model.tabs;
    switch (static_cast<screens::ProfileTab>(tab)) {
    case screens::ProfileTab::account:
        for (Window* window :
             {w->login_caption,
              w->login_name,
              w->status_caption,
              w->status,
              w->email_caption,
              w->email,
              w->password_caption})
            show_window(window, shown);
        show_window(&w->password->base, shown && t.password.shown);
        show_window(w->confirm_caption, shown && t.confirm.shown);
        show_window(&w->confirm->base, shown && t.confirm.shown);
        show_window(w->change, shown && t.change_shown);
        break;
    case screens::ProfileTab::personal:
        for (Window* window :
             {w->tag_caption,
              &w->tag_line->base,
              &w->core_contingency->base,
              &w->battle_tactics->base,
              w->forum_caption,
              &w->forum_alias->base,
              w->icq_caption,
              &w->icq->base,
              &w->email_alias->base})
            show_window(window, shown);
        break;
    case screens::ProfileTab::allegiance:
        show_window(w->user_icon, shown);
        show_window(w->user_name, shown);
        show_window(w->insignia, shown && t.logo_shown);
        show_window(w->side, shown && t.side_shown);
        show_window(w->rank, shown && t.rank_shown);
        show_window(w->join_caption, shown && t.join_shown);
        for (Window* window : {&w->arm->base, w->arm_insignia, &w->core->base, w->core_insignia})
            show_window(window, shown && t.radios_shown);
        show_window(&w->allegiance_page->base, shown);
        break;
    case screens::ProfileTab::game_preferences:
        for (Window* window :
             {&w->skill->base,
              &w->players->base,
              &w->commander_death->base,
              w->map_caption,
              &w->favorite_map->base})
            show_window(window, shown);
        break;
    case screens::ProfileTab::ladder:
        show_window(&w->ladder_page->base, shown);
        break;
    case screens::ProfileTab::awards:
        show_window(&w->awards_page->base, shown);
        break;
    case screens::ProfileTab::local_settings:
        for (Window* window :
             {w->unit_limit_caption,
              &w->unit_limit->base,
              w->slide_caption,
              &w->slide_steps->base,
              &w->sound_off->base})
            show_window(window, shown);
        break;
    }
}

void read_entry(const SingleEdit* edit, screens::ProfileEntry* entry) {
    oa::boneyards::utf8::copy(
        entry->text, sizeof entry->text, single_edit_text(edit), entry->max_length
    );
}

void read_text(const SingleEdit* edit, char* out, std::size_t capacity) {
    oa::boneyards::utf8::copy(out, capacity, single_edit_text(edit), 0);
}

} // namespace

void profile_tabs_read(const ProfileTabWindows* w, screens::ProfileConsole* model) {
    screens::ProfileTabControls& t = model->tabs;
    for (int32_t tab = 0; tab < static_cast<int32_t>(screens::kProfileTabCount); ++tab) {
        if (!w->built[tab])
            continue;
        switch (static_cast<screens::ProfileTab>(tab)) {
        case screens::ProfileTab::account:
            read_entry(w->password, &t.password);
            read_entry(w->confirm, &t.confirm);
            break;
        case screens::ProfileTab::personal:
            read_entry(w->tag_line, &t.tag_line);
            t.core_contingency_value = w->core_contingency->base.states.current;
            t.battle_tactics_value = w->battle_tactics->base.states.current;
            read_entry(w->forum_alias, &t.forum_alias);
            read_entry(w->icq, &t.icq);
            t.display_email_alias_value = w->email_alias->base.states.current;
            break;
        case screens::ProfileTab::allegiance:
            t.arm_value = w->arm->base.states.current;
            t.core_value = w->core->base.states.current;
            break;
        case screens::ProfileTab::game_preferences:
            screens::by_radio_group_read(w->skill, &t.skill);
            screens::by_radio_group_read(w->players, &t.players);
            screens::by_radio_group_read(w->commander_death, &t.commander_death);
            read_entry(w->favorite_map, &t.favorite_map);
            break;
        case screens::ProfileTab::ladder:
        case screens::ProfileTab::awards:
            break;
        case screens::ProfileTab::local_settings:
            read_text(w->unit_limit, model->unit_limit_text, sizeof model->unit_limit_text);
            read_text(w->slide_steps, model->slide_steps_text, sizeof model->slide_steps_text);
            model->sound_off_checked = w->sound_off->base.states.current == check_state::checked_up;
            break;
        }
    }
}

void profile_tabs_follow(
    ProfileTabWindows* windows, ProfileView* console, screens::ProfileConsole* model
) {
    for (int32_t tab = 0; tab < static_cast<int32_t>(screens::kProfileTabCount); ++tab) {
        if (model->tab_created[tab] && !windows->built[tab]) {
            build_tab(windows, console, *model, tab);
            windows->shown_revision[tab] = model->tab_revision[tab] - 1;
        }
        if (!windows->built[tab])
            continue;
        if (windows->shown_revision[tab] != model->tab_revision[tab]) {
            show_tab(windows, console, *model, tab);
            windows->shown_revision[tab] = model->tab_revision[tab];
        }
    }
    profile_tabs_read(windows, model);
    for (int32_t tab = 0; tab < static_cast<int32_t>(screens::kProfileTabCount); ++tab)
        if (windows->built[tab])
            show_visibility(windows, *model, tab);
}

bool profile_tabs_on_notify(
    ProfileTabWindows* windows, screens::ProfileConsole* model, const Notify* notify
) {
    if (!windows->built[static_cast<int32_t>(screens::ProfileTab::account)] ||
        notify->source != windows->change)
        return false;
    screens::profile_on_control(model, screens::ProfileControl::change_password_button, 0);
    // The account tab does not take the keyboard when it opens. CHANGE is
    // the moment a password is typed.
    if (Desktop* desktop = services().desktop; desktop != nullptr)
        desktop_set_focus(desktop, &windows->password->base);
    return true;
}

void profile_tabs_select_rejected(
    const ProfileTabWindows* windows, const screens::ProfileConsole* model
) {
    SingleEdit* edit = nullptr;
    switch (model->selected_field) {
    case 0:
        edit = windows->unit_limit;
        break;
    case 1:
        edit = windows->slide_steps;
        break;
    case 2:
        edit = windows->confirm;
        break;
    default:
        return;
    }
    if (edit != nullptr)
        single_edit_select(edit, 0, edit->length);
}

} // namespace oa::boneyards::console

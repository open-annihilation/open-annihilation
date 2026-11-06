// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Chat console's player, create-frequency and frequency information
// panes. The Main Frame builds each once; the chat decides which is shown,
// and these windows show its fields and hand it what the player does.
#include "oa/boneyards/utf8.hpp"

#include <cstdio>
#include <cstring>

#include "console_state.hpp"
#include "html_view.hpp"
#include "oa/boneyards/screens/player_pane.hpp"
#include "oa/present/raster.hpp"
#include "oa/present/surface.hpp"
#include "widgets.hpp"

namespace oa::boneyards::console {

namespace {

constexpr const char* k_green_button = "GREEN_BUTTON";
constexpr const char* k_go_caption = "GO";

// Label placements (lobby_label_create).
constexpr int32_t k_align_left = 3;
constexpr int32_t k_align_centre = 4;
constexpr int32_t k_align_centre_vertically = 5;
constexpr int32_t k_anchor_top_left = 0;
constexpr int32_t k_anchor_top_right = 1;

namespace create_layout {
constexpr int32_t entry_x = 0x78;
constexpr int32_t caption_right = 0x75;
constexpr int32_t caption_height = 0x15;
constexpr int32_t name_y = 0x23, password_y = 0x3b, description_y = 0x53;
constexpr int32_t name_width = 0x64, password_width = 0x64, description_width = 0x96;
constexpr int32_t name_max = 0x17, password_max = 0xf, description_max = 0x7f;
constexpr int32_t go_x = 0x78, go_y = 0x6c;
} // namespace create_layout

namespace info_layout {
constexpr int32_t x = 6;
constexpr int32_t margin = 0xc; // the rows are the pane's width less this
constexpr int32_t row_height = 0x10;
constexpr int32_t name_y = 0x13, type_y = 0x23, creator_y = 0x33, population_y = 0x43;
constexpr int32_t description_y = 0x53; // then a row apart
constexpr int32_t password_y = 0x83, password_height = 0x15;
constexpr int32_t password_gap = 9; // between the caption and the entry
constexpr int32_t password_width = 0x6e, password_max = 0xf;
constexpr int32_t go_x = 0x100, go_y = 0x83;
} // namespace info_layout

namespace player_layout {
constexpr int32_t rank_icon_x = 5, rank_icon_y = 7, rank_icon_size = 0xb;
constexpr int32_t name_x = 0x12, name_y = 7, name_width = 0x53, name_height = 0xb;
constexpr int32_t rank_x = 5, rank_y = 0x18, rank_width = 0x60;
constexpr int32_t insignia_x = 5, insignia_y = 0x29, insignia_size = 0x20;
constexpr int32_t clan_x = 5, clan_y = 0x4c, clan_width = 0x60;
constexpr int32_t check_x = 5, whisper_lock_y = 0x63, muzzle_y = 0x73;
constexpr int32_t page_x = 5, page_y = 0x83;
constexpr int32_t tab_x[k_player_pane_tabs] = {0x67, 0xbc, 0x111};
constexpr int32_t tab_y = 3, tab_width = 0x55, tab_height = 0x13;
// The rule under the tabs, open under the chosen one.
constexpr int32_t tab_rule_y = 0x15;
constexpr int32_t html_x = 0x6b, html_y = 0x19, html_width = 0x108, html_height = 0x81;
constexpr uint8_t rule_color = 0x5d;
} // namespace player_layout

constexpr const char* k_initial_rank = "Supreme Commander";
constexpr const char* k_tab_captions[k_player_pane_tabs] = {"General", "Ladder", "Awards"};
constexpr const char* k_tab_names[k_player_pane_tabs] = {
    "GeneralTabBtn", "LadderTabBtn", "AwardsTabBtn"
};
constexpr screens::PaneControl k_tab_controls[k_player_pane_tabs] = {
    screens::PaneControl::general_tab,
    screens::PaneControl::ladder_tab,
    screens::PaneControl::awards_tab,
};
constexpr const char* k_insignia_sequence = "Insignia";
constexpr const char* k_user_icons_sequence = "User_Icons";
constexpr int32_t k_image_state = 1;

// Capacities of the texts GO reads from the entries: one field per character
// the entry accepts, wide enough for any character.
constexpr std::size_t k_name_capacity =
    oa::boneyards::utf8::bytes_for(static_cast<std::size_t>(create_layout::name_max));
constexpr std::size_t k_password_capacity =
    oa::boneyards::utf8::bytes_for(static_cast<std::size_t>(create_layout::password_max));
constexpr std::size_t k_description_capacity =
    oa::boneyards::utf8::bytes_for(static_cast<std::size_t>(create_layout::description_max));

CreateFrequencyPane* as_create(Window* w) {
    return reinterpret_cast<CreateFrequencyPane*>(w);
}

FrequencyInfoPane* as_info(Window* w) {
    return reinterpret_cast<FrequencyInfoPane*>(w);
}

PlayerPaneView* as_player(Window* w) {
    return reinterpret_cast<PlayerPaneView*>(w);
}

screens::ChatConsole* chat_of(const InfoPane* pane) {
    return &pane->owner->navigation->chat;
}

void play_sound(const InfoPane* pane, const char* name) {
    const MainMenuHost& host = pane->owner->config.host;
    if (host.play_sound != nullptr)
        host.play_sound(host.user, name);
}

void set_state(Window* w, int32_t state) {
    if (w->states.current != state)
        window_set_state(w, state);
}

void set_entry_text(SingleEdit* entry, const char* text) {
    entry->base.ops->set_caption(&entry->base, text, 0);
}

void copy_entry(char* out, std::size_t capacity, const SingleEdit* entry) {
    oa::boneyards::utf8::copy(out, capacity, single_edit_text(entry), entry->max_length);
}

// A framed, scrolling entry in the pane's small font. A password entry shows
// an asterisk for each character and keeps the text that was typed. `name`
// is the name a program driving the game clicks.
SingleEdit* add_entry(
    InfoPane* pane,
    int32_t dx,
    int32_t dy,
    int32_t width,
    int32_t max_length,
    bool password,
    const char* name
) {
    SingleEdit* entry = screens::lobby_edit_create(
        &pane->base, dx, dy, width, pane->owner->system.hat_fonts.small, max_length, true
    );
    entry->scroll = 1;
    if (password)
        entry->mask = '*';
    if (name != nullptr && name[0] != '\0')
        window_set_name(&entry->base, name);
    return entry;
}

/// Runs the create-frequency pane's pulse while it is shown.
///
/// GO follows the chat's create request, frequency change and name entry,
/// and the three entries take input only while nothing is pending. The chat
/// decides from the name the entry holds and the face GO shows.
///
/// @param self the create-frequency pane
void create_pane_pulse(Window* self) {
    if (!window_is_shown(self))
        return;
    CreateFrequencyPane* pane = as_create(self);
    screens::ChatConsole* chat = chat_of(&pane->pane);
    screens::FrequencyDialog* d = &chat->create_dialog;
    copy_entry(d->name, sizeof d->name, pane->name);
    d->create_value = pane->go->states.current;
    screens::chat_create_dialog_pulse(chat);
    set_state(pane->go, d->create_value);
    for (SingleEdit* entry : {pane->name, pane->password, pane->description})
        set_state(&entry->base, d->entry_value);
}

/// Handles GO on the create-frequency pane; other presses go to the Close_Button.
///
/// Unless a request is pending: GREEN_BUTTON, then the chat asks Rover to
/// create the frequency named in the entry with the description and
/// password entered (slot 69, answered through the room-created callback).
///
/// @param self the create-frequency pane
/// @param notify control notice; may be null
void create_pane_on_notify(Window* self, const Notify* notify) {
    CreateFrequencyPane* pane = as_create(self);
    if (notify == nullptr || notify->source != pane->go) {
        (void)info_pane_on_close_click(&pane->pane, notify);
        return;
    }
    screens::ChatConsole* chat = chat_of(&pane->pane);
    if (chat->create_dialog.submitting)
        return;
    play_sound(&pane->pane, k_green_button);
    char name[k_name_capacity]{};
    char password[k_password_capacity]{};
    char description[k_description_capacity]{};
    copy_entry(name, sizeof name, pane->name);
    copy_entry(password, sizeof password, pane->password);
    copy_entry(description, sizeof description, pane->description);
    (void)screens::chat_create_room(chat, name, description, password);
}

/// Opens the create-frequency pane unless it shows (its open slot).
///
/// The entries are emptied unless a request is pending, which here also
/// covers a +create request the chat made before the pane opened, whose
/// texts the entries then show. The name takes the keyboard. Opening again
/// while the pane shows leaves the keyboard where the player moved it.
///
/// @param[in,out] base the pane
void create_pane_open(InfoPane* base) {
    if (window_is_shown(&base->base))
        return;
    CreateFrequencyPane* pane = as_create(&base->base);
    const screens::FrequencyDialog& d = chat_of(base)->create_dialog;
    set_entry_text(pane->name, d.submitting ? d.name : "");
    set_entry_text(pane->password, d.submitting ? d.password : "");
    set_entry_text(pane->description, d.submitting ? d.description : "");
    create_pane_pulse(&base->base);
    info_pane_open(base);
    services_desktop_focus(&pane->name->base);
}

/// Runs the frequency information pane's pulse while it is shown.
///
/// GO is off for the current frequency, while a frequency change is pending
/// and while a password frequency's entry is empty.
///
/// @param self the frequency information pane
void info_pane_pulse(Window* self) {
    if (!window_is_shown(self))
        return;
    FrequencyInfoPane* pane = as_info(self);
    const screens::ChatSession* session = chat_of(&pane->pane)->session;
    const bool off = pane->room_id == session->room_id || session->pending_room_id != 0 ||
                     (pane->needs_password != 0 && pane->password->length == 0);
    if (off)
        set_state(pane->go, button_state::disabled);
    else if (pane->go->states.current == button_state::disabled)
        window_set_state(pane->go, button_state::unpressed);
}

/// Handles GO on the frequency information pane, then runs its pulse; other presses go to the
/// Close_Button.
///
/// The chat goes to the frequency shown, with the password typed for one
/// that needs it.
///
/// @param self the frequency information pane
/// @param notify control notice; may be null
void info_pane_on_notify(Window* self, const Notify* notify) {
    FrequencyInfoPane* pane = as_info(self);
    if (notify == nullptr || notify->source != pane->go) {
        (void)info_pane_on_close_click(&pane->pane, notify);
        return;
    }
    char password[k_password_capacity]{};
    if (pane->needs_password != 0)
        copy_entry(password, sizeof password, pane->password);
    screens::ChatConsole* chat = chat_of(&pane->pane);
    (void)screens::chat_go_to_room(
        chat, chat->session->area_id, pane->room_id, password, pane->name
    );
    info_pane_pulse(self);
}

/// Opens the frequency information pane unless it shows, then runs its pulse.
///
/// @param[in,out] base the pane
void info_pane_open_slot(InfoPane* base) {
    if (window_is_shown(&base->base))
        return;
    info_pane_open(base);
    info_pane_pulse(&base->base);
}

// A hidden icon window at (dx, dy) inside the pane.
Window* add_icon(Window* pane, int32_t dx, int32_t dy, int32_t size) {
    Window* icon = static_create();
    window_set_bounds(icon, pane->x + dx, pane->y + dy, size, size, pane, true);
    icon->visible = 0;
    return icon;
}

const char* text_or_empty(const char* text) {
    return text != nullptr ? text : "";
}

/// Returns the small font in the side's colour: small_blue for Arm, small_red for Core and small
/// otherwise.
///
/// @param fonts the session's fonts
/// @param color colour of the label
/// @return the font to draw the label in
Handle side_font(const screens::HatFonts& fonts, screens::SideColor color) {
    switch (color) {
    case screens::SideColor::arm:
        return fonts.small_blue;
    case screens::SideColor::core:
        return fonts.small_red;
    default:
        return fonts.small;
    }
}

/// Draws the player pane.
///
/// While shown, rules the tabs in 0x5D (under each tab but the chosen one,
/// between the tabs, and down the left of the tabs and the page), then
/// draws the pane and its windows.
///
/// @param self the player pane
/// @return the background draw's result
bool player_pane_draw(Window* self) {
    namespace l = player_layout;
    PlayerPaneView* view = as_player(self);
    Surface* target = console_images_target(view->pane.owner->images);
    if (window_is_shown(self) && target != nullptr) {
        const int32_t tab = chat_of(&view->pane)->pane.tab;
        const int32_t rule_y = self->y + l::tab_rule_y;
        for (int32_t i = 0; i < k_player_pane_tabs; ++i) {
            const int32_t right =
                i + 1 < k_player_pane_tabs ? self->x + l::tab_x[i + 1] : self->x + self->width - 1;
            if (tab != i)
                (void)present::draw_clipped_line(
                    target, self->x + l::tab_x[i], rule_y, right, rule_y, l::rule_color
                );
        }
        const int32_t left = self->x + l::tab_x[0];
        const int32_t top = self->y + l::tab_y;
        (void)present::draw_clipped_line(
            target, left, top, left, self->y + self->height - 1, l::rule_color
        );
        for (int32_t i = 1; i < k_player_pane_tabs; ++i) {
            const int32_t x = self->x + l::tab_x[i];
            (void)present::draw_clipped_line(target, x, top, x, rule_y, l::rule_color);
        }
    }
    return window_draw_background(self);
}

/// Handles the player pane's controls.
///
/// Close_Button presses close the pane. PAGE, the Whisper Lock and Muzzle
/// boxes and the tab buttons go to the chat's pane, which acts only while it
/// holds a player's record: PAGE, the boxes' lock and muzzle and the
/// profile-shown notices reach the chat as they do in 3.1c, and a tab shows
/// its page.
///
/// @param self the player pane
/// @param notify control notice; may be null
void player_pane_on_notify(Window* self, const Notify* notify) {
    PlayerPaneView* view = as_player(self);
    (void)info_pane_on_close_click(&view->pane, notify);
    if (notify == nullptr || notify->code != NotifyCode::activated)
        return;
    screens::PaneControl control{};
    if (notify->source == view->page)
        control = screens::PaneControl::page_button;
    else if (notify->source == &view->whisper_lock->base)
        control = screens::PaneControl::whisper_icon;
    else if (notify->source == &view->muzzle->base)
        control = screens::PaneControl::silence_icon;
    else {
        int32_t tab = 0;
        while (tab < k_player_pane_tabs && notify->source != view->tabs[tab])
            ++tab;
        if (tab == k_player_pane_tabs)
            return;
        control = k_tab_controls[tab];
    }
    (void)screens::chat_pane_on_click(chat_of(&view->pane), control);
}

/// Opens the player pane unless it shows, showing what the chat's pane holds.
///
/// The chat has already blanked the pane and asked for the player's profile.
///
/// @param[in,out] base the pane
void player_pane_open(InfoPane* base) {
    if (window_is_shown(&base->base))
        return;
    player_pane_show(as_player(&base->base), chat_of(base)->pane, true);
    info_pane_open(base);
}

/// Closes the player pane, then the chat's pane lets go of its player and record.
///
/// @param[in,out] base the pane
void player_pane_close(InfoPane* base) {
    info_pane_close(base);
    screens::PlayerPane* pane = &chat_of(base)->pane;
    if (pane->visible)
        screens::chat_pane_close(pane);
}

/// Releases the two Insignia images, then the window; the record it shows is the chat's.
///
/// @param self the player pane
void player_pane_destroy(Window* self) {
    PlayerPaneView* view = as_player(self);
    image_release(view->arm_insignia);
    image_release(view->core_insignia);
    console_window_free(self);
}

WindowOps make_player_ops() {
    WindowOps ops = info_pane_ops();
    ops.destroy = player_pane_destroy;
    ops.on_notify = player_pane_on_notify;
    ops.draw = player_pane_draw;
    return ops;
}

WindowOps make_create_ops() {
    WindowOps ops = info_pane_ops();
    ops.pulse = create_pane_pulse;
    ops.on_notify = create_pane_on_notify;
    return ops;
}

WindowOps make_info_ops() {
    WindowOps ops = info_pane_ops();
    ops.pulse = info_pane_pulse;
    ops.on_notify = info_pane_on_notify;
    return ops;
}

const WindowOps k_player_ops = make_player_ops();
const WindowOps k_create_ops = make_create_ops();
const WindowOps k_info_ops = make_info_ops();
const InfoPaneSlots k_player_slots{player_pane_open, player_pane_close};
const InfoPaneSlots k_create_slots{create_pane_open, info_pane_close};
const InfoPaneSlots k_info_slots{info_pane_open_slot, info_pane_close};

// Wraps the description to the rows' width, one wrapped line to a row;
// lines past the third are not shown.
void show_description(FrequencyInfoPane* pane, const char* description) {
    char text[screens::kRoomDescriptionCapacity]{};
    char wrapped[screens::kRoomDescriptionCapacity * 2]{};
    oa::boneyards::utf8::copy(text, sizeof text, description, screens::kRoomDescriptionCharacters);
    const StatusWidget* owner = pane->pane.owner;
    int32_t lines = owner->toolkit != nullptr ? toolkit_wrap_text(
                                                    owner->toolkit,
                                                    pane->description[0]->width,
                                                    text,
                                                    wrapped,
                                                    static_cast<int32_t>(sizeof wrapped),
                                                    owner->system.hat_fonts.small
                                                )
                                              : 0;
    char* line = wrapped;
    for (int32_t row = 0; lines > 0 && row < k_info_description_lines; ++row, --lines) {
        char* end = lines == 1 ? nullptr : std::strchr(line, '\n');
        if (end == nullptr) {
            screens::lobby_label_set_text(pane->description[row], line);
            break;
        }
        *end = '\0';
        screens::lobby_label_set_text(pane->description[row], line);
        line = end + 1;
    }
}

} // namespace

InfoPane* player_pane_create(MainFrame* frame, const char* title) {
    namespace l = player_layout;
    auto* view = as_player(console_window_alloc(sizeof(PlayerPaneView), &k_player_ops));
    InfoPane* base = &view->pane;
    base->slots = &k_player_slots;
    StatusWidget* owner = frame->owner;
    info_pane_init(base, title, &frame->base, owner);
    view->arm_insignia = k_no_handle;
    view->core_insignia = k_no_handle;
    view->rank_frame = -1;
    Window* self = &base->base;
    const screens::HatFonts& f = owner->system.hat_fonts;
    const auto label = [&](int32_t x, int32_t y, const char* text, int32_t width, int32_t height) {
        return screens::lobby_label_create(
            self, x, y, text, f.small, k_align_left, k_anchor_top_left, width, height
        );
    };
    view->rank_icon = add_icon(self, l::rank_icon_x, l::rank_icon_y, l::rank_icon_size);
    view->name = label(l::name_x, l::name_y, "", l::name_width, l::name_height);
    view->rank = label(l::rank_x, l::rank_y, k_initial_rank, l::rank_width, 0);
    view->insignia = add_icon(self, l::insignia_x, l::insignia_y, l::insignia_size);
    view->clan = label(l::clan_x, l::clan_y, "", l::clan_width, 0);
    view->whisper_lock = screens::lobby_check_box_create(
        self, l::check_x, l::whisper_lock_y, "Whisper Lock", f.small_dim
    );
    view->muzzle =
        screens::lobby_check_box_create(self, l::check_x, l::muzzle_y, "Muzzle", f.small_dim);
    view->page = button_create();
    screens::function_button_setup(view->page, true, l::page_x, l::page_y, "PAGE", self);
    window_set_state(view->page, button_state::unpressed);
    for (int32_t i = 0; i < k_player_pane_tabs; ++i) {
        view->tab_labels[i] = screens::lobby_label_create(
            self,
            l::tab_x[i],
            l::tab_y,
            k_tab_captions[i],
            f.button_normal,
            k_align_centre,
            k_anchor_top_left,
            l::tab_width,
            l::tab_height
        );
        view->tabs[i] = hotspot_button_create();
        window_create(
            view->tabs[i],
            k_tab_names[i],
            self->x + l::tab_x[i],
            self->y + l::tab_y,
            l::tab_width,
            l::tab_height,
            self,
            true
        );
    }
    view->html = html_view_create(owner, self, l::html_x, l::html_y, l::html_width, l::html_height);
    view->html->base.visible = 1;
    view->arm_insignia = image_load(
        screens::kWidgetsGaf, k_insignia_sequence, screens::insignia_frame::arm, screens::kWidgetKey
    );
    view->core_insignia = image_load(
        screens::kWidgetsGaf,
        k_insignia_sequence,
        screens::insignia_frame::core,
        screens::kWidgetKey
    );
    return base;
}

void player_pane_show(PlayerPaneView* view, const screens::PlayerPane& pane, bool page) {
    const screens::HatFonts& f = view->pane.owner->system.hat_fonts;
    if (pane.user_icon_shown && pane.user_icon != view->rank_frame) {
        console_bind_images(
            view->rank_icon,
            screens::kWidgetsGaf,
            k_user_icons_sequence,
            pane.user_icon,
            screens::kWidgetKey
        );
        view->rank_frame = pane.user_icon;
    }
    view->rank_icon->visible = pane.user_icon_shown ? 1 : 0;
    const Handle font = side_font(f, pane.name_color);
    screens::lobby_label_set_text(view->name, pane.name);
    window_set_font_handle(view->name, font);
    screens::lobby_label_set_text(view->rank, text_or_empty(pane.rank));
    window_set_font_handle(view->rank, font);
    if (pane.insignia_shown)
        window_set_state_image(
            view->insignia,
            k_image_state,
            pane.insignia == screens::insignia_frame::arm ? view->arm_insignia : view->core_insignia
        );
    view->insignia->visible = pane.insignia_shown ? 1 : 0;
    screens::lobby_label_set_text(view->clan, pane.clan);
    set_state(view->page, pane.page_value);
    set_state(&view->whisper_lock->base, pane.whisper_icon_value);
    set_state(&view->muzzle->base, pane.silence_value);
    for (int32_t i = 0; i < k_player_pane_tabs; ++i) {
        set_state(view->tabs[i], pane.tab_values[i]);
        window_set_font_handle(
            view->tab_labels[i], pane.tab_highlighted[i] ? f.button_hover : f.button_normal
        );
    }
    if (page)
        html_view_set_text(view->html, text_or_empty(pane.html));
}

InfoPane* create_frequency_pane_create(MainFrame* frame, const char* title) {
    namespace l = create_layout;
    auto* pane = as_create(console_window_alloc(sizeof(CreateFrequencyPane), &k_create_ops));
    InfoPane* base = &pane->pane;
    base->slots = &k_create_slots;
    StatusWidget* owner = frame->owner;
    info_pane_init(base, title, &frame->base, owner);
    Window* self = &base->base;
    const Handle caption_font = owner->system.hat_fonts.medium;
    const auto caption = [&](int32_t y, const char* text) {
        screens::lobby_label_create(
            self,
            l::caption_right,
            y,
            text,
            caption_font,
            k_align_centre_vertically,
            k_anchor_top_right,
            0,
            l::caption_height
        );
    };
    pane->name =
        add_entry(base, l::entry_x, l::name_y, l::name_width, l::name_max, false, "Frequency name");
    caption(l::name_y, "Frequency Name");
    pane->password = add_entry(
        base,
        l::entry_x,
        l::password_y,
        l::password_width,
        l::password_max,
        true,
        "Frequency password"
    );
    caption(l::password_y, "Password");
    pane->description = add_entry(
        base,
        l::entry_x,
        l::description_y,
        l::description_width,
        l::description_max,
        false,
        "Frequency description"
    );
    caption(l::description_y, "Description");
    pane->go = button_create();
    screens::function_button_setup(pane->go, false, l::go_x, l::go_y, k_go_caption, self);
    window_set_state(pane->go, button_state::disabled);
    return base;
}

InfoPane* frequency_info_pane_create(MainFrame* frame, const char* title) {
    namespace l = info_layout;
    auto* pane = as_info(console_window_alloc(sizeof(FrequencyInfoPane), &k_info_ops));
    InfoPane* base = &pane->pane;
    base->slots = &k_info_slots;
    StatusWidget* owner = frame->owner;
    info_pane_init(base, title, &frame->base, owner);
    pane->room_id = screens::kNoRoomInfo;
    Window* self = &base->base;
    const screens::HatFonts* fonts = &owner->system.hat_fonts;
    const int32_t width = self->width - l::margin;
    const auto row = [&](int32_t y) {
        return screens::lobby_label_create(
            self, l::x, y, "", fonts->small, k_align_left, k_anchor_top_left, width, l::row_height
        );
    };
    const auto field = [&](int32_t y, const char* caption) {
        return screens::lobby_field_create(self, l::x, y, width, l::row_height, caption, fonts, "");
    };
    pane->name_label = row(l::name_y);
    pane->type = field(l::type_y, "Type:");
    pane->creator = field(l::creator_y, "Created by:");
    pane->population_field = field(l::population_y, "Population:");
    for (int32_t i = 0; i < k_info_description_lines; ++i)
        pane->description[i] = row(l::description_y + i * l::row_height);
    pane->password_label = screens::lobby_label_create(
        self,
        l::x,
        l::password_y,
        "Password:",
        fonts->medium,
        k_align_centre_vertically,
        k_anchor_top_left,
        0,
        l::password_height
    );
    pane->password = add_entry(
        base,
        pane->password_label->width + l::password_gap,
        l::password_y,
        l::password_width,
        l::password_max,
        true,
        "Frequency password"
    );
    pane->go = button_create();
    screens::function_button_setup(pane->go, false, l::go_x, l::go_y, k_go_caption, self);
    window_set_state(pane->go, button_state::unpressed);
    return base;
}

// A password frequency's entry takes the keyboard. The row click focuses
// the list again on the way up; the console then gives the next letter to
// this entry. A frequency that needs none leaves the keyboard, except when
// the player was typing in the password that just hid: that line goes back
// to the chat entry.
void focus_frequency_password(FrequencyInfoPane* pane) {
    Window* field = &pane->password->base;
    if (pane->needs_password != 0) {
        services_desktop_focus(field);
        return;
    }
    Desktop* desktop = services().desktop;
    if (desktop == nullptr || desktop->focus != field)
        return;
    if (ChatView* chat = console_open_chat(); chat != nullptr && chat->input != nullptr)
        services_desktop_focus(&chat->input->base);
    else
        services_desktop_focus(nullptr);
}

void frequency_info_pane_show(FrequencyInfoPane* pane, const screens::ChatRoom* room) {
    if (room->room_id != pane->room_id)
        play_sound(&pane->pane, k_green_button);
    pane->room_id = room->room_id;
    oa::boneyards::utf8::copy(
        pane->name, sizeof pane->name, room->name, screens::kRoomNameCharacters
    );
    screens::lobby_label_set_text(pane->name_label, pane->name);
    const char* type = "Unknown";
    const char* creator = "Unknown";
    switch (room->type) {
    case screens::room_type::planetary_default:
        type = "Planetary Default";
        creator = "System";
        break;
    case screens::room_type::battle_debriefing:
        type = "Battle Debriefing";
        creator = "System";
        break;
    case screens::room_type::user_created:
        type = "User Created";
        creator = room->tag;
        break;
    default:
        break;
    }
    screens::lobby_field_set_text(pane->type, type);
    screens::lobby_field_set_text(pane->creator, creator);
    pane->population = static_cast<uint32_t>(room->population);
    screens::lobby_field_set_count(pane->population_field, pane->population);
    pane->needs_password = room->password_required ? 1 : 0;
    pane->password_label->visible = pane->needs_password;
    pane->password->base.visible = pane->needs_password;
    for (Window* line : pane->description)
        screens::lobby_label_set_text(line, "");
    show_description(pane, room->description);
    focus_frequency_password(pane);
    pane->pane.base.ops->pulse(&pane->pane.base);
}

void chat_panes_sync(ChatView* view) {
    const screens::ChatConsole& chat = view->owner->navigation->chat;

    struct Pane {
        InfoPane* pane;
        bool wanted;
    };

    const Pane panes[] = {
        {view->player_pane != nullptr ? &view->player_pane->pane : nullptr, chat.pane.visible},
        {view->create_pane != nullptr ? &view->create_pane->pane : nullptr,
         chat.create_dialog.visible},
        {view->info_pane != nullptr ? &view->info_pane->pane : nullptr, chat.room_info_visible},
    };
    for (const Pane& p : panes)
        if (p.pane != nullptr && !p.wanted && window_is_shown(&p.pane->base))
            p.pane->slots->close(p.pane);
    for (const Pane& p : panes)
        if (p.pane != nullptr && p.wanted && !window_is_shown(&p.pane->base))
            p.pane->slots->open(p.pane);
}

} // namespace oa::boneyards::console

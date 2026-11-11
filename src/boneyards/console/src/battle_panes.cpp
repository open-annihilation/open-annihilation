// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battle console's panes: the Host pane that sets up a battle to host,
// the Join summary of a forming battle and the Scoreboard of one in
// progress. The Main Frame builds each once; the battle list decides which
// battle a pane shows, and these windows show it and hand the list what the
// player does.
#include "oa/boneyards/screens/after_battle.hpp"
#include "oa/boneyards/utf8.hpp"

#include <cstdio>
#include <cstring>

#include "battle_console.hpp"
#include "oa/present/raster.hpp"
#include "oa/present/surface.hpp"
#include "widgets.hpp"

namespace oa::boneyards::console {

namespace {

constexpr const char* k_green_button = "GREEN_BUTTON";
constexpr int32_t k_image_state = 1;

// Codes the panes send the battle console: the Host pane's START and the
// Join summary's JOIN.
constexpr NotifyCode k_notify_start = static_cast<NotifyCode>(0xca);
constexpr NotifyCode k_notify_join = static_cast<NotifyCode>(0xcb);

// Label placements (lobby_label_create).
constexpr int32_t k_align_top_left = 0;
constexpr int32_t k_align_top_centre = 1;
constexpr int32_t k_align_left = 3;
constexpr int32_t k_align_centre = 4;
constexpr int32_t k_align_right_middle = 5;
constexpr int32_t k_anchor_top_left = 0;
constexpr int32_t k_anchor_top_right = 1;

namespace host_layout {
// Captions end 3 pixels left of their controls, which start at 0x3d.
constexpr int32_t caption_right = 0x3a;
constexpr int32_t caption_height = 0x15;
constexpr int32_t entry_x = 0x3d, entry_width = 0x6d;
constexpr int32_t name_y = 0x1c, name_max = 0x1f;
constexpr int32_t password_y = 0x34, password_max = 0xe;
constexpr int32_t types_y = 0x4c, types_height = 0x45, types_caption_y = 0x4e;
constexpr int32_t groups_x = 0xb1;
constexpr int32_t skill_y = 0x1c, players_y = 0x3b, death_y = 0x5a;
constexpr int32_t start_y = 0x7b;
// A game-type row's name starts 3 pixels in.
constexpr int32_t type_label_x = 3;
} // namespace host_layout

namespace join_layout {
constexpr int32_t password_x = 0xf5, password_y = 0x1a, password_width = 0x6e;
constexpr int32_t password_max = 0xe;
constexpr int32_t caption_gap = 5; // the "Password" caption ends this far left of the entry
constexpr int32_t caption_height = 0x15;
constexpr int32_t name_x = 6, name_y = 0x13;
constexpr int32_t name_gap = 0xb; // the name ends this far left of the password caption
constexpr int32_t row_height = 0x11;
constexpr int32_t game_type_y = 0x24;
constexpr int32_t seat_x = 6;
constexpr int32_t seat_y[k_battle_seats] = {0x35, 0x46, 0x57, 0x68};
// The seats' icons sit 0xa past the widest caption, 3 rows down from the
// caption's top; their names 0x18 past it, up to 0x15b.
constexpr int32_t icon_gap = 0xa, icon_dy = 3, icon_size = 0xb;
constexpr int32_t seat_name_gap = 0x18, seat_names_end = 0x15b;
constexpr int32_t fields_width = 0x16d;
constexpr int32_t skill_y = 0x79, death_y = 0x8a;
constexpr int32_t join_x = 0x100, join_y = 0x83;
} // namespace join_layout

namespace score_layout {
constexpr uint8_t rule_color = 0x5d;
// Rules across the pane under the header and the seats, and down between
// the columns.
constexpr int32_t header_rule_y = 0x25, seats_rule_y = 0x6a;
constexpr int32_t column_x[5] = {0x75, 0xa7, 0xd9, 0x10b, 0x13d};
constexpr int32_t header_y = 0x17, row_height = 0x11;
constexpr int32_t column_width = 0x33, score_width = 0x39;
constexpr int32_t name_x = 6, name_width = 0x70;
constexpr int32_t first_seat_y = 0x26;
constexpr int32_t icon_x = 6, icon_dy = 3, icon_size = 0xb;
constexpr int32_t seat_name_x = 0x14, seat_name_width = 0x62;
} // namespace score_layout

constexpr const char* k_unknown = "Unknown";
constexpr const char* k_blocked = "Blocked";
constexpr const char* k_user_icons = "User_Icons";
constexpr int32_t k_blocked_frame = 1;
constexpr const char* k_seat_captions[k_battle_seats] = {
    "Host:", "Player 2:", "Player 3:", "Player 4:"
};
// A name and its rank title on a seat.
constexpr const char* k_title_separator = ", ";
constexpr std::size_t k_seat_text_capacity = 0x60;
// "Cartman" heads each report message row, as the Scoreboard shows them.
constexpr const char* k_message_name = "Cartman";

screens::BoneyardsScreens* nav_of(const InfoPane* pane) {
    return pane->owner->navigation;
}

screens::BattleList* battles_of(const InfoPane* pane) {
    return &nav_of(pane)->battles;
}

void play_sound(const InfoPane* pane, const char* name) {
    const MainMenuHost& host = pane->owner->config.host;
    if (host.play_sound != nullptr)
        host.play_sound(host.user, name);
}

void play_group_sound(void* user, const char* name) {
    play_sound(static_cast<const InfoPane*>(user), name);
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

void set_label(Window* label, const char* text) {
    screens::lobby_label_set_text(label, text);
}

// Tells the pane's parent (the battle console) about START or JOIN.
void notify_parent(InfoPane* pane, NotifyCode code) {
    Window* parent = pane->base.parent;
    if (parent == nullptr)
        return;
    Notify notify{code, &pane->base, 0, nullptr};
    parent->ops->on_notify(parent, &notify);
}

// ---- Host pane --------------------------------------------------------------

HostBattlePane* as_host(Window* w) {
    return reinterpret_cast<HostBattlePane*>(w);
}

// A game-type row: the type's name, and the mode it hosts.
struct GameTypeRow {
    screens::ByTextRow row;
    int32_t mode;
};

/// Gives the selected game-type row's title the light small font.
///
/// @param self the game-type row
void type_row_select(Window* self) {
    const screens::ByTextRow* row = reinterpret_cast<screens::ByTextRow*>(self);
    window_set_font_handle(row->label, row->fonts->small_light);
}

/// Gives any other game-type row's title the small font.
///
/// @param self the game-type row
void type_row_unselect(Window* self) {
    const screens::ByTextRow* row = reinterpret_cast<screens::ByTextRow*>(self);
    window_set_font_handle(row->label, row->fonts->small);
}

constexpr screens::ListRowLook k_type_row_look{type_row_select, type_row_unselect};

bool type_row_draw(Window* self) {
    const screens::ByTextRow* row = reinterpret_cast<screens::ByTextRow*>(self);
    return screens::list_row_draw(self, true, row->list->selected_item, k_type_row_look);
}

void type_row_on_notify(Window* self, const Notify* notify) {
    auto* row = reinterpret_cast<screens::ByTextRow*>(self);
    row->list->selected_item = self;
    Notify forwarded = *notify;
    forwarded.source = self;
    row->list->list.base.ops->on_notify(&row->list->list.base, &forwarded);
}

WindowOps make_type_row_ops() {
    WindowOps ops = k_static_ops;
    ops.destroy = console_window_free;
    ops.on_notify = type_row_on_notify;
    ops.draw = type_row_draw;
    return ops;
}

const WindowOps k_type_row_ops = make_type_row_ops();

/// Appends a game-type row: its name 3 pixels in, over the rest of the row in the small font.
///
/// @param[in,out] list the game-type list
/// @param type the type's name and mode
/// @param fonts the Hat fonts
/// @return the row
GameTypeRow* type_row_create(
    screens::ByListBox* list, const screens::GameModeEntry& type, const screens::HatFonts* fonts
) {
    namespace l = host_layout;
    auto* row =
        reinterpret_cast<GameTypeRow*>(console_window_alloc(sizeof(GameTypeRow), &k_type_row_ops));
    row->mode = static_cast<int32_t>(type.mode);
    row->row.base.focusable = 0;
    row->row.list = list;
    row->row.fonts = fonts;
    row->row.selectable = true;
    Window* list_window = &list->list.base;
    Window* self = &row->row.base;
    window_set_bounds(
        self,
        list_window->x,
        list_window->y,
        screens::by_list_row_width(list_window),
        screens::kByRowHeight,
        list_window,
        true
    );
    row->row.label = screens::lobby_label_create(
        self,
        l::type_label_x,
        0,
        type.name,
        fonts->small,
        k_align_left,
        k_anchor_top_left,
        screens::by_list_row_width(list_window) - l::type_label_x,
        screens::kByRowHeight
    );
    list_box_add(&list->list, self);
    return row;
}

/// Lists the game types the planet and the player's allegiance allow, the first one selected, and
/// enables every choice of the player-count and commander-death groups again.
///
/// @param[in,out] pane the Host pane
void host_rebuild_types(HostBattlePane* pane) {
    screens::BoneyardsScreens* nav = nav_of(&pane->pane);
    screens::boneyards_prepare_game_setup(nav);
    const screens::GameSetupPanel& setup = nav->setup;
    screens::by_list_box_clear(pane->types);
    Window* first = nullptr;
    for (std::size_t i = 0; i < setup.type_count; ++i) {
        GameTypeRow* row =
            type_row_create(pane->types, setup.types[i], &pane->pane.owner->system.hat_fonts);
        if (first == nullptr)
            first = &row->row.base;
    }
    pane->types->selected_item = first;
    screens::by_radio_group_refresh(pane->players);
    screens::by_radio_group_refresh(pane->commander_death);
}

/// Opens the Host pane (its open slot): the base open, then the game types are listed again.
/// The game name takes the keyboard. A later pulse does not take it back.
///
/// @param[in,out] base the pane
void host_pane_open(InfoPane* base) {
    info_pane_open(base);
    battles_of(base)->host_visible = true;
    HostBattlePane* pane = as_host(&base->base);
    host_rebuild_types(pane);
    services_desktop_focus(&pane->game_name->base);
    pane->awaiting_keyboard = 0;
}

/// Closes the Host pane (its close slot).
///
/// @param[in,out] base the pane
void host_pane_close(InfoPane* base) {
    info_pane_close(base);
    screens::battle_list_close_panel(battles_of(base), screens::BattlePanel::host);
}

/// Enables START while the game name entry holds text and disables it while it is empty, while the
/// pane shows.
///
/// @param self the Host pane
void host_pane_pulse(Window* self) {
    HostBattlePane* pane = as_host(self);
    if (!window_is_shown(self)) {
        // The battle list can show this pane without its open slot. The next
        // pulse after that show gives the game name the keyboard once.
        pane->awaiting_keyboard = 1;
        return;
    }
    if (pane->awaiting_keyboard != 0) {
        services_desktop_focus(&pane->game_name->base);
        pane->awaiting_keyboard = 0;
    }
    const bool disabled = pane->start->states.current == button_state::disabled;
    if (pane->game_name->length != 0) {
        if (disabled)
            window_set_state(pane->start, button_state::unpressed);
    } else if (!disabled) {
        window_set_state(pane->start, button_state::disabled);
    }
}

// The index of the selected game-type row, -1 with none.
int32_t selected_type(HostBattlePane* pane) {
    const Window* row = pane->types->selected_item;
    return row != nullptr ? list_box_index_of(&pane->types->list, row) : -1;
}

/// Applies a game type picked: GREEN_BUTTON, then the setup limits the player count and commander
/// death the type allows.
///
/// @param[in,out] pane the Host pane
void host_pick_type(HostBattlePane* pane) {
    play_sound(&pane->pane, k_green_button);
    screens::GameSetupPanel* setup = &nav_of(&pane->pane)->setup;
    screens::by_radio_group_read(pane->players, &setup->players);
    screens::by_radio_group_read(pane->commander_death, &setup->commander_death);
    screens::game_setup_select_type(setup, selected_type(pane));
    screens::by_radio_group_show(pane->players, setup->players);
    screens::by_radio_group_show(pane->commander_death, setup->commander_death);
}

/// Hosts the battle (START): GREEN_BUTTON, the battle console hears of it, the setup asks the
/// service to host a battle of the chosen type with the name, the password when one is entered
/// and the three choices, and the pane closes.
///
/// @param[in,out] pane the Host pane
void host_start(HostBattlePane* pane) {
    play_sound(&pane->pane, k_green_button);
    screens::GameSetupPanel* setup = &nav_of(&pane->pane)->setup;
    std::snprintf(
        setup->game_name, sizeof setup->game_name, "%s", single_edit_text(pane->game_name)
    );
    std::snprintf(
        setup->password,
        sizeof setup->password,
        "%s",
        pane->password->length != 0 ? single_edit_text(pane->password) : ""
    );
    screens::by_radio_group_read(pane->skill, &setup->skill);
    screens::by_radio_group_read(pane->players, &setup->players);
    screens::by_radio_group_read(pane->commander_death, &setup->commander_death);
    setup->selected_type = selected_type(pane);
    notify_parent(&pane->pane, k_notify_start);
    (void)screens::game_setup_start(setup);
    pane->pane.slots->close(&pane->pane);
}

/// Handles the game-type list and START; other presses go to the Close_Button.
///
/// @param self the Host pane
/// @param notify control notice; may be null
void host_pane_on_notify(Window* self, const Notify* notify) {
    HostBattlePane* pane = as_host(self);
    if (notify != nullptr && notify->source == &pane->types->list.base) {
        // A click has already named its row. An arrow names the row the list
        // shows only once Tab, Shift+Tab or F6 has shown the keyboard ring.
        // With no ring, the type the click named stays.
        Window* chosen = list_box_item(&pane->types->list, list_box_selected(&pane->types->list));
        const Desktop* desktop = services().desktop;
        const bool ring_shown = desktop != nullptr && desktop->keyboard_ring != 0;
        if (ring_shown && chosen != nullptr)
            pane->types->selected_item = chosen;
        host_pick_type(pane);
        return;
    }
    if (notify != nullptr && notify->source == pane->start) {
        host_start(pane);
        return;
    }
    (void)info_pane_on_close_click(&pane->pane, notify);
}

WindowOps make_host_ops() {
    WindowOps ops = info_pane_ops();
    ops.pulse = host_pane_pulse;
    ops.on_notify = host_pane_on_notify;
    return ops;
}

const WindowOps k_host_ops = make_host_ops();
const InfoPaneSlots k_host_slots{host_pane_open, host_pane_close};

// ---- Join summary -----------------------------------------------------------

JoinSummaryPane* as_join(Window* w) {
    return reinterpret_cast<JoinSummaryPane*>(w);
}

const screens::BattleEntry* join_battle(const JoinSummaryPane* pane) {
    const screens::BattleList* list = battles_of(&pane->pane);
    return screens::battle_list_entry(list, list->join_battle);
}

// Gives a seat a User_Icons frame, or no icon for -1.
void show_seat_icon(Window* icon, int32_t frame) {
    if (frame < 0) {
        window_set_state_image(icon, k_image_state, k_no_handle);
        return;
    }
    console_bind_images(icon, screens::kWidgetsGaf, k_user_icons, frame, screens::kWidgetKey);
}

/// Shows the Join summary's battle while the pane shows.
///
/// The battle's name and game type; its host and up to three more players,
/// each with their rank icon and "name, rank title"; seats the battle does
/// not have (past its player count) "Blocked" with the blocked icon, and
/// the others empty; the skill and commander-death labels; and the password
/// caption and entry only for a password game.
///
/// @param self the Join summary
void join_pane_pulse(Window* self) {
    JoinSummaryPane* pane = as_join(self);
    if (!window_is_shown(self)) {
        pane->password_pending = 1;
        return;
    }
    const screens::BattleEntry* battle = join_battle(pane);
    if (battle == nullptr)
        return;
    const screens::BattleList* list = battles_of(&pane->pane);
    set_label(pane->name, battle->name);
    if (battle->category >= 0 && static_cast<std::size_t>(battle->category) < list->category_count)
        screens::lobby_field_set_text(
            pane->game_type, list->categories[battle->category].mode.name
        );
    int32_t seats = k_battle_seats;
    if (battle->players == 0)
        seats = 2;
    else if (battle->players == 1)
        seats = 3;
    bool host_found = false;
    int32_t player = 0;
    for (const screens::BattlePlayer& score : battle->scores) {
        if (score.user_id == 0)
            continue;
        char text[k_seat_text_capacity];
        std::snprintf(text, sizeof text, "%s", score.name);
        const char* title = screens::rank_title(score.icon, false);
        if (title[0] != '\0') {
            std::snprintf(
                text + std::strlen(text), sizeof text - std::strlen(text), "%s", k_title_separator
            );
            std::snprintf(text + std::strlen(text), sizeof text - std::strlen(text), "%s", title);
        }
        int32_t seat = -1;
        if (std::strcmp(score.name, battle->host_name) == 0) {
            seat = 0;
            host_found = true;
        } else {
            if (player < k_battle_seats - 1)
                seat = player + 1;
            ++player;
        }
        if (seat >= 0) {
            show_seat_icon(pane->seat_icons[seat], score.icon);
            set_label(pane->seat_names[seat], text);
        }
    }
    if (!host_found) {
        show_seat_icon(pane->seat_icons[0], -1);
        set_label(pane->seat_names[0], "");
    }
    // The seats past the players shown: the second is never blocked, the
    // third and fourth are blocked past the battle's player count.
    for (int32_t seat = player + 1; seat < k_battle_seats; ++seat) {
        if (seat >= 2 && seat >= seats) {
            window_set_state_image(pane->seat_icons[seat], k_image_state, pane->blocked_icon);
            set_label(pane->seat_names[seat], k_blocked);
        } else {
            show_seat_icon(pane->seat_icons[seat], -1);
            set_label(pane->seat_names[seat], "");
        }
    }
    switch (battle->on_death) {
    case 1:
        screens::lobby_field_set_text(pane->commander_death, "Continues");
        break;
    case 2:
        screens::lobby_field_set_text(pane->commander_death, "Ends");
        break;
    case 3:
        screens::lobby_field_set_text(pane->commander_death, "Deathmatch");
        break;
    default:
        break;
    }
    switch (battle->skill) {
    case 0:
        screens::lobby_field_set_text(pane->skill, "Novice");
        break;
    case 1:
        screens::lobby_field_set_text(pane->skill, "Seasoned");
        break;
    case 2:
        screens::lobby_field_set_text(pane->skill, "Expert");
        break;
    default:
        break;
    }
    const uint8_t want = battle->has_password == 1 ? 1 : 0;
    Window* field = &pane->password->base;
    const uint8_t was = field->visible != 0 ? 1 : 0;
    field->visible = want;
    pane->password_label->visible = want;
    // The password takes the keyboard when it appears, and once when the
    // pane opens onto a battle that already shows it. A pulse that finds it
    // already up leaves the keyboard where the player moved it.
    if (want != 0 && (was == 0 || pane->password_pending != 0)) {
        services_desktop_focus(field);
        pane->password_pending = 0;
    } else if (want == 0) {
        pane->password_pending = 0;
        if (Desktop* desktop = services().desktop; desktop != nullptr && desktop->focus == field)
            services_desktop_focus(nullptr);
    }
}

/// Joins the battle (JOIN): the battle console hears of it, then the service is asked to join
/// the battle with the password entered, or none when the entry is empty. Other presses go to the
/// Close_Button.
///
/// @param self the Join summary
/// @param notify control notice; may be null
void join_pane_on_notify(Window* self, const Notify* notify) {
    JoinSummaryPane* pane = as_join(self);
    if (notify == nullptr || notify->source != pane->join) {
        (void)info_pane_on_close_click(&pane->pane, notify);
        return;
    }
    char password[screens::kBattleNameCapacity]{};
    const bool has_password = pane->password->length != 0;
    if (has_password)
        oa::boneyards::utf8::copy(
            password, sizeof password, single_edit_text(pane->password), pane->password->max_length
        );
    notify_parent(&pane->pane, k_notify_join);
    const screens::BattleEntry* battle = join_battle(pane);
    if (battle == nullptr)
        return;
    screens::arm_after_battle(battle->type);
    const BoneyardsService& service = nav_of(&pane->pane)->service;
    (void)service.join_game(
        service.user, battle->area_id, battle->game_id, has_password ? password : nullptr
    );
}

/// Closes the Join summary (its close slot): it lets go of its battle, then the base close.
///
/// @param[in,out] base the pane
void join_pane_close(InfoPane* base) {
    screens::battle_list_close_panel(battles_of(base), screens::BattlePanel::join);
    info_pane_close(base);
}

/// Releases the blocked icon, then the window.
///
/// @param self the Join summary
void join_pane_destroy(Window* self) {
    image_release(as_join(self)->blocked_icon);
    console_window_free(self);
}

WindowOps make_join_ops() {
    WindowOps ops = info_pane_ops();
    ops.destroy = join_pane_destroy;
    ops.pulse = join_pane_pulse;
    ops.on_notify = join_pane_on_notify;
    return ops;
}

const WindowOps k_join_ops = make_join_ops();
const InfoPaneSlots k_join_slots{info_pane_open, join_pane_close};

// ---- Scoreboard -------------------------------------------------------------

ScoreboardPane* as_scoreboard(Window* w) {
    return reinterpret_cast<ScoreboardPane*>(w);
}

/// Shows the Scoreboard's battle while the pane shows: its name, and each seat's rank icon, name
/// and numbers, empty seats blank.
///
/// @param self the Scoreboard
void scoreboard_pane_pulse(Window* self) {
    if (!window_is_shown(self))
        return;
    ScoreboardPane* pane = as_scoreboard(self);
    const screens::BattleList* list = battles_of(&pane->pane);
    const screens::BattleEntry* battle = screens::battle_list_entry(list, list->scoreboard_battle);
    if (battle == nullptr)
        return;
    set_label(pane->name, battle->name);
    for (int32_t i = 0; i < k_battle_seats; ++i) {
        const screens::BattlePlayer& score = battle->scores[i];
        ScoreboardSeat& seat = pane->seats[i];
        const bool empty = score.user_id == 0;
        show_seat_icon(seat.icon, empty ? -1 : score.icon);
        set_label(seat.name, empty ? "" : score.name);
        set_label(seat.kills, empty ? "" : score.kills);
        set_label(seat.losses, empty ? "" : score.losses);
        set_label(seat.energy, empty ? "" : score.energy);
        set_label(seat.metal, empty ? "" : score.metal);
        set_label(seat.score, empty ? "" : score.score);
    }
}

/// Closes the Scoreboard (its close slot): it lets go of its battle, then the base close.
///
/// @param[in,out] base the pane
void scoreboard_pane_close(InfoPane* base) {
    screens::battle_list_close_panel(battles_of(base), screens::BattlePanel::scoreboard);
    info_pane_close(base);
}

WindowOps make_scoreboard_ops() {
    WindowOps ops = info_pane_ops();
    ops.pulse = scoreboard_pane_pulse;
    return ops;
}

const WindowOps k_scoreboard_ops = make_scoreboard_ops();
const InfoPaneSlots k_scoreboard_slots{info_pane_open, scoreboard_pane_close};

/// Makes the Scoreboard's own image: keyed, ruled along its bottom, under the header and under the
/// seats, and down between the columns from the header to the seats' rule.
///
/// @param[in,out] pane the Scoreboard
void scoreboard_draw_rules(ScoreboardPane* pane) {
    namespace l = score_layout;
    Window* self = &pane->pane.base;
    const Handle image =
        image_create_blank(self->width, self->height, screens::kWidgetKey, screens::kWidgetKey);
    window_set_state_image(self, k_image_state, image);
    Surface surface{};
    if (image_surface(image, &surface)) {
        const int32_t right = self->width - 1;
        const int32_t bottom = self->height - 1;
        (void)present::draw_clipped_line(&surface, 0, bottom, right, bottom, l::rule_color);
        (void)present::draw_clipped_line(
            &surface, 0, l::header_rule_y, right, l::header_rule_y, l::rule_color
        );
        (void)present::draw_clipped_line(
            &surface, 0, l::seats_rule_y, right, l::seats_rule_y, l::rule_color
        );
        for (int32_t x : l::column_x)
            (void)present::draw_clipped_line(
                &surface, x, l::header_rule_y, x, l::seats_rule_y, l::rule_color
            );
    }
    image_release(image);
}

} // namespace

InfoPane* host_battle_pane_create(MainFrame* frame, const char* title) {
    namespace l = host_layout;
    auto* pane = as_host(console_window_alloc(sizeof(HostBattlePane), &k_host_ops));
    InfoPane* base = &pane->pane;
    base->slots = &k_host_slots;
    StatusWidget* owner = frame->owner;
    info_pane_init(base, title, &frame->base, owner);
    Window* self = &base->base;
    const screens::HatFonts& f = owner->system.hat_fonts;
    const auto caption = [&](int32_t y, const char* text, int32_t height) {
        screens::lobby_label_create(
            self,
            l::caption_right,
            y,
            text,
            f.medium,
            k_align_right_middle,
            k_anchor_top_right,
            0,
            height
        );
    };
    pane->game_name =
        add_entry(base, l::entry_x, l::name_y, l::entry_width, l::name_max, false, "Battle name");
    caption(l::name_y, "Game Name", l::caption_height);
    pane->password = add_entry(
        base, l::entry_x, l::password_y, l::entry_width, l::password_max, true, "Battle password"
    );
    caption(l::password_y, "Password", l::caption_height);
    pane->types = screens::by_list_box_create(true, false);
    window_set_name(&pane->types->list.base, "Game types");
    window_set_bounds(
        &pane->types->list.base,
        self->x + l::entry_x,
        self->y + l::types_y,
        l::entry_width,
        l::types_height,
        self,
        true
    );
    // A focus must not move the game-type list past the radio rows and START.
    window_keep_place(&pane->types->list.base);
    caption(l::types_caption_y, "Game Type", 0);
    const char* const skills[] = {"Novice", "Seasoned", "Expert"};
    const char* const players[] = {"2", "3", "4"};
    const char* const deaths[] = {"Ends", "Continues", "Deathmatch"};
    pane->skill = screens::by_radio_group_create(
        self, l::groups_x, l::skill_y, "Player Skill", skills, 3, &f
    );
    pane->players = screens::by_radio_group_create(
        self, l::groups_x, l::players_y, "Number of Players", players, 3, &f
    );
    pane->commander_death = screens::by_radio_group_create(
        self, l::groups_x, l::death_y, "On Commander Death", deaths, 3, &f
    );
    for (screens::ByRadioGroup* group : {pane->skill, pane->players, pane->commander_death}) {
        group->play_sound = play_group_sound;
        group->sound_user = base;
        // Each group is its own parent, so the same id is a separate stop.
        for (int32_t i = 0; i < group->count; ++i) {
            if (group->members[i] != nullptr)
                group->members[i]->base.radio_group = 1;
        }
    }
    pane->start = button_create();
    screens::function_button_setup(pane->start, false, l::groups_x, l::start_y, "START", self);
    pane->awaiting_keyboard = 1;
    return base;
}

InfoPane* join_summary_pane_create(MainFrame* frame, const char* title) {
    namespace l = join_layout;
    auto* pane = as_join(console_window_alloc(sizeof(JoinSummaryPane), &k_join_ops));
    InfoPane* base = &pane->pane;
    base->slots = &k_join_slots;
    StatusWidget* owner = frame->owner;
    info_pane_init(base, title, &frame->base, owner);
    Window* self = &base->base;
    const screens::HatFonts* f = &owner->system.hat_fonts;
    pane->password = add_entry(
        base,
        l::password_x,
        l::password_y,
        l::password_width,
        l::password_max,
        true,
        "Join password"
    );
    pane->password_label = screens::lobby_label_create(
        self,
        pane->password->base.x - self->x - l::caption_gap,
        l::password_y,
        "Password",
        f->medium,
        k_align_right_middle,
        k_anchor_top_right,
        0,
        l::caption_height
    );
    const int32_t width = pane->password_label->x - self->x - l::name_gap;
    pane->name = screens::lobby_label_create(
        self,
        l::name_x,
        l::name_y,
        k_unknown,
        f->small,
        k_align_left,
        k_anchor_top_left,
        width,
        l::row_height
    );
    pane->game_type = screens::lobby_field_create(
        self, l::name_x, l::game_type_y, width, l::row_height, "Game Type:", f, k_unknown
    );
    int32_t widest = 0;
    for (int32_t i = 0; i < k_battle_seats; ++i) {
        pane->seat_captions[i] = screens::lobby_label_create(
            self,
            l::seat_x,
            l::seat_y[i],
            k_seat_captions[i],
            f->small_dim,
            k_align_left,
            k_anchor_top_left,
            0,
            l::row_height
        );
        if (pane->seat_captions[i]->width > widest)
            widest = pane->seat_captions[i]->width;
    }
    for (int32_t i = 0; i < k_battle_seats; ++i) {
        Window* icon = static_create();
        window_set_bounds(
            icon,
            self->x + widest + l::icon_gap,
            self->y + l::seat_y[i] + l::icon_dy,
            l::icon_size,
            l::icon_size,
            self,
            true
        );
        pane->seat_icons[i] = icon;
        pane->seat_names[i] = screens::lobby_label_create(
            self,
            widest + l::seat_name_gap,
            l::seat_y[i],
            k_unknown,
            f->small,
            k_align_left,
            k_anchor_top_left,
            l::seat_names_end - widest,
            l::row_height
        );
    }
    pane->skill = screens::lobby_field_create(
        self, l::name_x, l::skill_y, l::fields_width, l::row_height, "Skill Level:", f, k_unknown
    );
    pane->commander_death = screens::lobby_field_create(
        self,
        l::name_x,
        l::death_y,
        l::fields_width,
        l::row_height,
        "On Commander Death:",
        f,
        k_unknown
    );
    pane->join = button_create();
    screens::function_button_setup(pane->join, false, l::join_x, l::join_y, "JOIN", self);
    window_set_state(pane->join, button_state::unpressed);
    pane->blocked_icon =
        image_load(screens::kWidgetsGaf, k_user_icons, k_blocked_frame, screens::kWidgetKey);
    // Hidden until a pulse shows it for a password battle, so that first
    // show is what gives the entry the keyboard.
    pane->password->base.visible = 0;
    pane->password_label->visible = 0;
    return base;
}

InfoPane* scoreboard_pane_create(MainFrame* frame, const char* title) {
    namespace l = score_layout;
    auto* pane = as_scoreboard(console_window_alloc(sizeof(ScoreboardPane), &k_scoreboard_ops));
    InfoPane* base = &pane->pane;
    base->slots = &k_scoreboard_slots;
    StatusWidget* owner = frame->owner;
    info_pane_init(base, title, &frame->base, owner);
    scoreboard_draw_rules(pane);
    Window* self = &base->base;
    const Handle medium = owner->system.hat_fonts.medium;
    const char* const headers[5] = {"Kills", "Losses", "Energy", "Metal", "Score"};
    for (int32_t i = 0; i < 5; ++i)
        screens::lobby_label_create(
            self,
            l::column_x[i],
            l::header_y,
            headers[i],
            medium,
            k_align_top_centre,
            k_anchor_top_left,
            i == 4 ? l::score_width : l::column_width,
            l::row_height
        );
    pane->name = screens::lobby_label_create(
        self,
        l::name_x,
        l::header_y,
        "",
        medium,
        k_align_top_left,
        k_anchor_top_left,
        l::name_width,
        l::row_height
    );
    for (int32_t i = 0; i < k_battle_seats; ++i) {
        const int32_t y = l::first_seat_y + i * l::row_height;
        ScoreboardSeat& seat = pane->seats[i];
        seat.icon = static_create();
        window_set_bounds(
            seat.icon,
            self->x + l::icon_x,
            self->y + y + l::icon_dy,
            l::icon_size,
            l::icon_size,
            self,
            true
        );
        seat.name = screens::lobby_label_create(
            self,
            l::seat_name_x,
            y,
            "",
            medium,
            k_align_left,
            k_anchor_top_left,
            l::seat_name_width,
            l::row_height
        );
        Window** columns[5] = {&seat.kills, &seat.losses, &seat.energy, &seat.metal, &seat.score};
        for (int32_t c = 0; c < 5; ++c)
            *columns[c] = screens::lobby_label_create(
                self,
                l::column_x[c],
                y,
                "",
                medium,
                k_align_centre,
                k_anchor_top_left,
                c == 4 ? l::score_width : l::column_width,
                l::row_height
            );
    }
    return base;
}

void scoreboard_pane_add_message(ScoreboardPane* pane, const char* message) {
    if (pane->messages == nullptr)
        return;
    screens::by_two_label_row_create(
        pane->messages, k_message_name, message, &pane->pane.owner->system.hat_fonts, false
    );
}

} // namespace oa::boneyards::console

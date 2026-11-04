// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Battle console: the planet's battles grouped by game mode, HOST, the planet
// intelligence log and the panes it opens over the log. The battle list is
// the navigation's screens::BattleList; the console's rows are rebuilt from
// its rows whenever they are marked for a rebuild.
#include <cstdlib>
#include <cstring>

#include "battle_console.hpp"
#include "oa/boneyards/release.hpp"
#include "widgets.hpp"

namespace oa::boneyards::console {

namespace {

constexpr const char* k_console_name = "BattleConsole";
constexpr const char* k_list_name = "BattleList";
constexpr const char* k_intel_name = "PlanetIntelList";
constexpr const char* k_green_button = "GREEN_BUTTON";
constexpr int32_t k_image_state = 1;

namespace layout {
// The title centred over the list's width.
constexpr int32_t title_y = 6, title_width = 0x7c;
// The BattleList, placed at this screen point, and HOST under it; both are
// shorter and higher with the matchmaking panel under them.
constexpr int32_t list_x = 0x17, list_y = 0x93, list_width = 0x7c;
constexpr int32_t list_height = 0x104, list_height_matchmaking = 0xf3;
constexpr int32_t host_x = 0xc, host_y = 0x11c, host_y_matchmaking = 0x10b;
// "PLANET INTELLIGENCE" centred over the log.
constexpr int32_t intel_title_x = 0x7c, intel_title_y = 6, intel_title_width = 0x176;
// The PlanetIntelList: 100 lines, bottom-aligned, a log.
constexpr int32_t intel_x = 0x7c, intel_y = 0x12, intel_width = 0x176, intel_height = 0x122;
constexpr int32_t intel_capacity = 100;
// Where an open pane moves the log and its title.
constexpr int32_t intel_open_y = 0xab, intel_open_height = 0x89;
constexpr int32_t intel_title_open_y = 0x9f;
// Intelligence lines continue this far in (indent 8).
constexpr int32_t intel_indent = 8;
} // namespace layout

// Label placements (lobby_label_create).
constexpr int32_t k_align_centre = 4;
constexpr int32_t k_align_left = 3;

// Row geometry: the rows are 0x11 high; a battle row's icon and title are
// indented under the mode's row.
constexpr int32_t k_row_icon_size = 0xb;
constexpr int32_t k_category_icon_inset = 3;
constexpr int32_t k_category_label_x = 0x10;
constexpr int32_t k_battle_icon_x = 3;
constexpr int32_t k_battle_icon_x_indented = 0x11;
constexpr int32_t k_battle_icon_y = 3;
constexpr int32_t k_battle_label_gap = 0xe; // past the icon's x, and short of the row's end
constexpr const char* k_battle_icon_name = "BattleIcon";
constexpr const char* k_category_icon_name = "CetegoryIcon";
constexpr const char* k_arrow_down = "Icon_Arrow_Down";
constexpr const char* k_arrow_up = "Icon_Arrow_Up";
constexpr const char* k_battle_icons = "Battle_Icons";
// Battle_Icons frames: a forming battle and one in progress.
constexpr int32_t k_frame_forming = 0;
constexpr int32_t k_frame_in_progress = 1;
constexpr int32_t k_no_frame = -1;

// Codes the panes send the console: opened (200), closed (0xC9), and the
// Host pane's START (0xCA) and the Join summary's JOIN (0xCB).
constexpr int32_t k_pane_start = 0xca;
constexpr int32_t k_pane_join = 0xcb;

// The console the battle list's lines and panel changes reach: set as it is
// made, cleared as it goes.
BattleConsole* g_open_battles = nullptr;

BattleConsole* as_console(Window* w) {
    return reinterpret_cast<BattleConsole*>(w);
}

screens::BattleList* list_of(const BattleConsole* console) {
    return &console->owner->navigation->battles;
}

void play_sound(const StatusWidget* owner, const char* name) {
    const MainMenuHost& host = owner->config.host;
    if (host.play_sound != nullptr)
        host.play_sound(host.user, name);
}

// ---- rows -------------------------------------------------------------------

// A battle row (BattleIcon): its record's Battle_Icons frame and name.
struct BattleIconRow {
    screens::ByTextRow row;
    int32_t battle;      // record index
    int32_t shown_frame; // the frame the icon shows, k_no_frame before the first
    Window* icon;
    BattleConsole* console;
};

// A mode row (CetegoryIcon): the mode's name and an arrow, down while its
// battles are listed under it.
struct CategoryIconRow {
    screens::ByTextRow row;
    std::size_t category;
    int32_t shown_expanded; // the arrow's state, -1 before the first
    Window* icon;
    BattleConsole* console;
};

BattleIconRow* as_battle_row(Window* w) {
    return reinterpret_cast<BattleIconRow*>(w);
}

CategoryIconRow* as_category_row(Window* w) {
    return reinterpret_cast<CategoryIconRow*>(w);
}

/// Gives the selected battle row's title the light small font.
///
/// @param self the battle row
void battle_row_select(Window* self) {
    const screens::ByTextRow& row = as_battle_row(self)->row;
    window_set_font_handle(row.label, row.fonts->small_light);
}

/// Gives any other battle row's title the small font.
///
/// @param self the battle row
void battle_row_unselect(Window* self) {
    const screens::ByTextRow& row = as_battle_row(self)->row;
    window_set_font_handle(row.label, row.fonts->small);
}

constexpr screens::ListRowLook k_battle_row_look{battle_row_select, battle_row_unselect};
// A mode row keeps its title's font whether or not it is selected.
constexpr screens::ListRowLook k_category_row_look{};

bool battle_row_draw(Window* self) {
    const screens::ByTextRow& row = as_battle_row(self)->row;
    return screens::list_row_draw(self, true, row.list->selected_item, k_battle_row_look);
}

bool category_row_draw(Window* self) {
    const screens::ByTextRow& row = as_category_row(self)->row;
    return screens::list_row_draw(self, true, row.list->selected_item, k_category_row_look);
}

/// Shows the battle's Battle_Icons frame (1 while it is in progress, 0 while it forms), loading it
/// again only when the battle's state has changed.
///
/// @param self the battle row
void battle_row_pulse(Window* self) {
    BattleIconRow* row = as_battle_row(self);
    const screens::BattleEntry* battle =
        screens::battle_list_entry(list_of(row->console), row->battle);
    if (battle == nullptr)
        return;
    const int32_t frame = battle->forming == 0 ? k_frame_in_progress : k_frame_forming;
    if (frame == row->shown_frame)
        return;
    row->shown_frame = frame;
    const Handle image =
        image_load(screens::kWidgetsGaf, k_battle_icons, frame, screens::kWidgetKey);
    window_set_state_image(row->icon, k_image_state, image);
    image_release(image);
}

/// Shows the mode's arrow, down while its battles are listed and up otherwise, binding it again
/// only when that has changed.
///
/// @param self the mode row
void category_row_pulse(Window* self) {
    CategoryIconRow* row = as_category_row(self);
    const screens::BattleList* list = list_of(row->console);
    if (row->category >= list->category_count)
        return;
    const int32_t expanded = list->categories[row->category].expanded ? 1 : 0;
    if (expanded == row->shown_expanded)
        return;
    row->shown_expanded = expanded;
    row->icon->ops->bind_images(
        row->icon,
        screens::kWidgetsGaf,
        expanded != 0 ? k_arrow_down : k_arrow_up,
        0,
        screens::kWidgetKey
    );
}

/// Makes a clicked row its list's selected row and forwards the notice to the list as the row's.
///
/// @param self the row
/// @param notify control notice
void row_on_notify(Window* self, const Notify* notify) {
    auto* row = reinterpret_cast<screens::ByTextRow*>(self);
    row->list->selected_item = self;
    Notify forwarded = *notify;
    forwarded.source = self;
    row->list->list.base.ops->on_notify(&row->list->list.base, &forwarded);
}

WindowOps make_battle_row_ops() {
    WindowOps ops = k_static_ops;
    ops.destroy = console_window_free;
    ops.pulse = battle_row_pulse;
    ops.on_notify = row_on_notify;
    ops.draw = battle_row_draw;
    return ops;
}

WindowOps make_category_row_ops() {
    WindowOps ops = k_static_ops;
    ops.destroy = console_window_free;
    ops.pulse = category_row_pulse;
    ops.on_notify = row_on_notify;
    ops.draw = category_row_draw;
    return ops;
}

const WindowOps k_battle_row_ops = make_battle_row_ops();
const WindowOps k_category_row_ops = make_category_row_ops();

// Starts a row across the list, 0x11 high, taking the selected look.
void row_init(screens::ByTextRow* row, screens::ByListBox* list, const screens::HatFonts* fonts) {
    row->base.focusable = 0;
    row->list = list;
    row->fonts = fonts;
    row->selectable = true;
    Window* list_window = &list->list.base;
    window_set_bounds(
        &row->base,
        list_window->x,
        list_window->y,
        screens::by_list_row_width(list_window),
        screens::kByRowHeight,
        list_window,
        true
    );
}

/// Appends a mode row: its arrow 3 pixels in from the row's corner and its name 0x10 in.
///
/// @param[in,out] console the battle console
/// @param category the mode's index in the battle list
/// @return the row
CategoryIconRow* category_row_create(BattleConsole* console, std::size_t category) {
    screens::ByListBox* list = console->list;
    const screens::HatFonts* fonts = &console->owner->system.hat_fonts;
    auto* row = as_category_row(console_window_alloc(sizeof(CategoryIconRow), &k_category_row_ops));
    row->console = console;
    row->category = category;
    row->shown_expanded = -1;
    row_init(&row->row, list, fonts);
    Window* self = &row->row.base;
    row->icon = static_create();
    window_create(
        row->icon,
        k_category_icon_name,
        self->x + k_category_icon_inset,
        self->y + k_category_icon_inset,
        k_row_icon_size,
        k_row_icon_size,
        self,
        true
    );
    const screens::BattleCategory& c = list_of(console)->categories[category];
    row->row.label = screens::lobby_label_create(
        self,
        k_category_label_x,
        0,
        c.mode.name,
        fonts->small,
        k_align_left,
        0,
        screens::by_list_row_width(&list->list.base) - k_category_label_x,
        screens::kByRowHeight
    );
    list_box_add(&list->list, self);
    category_row_pulse(self);
    return row;
}

/// Appends a battle row: its Battle_Icons icon and the battle's name, indented under its mode.
///
/// @param[in,out] console the battle console
/// @param battle the record's index
/// @param indent whether the row sits under its mode's row
/// @return the row
/// @quirk The icon is made at the screen point (3, 3), or (0x11, 3) indented, rather than inside
///     the row, and the name 0xe past that point is as wide as the row less 0xe, running past
///     the row's end; adding the row to the list clamps both into the row, the icon into its
///     top-left corner and the name to end at its right edge (0xe in).
BattleIconRow* battle_row_create(BattleConsole* console, int32_t battle, bool indent) {
    screens::ByListBox* list = console->list;
    const screens::HatFonts* fonts = &console->owner->system.hat_fonts;
    auto* row = as_battle_row(console_window_alloc(sizeof(BattleIconRow), &k_battle_row_ops));
    row->console = console;
    row->battle = battle;
    row->shown_frame = k_no_frame;
    row_init(&row->row, list, fonts);
    Window* self = &row->row.base;
    const int32_t icon_x = indent ? k_battle_icon_x_indented : k_battle_icon_x;
    row->icon = static_create();
    window_create(
        row->icon,
        k_battle_icon_name,
        icon_x,
        k_battle_icon_y,
        k_row_icon_size,
        k_row_icon_size,
        self,
        true
    );
    const screens::BattleEntry* entry = screens::battle_list_entry(list_of(console), battle);
    row->row.label = screens::lobby_label_create(
        self,
        icon_x + k_battle_label_gap,
        0,
        entry != nullptr ? entry->name : "",
        fonts->small,
        k_align_left,
        0,
        screens::by_list_row_width(&list->list.base) - k_battle_label_gap,
        screens::kByRowHeight
    );
    list_box_add(&list->list, self);
    battle_row_pulse(self);
    return row;
}

/// Rebuilds the BattleList's rows when the battle list marks them: every mode, with its battles
/// under it while it is open.
///
/// @param[in,out] console the battle console
void rebuild_rows(BattleConsole* console) {
    screens::BattleList* list = list_of(console);
    if (!list->rows_dirty)
        return;
    screens::battle_list_rebuild_rows(list);
    screens::by_list_box_clear(console->list);
    for (std::size_t i = 0; i < list->row_count; ++i) {
        const screens::BattleRow& row = list->rows[i];
        if (row.is_category)
            category_row_create(console, row.category);
        else
            battle_row_create(console, row.battle, true);
    }
}

// ---- panes ------------------------------------------------------------------

InfoPane* pane_of(const BattleConsole* console, screens::BattlePanel panel) {
    return console->panes[static_cast<int32_t>(panel)];
}

bool pane_shown(const BattleConsole* console, screens::BattlePanel panel) {
    const InfoPane* pane = pane_of(console, panel);
    return pane != nullptr && window_is_shown(&pane->base);
}

/// Closes each shown pane through its close slot: the Host pane, then the Join summary and the
/// Scoreboard, which let go of their battles.
///
/// @param[in,out] console the battle console
void close_panes(BattleConsole* console) {
    for (screens::BattlePanel panel :
         {screens::BattlePanel::host,
          screens::BattlePanel::join,
          screens::BattlePanel::scoreboard}) {
        if (pane_shown(console, panel)) {
            InfoPane* pane = pane_of(console, panel);
            pane->slots->close(pane);
        }
    }
}

/// Follows the battle list's panels after a battle's feed changed them: a pane it closed closes,
/// then one it opened opens.
///
/// @param user unused context of the battle list's output hook
void follow_panels(void* /*user*/) {
    BattleConsole* console = g_open_battles;
    if (console == nullptr)
        return;
    const screens::BattleList* list = list_of(console);
    constexpr screens::BattlePanel panels[] = {
        screens::BattlePanel::host, screens::BattlePanel::join, screens::BattlePanel::scoreboard
    };
    for (screens::BattlePanel panel : panels) {
        InfoPane* pane = pane_of(console, panel);
        if (pane != nullptr && window_is_shown(&pane->base) &&
            !screens::battle_list_panel_shown(list, panel))
            info_pane_close(pane);
    }
    for (screens::BattlePanel panel : panels) {
        InfoPane* pane = pane_of(console, panel);
        if (pane != nullptr && !window_is_shown(&pane->base) &&
            screens::battle_list_panel_shown(list, panel))
            info_pane_open(pane);
    }
}

/// Wraps a planet intelligence line into the log, continuation rows eight pixels in.
///
/// @param user unused context of the battle list's output hook
/// @param text the line
void intel_line(void* /*user*/, const char* text) {
    BattleConsole* console = g_open_battles;
    if (console == nullptr)
        return;
    screens::by_list_wrap_text(
        console->intel, text, &console->owner->system.hat_fonts, layout::intel_indent, k_no_handle
    );
}

/// Hands a report message to the Scoreboard showing its battle.
///
/// @param user unused context of the battle list's output hook
/// @param text the message
void report_message(void* /*user*/, const char* text) {
    BattleConsole* console = g_open_battles;
    if (console == nullptr)
        return;
    InfoPane* pane = pane_of(console, screens::BattlePanel::scoreboard);
    if (pane != nullptr)
        scoreboard_pane_add_message(reinterpret_cast<ScoreboardPane*>(pane), text);
}

// ---- the console ------------------------------------------------------------

/// Handles HOST, the panes' notices and the BattleList.
///
/// HOST closes the shown panes and, unless the Host pane was among them,
/// sounds GREEN_BUTTON and opens it. A pane's notice sounds GREEN_BUTTON
/// unless it closed; the Host pane's START and the Join summary's JOIN start
/// the activity light. A mode row picked sounds GREEN_BUTTON and shows or
/// hides its battles; a battle row sounds GREEN_BUTTON, asks for the
/// battle's intelligence and shows it on the Scoreboard while in progress
/// or on the Join summary while it forms, closing the shown panes first.
/// Notices from any other window, the matchmaking panel's SETTINGS among
/// them, change nothing.
///
/// @param self the battle console
/// @param notify control notice
void battle_console_on_notify(Window* self, const Notify* notify) {
    BattleConsole* console = as_console(self);
    screens::BattleList* list = list_of(console);
    const Window* source = notify->source;
    if (source == console->host_button) {
        const bool was_open = pane_shown(console, screens::BattlePanel::host);
        close_panes(console);
        if (was_open)
            return;
        play_sound(console->owner, k_green_button);
        InfoPane* host = pane_of(console, screens::BattlePanel::host);
        if (host != nullptr)
            host->slots->open(host);
        return;
    }
    for (InfoPane* pane : console->panes) {
        if (pane == nullptr || source != &pane->base)
            continue;
        const int32_t code = static_cast<int32_t>(notify->code);
        if (code != pane_notify::closed)
            play_sound(console->owner, k_green_button);
        if ((code == k_pane_start || code == k_pane_join) &&
            console->frame->activity_light != nullptr)
            activity_light_start(console->frame->activity_light);
        return;
    }
    if (source != &console->list->list.base)
        return;
    play_sound(console->owner, k_green_button);
    const Window* item = console->list->selected_item;
    if (item == nullptr)
        return;
    if (item->ops == &k_category_row_ops) {
        const std::size_t category = reinterpret_cast<const CategoryIconRow*>(item)->category;
        if (category < list->category_count) {
            list->rows_dirty = true;
            list->categories[category].expanded = !list->categories[category].expanded;
        }
        return;
    }
    if (item->ops != &k_battle_row_ops)
        return;
    const int32_t battle = reinterpret_cast<const BattleIconRow*>(item)->battle;
    const screens::BattleEntry* entry = screens::battle_list_entry(list, battle);
    if (entry == nullptr)
        return;
    screens::battle_list_request_intel(list, battle);
    const screens::BattlePanel panel =
        entry->forming == 0 ? screens::BattlePanel::scoreboard : screens::BattlePanel::join;
    if (entry->forming != 0 && entry->forming != 1)
        return;
    close_panes(console);
    screens::battle_list_hold(list, panel, battle);
    InfoPane* pane = pane_of(console, panel);
    if (pane != nullptr)
        pane->slots->open(pane);
}

/// Rebuilds the BattleList's rows once the battle list marks them.
///
/// @param self the battle console
void battle_console_pulse(Window* self) {
    rebuild_rows(as_console(self));
}

/// Builds the battle console hidden when its bounds are set.
///
/// Adds the shared console image, the "BATTLES" title, the BattleList, HOST,
/// the "PLANET INTELLIGENCE" title over the PlanetIntelList log, and when
/// Matchmaking is on screen the matchmaking panel under a shorter list and a
/// higher HOST.
///
/// @param self the battle console
void battle_console_build(Window* self) {
    namespace l = layout;
    BattleConsole* console = as_console(self);
    StatusWidget* owner = console->owner;
    const screens::HatFonts& fonts = owner->system.hat_fonts;
    const bool matchmaking =
        gated_feature_shown(ReleaseFeature::matchmaking, owner->system.matchmaking_enabled);
    window_set_state_image(self, k_image_state, owner->system.profile_image);
    screens::lobby_label_create(
        self, 0, l::title_y, "BATTLES", fonts.big, k_align_centre, 0, l::title_width
    );
    console->list = screens::by_list_box_create(false, false);
    window_create(
        &console->list->list.base,
        k_list_name,
        l::list_x,
        l::list_y,
        l::list_width,
        matchmaking ? l::list_height_matchmaking : l::list_height,
        self,
        true
    );
    console->host_button = button_create();
    screens::function_button_setup(
        console->host_button,
        false,
        l::host_x,
        matchmaking ? l::host_y_matchmaking : l::host_y,
        "HOST",
        self
    );
    window_set_state(console->host_button, button_state::unpressed);
    console->intel_title = screens::lobby_label_create(
        self,
        l::intel_title_x,
        l::intel_title_y,
        "PLANET INTELLIGENCE",
        fonts.big,
        k_align_centre,
        0,
        l::intel_title_width
    );
    console->intel = screens::by_list_box_create(false, false);
    ListBox* intel = &console->intel->list;
    window_create(
        &intel->base,
        k_intel_name,
        self->x + l::intel_x,
        self->y + l::intel_y,
        l::intel_width,
        l::intel_height,
        self,
        true
    );
    intel->bottom_align = 1;
    intel->capacity = l::intel_capacity;
    intel->log_mode = 1;
    if (matchmaking)
        console->match_control =
            match_control_panel_create(owner, &owner->navigation->matchmaking, self);
    self->visible = 0;
}

// Gives the Main Frame back the panes this console holds, hidden and making
// room for nothing, so they outlive it.
void battle_console_release_panes(Window* self) {
    BattleConsole* console = as_console(self);
    for (InfoPane* pane : console->frame->panes) {
        if (pane == nullptr || pane->base.parent != self)
            continue;
        pane->base.visible = 0;
        info_pane_set_layout(pane, InfoPaneLayout{});
        window_set_parent(&pane->base, &console->frame->base);
    }
}

/// Stops being the console the battle list's lines reach and frees the window.
///
/// The link is cleared only while it still names this console, as a replaced
/// console is destroyed after its successor is built.
///
/// @param self the battle console
void battle_console_destroy(Window* self) {
    if (g_open_battles == as_console(self))
        g_open_battles = nullptr;
    console_window_free(self);
}

WindowOps make_console_ops() {
    WindowOps ops = console_panel_ops();
    ops.destroy = battle_console_destroy;
    ops.on_destroy = battle_console_release_panes;
    ops.on_bounds = battle_console_build;
    ops.pulse = battle_console_pulse;
    ops.on_notify = battle_console_on_notify;
    return ops;
}

const WindowOps k_console_ops = make_console_ops();

} // namespace

void battle_console_open(Window* self) {
    namespace l = layout;
    BattleConsole* console = as_console(self);
    // Matchmaking may be turned on after the console was built. The next
    // opening then shortens the list, raises HOST and adds the panel.
    const bool matchmaking = gated_feature_shown(
        ReleaseFeature::matchmaking, console->owner->system.matchmaking_enabled
    );
    if (matchmaking && console->match_control == nullptr) {
        console->list->list.base.height = l::list_height_matchmaking;
        console->host_button->y = self->y + l::host_y_matchmaking;
        console->match_control = match_control_panel_create(
            console->owner, &console->owner->navigation->matchmaking, self
        );
    }
    const InfoPaneLayout panes{
        &console->intel->list,
        l::intel_y,
        l::intel_height,
        l::intel_open_y,
        l::intel_open_height,
        console->intel_title,
        l::intel_title_y,
        l::intel_title_open_y
    };
    constexpr screens::BattlePanel order[] = {
        screens::BattlePanel::host, screens::BattlePanel::scoreboard, screens::BattlePanel::join
    };
    constexpr PaneKind kinds[] = {
        PaneKind::host_battle, PaneKind::join_summary, PaneKind::scoreboard
    };
    for (screens::BattlePanel panel : order) {
        const auto index = static_cast<int32_t>(panel);
        console->panes[index] = main_frame_pane(console->frame, kinds[index], self, panes);
    }
    self->visible = 1;
    for (screens::BattlePanel panel : order) {
        if (pane_shown(console, panel)) {
            InfoPane* pane = pane_of(console, panel);
            pane->slots->open(pane);
            break;
        }
    }
    auto* button = reinterpret_cast<ConsoleNavButton*>(console->battle_button);
    window_set_state_image(&button->base, button_state::unpressed, button->lit);
    screens::BoneyardsScreens* nav = console->owner->navigation;
    screens::battle_list_open(&nav->battles, nav->session.area_id);
}

void battle_console_close(Window* self) {
    BattleConsole* console = as_console(self);
    for (screens::BattlePanel panel :
         {screens::BattlePanel::host,
          screens::BattlePanel::scoreboard,
          screens::BattlePanel::join}) {
        if (pane_shown(console, panel)) {
            info_pane_close(pane_of(console, panel));
            break;
        }
    }
    auto* button = reinterpret_cast<ConsoleNavButton*>(console->battle_button);
    window_set_state_image(&button->base, button_state::unpressed, button->normal);
    screens::by_list_box_clear(console->intel);
    self->visible = 0;
    screens::BoneyardsScreens* nav = console->owner->navigation;
    screens::battle_list_close(&nav->battles, nav->session.area_id);
}

Window* battle_console_create(
    StatusWidget* owner, Window* frame, int32_t x, int32_t y, int32_t width, int32_t height
) {
    auto* console = as_console(console_window_alloc(sizeof(BattleConsole), &k_console_ops));
    auto* main_frame = reinterpret_cast<MainFrame*>(frame);
    console->frame = main_frame;
    console->battle_button =
        main_frame->console_buttons[static_cast<int32_t>(ConsoleButton::battle)];
    console->owner = owner;
    g_open_battles = console;
    screens::BoneyardsScreens* nav = owner->navigation;
    screens::boneyards_open(nav, screens::ByScreen::battle_list);
    // The rows are built on the console's first pulse.
    nav->battles.rows_dirty = true;
    screens::BattleUi& ui = nav->battles.ui;
    ui.intel = intel_line;
    ui.report_message = report_message;
    ui.panels_changed = follow_panels;
    window_create(&console->base, k_console_name, x, y, width, height, frame, true);
    return &console->base;
}

} // namespace oa::boneyards::console

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Locator console. The Main Frame opens it for an account that sees Locator.
// The window is gui/locator.gui. This file wires that screen to the search:
// the field's prompt and font, which of RELOCATE, JOIN and PAGE are enabled,
// and the rows filled into the results list, the selected one on a band of
// palette 44. Return in the field runs SEARCH.
#include "oa/boneyards/console/feature_consoles.hpp"

#include "console_layer.hpp"
#include "console_state.hpp"
#include "widgets.hpp"

#include "oa/boneyards/console/gui_screen.hpp"
#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/screens/battle_list.hpp"
#include "oa/boneyards/screens/galaxy.hpp"
#include "oa/boneyards/screens/lobby_widgets.hpp"
#include "oa/boneyards/screens/chat.hpp"
#include "oa/boneyards/screens/locator.hpp"
#include "oa/boneyards/shown_text.hpp"
#include "oa/boneyards/utf8.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace oa::boneyards::console {
namespace {

constexpr int32_t k_kinds = 5;
constexpr int32_t k_drawn_rows = 8;
constexpr int32_t k_row_height = 15;
constexpr int32_t k_detail_lines = 3;
constexpr std::size_t k_detail_line_bytes = 96;
// The detail breaks between words before this many pixels, as the design
// breaks its lines; the rows themselves are 340 wide.
constexpr int32_t k_detail_wrap_width = 260;
constexpr int32_t k_icon_size = 11;
constexpr int32_t k_host_capacity = 4;
// Where each column of a row starts, from the list's left edge.
constexpr int32_t k_icon_x = 4;
constexpr int32_t k_name_x = 18;
constexpr int32_t k_where_x = 141;
constexpr int32_t k_status_x = 271;
constexpr int32_t k_column_width = 120;
// The selected row's band.
constexpr uint8_t k_selected_band = 44;
constexpr const char* k_icons = "BY_New.gaf";
constexpr const char* k_icon_sequence = "Locator_Icons";
constexpr int32_t k_field_style = text_style::right | text_style::top;
constexpr int32_t k_line_style = text_style::left | text_style::top;

const screens::HatFonts k_empty_fonts{};

struct ResultWidgets {
    Window* band{}; // the selected row's band; null on the others
    Window* icon;
    Window* name;
    Window* where;
    Window* status;
    Window* hit;
};

struct LocatorHost {
    WindowOps ops;
    void (*release)(Window*);
    void (*note)(Window*, const Notify*);
    bool (*key_down)(Window*, const Message*){};
    Window* (*focus_target)(Window*, Window*, bool){};
    GuiScreen* screen;
    Window* root;
    StatusWidget* owner;
    screens::Locator model;
    const screens::HatFonts* fonts;
    ResultWidgets rows[k_drawn_rows];
    int32_t row_count;
    bool refresh_pending;
    // The name field takes the keyboard once per open and once per search
    // face. A result refresh does not move it.
    bool keyboard_taken{};
    bool shown_before{};
    int32_t keyboard_kind{};
};

LocatorHost* g_hosts[k_host_capacity]{};

LocatorHost* host_of(const Window* window) {
    if (window == nullptr)
        return nullptr;
    for (LocatorHost* host : g_hosts) {
        if (host != nullptr && host->root == window)
            return host;
    }
    return nullptr;
}

void remember(LocatorHost* host) {
    for (LocatorHost*& slot : g_hosts) {
        if (slot == nullptr) {
            slot = host;
            return;
        }
    }
}

void forget(LocatorHost* host) {
    for (LocatorHost*& slot : g_hosts) {
        if (slot == host)
            slot = nullptr;
    }
}

Window* named(LocatorHost* host, const char* name) {
    const GuiControl* control = gui_screen_find(host->screen, name);
    return control != nullptr ? control->window : nullptr;
}

void set_shown(Window* window, bool shown) {
    if (window != nullptr)
        window->visible = shown ? 1 : 0;
}

void set_text(Window* window, const char* text, int32_t style) {
    if (window != nullptr && window->ops != nullptr && window->ops->set_caption != nullptr)
        window->ops->set_caption(window, text != nullptr ? text : "", style);
}

void use_font(Window* window, Handle font) {
    if (window != nullptr && font >= 0)
        window_set_font_handle(window, font);
}

void clear_fill(Window* window) {
    if (window == nullptr)
        return;
    if (window->color >= 0)
        color_release(window->color);
    window->color = k_no_handle;
}

const char* typed_query(const Window* name) {
    if (name == nullptr || name->ops == nullptr || name->ops->is_class == nullptr)
        return "";
    if (!name->ops->is_class(class_name::edit_box))
        return "";
    const char* text = reinterpret_cast<const EditBox*>(name)->text;
    return text != nullptr ? text : "";
}

void show_icon(Window* icon, int32_t frame) {
    if (icon == nullptr)
        return;
    const Handle image = image_load(k_icons, k_icon_sequence, frame, k_overlay_transparent);
    if (image < 0)
        return;
    window_set_state_image(icon, 0, image);
    window_set_state_image(icon, 1, image);
    image_release(image);
}

void shift_children(Window* window, int32_t dx, int32_t dy) {
    for (int32_t i = 0; i < window->children.count; ++i) {
        Window* child = window->children.items[i];
        if (child == nullptr)
            continue;
        child->x += dx;
        child->y += dy;
        shift_children(child, dx, dy);
    }
}

void sync_from_owner(LocatorHost* host) {
    if (host->owner == nullptr || host->owner->navigation == nullptr)
        return;
    screens::BoneyardsScreens* navigation = host->owner->navigation;
    screens::locator_set_identity(
        &host->model, navigation->login.session_name, navigation->login.session_password
    );
    screens::locator_set_here(
        &host->model, navigation->session.area_id, navigation->session.room_id
    );
    screens::LocatorPlanet planets[screens::kLocatorPlanetCapacity];
    int32_t count = static_cast<int32_t>(navigation->galaxy.planet_count);
    if (count > screens::kLocatorPlanetCapacity)
        count = screens::kLocatorPlanetCapacity;
    if (count < 0)
        count = 0;
    for (int32_t i = 0; i < count; ++i) {
        const screens::GalaxyPlanet& planet = navigation->galaxy.planets[i];
        screens::LocatorPlanet& out = planets[i];
        out = {};
        out.id = static_cast<uint32_t>(planet.id);
        copy_text(out.name, sizeof out.name, planet.name);
        out.side = planet.side;
        copy_text(out.ta_map, sizeof out.ta_map, planet.ta_map);
        out.map_source = static_cast<int32_t>(planet.map_source);
        out.in_progress = planet.in_progress;
        out.forming = planet.forming;
    }
    screens::locator_set_planets(&host->model, planets, count);
}

void open_chat(void* user, uint32_t area_id, uint32_t room_id, const char* page_line) {
    auto* host = static_cast<LocatorHost*>(user);
    if (host->owner == nullptr || host->owner->navigation == nullptr)
        return;
    screens::BoneyardsScreens* navigation = host->owner->navigation;
    if (area_id != 0)
        navigation->session.area_id = area_id;
    // Opening chat on a planet looks at that planet, so the room list is
    // the one the player asked for and not whichever planet was selected.
    if (area_id != 0) {
        screens::GalaxyView* galaxy = &navigation->galaxy;
        const int32_t count = static_cast<int32_t>(galaxy->planet_count);
        for (int32_t i = 0; i < count; ++i) {
            if (galaxy->planets[i].id != static_cast<int32_t>(area_id))
                continue;
            uint32_t selected = 0;
            if (screens::galaxy_select_planet(galaxy, i, &selected))
                navigation->session.area_id = area_id;
            break;
        }
    }
    if (room_id != 0)
        navigation->session.room_id = room_id;
    const bool page = page_line != nullptr && page_line[0] != '\0';
    if (page)
        oa::boneyards::utf8::copy(
            navigation->chat.input,
            sizeof navigation->chat.input,
            page_line,
            screens::kChatLineCharacters
        );
    if (MainFrame* frame = host->owner->system.main_frame; frame != nullptr)
        main_frame_show_screen(frame, console_screen::chat);
    if (page && navigation->chat.ui.show_input != nullptr)
        navigation->chat.ui.show_input(navigation->chat.ui.user);
}

void open_planet(void* user, uint32_t area_id) {
    auto* host = static_cast<LocatorHost*>(user);
    if (host->owner == nullptr || host->owner->navigation == nullptr)
        return;
    screens::BoneyardsScreens* navigation = host->owner->navigation;
    navigation->session.area_id = area_id;
    screens::GalaxyView* galaxy = &navigation->galaxy;
    const int32_t count = static_cast<int32_t>(galaxy->planet_count);
    for (int32_t i = 0; i < count; ++i) {
        if (galaxy->planets[i].id != static_cast<int32_t>(area_id))
            continue;
        uint32_t selected = 0;
        screens::galaxy_select_planet(galaxy, i, &selected);
        navigation->session.area_id = area_id;
        break;
    }
    if (MainFrame* frame = host->owner->system.main_frame; frame != nullptr)
        main_frame_close_screen(frame);
}

void open_battle_screen(void* user, uint32_t area_id) {
    auto* host = static_cast<LocatorHost*>(user);
    if (host->owner == nullptr || host->owner->navigation == nullptr)
        return;
    host->owner->navigation->session.area_id = area_id;
    if (MainFrame* frame = host->owner->system.main_frame; frame != nullptr)
        main_frame_show_screen(frame, console_screen::battle);
}

void open_join(void* user, uint32_t area_id, const char* game_id, bool needs_password) {
    open_battle_screen(user, area_id);
    if (!needs_password || game_id == nullptr || game_id[0] == '\0')
        return;
    auto* host = static_cast<LocatorHost*>(user);
    if (host->owner == nullptr || host->owner->navigation == nullptr)
        return;
    const screens::LocatorResult* selected = nullptr;
    if (host->model.selected >= 0 && host->model.selected < host->model.result_count)
        selected = &host->model.results[host->model.selected];
    const char* battle_name =
        selected != nullptr && selected->name[0] != '\0' ? selected->name : game_id;
    const int32_t battle_type = selected != nullptr ? selected->battle_type : 1;
    screens::BattleList* list = &host->owner->navigation->battles;
    screens::battle_list_on_battle_info(list, area_id, 0, game_id, battle_name, battle_type, 1, 0);
    const int32_t battle = screens::battle_list_find(list, game_id);
    if (battle < 0)
        return;
    BattleIntel intel{};
    intel.game_id = game_id;
    intel.user_name = "";
    intel.mission = "";
    intel.has_password = 1;
    intel.on_death = 1;
    screens::battle_list_on_battle_intel(list, &intel);
    screens::battle_list_hold(list, screens::BattlePanel::join, battle);
    MainFrame* frame = host->owner->system.main_frame;
    if (frame == nullptr)
        return;
    InfoPane* pane = frame->panes[static_cast<int32_t>(PaneKind::join_summary)];
    if (pane != nullptr && pane->slots != nullptr && pane->slots->open != nullptr)
        pane->slots->open(pane);
}

void on_changed(void* user) {
    static_cast<LocatorHost*>(user)->refresh_pending = true;
}

void clear_rows(LocatorHost* host) {
    for (int32_t i = 0; i < host->row_count; ++i) {
        ResultWidgets& row = host->rows[i];
        window_destroy(row.hit);
        window_destroy(row.status);
        window_destroy(row.where);
        window_destroy(row.name);
        window_destroy(row.icon);
        window_destroy(row.band);
        row = {};
    }
    host->row_count = 0;
}

// Width of `text` in `font`, in pixels; 0 without a text service.
int32_t text_width(Handle font, const char* text) {
    int32_t width = 0;
    int32_t height = 0;
    text_measure_string(font, text, &width, &height);
    return width;
}

// Copies the first `count` bytes of `text` into a detail line.
void take_line(char* line, const char* text, std::size_t count) {
    if (count > k_detail_line_bytes - 1)
        count = k_detail_line_bytes - 1;
    std::memcpy(line, text, count);
    line[count] = '\0';
}

// Splits a detail into its lines. A detail that holds line breaks keeps
// them; any other breaks between words, each line as long as fits in
// k_detail_wrap_width pixels of `font`. A word wider than that is a line
// of its own.
void detail_lines(const char* detail, Handle font, char lines[][k_detail_line_bytes]) {
    for (int32_t i = 0; i < k_detail_lines; ++i)
        lines[i][0] = '\0';
    if (detail == nullptr || detail[0] == '\0')
        return;
    if (std::strchr(detail, '\n') != nullptr) {
        const char* cursor = detail;
        for (int32_t i = 0; i < k_detail_lines && *cursor != '\0'; ++i) {
            const char* end = std::strchr(cursor, '\n');
            take_line(
                lines[i],
                cursor,
                end != nullptr ? static_cast<std::size_t>(end - cursor) : std::strlen(cursor)
            );
            if (end == nullptr)
                break;
            cursor = end + 1;
        }
        return;
    }
    const char* cursor = detail;
    for (int32_t i = 0; i < k_detail_lines && *cursor != '\0'; ++i) {
        char trial[k_detail_line_bytes];
        std::size_t count = 0;
        for (std::size_t end = 0;; ++end) {
            const char c = cursor[end];
            if (c != ' ' && c != '\0')
                continue;
            take_line(trial, cursor, end);
            if (count != 0 &&
                (end > k_detail_line_bytes - 1 || text_width(font, trial) > k_detail_wrap_width))
                break;
            count = end;
            if (c == '\0')
                break;
        }
        take_line(lines[i], cursor, count);
        cursor += count;
        while (*cursor == ' ')
            ++cursor;
    }
}

int32_t row_clicked(const LocatorHost* host, const Window* source);

Window*
add_static(Window* parent, const char* name, int32_t x, int32_t y, int32_t width, int32_t height) {
    Window* window = static_create();
    window_create(window, name, x, y, width, height, parent, true);
    clear_fill(window);
    return window;
}

void add_result_row(
    LocatorHost* host, Window* list, int32_t index, const screens::LocatorResult* result
) {
    const int32_t y = list->y + index * k_row_height;
    const bool selected = index == screens::locator_selected(&host->model);
    const int32_t kind = screens::locator_kind_of(&host->model);
    const screens::HatFonts* fonts = host->fonts;
    char name[16];
    ResultWidgets& row = host->rows[index];
    if (selected) {
        std::snprintf(name, sizeof name, "rband%d", index);
        row.band = add_static(host->root, name, list->x, y, list->width, k_row_height);
        const Handle band =
            image_create_blank(list->width, k_row_height, k_selected_band, k_overlay_transparent);
        if (band >= 0) {
            window_set_state_image(row.band, 0, band);
            window_set_state_image(row.band, 1, band);
            image_release(band);
        }
    }
    std::snprintf(name, sizeof name, "ricon%d", index);
    row.icon = add_static(host->root, name, list->x + k_icon_x, y + 2, k_icon_size, k_icon_size);
    show_icon(row.icon, kind * 2 + (selected ? 1 : 0));
    std::snprintf(name, sizeof name, "rname%d", index);
    row.name = add_static(host->root, name, list->x + k_name_x, y + 3, k_column_width, 12);
    set_text(row.name, result->name, k_line_style);
    use_font(row.name, selected ? fonts->small_light : fonts->small);
    std::snprintf(name, sizeof name, "rwhere%d", index);
    row.where = add_static(host->root, name, list->x + k_where_x, y + 3, k_column_width, 12);
    set_text(row.where, result->where, k_line_style);
    use_font(row.where, fonts->small);
    // The status runs to the list's right edge.
    std::snprintf(name, sizeof name, "rstatus%d", index);
    row.status =
        add_static(host->root, name, list->x + k_status_x, y + 3, list->width - k_status_x, 12);
    set_text(row.status, result->status, k_line_style);
    use_font(row.status, result->bright ? fonts->small_green : fonts->small);
    std::snprintf(name, sizeof name, "rhit%d", index);
    row.hit = hotspot_button_create();
    window_create(row.hit, name, list->x, y, list->width, k_row_height, host->root, true);
}

// Gives the name field the keyboard on a face that asks for a name, once
// for that face. Server Transfer has no field: a key must not fill the
// hidden name. Nothing moves while the console is hidden, or once this
// face already took the keyboard.
void focus_locator_field(LocatorHost* host) {
    if (!window_is_shown(host->root))
        return;
    const int32_t kind = screens::locator_kind_of(&host->model);
    Window* name = named(host, "Name");
    if (!screens::locator_shows_field(kind)) {
        if (Desktop* desktop = services().desktop; desktop != nullptr && desktop->focus == name)
            services_desktop_focus(nullptr);
        host->keyboard_taken = true;
        host->keyboard_kind = kind;
        return;
    }
    if (host->keyboard_taken && host->keyboard_kind == kind)
        return;
    if (name != nullptr)
        services_desktop_focus(name);
    host->keyboard_taken = true;
    host->keyboard_kind = kind;
}

void refresh(LocatorHost* host) {
    const screens::Locator& model = host->model;
    const int32_t kind = screens::locator_kind_of(&model);
    const screens::HatFonts* fonts = host->fonts;
    char control_name[16];
    for (int32_t i = 0; i < k_kinds; ++i) {
        const bool selected = i == kind;
        std::snprintf(control_name, sizeof control_name, "icon_%d", i);
        Window* icon = named(host, control_name);
        show_icon(icon, i * 2 + (selected ? 1 : 0));
        // The icon and the caption sit on the kind row. A click hits them,
        // and the pointer release would take the keyboard back off the name
        // field unless they pass that request on.
        if (icon != nullptr)
            icon->focusable = 0;
        std::snprintf(control_name, sizeof control_name, "kind_%d", i);
        Window* caption = named(host, control_name);
        use_font(caption, selected ? fonts->small_light : fonts->small);
        if (caption != nullptr)
            caption->focusable = 0;
    }
    const bool field = screens::locator_shows_field(kind);
    const char* prompt = screens::locator_field_label(kind);
    Window* field_label = named(host, "Field");
    set_text(field_label, shown_text(prompt != nullptr ? prompt : ""), k_field_style);
    use_font(field_label, fonts->medium);
    set_shown(field_label, field);
    set_shown(named(host, "NameBox"), field);
    Window* name_field = named(host, "Name");
    use_font(name_field, fonts->small);
    set_shown(name_field, field);
    set_shown(named(host, "Search"), field);
    const bool page = screens::locator_shows_page(kind);
    const bool join = screens::locator_shows_join(kind);
    Window* extra = named(host, "Page");
    if (extra != nullptr)
        screens::window_set_four_state_texts(extra, join ? "JOIN" : "PAGE");
    set_shown(extra, page || join);
    Window* relocate = named(host, "Relocate");
    if (relocate != nullptr) {
        window_set_state(
            relocate,
            screens::locator_relocate_enabled(&model) ? button_state::unpressed
                                                      : button_state::disabled
        );
    }
    if (extra != nullptr) {
        window_set_state(
            extra,
            screens::locator_extra_enabled(&model) ? button_state::unpressed
                                                   : button_state::disabled
        );
    }
    use_font(named(host, "Title"), fonts->big);
    use_font(named(host, "HdrName"), fonts->small_dim);
    use_font(named(host, "HdrWhere"), fonts->small_dim);
    use_font(named(host, "HdrStatus"), fonts->small_dim);
    for (int32_t i = 0; i < 4; ++i) {
        std::snprintf(control_name, sizeof control_name, "Help%d", i);
        use_font(named(host, control_name), fonts->small);
    }
    char lines[k_detail_lines][k_detail_line_bytes];
    detail_lines(screens::locator_detail(&model), fonts->small_light, lines);
    for (int32_t i = 0; i < k_detail_lines; ++i) {
        std::snprintf(control_name, sizeof control_name, "Detail%d", i);
        Window* detail = named(host, control_name);
        set_text(detail, lines[i], k_line_style);
        use_font(detail, fonts->small_light);
    }
    // A result that has the keyboard is about to be destroyed. The rebuilt
    // row of the same index takes it, or Relocate when that row is gone.
    int32_t held_row = -1;
    if (Desktop* desktop = services().desktop; desktop != nullptr)
        held_row = row_clicked(host, desktop->focus);
    clear_rows(host);
    Window* list = named(host, "Results");
    if (list == nullptr) {
        focus_locator_field(host);
        if (held_row >= 0) {
            if (Window* next = named(host, "Relocate"); next != nullptr)
                services_desktop_focus(next);
        }
        return;
    }
    int32_t limit = list->height / k_row_height;
    if (limit > k_drawn_rows)
        limit = k_drawn_rows;
    if (limit < 0)
        limit = 0;
    const int32_t shown = screens::locator_result_count(&model);
    const int32_t count = shown < limit ? shown : limit;
    for (int32_t i = 0; i < count; ++i)
        add_result_row(host, list, i, screens::locator_result(&model, i));
    host->row_count = count;
    // The five find rows stay where the layout put them. Then Name, Search,
    // the result rows, Relocate and Page.
    window_move_child_last(host->root, name_field);
    window_move_child_last(host->root, named(host, "Search"));
    for (int32_t i = 0; i < count; ++i)
        window_move_child_last(host->root, host->rows[i].hit);
    window_move_child_last(host->root, named(host, "Relocate"));
    window_move_child_last(host->root, named(host, "Page"));
    focus_locator_field(host);
    if (held_row >= 0) {
        Window* next =
            held_row < host->row_count ? host->rows[held_row].hit : named(host, "Relocate");
        if (next != nullptr)
            services_desktop_focus(next);
    }
}

int32_t kind_clicked(const Window* source) {
    if (source == nullptr || source->name == nullptr)
        return -1;
    const char* name = source->name;
    const char* index = nullptr;
    if (std::strncmp(name, "row_", 4) == 0)
        index = name + 4;
    else if (std::strncmp(name, "kind_", 5) == 0)
        index = name + 5;
    else if (std::strncmp(name, "icon_", 5) == 0)
        index = name + 5;
    else
        return -1;
    if (index[0] < '0' || index[0] >= '0' + k_kinds || index[1] != '\0')
        return -1;
    return index[0] - '0';
}

// A click on a search face focuses the row on the way down and again on the
// way up, around the refresh that shows the face. The name field is where
// that keyboard belongs while the face asks for a name.
Window* on_focus_target(Window* self, Window* from, bool backwards) {
    LocatorHost* host = host_of(self);
    if (host == nullptr)
        return nullptr;
    if (from != nullptr && kind_clicked(from) >= 0 &&
        screens::locator_shows_field(screens::locator_kind_of(&host->model))) {
        Window* name = named(host, "Name");
        if (name != nullptr && window_is_shown(name))
            return name;
    }
    if (host->focus_target != nullptr)
        return host->focus_target(self, from, backwards);
    return nullptr;
}

int32_t row_clicked(const LocatorHost* host, const Window* source) {
    if (source == nullptr)
        return -1;
    for (int32_t i = 0; i < host->row_count; ++i) {
        const ResultWidgets& row = host->rows[i];
        if (source == row.hit || source == row.name || source == row.where ||
            source == row.status || source == row.icon || source == row.band)
            return i;
    }
    return -1;
}

void run_search(LocatorHost* host) {
    sync_from_owner(host);
    screens::locator_set_query(&host->model, typed_query(named(host, "Name")));
    screens::locator_search(&host->model);
}

void on_notify(Window* self, const Notify* notify) {
    LocatorHost* host = host_of(self);
    if (host == nullptr || notify == nullptr)
        return;
    if (host->note != nullptr)
        host->note(self, notify);
    if (notify->code != NotifyCode::activated)
        return;
    if (const int32_t kind = kind_clicked(notify->source); kind >= 0) {
        sync_from_owner(host);
        screens::locator_set_kind(&host->model, kind);
        // The next click can be SEARCH, which Server Transfer had hidden.
        // Show or hide it now. A result-row click must not refresh here:
        // that destroys the row whose button is still handling the click.
        if (host->refresh_pending) {
            host->refresh_pending = false;
            refresh(host);
        }
        return;
    }
    if (const int32_t row = row_clicked(host, notify->source); row >= 0) {
        screens::locator_select(&host->model, row);
        return;
    }
    if (notify->source == named(host, "Search")) {
        run_search(host);
        return;
    }
    if (notify->source == named(host, "Relocate")) {
        sync_from_owner(host);
        screens::locator_relocate(&host->model);
        return;
    }
    if (notify->source == named(host, "Page")) {
        sync_from_owner(host);
        screens::locator_extra(&host->model);
    }
}

// Return in the field runs SEARCH, as the layout's crdefault names it.
// A letter that arrives while the name field is not focused still belongs
// there: the kind row's click puts the keyboard back on the row after the
// face has refreshed. Enter stays with SEARCH.
bool deliver_typed_name(LocatorHost* host, const Message* msg) {
    if (msg == nullptr || !screens::locator_shows_field(screens::locator_kind_of(&host->model)))
        return false;
    const int32_t key = msg->key;
    if (key < 0 || (key >= key::del && key <= key::system) ||
        !utf8::taken(static_cast<char32_t>(key)))
        return false;
    Window* name = named(host, "Name");
    if (name == nullptr || name->focused != 0 || !window_is_shown(name) || name->ops == nullptr ||
        name->ops->key_down == nullptr)
        return false;
    services_desktop_focus(name);
    host->keyboard_taken = true;
    host->keyboard_kind = screens::locator_kind_of(&host->model);
    return name->ops->key_down(name, msg);
}

bool on_key_down(Window* self, const Message* msg) {
    LocatorHost* host = host_of(self);
    if (host != nullptr && deliver_typed_name(host, msg))
        return true;
    if (host != nullptr && msg != nullptr && msg->key == key::enter) {
        if (screens::locator_shows_field(screens::locator_kind_of(&host->model)))
            run_search(host);
        return true;
    }
    return host != nullptr && host->key_down != nullptr ? host->key_down(self, msg) : false;
}

void on_pulse(Window* self) {
    LocatorHost* host = host_of(self);
    if (host == nullptr)
        return;
    if (!window_is_shown(self)) {
        host->model.listening_rooms = false;
        host->model.listening_battles = false;
        host->model.awaiting_intel = false;
        host->keyboard_taken = false;
        host->shown_before = false;
        return;
    }
    const bool opened = !host->shown_before;
    host->shown_before = true;
    if (opened)
        host->keyboard_taken = false;
    screens::locator_pump(&host->model);
    if (host->refresh_pending) {
        host->refresh_pending = false;
        refresh(host);
    } else if (opened) {
        focus_locator_field(host);
    }
}

void on_destroy(Window* self) {
    LocatorHost* host = host_of(self);
    if (host == nullptr)
        return;
    void (*release)(Window*) = host->release;
    forget(host);
    if (release != nullptr)
        release(self);
    std::free(host);
}

} // namespace

Window* locator_console_create(
    StatusWidget* owner, Window* frame, int32_t x, int32_t y, int32_t width, int32_t height
) {
    GuiLoadError error{};
    GuiScreen* screen = gui_screen_load("gui/locator.gui", &error);
    if (screen == nullptr) {
        if (message_box_find_open(services().desktop) == nullptr) {
            message_box_create(
                "Locator needs the Boneyards mod.",
                message_box_buttons::ok,
                nullptr,
                nullptr,
                nullptr
            );
        }
        return nullptr;
    }
    Window* root = gui_screen_root(screen);
    bool room = false;
    for (LocatorHost* host : g_hosts) {
        if (host == nullptr)
            room = true;
    }
    if (root == nullptr || root->ops == nullptr || !room) {
        gui_screen_destroy(screen);
        return nullptr;
    }
    auto* host = static_cast<LocatorHost*>(std::calloc(1, sizeof(LocatorHost)));
    if (host == nullptr) {
        gui_screen_destroy(screen);
        return nullptr;
    }
    host->screen = screen;
    host->root = root;
    host->owner = owner;
    host->fonts = owner != nullptr ? &owner->system.hat_fonts : &k_empty_fonts;
    host->release = root->ops->destroy;
    host->note = root->ops->on_notify;
    host->key_down = root->ops->key_down;
    host->focus_target = root->ops->focus_target;
    host->ops = *root->ops;
    host->ops.pulse = on_pulse;
    host->ops.on_notify = on_notify;
    host->ops.key_down = on_key_down;
    host->ops.focus_target = on_focus_target;
    host->ops.destroy = on_destroy;
    root->ops = &host->ops;
    remember(host);

    const int32_t dx = x - root->x;
    const int32_t dy = y - root->y;
    window_set_bounds(root, x, y, width, height, frame, false);
    if (dx != 0 || dy != 0)
        shift_children(root, dx, dy);

    screens::locator_init(&host->model, owner != nullptr ? &owner->config.service : nullptr);
    host->model.user = host;
    host->model.changed = on_changed;
    host->model.open_chat = open_chat;
    host->model.open_planet = open_planet;
    host->model.open_battle = open_battle_screen;
    host->model.open_join = open_join;
    clear_fill(named(host, "Name"));
    set_text(named(host, "Name"), "", text_style::left);
    // Before the first focus, so raising Name does not move it past Page.
    window_keep_place(named(host, "Name"));
    refresh(host);
    return root;
}

screens::Locator* locator_console_state(Window* console) {
    LocatorHost* host = host_of(console);
    return host != nullptr ? &host->model : nullptr;
}

bool locator_console_takes_rooms(const Window* console) {
    const LocatorHost* host = host_of(console);
    return host != nullptr && screens::locator_listening_rooms(&host->model);
}

bool locator_console_takes_intel(const Window* console) {
    const LocatorHost* host = host_of(console);
    return host != nullptr && screens::locator_awaiting_intel(&host->model);
}

void locator_console_on_room(
    Window* console,
    int32_t change,
    uint32_t area_id,
    uint32_t room_id,
    const char* name,
    bool needs_password,
    int32_t users,
    int32_t type,
    bool last,
    bool global_known,
    int32_t global
) {
    LocatorHost* host = host_of(console);
    if (host == nullptr)
        return;
    screens::locator_on_room(
        &host->model,
        change,
        area_id,
        room_id,
        name,
        needs_password,
        users,
        type,
        last,
        global_known,
        global
    );
}

void locator_console_on_intel(
    Window* console, const char* game_id, const char* host_name, int32_t skill, bool failed
) {
    LocatorHost* host = host_of(console);
    if (host == nullptr)
        return;
    screens::locator_on_intel(&host->model, game_id, host_name, skill, failed);
}

void locator_console_on_login_reset(Window* console) {
    LocatorHost* host = host_of(console);
    if (host == nullptr)
        return;
    screens::locator_on_login_reset(&host->model);
}

} // namespace oa::boneyards::console

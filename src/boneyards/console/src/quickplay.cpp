// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// QuickPlay console. The window, the idle face, the searching face and the
// battle-found offer come from gui/quickplay.gui. FIND BATTLE, CANCEL SEARCH
// and the offer's Yes and No send the QuickPlay records. Status, scan and
// found come back through the service.
#include "oa/boneyards/console/feature_consoles.hpp"

#include "console_state.hpp"
#include "widgets.hpp"

#include "oa/boneyards/console/gui_screen.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/screens/lobby_widgets.hpp"
#include "oa/boneyards/screens/quickplay.hpp"
#include "oa/boneyards/shown_text.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace oa::boneyards::console {
namespace {

constexpr int32_t k_bright = 0xe9;
constexpr int32_t k_check_gap = 3;
constexpr int32_t k_check_face = 9;
constexpr int32_t k_check_height = 11;
constexpr int32_t k_host_capacity = 8;
constexpr const char* k_click = "GREEN_BUTTON";
constexpr const char* k_screen_path = "gui/quickplay.gui";
// Distinct among siblings. The five game types share one stop; where, skill
// and side are the other stops.
constexpr uint8_t k_pick_group = 1;
constexpr uint8_t k_where_group = 2;
constexpr uint8_t k_skill_group = 3;
constexpr uint8_t k_side_group = 4;
// Blurb, timer and scan lines: left aligned, their tops on the layout's lines.
constexpr int32_t k_line_style = text_style::left | text_style::top;

struct QuickPlayView {
    GuiScreen* screen;
    Window* root;
    StatusWidget* owner;
    screens::QuickPlay model;
    screens::HatFonts fonts;
    Handle medium_bright;
    bool own_fonts;
    bool was_shown;
    bool have_tick;
    uint32_t last_tick;
    int32_t ms_remainder;
    Handle scanner[screens::kQuickPlayScannerFrames];
    Handle meter_frames[screens::kQuickPlayMeterLast + 1]{};
    Handle icon_on;
    Handle icon_off;
    Window* icons[screens::kQuickPlayTypeCount];
    Window* type_labels[screens::kQuickPlayTypeCount];
    Window* picks[screens::kQuickPlayTypeCount];
    Window* blurbs[screens::kQuickPlayBlurbLines];
    Window* help[4];
    Window* where_any;
    Window* where_fuel;
    Window* skills[3];
    Window* side_arm;
    Window* side_core;
    Window* side_either;
    Window* favorite;
    Window* find;
    Window* cancel;
    Window* scanner_view;
    Window* meter;
    Window* log_well;
    Window* searching;
    Window* timer;
    Window* lines[screens::kQuickPlayScanCapacity];
    Window* offer;
    Window* offer_text = nullptr;
    Window* yes = nullptr;
    Window* no = nullptr;
    WindowOps ops;
    WindowOps offer_ops;
    void (*release)(Window*);
    void (*note)(Window*, const Notify*);
    char timer_text[16];
    char line_text[screens::kQuickPlayScanCapacity][screens::kQuickPlayTextCapacity];
    char favorite_text[screens::kQuickPlayTextCapacity];
};

QuickPlayView* g_views[k_host_capacity]{};

void remember(QuickPlayView* view) {
    for (QuickPlayView*& slot : g_views) {
        if (slot == nullptr) {
            slot = view;
            return;
        }
    }
}

void forget(QuickPlayView* view) {
    for (QuickPlayView*& slot : g_views) {
        if (slot == view)
            slot = nullptr;
    }
}

QuickPlayView* view_of(const Window* window) {
    if (window == nullptr)
        return nullptr;
    for (QuickPlayView* view : g_views) {
        if (view != nullptr && view->root == window)
            return view;
    }
    return nullptr;
}

Window* find_window(GuiScreen* screen, const char* name) {
    const GuiControl* control = gui_screen_find(screen, name);
    return control != nullptr ? control->window : nullptr;
}

void set_shown(Window* window, bool shown) {
    if (window != nullptr)
        window->visible = shown ? 1 : 0;
}

void set_text(Window* window, const char* text, int32_t style) {
    if (window != nullptr && window->ops != nullptr && window->ops->set_caption != nullptr)
        window->ops->set_caption(window, shown_text(text != nullptr ? text : ""), style);
}

void use_font(Window* window, Handle font) {
    if (window != nullptr && font >= 0)
        window_set_font_handle(window, font);
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

void play_click(const QuickPlayView* view) {
    if (view->owner == nullptr)
        return;
    const MainMenuHost& host = view->owner->config.host;
    if (host.play_sound != nullptr)
        host.play_sound(host.user, k_click);
}

void read_navigation(QuickPlayView* view) {
    screens::BoneyardsScreens* navigation =
        view->owner != nullptr ? view->owner->navigation : nullptr;
    if (navigation == nullptr) {
        screens::quickplay_set_place(&view->model, false, 0, 'U');
        screens::quickplay_set_favorite(&view->model, nullptr);
        return;
    }
    const screens::GalaxyView& galaxy = navigation->galaxy;
    const bool has_planet = galaxy.selected != screens::kNoPlanet &&
                            static_cast<std::size_t>(galaxy.selected) < galaxy.planet_count;
    const char side = has_planet ? galaxy.planets[galaxy.selected].side : 0;
    screens::quickplay_set_place(&view->model, has_planet, side, navigation->allegiance);
    screens::quickplay_set_favorite(&view->model, navigation->profile.profile.favorite_map);
}

// A hotspot draws nothing of its own, so Pressed and Disabled change no
// pixel. Pressed is the chosen type (the one keyboard stop). Disabled is a
// type this planet does not offer, so the arrows skip it.
bool pick_is_class(const char* name) {
    return name != nullptr && (std::strcmp(name, class_name::button) == 0 ||
                               std::strcmp(name, class_name::radio_button) == 0);
}

const WindowOps* game_type_ops() {
    static WindowOps ops{};
    static bool ready = false;
    if (!ready) {
        ops = k_hotspot_button_ops;
        ops.is_class = pick_is_class;
        ready = true;
    }
    return &ops;
}

void refresh_types(QuickPlayView* view) {
    for (int32_t i = 0; i < screens::kQuickPlayTypeCount; ++i) {
        const bool selected = view->model.selected == i;
        const bool allowed = screens::quickplay_type_allowed(&view->model, i);
        Window* pick = view->picks[i];
        if (pick != nullptr) {
            const int32_t state = !allowed   ? button_state::disabled
                                  : selected ? button_state::pressed
                                             : button_state::unpressed;
            if (pick->states.current != state)
                window_set_state(pick, state);
        }
        Handle font = view->fonts.small;
        if (selected && view->fonts.small_light >= 0)
            font = view->fonts.small_light;
        use_font(view->type_labels[i], font);
        const Handle icon = selected ? view->icon_on : view->icon_off;
        Window* picture = view->icons[i];
        if (picture != nullptr && icon >= 0)
            window_set_state_image(picture, picture->states.current, icon);
    }
    for (int32_t line = 0; line < screens::kQuickPlayBlurbLines; ++line) {
        set_text(
            view->blurbs[line], screens::quickplay_blurb(view->model.selected, line), k_line_style
        );
    }
}

void refresh_favorite(QuickPlayView* view) {
    if (view->favorite == nullptr)
        return;
    screens::quickplay_favorite_caption(
        &view->model, view->favorite_text, sizeof view->favorite_text
    );
    int32_t width = 0;
    int32_t height = 0;
    text_measure_string(view->fonts.medium, view->favorite_text, &width, &height);
    auto* box = reinterpret_cast<CheckBox*>(view->favorite);
    window_set_size(view->favorite, width + k_check_gap + k_check_face, k_check_height);
    box->label_gap = k_check_gap;
    set_text(view->favorite, view->favorite_text, text_style::left | text_style::top);
}

void refresh_chrome(QuickPlayView* view) {
    const bool searching = view->model.searching;
    set_shown(view->find, !searching);
    set_shown(view->cancel, searching);
    set_shown(view->meter, searching);
    set_shown(view->log_well, searching);
    set_shown(view->searching, searching);
    set_shown(view->timer, searching);
    for (Window* line : view->help)
        set_shown(line, !searching);
    const int32_t shown = screens::quickplay_visible_lines(&view->model);
    for (int32_t i = 0; i < screens::kQuickPlayScanCapacity; ++i) {
        set_shown(view->lines[i], searching && i < shown);
        if (view->lines[i] != nullptr && i < shown) {
            screens::quickplay_scan_line(
                &view->model, i, view->line_text[i], sizeof view->line_text[i]
            );
            set_text(view->lines[i], view->line_text[i], k_line_style);
        }
    }
    if (view->timer != nullptr) {
        screens::quickplay_timer(&view->model, view->timer_text, sizeof view->timer_text);
        set_text(view->timer, view->timer_text, k_line_style);
    }
    const int32_t frame = screens::quickplay_scanner_frame(&view->model);
    if (view->scanner_view != nullptr && view->scanner[frame] >= 0)
        window_set_state_image(
            view->scanner_view, view->scanner_view->states.current, view->scanner[frame]
        );
    if (view->meter != nullptr && searching && view->model.has_status) {
        const int32_t meter = screens::quickplay_meter_frame(&view->model);
        if (view->meter_frames[meter] >= 0)
            window_set_state_image(
                view->meter, view->meter->states.current, view->meter_frames[meter]
            );
    }
    const bool offer_open = view->model.offer_open;
    set_shown(view->offer, offer_open);
    set_shown(view->offer_text, offer_open);
    set_shown(view->yes, offer_open);
    set_shown(view->no, offer_open);
    if (offer_open && view->offer_text != nullptr) {
        char offer_line[192];
        screens::quickplay_offer_line(&view->model, offer_line, sizeof offer_line);
        set_text(view->offer_text, offer_line, k_line_style);
    }
}

void link_group(Window* const* radios, int32_t count, uint8_t group) {
    for (int32_t i = 0; i < count; ++i) {
        if (radios[i] == nullptr)
            continue;
        radios[i]->radio_group = group;
        auto* radio = reinterpret_cast<screens::LobbyRadio*>(radios[i]);
        for (int32_t other = 0; other < count; ++other) {
            if (other != i && radios[other] != nullptr)
                screens::lobby_radio_add_member(radio, radios[other]);
        }
    }
}

void take_fonts(QuickPlayView* view) {
    view->fonts = {};
    view->medium_bright = k_no_handle;
    view->own_fonts = false;
    if (view->owner != nullptr && view->owner->system.fonts_loaded) {
        view->fonts = view->owner->system.hat_fonts;
    } else {
        view->own_fonts = true;
        view->fonts.small = font_load("BYHatFontSmall87.gaf");
        view->fonts.medium = font_load("BYHatFontMedium90.gaf");
        view->fonts.big = font_load("BYHatFontBig84.gaf");
        Toolkit* toolkit = view->owner != nullptr ? view->owner->toolkit : nullptr;
        if (toolkit != nullptr) {
            view->fonts.small_dim = toolkit_font_tint(toolkit, view->fonts.small, 0x5a);
            view->fonts.small_light = toolkit_font_tint(toolkit, view->fonts.small, 0x54);
            view->fonts.button_pressed = toolkit_font_tint(toolkit, view->fonts.medium, 0x5d);
            view->fonts.button_normal = toolkit_font_tint(toolkit, view->fonts.medium, 0x57);
            view->fonts.button_hover = toolkit_font_tint(toolkit, view->fonts.medium, 0x54);
        }
        screens::function_button_fonts(&view->fonts);
    }
    Toolkit* toolkit = view->owner != nullptr ? view->owner->toolkit : nullptr;
    if (toolkit != nullptr && view->fonts.medium >= 0)
        view->medium_bright =
            toolkit_font_tint(toolkit, view->fonts.medium, static_cast<uint8_t>(k_bright));
}

void apply_fonts(QuickPlayView* view) {
    use_font(view->root, view->fonts.medium);
    use_font(find_window(view->screen, "Title"), view->fonts.big);
    use_font(find_window(view->screen, "Heading"), view->fonts.big);
    use_font(find_window(view->screen, "Where"), view->fonts.medium);
    use_font(find_window(view->screen, "Skill"), view->fonts.medium);
    use_font(find_window(view->screen, "Side"), view->fonts.medium);
    use_font(find_window(view->screen, "Map"), view->fonts.medium);
    use_font(find_window(view->screen, "ArmName"), view->fonts.medium);
    use_font(find_window(view->screen, "CoreName"), view->fonts.medium);
    const Handle searching_font =
        view->medium_bright >= 0 ? view->medium_bright : view->fonts.medium;
    use_font(view->searching, searching_font);
    use_font(view->timer, view->fonts.medium);
    use_font(view->favorite, view->fonts.medium);
    use_font(view->where_any, view->fonts.medium);
    use_font(view->where_fuel, view->fonts.medium);
    use_font(view->side_either, view->fonts.medium);
    for (Window* skill : view->skills)
        use_font(skill, view->fonts.medium);
    for (Window* blurb : view->blurbs)
        use_font(blurb, view->fonts.small);
    for (Window* line : view->help)
        use_font(line, view->fonts.small);
    for (Window* line : view->lines)
        use_font(line, view->fonts.small);
    for (Window* label : view->type_labels)
        use_font(label, view->fonts.small);
}

void take_controls(QuickPlayView* view) {
    GuiScreen* screen = view->screen;
    for (int32_t i = 0; i < screens::kQuickPlayTypeCount; ++i) {
        char type_name[8] = "Type0";
        char label_name[8] = "Label0";
        char pick_name[8] = "Pick0";
        type_name[4] = static_cast<char>('0' + i);
        label_name[5] = static_cast<char>('0' + i);
        pick_name[4] = static_cast<char>('0' + i);
        view->icons[i] = find_window(screen, type_name);
        view->type_labels[i] = find_window(screen, label_name);
        view->picks[i] = find_window(screen, pick_name);
        if (view->picks[i] != nullptr) {
            view->picks[i]->ops = game_type_ops();
            view->picks[i]->radio_group = k_pick_group;
        }
    }
    for (int32_t line = 0; line < screens::kQuickPlayBlurbLines; ++line) {
        char name[8] = "Blurb0";
        name[5] = static_cast<char>('0' + line);
        view->blurbs[line] = find_window(screen, name);
    }
    for (int32_t line = 0; line < 4; ++line) {
        char name[8] = "Help0";
        name[4] = static_cast<char>('0' + line);
        view->help[line] = find_window(screen, name);
    }
    for (int32_t line = 0; line < screens::kQuickPlayScanCapacity; ++line) {
        char name[8] = "Line0";
        name[4] = static_cast<char>('0' + line);
        view->lines[line] = find_window(screen, name);
    }
    view->where_any = find_window(screen, "Any planet");
    view->where_fuel = find_window(screen, "Fuel Dump only");
    view->skills[0] = find_window(screen, "Novice");
    view->skills[1] = find_window(screen, "Seasoned");
    view->skills[2] = find_window(screen, "Expert");
    view->side_arm = find_window(screen, "Arm");
    view->side_core = find_window(screen, "Core");
    view->side_either = find_window(screen, "Either");
    view->favorite = find_window(screen, "Favorite");
    view->find = find_window(screen, "FIND BATTLE");
    view->cancel = find_window(screen, "CANCEL SEARCH");
    view->scanner_view = find_window(screen, "Scanner");
    view->meter = find_window(screen, "Queue");
    view->log_well = find_window(screen, "LogWell");
    view->searching = find_window(screen, "SEARCHING");
    view->timer = find_window(screen, "Timer");
    view->offer = find_window(screen, "QuickPlayFound");
    view->offer_text = find_window(screen, "Text");
    view->yes = find_window(screen, "Yes");
    view->no = find_window(screen, "No");
}

void bind_choices(QuickPlayView* view) {
    Window* where[2] = {view->where_any, view->where_fuel};
    Window* sides[3] = {view->side_arm, view->side_core, view->side_either};
    link_group(where, 2, k_where_group);
    link_group(view->skills, 3, k_skill_group);
    link_group(sides, 3, k_side_group);
    if (view->where_any != nullptr)
        window_set_state(view->where_any, button_state::pressed);
    if (view->skills[1] != nullptr)
        window_set_state(view->skills[1], button_state::pressed);
    if (view->side_either != nullptr)
        window_set_state(view->side_either, button_state::pressed);
    if (view->favorite != nullptr)
        window_set_state(view->favorite, check_state::checked_up);
}

void load_art(QuickPlayView* view) {
    for (int32_t frame = 0; frame < screens::kQuickPlayScannerFrames; ++frame)
        view->scanner[frame] = image_load("BY_New.gaf", "Scanner", frame, k_overlay_transparent);
    for (int32_t frame = 0; frame <= screens::kQuickPlayMeterLast; ++frame)
        view->meter_frames[frame] =
            image_load("BY_New.gaf", "Queue_Meter", frame, k_overlay_transparent);
    view->icon_on = image_load("Widgets.gaf", "Icon_Circle", 1, k_overlay_transparent);
    view->icon_off = image_load("Widgets.gaf", "Icon_Circle", 2, k_overlay_transparent);
}

void release_art(QuickPlayView* view) {
    for (Handle& frame : view->scanner) {
        if (frame >= 0)
            image_release(frame);
        frame = k_no_handle;
    }
    for (Handle& frame : view->meter_frames) {
        if (frame >= 0)
            image_release(frame);
        frame = k_no_handle;
    }
    if (view->icon_on >= 0)
        image_release(view->icon_on);
    if (view->icon_off >= 0)
        image_release(view->icon_off);
    view->icon_on = k_no_handle;
    view->icon_off = k_no_handle;
    if (view->medium_bright >= 0 && view->medium_bright != view->fonts.medium)
        font_release(view->medium_bright);
    view->medium_bright = k_no_handle;
    if (!view->own_fonts)
        return;
    screens::function_button_fonts(nullptr);
    const Handle owned[] = {
        view->fonts.small,
        view->fonts.small_dim,
        view->fonts.small_light,
        view->fonts.medium,
        view->fonts.big,
        view->fonts.button_pressed,
        view->fonts.button_normal,
        view->fonts.button_hover,
    };
    for (const Handle font : owned) {
        if (font >= 0)
            font_release(font);
    }
}

int32_t pick_index(const QuickPlayView* view, const Window* source) {
    for (int32_t i = 0; i < screens::kQuickPlayTypeCount; ++i) {
        if (source == view->picks[i])
            return i;
    }
    return -1;
}

void read_choice(QuickPlayView* view, Window* source) {
    if (source == view->where_fuel)
        view->model.where = screens::QuickPlayWhere::fuel_dump;
    else if (source == view->where_any)
        view->model.where = screens::QuickPlayWhere::any_planet;
    else if (source == view->skills[0])
        view->model.skill = screens::QuickPlaySkill::novice;
    else if (source == view->skills[1])
        view->model.skill = screens::QuickPlaySkill::seasoned;
    else if (source == view->skills[2])
        view->model.skill = screens::QuickPlaySkill::expert;
    else if (source == view->side_arm)
        view->model.side = screens::QuickPlaySide::arm;
    else if (source == view->side_core)
        view->model.side = screens::QuickPlaySide::core;
    else if (source == view->side_either)
        view->model.side = screens::QuickPlaySide::either;
}

BoneyardsService* online_of(const QuickPlayView* view) {
    if (view == nullptr || view->owner == nullptr)
        return nullptr;
    return &view->owner->config.service;
}

void note_sent(QuickPlayView* view, ServiceResult result) {
    if (result == ServiceResult::pending)
        ++view->model.requests_sent;
}

bool selected_area(const StatusWidget* owner, uint32_t* area_id) {
    if (owner == nullptr || owner->navigation == nullptr || area_id == nullptr)
        return false;
    const screens::GalaxyView& galaxy = owner->navigation->galaxy;
    if (galaxy.selected < 0 || static_cast<std::size_t>(galaxy.selected) >= galaxy.planet_count)
        return false;
    const int32_t id = galaxy.planets[galaxy.selected].id;
    if (id < 0)
        return false;
    *area_id = static_cast<uint32_t>(id);
    return true;
}

void show_words(const char* english) {
    if (english == nullptr || english[0] == '\0')
        return;
    message_box_create(english, message_box_buttons::ok, nullptr, nullptr, nullptr);
}

void send_join(QuickPlayView* view) {
    BoneyardsService* online = online_of(view);
    if (online == nullptr || online->quickplay_join == nullptr)
        return;
    uint32_t area_id = 0;
    const bool known = selected_area(view->owner, &area_id);
    QuickPlayQuery query{};
    screens::quickplay_query(&view->model, area_id, known, &query);
    note_sent(view, online->quickplay_join(online->user, &query));
}

void send_cancel(QuickPlayView* view) {
    BoneyardsService* online = online_of(view);
    if (online == nullptr || online->quickplay_cancel == nullptr)
        return;
    note_sent(view, online->quickplay_cancel(online->user));
}

void send_answer(QuickPlayView* view, int32_t accept) {
    BoneyardsService* online = online_of(view);
    if (online == nullptr || online->quickplay_answer == nullptr)
        return;
    note_sent(view, online->quickplay_answer(online->user, view->model.offer_id, accept));
}

void launch_effect(QuickPlayView* view, const screens::QuickPlayEffect& effect) {
    BoneyardsService* online = online_of(view);
    if (online == nullptr)
        return;
    if (effect.launch_host && online->host_game != nullptr) {
        HostGameRequest request{};
        if (screens::quickplay_host_request(&view->model, &request))
            online->host_game(online->user, &request);
    }
    if (effect.launch_join && online->join_game != nullptr)
        online->join_game(online->user, view->model.offer_area, view->model.offer_game_id, nullptr);
}

void show_effect(const screens::QuickPlayEffect& effect) {
    if (effect.show_box)
        show_words(screens::quickplay_result_text(effect.box_result));
}

void take_notices(QuickPlayView* view) {
    BoneyardsService* online = online_of(view);
    if (online == nullptr || online->quickplay_take == nullptr)
        return;
    // A sweep can arrive in one pump. Drain it before the clock moves, so a late line is kept
    // and revealed on the search clock rather than the moment it arrived.
    for (int32_t taken = 0; taken < 96; ++taken) {
        QuickPlayNotice notice{};
        if (!online->quickplay_take(online->user, &notice))
            return;
        screens::QuickPlayEffect effect{};
        if (notice.kind == QuickPlayNoticeKind::status)
            effect = screens::quickplay_apply_status(&view->model, &notice.status);
        else if (notice.kind == QuickPlayNoticeKind::scan)
            screens::quickplay_apply_scan(&view->model, &notice.scan);
        else if (notice.kind == QuickPlayNoticeKind::found)
            effect = screens::quickplay_apply_found(&view->model, &notice.found);
        else if (notice.kind == QuickPlayNoticeKind::withdrawn)
            effect = screens::quickplay_apply_withdrawn(&view->model, &notice.withdrawn);
        show_effect(effect);
        if (effect.show_offer) {
            char offer_line[192];
            screens::quickplay_offer_line(&view->model, offer_line, sizeof offer_line);
            if (offer_line[0] != '\0')
                boneyards_console_driver_note("alert", offer_line);
        }
    }
}

void give_up(QuickPlayView* view) {
    screens::quickplay_cancel(&view->model);
    show_words(screens::quickplay_unavailable_text());
}

// The player has left this console, or the Boneyards session is ending. A running search stops
// the way CANCEL SEARCH stops it. An offer still on screen is declined first, so that battle's
// seat is free at once. A Yes that is already launching a battle is left to launch.
void end_running_search(QuickPlayView* view) {
    if (view == nullptr || view->model.launch_pending)
        return;
    if (!view->model.searching && !view->model.offer_open)
        return;
    if (view->model.offer_open) {
        screens::quickplay_decide(&view->model, 0);
        send_answer(view, 0);
    }
    if (view->model.searching) {
        screens::quickplay_cancel(&view->model);
        send_cancel(view);
        view->have_tick = false;
    }
    refresh_chrome(view);
}

void drive(QuickPlayView* view, int32_t ms) {
    take_notices(view);
    if (ms > 0)
        screens::quickplay_advance(&view->model, ms);
    if (screens::quickplay_status_overdue(&view->model))
        give_up(view);
    refresh_chrome(view);
}

void on_offer_notify(Window* self, const Notify* notify) {
    if (notify == nullptr || self == nullptr || self->parent == nullptr ||
        self->parent->ops == nullptr || self->parent->ops->on_notify == nullptr)
        return;
    self->parent->ops->on_notify(self->parent, notify);
}

void on_notify(Window* self, const Notify* notify) {
    QuickPlayView* view = view_of(self);
    if (view == nullptr || notify == nullptr)
        return;
    if (view->note != nullptr)
        view->note(self, notify);
    if (notify->code != NotifyCode::activated || notify->source == nullptr)
        return;
    const int32_t pick = pick_index(view, notify->source);
    if (pick >= 0) {
        if (!screens::quickplay_select_type(&view->model, pick))
            return;
        play_click(view);
        refresh_types(view);
        return;
    }
    if (notify->source == view->find) {
        play_click(view);
        if (!view->model.searching) {
            screens::quickplay_find(&view->model);
            send_join(view);
            view->have_tick = false;
        }
        refresh_chrome(view);
        return;
    }
    if (notify->source == view->cancel) {
        play_click(view);
        const bool was_searching = view->model.searching;
        screens::quickplay_cancel(&view->model);
        if (was_searching)
            send_cancel(view);
        view->have_tick = false;
        refresh_chrome(view);
        return;
    }
    if (notify->source == view->yes || notify->source == view->no) {
        if (!view->model.offer_open)
            return;
        play_click(view);
        const int32_t accept = notify->source == view->yes ? 1 : 0;
        const screens::QuickPlayEffect effect = screens::quickplay_decide(&view->model, accept);
        send_answer(view, accept);
        launch_effect(view, effect);
        // Yes that launches a battle ends the search: back from the battle,
        // the card shows FIND BATTLE.
        if (effect.leave_search)
            view->have_tick = false;
        refresh_chrome(view);
        return;
    }
    if (notify->source == view->favorite) {
        view->model.prefer_favorite = notify->source->states.current == check_state::checked_up;
        play_click(view);
        return;
    }
    read_choice(view, notify->source);
}

void on_pulse(Window* self) {
    QuickPlayView* view = view_of(self);
    if (view == nullptr)
        return;
    if (!window_is_shown(self)) {
        // Hidden behind another console, or behind the main menu once the session ends. The
        // search stops here as well as at the moment of hiding, in case that moment was missed.
        end_running_search(view);
        view->was_shown = false;
        view->have_tick = false;
        return;
    }
    if (!view->was_shown) {
        view->was_shown = true;
        read_navigation(view);
        refresh_types(view);
        refresh_favorite(view);
    }
    int32_t ms = 0;
    if (view->model.searching && view->owner != nullptr) {
        const auto tick = view->owner->config.host.current_tick;
        const uint32_t now =
            tick != nullptr ? tick(view->owner->config.host.user) : view->last_tick + 1;
        if (!view->have_tick) {
            view->have_tick = true;
            view->last_tick = now;
        } else {
            const uint32_t delta = now - view->last_tick;
            view->last_tick = now;
            if (delta != 0) {
                const int64_t sum =
                    static_cast<int64_t>(view->ms_remainder) + static_cast<int64_t>(delta) * 1000;
                view->ms_remainder = static_cast<int32_t>(sum % 30);
                ms = static_cast<int32_t>(sum / 30);
            }
        }
    }
    drive(view, ms);
}

void on_destroy(Window* self) {
    QuickPlayView* view = view_of(self);
    if (view == nullptr)
        return;
    end_running_search(view);
    forget(view);
    release_art(view);
    void (*release)(Window*) = view->release;
    if (release != nullptr)
        release(self);
    std::free(view);
}

Window* empty_console(Window* frame, int32_t x, int32_t y, int32_t width, int32_t height) {
    Window* window = panel_create();
    window_create(window, k_quickplay_console_name, x, y, width, height, frame, false);
    return window;
}

void clear_handles(QuickPlayView* view) {
    view->medium_bright = k_no_handle;
    view->icon_on = k_no_handle;
    view->icon_off = k_no_handle;
    for (Handle& frame : view->scanner)
        frame = k_no_handle;
    for (Handle& frame : view->meter_frames)
        frame = k_no_handle;
}

} // namespace

Window* quickplay_console_create(
    StatusWidget* owner, Window* frame, int32_t x, int32_t y, int32_t width, int32_t height
) {
    GuiScreen* screen = gui_screen_load(k_screen_path, nullptr);
    if (screen == nullptr) {
        if (message_box_find_open(services().desktop) == nullptr) {
            message_box_create(
                "QuickPlay needs the Boneyards mod.",
                message_box_buttons::ok,
                nullptr,
                nullptr,
                nullptr
            );
        }
        return nullptr;
    }
    Window* root = gui_screen_root(screen);
    if (root == nullptr || root->ops == nullptr) {
        gui_screen_destroy(screen);
        return empty_console(frame, x, y, width, height);
    }
    auto* view = static_cast<QuickPlayView*>(std::calloc(1, sizeof(QuickPlayView)));
    if (view == nullptr)
        std::abort();
    clear_handles(view);
    view->screen = screen;
    view->root = root;
    view->owner = owner;
    screens::quickplay_init(&view->model);
    take_controls(view);
    take_fonts(view);
    apply_fonts(view);
    read_navigation(view);
    bind_choices(view);
    load_art(view);
    refresh_types(view);
    refresh_favorite(view);
    refresh_chrome(view);
    set_shown(view->offer, false);
    set_shown(view->offer_text, false);
    set_shown(view->yes, false);
    set_shown(view->no, false);
    if (view->offer != nullptr && view->offer->ops != nullptr) {
        view->offer_ops = *view->offer->ops;
        view->offer_ops.on_notify = on_offer_notify;
        view->offer->ops = &view->offer_ops;
    }

    view->release = root->ops->destroy;
    view->note = root->ops->on_notify;
    view->ops = *root->ops;
    view->ops.pulse = on_pulse;
    view->ops.on_notify = on_notify;
    view->ops.destroy = on_destroy;
    root->ops = &view->ops;
    remember(view);

    const int32_t dx = x - root->x;
    const int32_t dy = y - root->y;
    window_set_bounds(root, x, y, width, height, frame, false);
    if (dx != 0 || dy != 0)
        shift_children(root, dx, dy);
    return root;
}

screens::QuickPlay* quickplay_console_model(Window* console) {
    QuickPlayView* view = view_of(console);
    return view != nullptr ? &view->model : nullptr;
}

void quickplay_console_advance(Window* console, int32_t ms) {
    QuickPlayView* view = view_of(console);
    if (view == nullptr)
        return;
    drive(view, ms);
}

void quickplay_console_end_search(Window* console) {
    end_running_search(view_of(console));
}

} // namespace oa::boneyards::console

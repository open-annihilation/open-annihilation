// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Matchmaking dialog over Chat or Battle. The glass, the choices, the queue
// meter, both faces and the opponent box come from gui/matchmaking.gui.
// ACTIVATE joins the queue. Close hides the dialog; a search keeps running
// and MATCHMAKING blinks. DEACTIVATE leaves the queue.
#include "oa/boneyards/console/feature_consoles.hpp"

#include "battle_console.hpp"
#include "console_state.hpp"
#include "oa/boneyards/console/gui_screen.hpp"
#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/screens/matchmaking.hpp"
#include "widgets.hpp"

#include <cstdio>
#include <vector>

namespace oa::boneyards::console {
namespace {

constexpr int32_t k_shown = 1;
constexpr int32_t k_blink_pulses = 15;
constexpr int32_t k_minimize_rest = 2;
constexpr int32_t k_minimize_lit = 3;
constexpr int32_t k_caption = text_style::left | text_style::top;
constexpr const char* k_green_button = "GREEN_BUTTON";
constexpr const char* k_new_gaf = "BY_New.gaf";
constexpr const char* k_gui_file = "gui/matchmaking.gui";
constexpr const char* k_minimize = "Minimize_Button";
constexpr const char* k_minimize_open = "Minimize_Button_180";

struct MatchmakingDialog {
    Window base;
    StatusWidget* owner{};
    screens::MatchmakingChoices choices{};
    int32_t face{};
    bool built{};
    bool unavailable{};
    int32_t scanner_shown{-1};
    int32_t blink_ticks{};
    bool blink_lit{};
    Window* close{};
    Window* activate{};
    Window* deactivate{};
    Window* scanner{};
    Window* meter{};
    Window* queue_line{};
    Window* queue_count{};
    Window* blurb[3]{};
    Window* status{};
    Window* detail{};
    Window* offer{};
    Window* offer_text{};
    Window* yes{};
    Window* no{};
    screens::LobbyRadio* game[3]{};
    screens::LobbyRadio* skill[2]{};
    screens::LobbyRadio* side[3]{};
    screens::LobbyRadio* planets[2]{};
};

MatchmakingDialog* as_dialog(Window* window) {
    return reinterpret_cast<MatchmakingDialog*>(window);
}

const MatchmakingDialog* as_dialog(const Window* window) {
    return reinterpret_cast<const MatchmakingDialog*>(window);
}

void play_green(const MatchmakingDialog* dialog) {
    const MainMenuHost& host = dialog->owner->config.host;
    if (host.play_sound != nullptr)
        host.play_sound(host.user, k_green_button);
}

const char* selected_planet(const StatusWidget* owner) {
    if (owner == nullptr || owner->navigation == nullptr)
        return "Fuel Dump";
    const screens::GalaxyView& galaxy = owner->navigation->galaxy;
    if (galaxy.selected < 0 || static_cast<std::size_t>(galaxy.selected) >= galaxy.planet_count)
        return "Fuel Dump";
    const char* name = galaxy.planets[galaxy.selected].name;
    return name[0] != '\0' ? name : "Fuel Dump";
}

Handle load_frame(ConsoleImages* images, const char* gaf, const char* sequence, int32_t frame) {
    return console_images_load(
        images, gaf, sequence, frame, static_cast<uint8_t>(screens::kWidgetKey)
    );
}

void set_label(Window* label, const char* text) {
    if (label == nullptr || label->ops == nullptr || label->ops->set_caption == nullptr)
        return;
    label->ops->set_caption(label, text, k_caption);
}

void refresh_texts(MatchmakingDialog* dialog) {
    if (dialog->queue_line == nullptr || dialog->status == nullptr)
        return;
    char line[128];
    screens::matchmaking_queue_line(
        &dialog->choices, selected_planet(dialog->owner), line, sizeof line
    );
    set_label(dialog->queue_line, line);
    std::snprintf(line, sizeof line, "%d", screens::matchmaking_queue_count(&dialog->choices));
    set_label(dialog->queue_count, line);
    screens::matchmaking_search_line(&dialog->choices, line, sizeof line);
    set_label(dialog->status, line);
    screens::matchmaking_detail_line(&dialog->choices, line, sizeof line);
    set_label(dialog->detail, line);
}

void refresh_meter(MatchmakingDialog* dialog) {
    if (dialog->meter == nullptr)
        return;
    const Handle image = load_frame(
        dialog->owner->images,
        k_new_gaf,
        "Queue_Meter",
        screens::matchmaking_meter_frame(&dialog->choices)
    );
    if (image < 0)
        return;
    window_set_state_image(dialog->meter, k_shown, image);
    image_release(image);
}

void refresh_scanner(MatchmakingDialog* dialog) {
    if (dialog->scanner == nullptr)
        return;
    int32_t frame = 0;
    if (dialog->face == k_matchmaking_searching)
        frame = screens::matchmaking_scanner_frame(&dialog->choices);
    if (frame == dialog->scanner_shown)
        return;
    dialog->scanner_shown = frame;
    const Handle image = load_frame(dialog->owner->images, k_new_gaf, "Scanner", frame);
    if (image < 0)
        return;
    window_set_state_image(dialog->scanner, k_shown, image);
    image_release(image);
}

void show_face(MatchmakingDialog* dialog) {
    if (dialog->activate == nullptr || dialog->deactivate == nullptr || dialog->status == nullptr)
        return;
    const bool searching = dialog->face == k_matchmaking_searching;
    dialog->activate->visible = searching ? 0 : 1;
    dialog->deactivate->visible = searching ? 1 : 0;
    for (Window* blurb : dialog->blurb)
        if (blurb != nullptr)
            blurb->visible = searching ? 0 : 1;
    dialog->status->visible = searching ? 1 : 0;
    if (dialog->detail != nullptr)
        dialog->detail->visible = searching ? 1 : 0;
    refresh_texts(dialog);
    refresh_meter(dialog);
    refresh_scanner(dialog);
}

void show_offer(MatchmakingDialog* dialog, bool shown) {
    dialog->choices.offer_open = shown;
    const int32_t visible = shown ? 1 : 0;
    Window* parts[] = {dialog->offer, dialog->offer_text, dialog->yes, dialog->no};
    for (Window* part : parts)
        if (part != nullptr)
            part->visible = static_cast<uint8_t>(visible);
}

void set_minimize_frame(
    Window* button, ConsoleImages* images, const char* sequence, int32_t frame
) {
    if (button == nullptr)
        return;
    const Handle image = load_frame(images, screens::kWidgetsGaf, sequence, frame);
    if (image < 0)
        return;
    window_set_state_image(button, button_state::unpressed, image);
    image_release(image);
}

void apply_blink(MatchmakingDialog* dialog, bool lit) {
    const int32_t frame = lit ? k_minimize_lit : k_minimize_rest;
    ConsoleImages* images = dialog->owner->images;
    Window* main = &dialog->owner->system.main_frame->base;
    Window* chat = window_find_named(main, "ChatConsole", nullptr);
    if (chat != nullptr)
        set_minimize_frame(
            reinterpret_cast<ChatView*>(chat)->matchmaking, images, k_minimize, frame
        );
    Window* battle = window_find_named(main, "BattleConsole", nullptr);
    if (battle == nullptr)
        return;
    MatchControlPanel* panel = reinterpret_cast<BattleConsole*>(battle)->match_control;
    if (panel == nullptr)
        return;
    const char* sequence = panel->state != nullptr && panel->state->face != match_face::minimized
                               ? k_minimize_open
                               : k_minimize;
    set_minimize_frame(panel->minimize, images, sequence, frame);
}

void rest_blink(MatchmakingDialog* dialog) {
    dialog->blink_ticks = 0;
    if (!dialog->blink_lit)
        return;
    dialog->blink_lit = false;
    apply_blink(dialog, false);
}

BoneyardsService* online_of(const MatchmakingDialog* dialog) {
    if (dialog == nullptr || dialog->owner == nullptr)
        return nullptr;
    return &dialog->owner->config.service;
}

void note_sent(MatchmakingDialog* dialog, ServiceResult result) {
    if (result == ServiceResult::pending)
        ++dialog->choices.requests_sent;
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

void fill_query(const MatchmakingDialog* dialog, MatchmakingQuery* query) {
    uint32_t area_id = 0;
    const bool known = selected_area(dialog->owner, &area_id);
    screens::matchmaking_query(&dialog->choices, area_id, known, query);
}

void begin_status_wait(screens::MatchmakingChoices* choices) {
    choices->awaiting_status = true;
    choices->status_wait = 0;
    choices->look_wait = 0;
}

void send_look(MatchmakingDialog* dialog) {
    BoneyardsService* online = online_of(dialog);
    if (online == nullptr || online->matchmaking_look == nullptr)
        return;
    MatchmakingQuery query{};
    fill_query(dialog, &query);
    note_sent(dialog, online->matchmaking_look(online->user, &query));
    dialog->choices.look_wait = 0;
}

void send_join(MatchmakingDialog* dialog) {
    begin_status_wait(&dialog->choices);
    BoneyardsService* online = online_of(dialog);
    if (online == nullptr || online->matchmaking_join == nullptr)
        return;
    MatchmakingQuery query{};
    fill_query(dialog, &query);
    note_sent(dialog, online->matchmaking_join(online->user, &query));
}

void send_cancel(MatchmakingDialog* dialog) {
    BoneyardsService* online = online_of(dialog);
    if (online == nullptr || online->matchmaking_cancel == nullptr)
        return;
    note_sent(dialog, online->matchmaking_cancel(online->user));
}

void send_answer(MatchmakingDialog* dialog, int32_t accept) {
    BoneyardsService* online = online_of(dialog);
    if (online == nullptr || online->matchmaking_answer == nullptr)
        return;
    note_sent(dialog, online->matchmaking_answer(online->user, dialog->choices.offer_id, accept));
}

// Searching joins, or replaces the filters of a search already queued. Settings asks the count
// only while the player is not queued. Coming back to settings after a failure is not an open.
void send_for_face(MatchmakingDialog* dialog) {
    if (dialog->face == k_matchmaking_searching) {
        screens::matchmaking_queue_activate(&dialog->choices);
        send_join(dialog);
        return;
    }
    if (!dialog->choices.queued)
        send_look(dialog);
}

void show_words(const char* english) {
    if (english == nullptr || english[0] == '\0')
        return;
    message_box_create(english, message_box_buttons::ok, nullptr, nullptr, nullptr);
}

void write_offer_text(MatchmakingDialog* dialog) {
    char line[192];
    screens::matchmaking_offer_line(&dialog->choices, line, sizeof line);
    set_label(dialog->offer_text, line);
}

void apply_effect(
    MatchmakingDialog* dialog,
    const screens::MatchmakingEffect& effect,
    const MatchmakingNotice* notice
) {
    if (effect.face == screens::MatchmakingFaceChange::settings)
        dialog->face = k_matchmaking_settings;
    else if (effect.face == screens::MatchmakingFaceChange::searching) {
        dialog->face = k_matchmaking_searching;
        dialog->base.visible = 1;
    }
    if (effect.close_offer)
        show_offer(dialog, false);
    if (effect.show_offer) {
        dialog->face = k_matchmaking_searching;
        dialog->base.visible = 1;
        write_offer_text(dialog);
        show_offer(dialog, true);
        rest_blink(dialog);
    }
    if (effect.show_box)
        show_words(screens::matchmaking_result_text(effect.box_result));
    if (effect.launch_host && notice != nullptr) {
        HostGameRequest request{};
        if (screens::matchmaking_host_request(&notice->go, &request)) {
            BoneyardsService* online = online_of(dialog);
            if (online != nullptr && online->host_game != nullptr)
                online->host_game(online->user, &request);
        }
    }
    if (effect.launch_join && notice != nullptr) {
        BoneyardsService* online = online_of(dialog);
        if (online != nullptr && online->join_game != nullptr)
            online->join_game(online->user, notice->go.area_id, notice->go.game_id, nullptr);
    }
    if (dialog->built && !dialog->unavailable)
        show_face(dialog);
}

void take_notices(MatchmakingDialog* dialog) {
    BoneyardsService* online = online_of(dialog);
    if (online == nullptr || online->matchmaking_take == nullptr)
        return;
    for (int32_t taken = 0; taken < 8; ++taken) {
        MatchmakingNotice notice{};
        if (!online->matchmaking_take(online->user, &notice))
            return;
        screens::MatchmakingEffect effect{};
        if (notice.kind == MatchmakingNoticeKind::status)
            effect = screens::matchmaking_apply_status(&dialog->choices, &notice.status);
        else if (notice.kind == MatchmakingNoticeKind::found)
            effect = screens::matchmaking_apply_found(&dialog->choices, &notice.found);
        else if (notice.kind == MatchmakingNoticeKind::go)
            effect = screens::matchmaking_apply_go(&dialog->choices, &notice.go);
        apply_effect(dialog, effect, &notice);
    }
}

// Five seconds with no status after a join. The dialog is not forced open, and cancel is not sent.
void queue_unavailable(MatchmakingDialog* dialog) {
    screens::matchmaking_queue_cancel(&dialog->choices);
    dialog->face = k_matchmaking_settings;
    show_offer(dialog, false);
    if (dialog->built && !dialog->unavailable)
        show_face(dialog);
    show_words(screens::matchmaking_unavailable_text());
}

bool same_radio(const screens::LobbyRadio* radio, const Window* source) {
    return radio != nullptr && source == &radio->base;
}

void take_radio(MatchmakingDialog* dialog, Window* source) {
    screens::MatchmakingChoices& choices = dialog->choices;
    if (same_radio(dialog->game[0], source))
        choices.game = screens::MatchGame::duel;
    else if (same_radio(dialog->game[1], source))
        choices.game = screens::MatchGame::winner_takes_all;
    else if (same_radio(dialog->game[2], source))
        choices.game = screens::MatchGame::for_fun;
    else if (same_radio(dialog->skill[0], source))
        choices.skill = screens::MatchSkill::near_mine;
    else if (same_radio(dialog->skill[1], source))
        choices.skill = screens::MatchSkill::any;
    else if (same_radio(dialog->side[0], source))
        choices.side = screens::MatchSide::arm;
    else if (same_radio(dialog->side[1], source))
        choices.side = screens::MatchSide::core;
    else if (same_radio(dialog->side[2], source))
        choices.side = screens::MatchSide::either;
    else if (same_radio(dialog->planets[0], source))
        choices.planets = screens::MatchPlanets::this_planet;
    else if (same_radio(dialog->planets[1], source))
        choices.planets = screens::MatchPlanets::any_planet;
    else
        return;
    refresh_texts(dialog);
    if (dialog->choices.queued)
        send_join(dialog);
    else if (dialog->base.visible != 0)
        send_look(dialog);
}

void dialog_on_notify(Window* self, const Notify* notify) {
    if (notify == nullptr || notify->code != NotifyCode::activated)
        return;
    MatchmakingDialog* dialog = as_dialog(self);
    if (!dialog->built)
        return;
    if (notify->source == dialog->activate) {
        play_green(dialog);
        dialog->face = k_matchmaking_searching;
        screens::matchmaking_queue_activate(&dialog->choices);
        show_face(dialog);
        send_join(dialog);
        return;
    }
    if (notify->source == dialog->deactivate) {
        play_green(dialog);
        screens::matchmaking_queue_cancel(&dialog->choices);
        dialog->face = k_matchmaking_settings;
        show_face(dialog);
        rest_blink(dialog);
        show_offer(dialog, false);
        send_cancel(dialog);
        return;
    }
    if (notify->source == dialog->close) {
        self->visible = 0;
        rest_blink(dialog);
        return;
    }
    if (notify->source == dialog->yes || notify->source == dialog->no) {
        send_answer(dialog, notify->source == dialog->yes ? 1 : 0);
        show_offer(dialog, false);
        return;
    }
    take_radio(dialog, notify->source);
}

void dialog_pulse(Window* self) {
    MatchmakingDialog* dialog = as_dialog(self);
    if (!dialog->built || dialog->unavailable)
        return;
    take_notices(dialog);
    if (dialog->choices.awaiting_status && dialog->choices.queued) {
        ++dialog->choices.status_wait;
        if (dialog->choices.status_wait >= screens::k_matchmaking_status_wait)
            queue_unavailable(dialog);
    }
    if (dialog->choices.queued)
        screens::matchmaking_queue_pulse(&dialog->choices);
    if (self->visible != 0 && !dialog->choices.queued) {
        ++dialog->choices.look_wait;
        if (dialog->choices.look_wait >= screens::k_matchmaking_look_period)
            send_look(dialog);
    }
    refresh_texts(dialog);
    refresh_scanner(dialog);
    if (dialog->choices.queued && self->visible == 0) {
        ++dialog->blink_ticks;
        if (dialog->blink_ticks >= k_blink_pulses) {
            dialog->blink_ticks = 0;
            dialog->blink_lit = !dialog->blink_lit;
            apply_blink(dialog, dialog->blink_lit);
        }
        return;
    }
    rest_blink(dialog);
}

void dialog_destroy(Window* self) {
    console_window_free(self);
}

void link_group(screens::LobbyRadio** radios, int32_t count) {
    for (int32_t i = 0; i < count; ++i) {
        if (radios[i] == nullptr)
            continue;
        for (int32_t j = 0; j < count; ++j)
            if (i != j && radios[j] != nullptr)
                screens::lobby_radio_add_member(radios[i], &radios[j]->base);
    }
}

void choose_one(screens::LobbyRadio** radios, int32_t count, int32_t index) {
    for (int32_t i = 0; i < count; ++i) {
        if (radios[i] == nullptr)
            continue;
        window_set_state(
            &radios[i]->base, i == index ? button_state::pressed : button_state::unpressed
        );
    }
}

Window* control_window(const GuiScreen* screen, const char* name) {
    const GuiControl* control = gui_screen_find(screen, name);
    return control != nullptr ? control->window : nullptr;
}

screens::LobbyRadio* control_radio(const GuiScreen* screen, const char* name) {
    return reinterpret_cast<screens::LobbyRadio*>(control_window(screen, name));
}

void reparent_children(Window* from, Window* to) {
    std::vector<Window*> children;
    children.reserve(static_cast<std::size_t>(from->children.count));
    for (int32_t i = 0; i < from->children.count; ++i)
        children.push_back(from->children.items[i]);
    for (Window* child : children)
        window_set_parent(child, to);
}

void say_needs_mod() {
    if (message_box_find_open(services().desktop) != nullptr)
        return;
    message_box_create(
        "Matchmaking needs the Boneyards mod.", message_box_buttons::ok, nullptr, nullptr, nullptr
    );
}

// The file supplies every control. A missing file hides the dialog and shows
// the box that says Matchmaking needs the mod, so Chat and Battle do not
// open an empty window.
void adopt(MatchmakingDialog* dialog) {
    GuiLoadError error{};
    GuiScreen* screen = gui_screen_load(k_gui_file, &error);
    if (screen == nullptr) {
        dialog->unavailable = true;
        dialog->base.visible = 0;
        say_needs_mod();
        return;
    }
    dialog->close = control_window(screen, "Close");
    dialog->activate = control_window(screen, "Activate");
    dialog->deactivate = control_window(screen, "Deactivate");
    dialog->scanner = control_window(screen, "Scanner");
    dialog->meter = control_window(screen, "QueueMeter");
    dialog->queue_line = control_window(screen, "QueueLine");
    dialog->queue_count = control_window(screen, "QueueCount");
    dialog->blurb[0] = control_window(screen, "Blurb1");
    dialog->blurb[1] = control_window(screen, "Blurb2");
    dialog->blurb[2] = control_window(screen, "Blurb3");
    dialog->status = control_window(screen, "SearchLine");
    dialog->detail = control_window(screen, "SearchDetail");
    dialog->offer = control_window(screen, "OfferFace");
    dialog->offer_text = control_window(screen, "OfferText");
    dialog->yes = control_window(screen, "Yes");
    dialog->no = control_window(screen, "No");
    dialog->game[0] = control_radio(screen, "Duel");
    dialog->game[1] = control_radio(screen, "WinnerTakesAll");
    dialog->game[2] = control_radio(screen, "For Fun");
    dialog->skill[0] = control_radio(screen, "Near mine");
    dialog->skill[1] = control_radio(screen, "Any");
    dialog->side[0] = control_radio(screen, "Arm");
    dialog->side[1] = control_radio(screen, "Core");
    dialog->side[2] = control_radio(screen, "Either");
    dialog->planets[0] = control_radio(screen, "This planet");
    dialog->planets[1] = control_radio(screen, "Any planet");
    reparent_children(gui_screen_root(screen), &dialog->base);
    gui_screen_destroy(screen);

    link_group(dialog->game, 3);
    link_group(dialog->skill, 2);
    link_group(dialog->side, 3);
    link_group(dialog->planets, 2);
    choose_one(dialog->game, 3, static_cast<int32_t>(dialog->choices.game));
    choose_one(dialog->skill, 2, static_cast<int32_t>(dialog->choices.skill));
    choose_one(dialog->side, 3, static_cast<int32_t>(dialog->choices.side));
    choose_one(dialog->planets, 2, static_cast<int32_t>(dialog->choices.planets));
    show_offer(dialog, false);
}

void dialog_on_bounds(Window* self) {
    MatchmakingDialog* dialog = as_dialog(self);
    if (dialog->built || dialog->owner == nullptr)
        return;
    dialog->built = true;
    if (self->color >= 0) {
        color_release(self->color);
        self->color = k_no_handle;
    }
    adopt(dialog);
    if (dialog->unavailable)
        return;
    show_face(dialog);
    send_for_face(dialog);
}

const WindowOps* dialog_ops() {
    static const WindowOps ops = [] {
        WindowOps made = console_panel_ops();
        made.destroy = dialog_destroy;
        made.on_notify = dialog_on_notify;
        made.on_bounds = dialog_on_bounds;
        made.pulse = dialog_pulse;
        return made;
    }();
    return &ops;
}

void present(MatchmakingDialog* dialog, int32_t face) {
    if (dialog->unavailable) {
        dialog->base.visible = 0;
        say_needs_mod();
        return;
    }
    dialog->face = face;
    if (face == k_matchmaking_searching)
        screens::matchmaking_queue_activate(&dialog->choices);
    dialog->base.visible = 1;
    if (dialog->built)
        show_face(dialog);
    rest_blink(dialog);
    if (dialog->built)
        send_for_face(dialog);
    dialog->base.ops->raise(&dialog->base);
}

} // namespace

Window* matchmaking_dialog_open(StatusWidget* owner, int32_t searching) {
    if (owner == nullptr || owner->system.main_frame == nullptr)
        return nullptr;
    const int32_t face =
        searching == k_matchmaking_searching ? k_matchmaking_searching : k_matchmaking_settings;
    Window* existing = matchmaking_dialog_find(owner);
    if (existing != nullptr) {
        present(as_dialog(existing), face);
        return existing;
    }
    auto* dialog = as_dialog(console_window_alloc(sizeof(MatchmakingDialog), dialog_ops()));
    dialog->owner = owner;
    dialog->face = face;
    dialog->scanner_shown = -1;
    screens::matchmaking_setup(&dialog->choices);
    if (face == k_matchmaking_searching)
        screens::matchmaking_queue_activate(&dialog->choices);
    dialog->base.focusable = 0;
    namespace box = matchmaking_dialog_layout;
    window_create(
        &dialog->base,
        k_matchmaking_dialog_name,
        box::x,
        box::y,
        box::width,
        box::height,
        &owner->system.main_frame->base,
        true
    );
    return &dialog->base;
}

Window* matchmaking_dialog_find(const StatusWidget* owner) {
    if (owner == nullptr || owner->system.main_frame == nullptr)
        return nullptr;
    return window_find_named(&owner->system.main_frame->base, k_matchmaking_dialog_name, nullptr);
}

int32_t matchmaking_dialog_searching(const Window* dialog) {
    if (dialog == nullptr || !window_is_named(dialog, k_matchmaking_dialog_name))
        return k_matchmaking_settings;
    return as_dialog(dialog)->face;
}

bool matchmaking_dialog_queued(const Window* dialog) {
    if (dialog == nullptr || !window_is_named(dialog, k_matchmaking_dialog_name))
        return false;
    return as_dialog(dialog)->choices.queued;
}

void matchmaking_dialog_show_offer(Window* dialog) {
    if (dialog == nullptr || !window_is_named(dialog, k_matchmaking_dialog_name))
        return;
    MatchmakingDialog* panel = as_dialog(dialog);
    write_offer_text(panel);
    show_offer(panel, true);
}

int32_t matchmaking_dialog_requests(const Window* dialog) {
    if (dialog == nullptr || !window_is_named(dialog, k_matchmaking_dialog_name))
        return 0;
    return as_dialog(dialog)->choices.requests_sent;
}

} // namespace oa::boneyards::console

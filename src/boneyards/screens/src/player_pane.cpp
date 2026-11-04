// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/screens/player_pane.hpp"

#include "text.hpp"

namespace oa::boneyards::screens {
namespace {

constexpr const char* kNoDataHtml = "<html><BODY>No Data<P></BODY></html>";

// Shows the record's page for the current tab.
void show_page(PlayerPane* p, const ProfileRecord* r) {
    switch (static_cast<PaneTab>(p->tab)) {
    case PaneTab::general:
        p->html = profile_report_html(r->general_html);
        break;
    case PaneTab::ladder:
        p->html = profile_report_html(r->ladder_html);
        break;
    case PaneTab::awards:
        p->html = profile_report_html(r->awards_html);
        break;
    }
}

// Moves the highlight to a tab's label and shows its page.
bool select_page(ChatConsole* c, PaneTab tab) {
    PlayerPane* p = &c->pane;
    const int32_t index = static_cast<int32_t>(tab);
    if (p->tab == index)
        return false;
    p->tab_highlighted[p->tab] = false;
    p->tab_highlighted[index] = true;
    p->tab = index;
    show_page(p, p->record);
    chat_pane_changed(c, true);
    return true;
}

// Posts a pane message to the console with the pane's user.
void post_pane_message(ChatConsole* c, int32_t code) {
    char name[0x20];
    chat_format_player_name(c, c->pane.user_id, name, sizeof name);
    chat_handle_pane_message(c, code, c->pane.user_id, name);
}

} // namespace

void chat_pane_build(ChatConsole* c) {
    PlayerPane* p = &c->pane;
    p->user_id = 0;
    p->sysop = false;
    p->record = nullptr;
    p->tab = static_cast<int32_t>(PaneTab::general);
    p->user_icon_shown = false;
    p->name[0] = '\0';
    p->rank = "";
    p->insignia = insignia_frame::none;
    p->insignia_shown = false;
    p->clan[0] = '\0';
    p->page_value = widget_value::enabled;
    for (bool& highlighted : p->tab_highlighted)
        highlighted = false;
}

void chat_pane_changed(ChatConsole* c, bool page) {
    if (c->ui.pane_changed != nullptr)
        c->ui.pane_changed(c->ui.user, page);
}

void chat_pane_start_activity_light(ChatConsole* c) {
    if (c->ui.activity != nullptr)
        c->ui.activity(c->ui.user, true);
}

void chat_pane_stop_activity_light(ChatConsole* c) {
    if (c->ui.activity != nullptr)
        c->ui.activity(c->ui.user, false);
}

void chat_pane_on_ladder(
    void* console, int32_t ladder_id, int32_t ladder_subid, const ProfileFields* fields
) {
    auto* c = static_cast<ChatConsole*>(console);
    if (c->pane.record != nullptr)
        ladder_profile_apply(
            &c->pane.profile, profile_read_templates(c->service), ladder_id, ladder_subid, fields
        );
}

void chat_pane_on_profile(void* console, bool ok, const ProfileFields* fields) {
    auto* c = static_cast<ChatConsole*>(console);
    chat_pane_stop_activity_light(c);
    if (ok && c->pane.record != nullptr)
        profile_from_fields(&c->pane.profile, profile_read_templates(c->service), fields);
    chat_pane_render(c, ok);
}

void chat_pane_render(ChatConsole* c, bool ok) {
    PlayerPane* p = &c->pane;
    const ProfileRecord* r = p->record;
    if (!ok || r == nullptr) {
        p->html = kNoDataHtml;
        post_pane_message(c, chat_message::unlock_whisper);
        chat_pane_changed(c, true);
        return;
    }
    if (r->user_icon == 0) {
        p->user_icon_shown = false;
    } else {
        p->user_icon = r->user_icon;
        p->user_icon_shown = true;
    }
    copy_bounded(p->name, sizeof p->name, r->name);
    p->rank = rank_title(r->user_icon, true);
    switch (r->allegiance) {
    case 'A':
        p->name_color = SideColor::arm;
        p->insignia = insignia_frame::arm;
        p->insignia_shown = true;
        break;
    case 'C':
        p->name_color = SideColor::core;
        p->insignia = insignia_frame::core;
        p->insignia_shown = true;
        break;
    default:
        p->name_color = SideColor::no_side_pane;
        p->insignia_shown = false;
        break;
    }
    copy_bounded(p->clan, sizeof p->clan, r->clan != nullptr ? r->clan : "");
    show_page(p, r);
    p->tab_highlighted[p->tab] = true;
    if (r->user_id != static_cast<int32_t>(c->session->self_user_id)) {
        p->page_value = widget_value::enabled;
        p->whisper_icon_value = widget_value::icon_off;
        if (!p->sysop)
            p->silence_value = widget_value::icon_off;
    }
    for (int32_t& value : p->tab_values)
        value = widget_value::enabled;
    post_pane_message(c, chat_message::profile_shown);
    chat_pane_changed(c, true);
}

void chat_pane_whisper_icon_off(PlayerPane* pane) {
    pane->whisper_icon_value = widget_value::icon_off;
}

bool chat_pane_on_click(ChatConsole* c, PaneControl control) {
    PlayerPane* p = &c->pane;
    if (p->user_id == 0 || p->record == nullptr)
        return false;
    switch (control) {
    case PaneControl::page_button:
        chat_handle_pane_message(c, chat_message::go_to_room, 0, p->record->name);
        return true;
    case PaneControl::whisper_icon:
        chat_handle_pane_message(c, chat_message::lock_whisper, p->user_id, nullptr);
        return true;
    case PaneControl::silence_icon:
        chat_handle_pane_message(c, chat_message::toggle_muzzle, p->user_id, nullptr);
        return true;
    case PaneControl::general_tab:
        return select_page(c, PaneTab::general);
    case PaneControl::ladder_tab:
        return select_page(c, PaneTab::ladder);
    case PaneControl::awards_tab:
        return select_page(c, PaneTab::awards);
    }
    return false;
}

void chat_pane_disable_whisper_icon(PlayerPane* pane) {
    pane->whisper_icon_value = widget_value::disabled;
}

void chat_pane_close(PlayerPane* pane) {
    pane->visible = false;
    profile_record_release(&pane->profile);
    pane->record = nullptr;
    pane->user_id = 0;
    // The view keeps its own copy of the page; the text left here must not
    // point into the record just freed.
    pane->html = kPaneEmptyPage;
}

} // namespace oa::boneyards::screens

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/screens/profile.hpp"

#include "oa/boneyards/screens/chat.hpp"
#include "text.hpp"

#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <type_traits>

namespace oa::boneyards::screens {
namespace {

// Installed by the extension. Opening a console does not clear it.
void (*g_unit_limit_apply)(int32_t units_per_player) = nullptr;

void show_message(void* user, void (*box)(void*, const char*), const char* text) {
    if (box != nullptr)
        box(user, text);
}

bool save_number(ProfileConsole* c, const char* key, std::int32_t value) {
    return c->settings.save_number != nullptr &&
           c->settings.save_number(c->settings.user, key, value);
}

void set_activity(const ProfileConsole* c, bool running) {
    if (c->activity != nullptr)
        c->activity(c->ui_user, running);
}

bool valid_tab(std::int32_t tab) {
    return tab >= 0 && tab < static_cast<std::int32_t>(kProfileTabCount);
}

// Rejects an entry: shows the message, switches to the owning tab and selects
// the offending field.
void reject_entry(ProfileConsole* c, const char* text, ProfileTab tab, std::int32_t field) {
    show_message(c->ui_user, c->message_box, text);
    profile_select_tab(c, static_cast<std::int32_t>(tab));
    c->selected_field = field;
}

/// Tells whether an edit field holds the record's text; a field the reply omitted compares as
/// empty.
///
/// @param field the edit field's text
/// @param record the record's text, or null when the reply omitted it
/// @return true when the texts are equal
bool same_text(const char* field, const char* record) {
    return std::strcmp(field, record != nullptr ? record : "") == 0;
}

/// Tells whether a check box's value is Checked Up.
///
/// @param value widget value
/// @return true when checked
bool is_checked(int32_t value) {
    return value == widget_value::icon_on;
}

/// Returns the name of a radio group's selected member, or "" with none selected.
///
/// @param group the radio group
/// @return the selected member's name, or ""
const char* selected_name(const RadioGroup& group) {
    if (group.selected < 0 || group.selected >= group.count)
        return "";
    return group.members[group.selected].name;
}

// Edit fields that can be selected after a rejected entry.
constexpr std::int32_t kFieldUnitLimit = 0;
constexpr std::int32_t kFieldSlideSteps = 1;
constexpr std::int32_t kFieldConfirm = 2;

// Radio member names of the setup panel.
constexpr const char* kPlayers2 = "2";
constexpr const char* kPlayers3 = "3";
constexpr const char* kPlayers4 = "4";

// The set_profile property bag's keys.
constexpr const char* kEditPassword = "PASSWORD";
constexpr const char* kEditTag = "TAG";
constexpr const char* kEditCoreContingency = "HAS_CC";
constexpr const char* kEditBattleTactics = "HAS_BT";
constexpr const char* kEditForumAlias = "TAFORUM_ALIAS";
constexpr const char* kEditIcq = "ICQ_ALIAS";
constexpr const char* kEditEmailAlias = "EMAIL_ALIAS";
constexpr const char* kEditAllegiance = "ALLEGIANCE";
constexpr const char* kEditPlayers = "NUMPLAYERS";
constexpr const char* kEditSkill = "PLAYERSKILL";
constexpr const char* kEditDeath = "ONCOMDEATH";
constexpr const char* kEditFavoriteMap = "FAVORITEMAP";
// A text field emptied is sent as a single space, which the reply's readers
// take back as empty.
constexpr const char* kEditBlank = " ";
constexpr const char* kDoesntMatter = "Doesn't Matter";

void add_edit(ProfileEdits* edits, const char* key, const char* value) {
    if (edits->count >= kProfileEditCapacity)
        return;
    ProfileEditField& field = edits->fields[edits->count++];
    field.key = key;
    copy_bounded(field.value, sizeof field.value, value);
}

/// Adds a text field that differs from the record's; an emptied one goes as a single space.
///
/// @param[in,out] edits the bag
/// @param key field name
/// @param text the entry's text
/// @param record the record's text, or null when the reply omitted it (compared as empty)
void add_changed_text(ProfileEdits* edits, const char* key, const char* text, const char* record) {
    if (same_text(text, record))
        return;
    add_edit(edits, key, text[0] != '\0' ? text : kEditBlank);
}

/// Adds an expansion check ("Yes" or "No") that differs from the record's, or whatever it is when
/// the reply gave the record's empty.
///
/// @param[in,out] edits the bag
/// @param key HAS_CC or HAS_BT
/// @param checked whether the box is checked
/// @param record_yes the record's value
/// @param record_set whether the record's value was given
void add_expansion(
    ProfileEdits* edits, const char* key, bool checked, bool record_yes, bool record_set
) {
    if (checked == record_yes && record_set)
        return;
    add_edit(edits, key, checked ? "Yes" : "No");
}

/// Adds EMAIL_ALIAS when the Display Email Alias check differs from whether the record has an
/// alias: the record's email when checked and known, else a single space.
///
/// @param[in,out] edits the bag
/// @param checked whether the box is checked
/// @param r the record
void add_email_alias(ProfileEdits* edits, bool checked, const ProfileRecord* r) {
    const bool has_alias = r->email_alias != nullptr && r->email_alias[0] != '\0';
    if (checked == has_alias)
        return;
    const bool has_email = r->email != nullptr && r->email[0] != '\0';
    add_edit(edits, kEditEmailAlias, checked && has_email ? r->email : kEditBlank);
}

/// Adds ALLEGIANCE while the side can still change: 'U' with neither radio, 'ARM' or 'CORE'
/// (Core when both are). A side the record already has is not sent again. A guest, a locked
/// side, or a reply that omitted the lock once a side is set, sends nothing.
///
/// @param[in,out] edits the bag
/// @param arm whether the Arm radio is selected
/// @param core whether the Core radio is selected
/// @param r the record
void add_allegiance(ProfileEdits* edits, bool arm, bool core, const ProfileRecord* r) {
    if (!profile_allegiance_open(r))
        return;
    const char* value = "U";
    if (arm)
        value = "ARM";
    if (core)
        value = "CORE";
    if (std::strcmp(value, "ARM") == 0 && r->allegiance == 'A')
        return;
    if (std::strcmp(value, "CORE") == 0 && r->allegiance == 'C')
        return;
    if (std::strcmp(value, "U") == 0 && r->allegiance != 'U')
        return;
    add_edit(edits, kEditAllegiance, value);
}

/// Adds NUMPLAYERS unless the choice is the record's and the reply gave it; "Doesn't Matter" goes
/// as "1".
///
/// @param[in,out] edits the bag
/// @param name the selected radio's name
/// @param r the record
void add_players(ProfileEdits* edits, const char* name, const ProfileRecord* r) {
    if (same_text(name, r->preferred_players) && r->players_given)
        return;
    add_edit(edits, kEditPlayers, std::strcmp(name, kDoesntMatter) == 0 ? "1" : name);
}

/// Adds PLAYERSKILL unless the choice is the record's and the reply gave it: Novice "1", Seasoned
/// "2", Expert "3", anything else "0".
///
/// @param[in,out] edits the bag
/// @param name the selected radio's name
/// @param r the record
void add_skill(ProfileEdits* edits, const char* name, const ProfileRecord* r) {
    if (same_text(name, r->preferred_skill) && r->skill_given)
        return;
    const char* value = "0";
    if (std::strcmp(name, "Novice") == 0)
        value = "1";
    else if (std::strcmp(name, "Seasoned") == 0)
        value = "2";
    else if (std::strcmp(name, "Expert") == 0)
        value = "3";
    add_edit(edits, kEditSkill, value);
}

/// Adds ONCOMDEATH unless the choice is the record's and the reply gave it: Continues "0", Ends
/// "1", Deathmatch "2", anything else "3".
///
/// @param[in,out] edits the bag
/// @param name the selected radio's name
/// @param r the record
void add_death(ProfileEdits* edits, const char* name, const ProfileRecord* r) {
    if (same_text(name, r->preferred_death) && r->death_given)
        return;
    const char* value = "3";
    if (std::strcmp(name, "Continues") == 0)
        value = "0";
    else if (std::strcmp(name, "Ends") == 0)
        value = "1";
    else if (std::strcmp(name, "Deathmatch") == 0)
        value = "2";
    add_edit(edits, kEditDeath, value);
}

// The answer to the console's profile change.
void profile_updated(void* console, bool ok) {
    profile_on_submitted(static_cast<ProfileConsole*>(console), ok);
}

} // namespace

void profile_console_init(
    ProfileConsole* c,
    const BoneyardsService* service,
    ChatSession* session,
    SettingsStore settings,
    void* ui_user,
    void (*message_box)(void* user, const char* text),
    void (*activity)(void* user, bool running)
) {
    profile_record_release(&c->profile);
    // The console is trivially copyable; it is cleared in place.
    static_assert(std::is_trivially_copyable_v<ProfileConsole>);
    std::memset(static_cast<void*>(c), 0, sizeof *c);
    c->service = service;
    c->session = session;
    c->settings = settings;
    c->ui_user = ui_user;
    c->message_box = message_box;
    c->activity = activity;
    c->current_tab = -1;
    c->selected_field = -1;
    c->templates = profile_read_templates(service);
    profile_record_init(&c->profile);
    c->record = &c->profile;
    profile_select_tab(c, static_cast<int32_t>(ProfileTab::account));
    profile_begin_receive(c);
}

void profile_show_tab(ProfileConsole* c, std::int32_t tab) {
    if (!valid_tab(tab))
        return;
    c->tab_visible[tab] = true;
    ProfileTabControls* t = &c->tabs;
    switch (static_cast<ProfileTab>(tab)) {
    case ProfileTab::account:
        if (c->password_fields_shown) {
            t->password.shown = true;
            t->confirm.shown = true;
        } else {
            t->change_shown = true;
        }
        break;
    case ProfileTab::personal:
        t->tag_line.shown = true;
        t->forum_alias.shown = true;
        t->icq.shown = true;
        break;
    case ProfileTab::allegiance:
        profile_allegiance_show(t, c->record);
        break;
    case ProfileTab::game_preferences:
        t->favorite_map.shown = true;
        break;
    case ProfileTab::ladder:
    case ProfileTab::awards:
    case ProfileTab::local_settings:
        break;
    }
}

void profile_hide_tab(ProfileConsole* c, std::int32_t tab) {
    if (!valid_tab(tab))
        return;
    c->tab_visible[tab] = false;
    ProfileTabControls* t = &c->tabs;
    switch (static_cast<ProfileTab>(tab)) {
    case ProfileTab::account:
        t->password.shown = false;
        t->confirm.shown = false;
        t->change_shown = false;
        break;
    case ProfileTab::personal:
        t->tag_line.shown = false;
        t->forum_alias.shown = false;
        t->icq.shown = false;
        break;
    case ProfileTab::allegiance:
        t->logo_shown = false;
        t->side_shown = false;
        t->rank_shown = false;
        t->join_shown = false;
        t->radios_shown = false;
        break;
    case ProfileTab::game_preferences:
        t->favorite_map.shown = false;
        break;
    case ProfileTab::ladder:
    case ProfileTab::awards:
    case ProfileTab::local_settings:
        break;
    }
}

void profile_select_tab(ProfileConsole* c, std::int32_t tab) {
    if (c->current_tab == tab || !valid_tab(tab))
        return;
    profile_create_tab(c, tab);
    profile_hide_tab(c, c->current_tab);
    profile_show_tab(c, tab);
    c->current_tab = tab;
    const char* description =
        c->templates != nullptr ? c->templates->category_descriptions[tab] : nullptr;
    c->tab_description = description != nullptr ? description : "";
    c->tab_list_row = tab;
}

void profile_on_unit_limit(void (*apply)(int32_t units_per_player)) {
    g_unit_limit_apply = apply;
}

void profile_apply(ProfileConsole* c) {
    c->selected_field = -1;
    if (c->tab_dirty[static_cast<int>(ProfileTab::local_settings)]) {
        bool stored = true;
        const std::int32_t unit_limit = std::atoi(c->unit_limit_text);
        if (c->unit_limit != unit_limit) {
            if (unit_limit < kUnitLimitMin || unit_limit > kUnitLimitMax) {
                reject_entry(
                    c,
                    "Invalid Entry!\nMax User Limit Must be a number between 21 and 500",
                    ProfileTab::local_settings,
                    kFieldUnitLimit
                );
                return;
            }
            if (save_number(c, kUnitLimitKey, unit_limit)) {
                c->unit_limit = unit_limit;
                c->game_unit_limit = static_cast<std::int16_t>(unit_limit);
                if (g_unit_limit_apply != nullptr)
                    g_unit_limit_apply(unit_limit);
            } else {
                stored = false;
            }
        }
        const std::int32_t slide = std::atoi(c->slide_steps_text);
        if (c->slide_steps != slide) {
            if (slide < kSlideStepsMin || slide > kSlideStepsMax) {
                reject_entry(
                    c,
                    "Invalid Entry!\nSlide Steps Must be a number between 1 and 40",
                    ProfileTab::local_settings,
                    kFieldSlideSteps
                );
                return;
            }
            if (save_number(c, kConsoleSlideKey, slide)) {
                c->slide_steps = slide;
                if (c->set_slide_steps != nullptr)
                    c->set_slide_steps(c->slide_user, slide);
            } else {
                stored = false;
            }
        }
        if (c->sound_off != c->sound_off_checked) {
            if (save_number(c, kSoundOffKey, c->sound_off_checked ? 1 : 0)) {
                c->sound_off = c->sound_off_checked;
                if (c->session != nullptr)
                    c->session->background_sound = !c->sound_off;
                if (c->background_sound != nullptr)
                    c->background_sound(c->sound_user, !c->sound_off);
            } else {
                stored = false;
            }
        }
        if (!stored) {
            show_message(
                c->ui_user,
                c->message_box,
                "Update aborted! Problem storing local configurantion changes.\n"
                "Check Totala.ini for errors."
            );
            return;
        }
        c->tab_dirty[static_cast<int>(ProfileTab::local_settings)] = false;
    }

    ProfileEdits* edits = &c->edits;
    edits->count = 0;
    const ProfileRecord* r = c->record;
    const ProfileTabControls* t = &c->tabs;
    const char* password = t->password.text;
    const char* confirm = t->confirm.text;
    if (c->tab_dirty[static_cast<int>(ProfileTab::account)] &&
        (password[0] != '\0' || confirm[0] != '\0')) {
        if (std::strcmp(password, confirm) != 0) {
            reject_entry(
                c,
                "Invalid Entry!\nPassword confirmation does not match",
                ProfileTab::account,
                kFieldConfirm
            );
            return;
        }
        if (std::strchr(password, '\'') != nullptr) {
            reject_entry(
                c,
                "Invalid Entry!\nPassword may not contain the ' character",
                ProfileTab::account,
                kFieldConfirm
            );
            return;
        }
        add_edit(edits, kEditPassword, password);
    }
    if (c->tab_dirty[static_cast<int>(ProfileTab::personal)]) {
        add_changed_text(edits, kEditTag, t->tag_line.text, r->tag_line);
        add_expansion(
            edits,
            kEditCoreContingency,
            is_checked(t->core_contingency_value),
            r->has_core_contingency,
            r->core_contingency_set
        );
        add_expansion(
            edits,
            kEditBattleTactics,
            is_checked(t->battle_tactics_value),
            r->has_battle_tactics,
            r->battle_tactics_set
        );
        add_changed_text(edits, kEditForumAlias, t->forum_alias.text, r->forum_alias);
        add_changed_text(edits, kEditIcq, t->icq.text, r->icq);
        add_email_alias(edits, is_checked(t->display_email_alias_value), r);
    }
    if (c->tab_dirty[static_cast<int>(ProfileTab::allegiance)])
        add_allegiance(
            edits,
            t->arm_value == widget_value::selected,
            t->core_value == widget_value::selected,
            r
        );
    if (c->tab_dirty[static_cast<int>(ProfileTab::game_preferences)]) {
        add_players(edits, selected_name(t->players), r);
        add_skill(edits, selected_name(t->skill), r);
        add_death(edits, selected_name(t->commander_death), r);
        add_changed_text(edits, kEditFavoriteMap, t->favorite_map.text, r->favorite_map);
    }
    if (edits->count == 0)
        return;
    const uint32_t user_id = c->session != nullptr ? c->session->self_user_id : 0;
    const ServiceResult sent =
        c->service->submit_profile(c->service->user, user_id, edits, profile_updated, c);
    if (sent == ServiceResult::ok || sent == ServiceResult::pending) {
        c->busy = true;
        c->tab_list_value = widget_value::disabled;
        profile_hide_tab(c, c->current_tab);
        copy_bounded(c->status, sizeof c->status, "TRANSMITTING DATA...");
        set_activity(c, true);
    }
}

void profile_check_tab(ProfileConsole* c, int32_t tab) {
    if (!valid_tab(tab) || !c->tab_created[tab])
        return;
    const ProfileRecord* r = c->record;
    const ProfileTabControls* t = &c->tabs;
    bool dirty = false;
    switch (static_cast<ProfileTab>(tab)) {
    case ProfileTab::account:
        dirty = t->password.text[0] != '\0' || t->confirm.text[0] != '\0';
        break;
    case ProfileTab::personal: {
        const bool has_email_alias = r->email_alias != nullptr && r->email_alias[0] != '\0';
        dirty = !same_text(t->tag_line.text, r->tag_line) ||
                is_checked(t->core_contingency_value) != r->has_core_contingency ||
                is_checked(t->battle_tactics_value) != r->has_battle_tactics ||
                !same_text(t->forum_alias.text, r->forum_alias) ||
                !same_text(t->icq.text, r->icq) ||
                is_checked(t->display_email_alias_value) != has_email_alias;
        break;
    }
    case ProfileTab::allegiance:
        if (profile_allegiance_open(r)) {
            char chosen = '\0';
            if (t->arm_value == widget_value::selected)
                chosen = 'A';
            if (t->core_value == widget_value::selected)
                chosen = 'C';
            if (r->allegiance == 'U')
                dirty = chosen != '\0';
            else
                dirty = chosen != '\0' && chosen != r->allegiance;
        }
        break;
    case ProfileTab::game_preferences:
        if (r->user_id >= 0)
            dirty = !same_text(selected_name(t->players), r->preferred_players) ||
                    !same_text(selected_name(t->skill), r->preferred_skill) ||
                    !same_text(selected_name(t->commander_death), r->preferred_death) ||
                    !same_text(t->favorite_map.text, r->favorite_map);
        break;
    case ProfileTab::ladder:
    case ProfileTab::awards:
        break;
    case ProfileTab::local_settings:
        dirty = std::atoi(c->unit_limit_text) != c->unit_limit ||
                std::atoi(c->slide_steps_text) != c->slide_steps ||
                c->sound_off != c->sound_off_checked;
        break;
    }
    c->tab_dirty[tab] = dirty;
}

void profile_update_buttons(ProfileConsole* c) {
    profile_check_tab(c, c->current_tab);
    bool dirty = false;
    for (bool d : c->tab_dirty)
        dirty = dirty || d;
    const std::int32_t value = dirty && !c->busy ? widget_value::enabled : widget_value::disabled;
    c->apply_value = value;
    c->reset_value = value;
}

void profile_on_control(ProfileConsole* c, ProfileControl control, std::int32_t value) {
    switch (control) {
    case ProfileControl::tab_list:
        profile_select_tab(c, value);
        break;
    case ProfileControl::change_password_button:
        c->password_fields_shown = true;
        c->tabs.password.shown = true;
        c->tabs.confirm.shown = true;
        c->tabs.change_shown = false;
        break;
    case ProfileControl::reset_button:
        for (std::size_t i = 0; i < kProfileTabCount; ++i) {
            if (!c->tab_created[i])
                continue;
            profile_update_tab(c, static_cast<int32_t>(i));
            profile_check_tab(c, static_cast<int32_t>(i));
        }
        break;
    case ProfileControl::apply_button:
        profile_apply(c);
        break;
    }
}

void profile_begin_receive(ProfileConsole* c) {
    c->templates = profile_read_templates(c->service);
    profile_record_release(&c->profile);
    c->record = &c->profile;
    c->busy = true;
    c->tab_list_value = widget_value::disabled;
    profile_hide_tab(c, c->current_tab);
    copy_bounded(c->status, sizeof c->status, "RECEIVING DATA...");
    c->status_visible = true;
    set_activity(c, true);
    c->service->request_profile(
        c->service->user, c->session->self_user_id, profile_on_received, profile_on_ladder, c
    );
}

void profile_on_ladder(
    void* console, int32_t ladder_id, int32_t ladder_subid, const ProfileFields* fields
) {
    auto* c = static_cast<ProfileConsole*>(console);
    if (c->record != nullptr)
        ladder_profile_apply(&c->profile, c->templates, ladder_id, ladder_subid, fields);
}

void profile_on_received(void* console, bool ok, const ProfileFields* fields) {
    auto* c = static_cast<ProfileConsole*>(console);
    set_activity(c, false);
    if (!ok || c->record == nullptr) {
        copy_bounded(c->status, sizeof c->status, "ACCESS FAILED. Try again later.");
        return;
    }
    profile_from_fields(&c->profile, c->templates, fields);
    c->busy = false;
    c->tab_list_value = widget_value::selected;
    profile_show_tab(c, c->current_tab);
    c->status_visible = false;
    for (std::size_t tab = 0; tab < kProfileTabCount; ++tab)
        profile_update_tab(c, static_cast<int32_t>(tab));
}

void profile_on_submitted(ProfileConsole* c, bool ok) {
    c->busy = false;
    c->tab_list_value = widget_value::selected;
    profile_show_tab(c, c->current_tab);
    c->status_visible = false;
    set_activity(c, false);
    if (!ok)
        show_message(
            c->ui_user,
            c->message_box,
            "Update Failure\n.Some or all of the changes you requested were not completed."
        );
    profile_begin_receive(c);
}

std::size_t game_mode_list(GameModeEntry* out, std::size_t capacity) {
    static constexpr GameModeEntry kModes[kGameModeCount] = {
        {"For Fun", GameMode::for_fun},
        {"Duel", GameMode::duel},
        {"Winner Takes All", GameMode::winner_takes_all},
        {"Galactic War", GameMode::galactic_war},
        {"War Training", GameMode::war_training},
    };
    std::size_t n = 0;
    for (; n < kGameModeCount && n < capacity; ++n)
        out[n] = kModes[n];
    return n;
}

void game_setup_init(
    GameSetupPanel* p,
    const BoneyardsService* service,
    void* ui_user,
    void (*message_box)(void* user, const char* text)
) {
    std::memset(p, 0, sizeof *p);
    p->service = service;
    p->ui_user = ui_user;
    p->message_box = message_box;
    p->player_allegiance = 'U';
    for (RadioGroup* g : {&p->skill, &p->players, &p->commander_death})
        g->selected = -1;
    for (const char* name : {"Novice", "Seasoned", "Expert"})
        radio_add(&p->skill, name, widget_value::enabled);
    for (const char* name : {kPlayers2, kPlayers3, kPlayers4})
        radio_add(&p->players, name, widget_value::enabled);
    for (const char* name : {"Ends", "Continues", "Deathmatch"})
        radio_add(&p->commander_death, name, widget_value::enabled);
    p->visible = true;
    game_setup_update_types(p);
}

void game_setup_update_types(GameSetupPanel* p) {
    p->type_count = 0;
    p->types[p->type_count++] = {"For Fun", GameMode::for_fun};
    p->types[p->type_count++] = {"Duel", GameMode::duel};
    p->types[p->type_count++] = {"Winner Takes All", GameMode::winner_takes_all};
    if (p->has_planet) {
        switch (p->planet_affiliation) {
        case 'I':
        case 'i':
            if (p->player_allegiance != 'U')
                p->types[p->type_count++] = {"Galactic War", GameMode::galactic_war};
            break;
        case 'A':
        case 'a':
            if (p->player_allegiance == 'A')
                p->types[p->type_count++] = {"War Training", GameMode::war_training};
            break;
        case 'C':
        case 'c':
            if (p->player_allegiance == 'C')
                p->types[p->type_count++] = {"War Training", GameMode::war_training};
            break;
        default:
            break;
        }
    }
    p->selected_type = 0;
    radio_refresh(&p->players);
    radio_refresh(&p->commander_death);
}

void game_setup_select_type(GameSetupPanel* p, std::int32_t index) {
    if (index < 0 || index >= static_cast<std::int32_t>(p->type_count))
        return;
    p->selected_type = index;
    const GameMode mode = p->types[index].mode;
    radio_refresh(&p->players);
    radio_refresh(&p->commander_death);
    switch (mode) {
    case GameMode::duel:
        radio_select_by_name(&p->players, kPlayers2);
        radio_disable_others(&p->players);
        return;
    case GameMode::winner_takes_all:
        radio_select_by_name(&p->players, kPlayers3);
        radio_disable_by_name(&p->players, kPlayers2);
        return;
    case GameMode::galactic_war:
    case GameMode::war_training:
        break;
    default:
        return;
    }
    if (!p->has_planet) {
        show_message(
            p->ui_user,
            p->message_box,
            mode == GameMode::galactic_war ? "In order to play a Galactic War game you must select "
                                             "a planet from the Galactic Map"
                                           : "In order to play a War Training game you must select "
                                             "a planet from the Galactic Map"
        );
        return;
    }
    radio_select_by_name(&p->players, kPlayers2);
    radio_disable_by_name(&p->players, kPlayers3);
    if (mode != GameMode::galactic_war || p->planet_players_limit != 4)
        radio_disable_by_name(&p->players, kPlayers4);
    switch (p->planet_commander_death) {
    case 1:
        radio_select_by_name(&p->commander_death, "Continues");
        break;
    case 2:
        radio_select_by_name(&p->commander_death, "Ends");
        break;
    case 3:
        radio_select_by_name(&p->commander_death, "Deathmatch");
        break;
    default:
        break;
    }
    radio_disable_others(&p->commander_death);
}

bool game_setup_start(GameSetupPanel* p) {
    if (p->type_count == 0 || p->selected_type < 0)
        return false;
    static constexpr std::int32_t kDeathCodes[] = {2, 1, 3}; // Ends, Continues, Deathmatch
    HostGameRequest request{};
    request.area_id = p->area_id;
    request.game_mode = static_cast<std::int32_t>(p->types[p->selected_type].mode);
    request.name = p->game_name;
    request.password = p->password[0] != '\0' ? p->password : nullptr;
    request.skill = p->skill.selected;
    request.players = p->players.selected;
    request.commander_death = p->commander_death.selected >= 0 && p->commander_death.selected < 3
                                  ? kDeathCodes[p->commander_death.selected]
                                  : 0;
    p->service->host_game(p->service->user, &request);
    p->visible = false;
    return true;
}

} // namespace oa::boneyards::screens

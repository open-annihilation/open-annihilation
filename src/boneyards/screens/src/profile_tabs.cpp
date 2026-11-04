// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Profile console tabs: the controls of each tab and their refresh from the
// player's profile record, and the record defaults and labels they share
// with the player pane.
#include "oa/boneyards/screens/profile.hpp"

#include "text.hpp"

#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace oa::boneyards::screens {
namespace {

// Characters accepted by the tab entries.
namespace entry_length {
constexpr int32_t password = 0xf;
constexpr int32_t tag_line = 0xff;
constexpr int32_t forum_alias = 0xf;
constexpr int32_t icq = 0xd;
constexpr int32_t favorite_map = 0x3f;
} // namespace entry_length

struct RankTitle {
    const char* qualified;
    const char* short_form;
};

// Rank titles by User_Icons frame.
const RankTitle* rank_entry(int32_t icon) {
    static constexpr RankTitle kCore[] = {
        {"Core Private", "Private"},
        {"Core Corporal", "Corporal"},
        {"Core Sergeant", "Sergeant"},
        {"Core Lieutenant", "Lieutenant"},
        {"Core Major", "Major"},
        {"Core Colonel", "Colonel"},
        {"Core Commander", "Commander"},
        {"Core High Commander", "High Commander"},
        {"Core Supreme Commander", "Supreme Commander"},
    };
    static constexpr RankTitle kArm[] = {
        {"Arm Private", "Private"},
        {"Arm Corporal", "Corporal"},
        {"Arm Sergeant", "Sergeant"},
        {"Arm Lieutenant", "Lieutenant"},
        {"Arm Major", "Major"},
        {"Arm Colonel", "Colonel"},
        {"Arm Commander", "Commander"},
        {"Arm High Commander", "High Commander"},
        {"Arm Supreme Commander", "Supreme Commander"},
    };
    static constexpr RankTitle kNewsbot{"Newsbot", "Newsbot"};
    static constexpr RankTitle kArmSystem{"Arm System Commander", "System Commander"};
    static constexpr RankTitle kYardDog{"Yard Dog", "Yard Dog"};
    static constexpr RankTitle kCoreSystem{"Core System Commander", "System Commander"};
    if (icon >= user_icon::core_private && icon <= user_icon::core_supreme_commander)
        return &kCore[icon - user_icon::core_private];
    if (icon >= user_icon::arm_private && icon <= user_icon::arm_supreme_commander)
        return &kArm[icon - user_icon::arm_private];
    if (icon >= user_icon::core_repeat_first && icon <= user_icon::core_repeat_supreme_commander)
        return &kCore[icon - user_icon::core_repeat_first + user_icon::repeated_rank_offset];
    if (icon >= user_icon::arm_repeat_first && icon <= user_icon::arm_repeat_supreme_commander)
        return &kArm[icon - user_icon::arm_repeat_first + user_icon::repeated_rank_offset];
    switch (icon) {
    case user_icon::newsbot:
        return &kNewsbot;
    case user_icon::arm_system_commander:
        return &kArmSystem;
    case user_icon::sysop:
        return &kYardDog;
    case user_icon::core_system_commander:
        return &kCoreSystem;
    default:
        return nullptr;
    }
}

bool is_guest(const ProfileRecord* r) {
    return r->user_id < 0;
}

const char* text_or_empty(const char* text) {
    return text != nullptr ? text : "";
}

int32_t check_value(bool checked) {
    return widget_value::icon_off + (checked ? 1 : 0);
}

void disable_entry(ProfileEntry* entry) {
    if (entry->value != widget_value::disabled)
        entry->value = widget_value::disabled;
}

void set_entry_text(ProfileEntry* entry, const char* text) {
    copy_chars(entry->text, sizeof entry->text, text_or_empty(text), entry->max_length);
}

int32_t read_setting(const ProfileConsole* c, const char* key, int32_t fallback) {
    if (c->settings.read_number == nullptr)
        return fallback;
    return c->settings.read_number(c->settings.user, key, fallback);
}

void build_radio(RadioGroup* group, std::initializer_list<const char*> names) {
    std::memset(group, 0, sizeof *group);
    group->selected = -1;
    for (const char* name : names)
        radio_add(group, name, widget_value::enabled);
}

/// Names a record's side: "Arm" or "Core", empty otherwise.
///
/// @param r profile record
/// @return static side name
const char* profile_side_name(const ProfileRecord* r) {
    if (r->allegiance == 'A')
        return "Arm";
    if (r->allegiance == 'C')
        return "Core";
    return "";
}

void update_account(ProfileConsole* c, const ProfileRecord* r) {
    ProfileTabControls* t = &c->tabs;
    copy_bounded(t->login_name, sizeof t->login_name, r->name);
    t->status = profile_status_label(r);
    copy_bounded(t->email, sizeof t->email, text_or_empty(r->email));
    c->password_fields_shown = false;
    t->password.shown = false;
    t->password.text[0] = '\0';
    t->confirm.shown = false;
    t->confirm.text[0] = '\0';
    if (is_guest(r)) {
        if (t->change_value != widget_value::disabled)
            t->change_value = widget_value::disabled;
    } else if (t->change_value == widget_value::disabled) {
        t->change_value = widget_value::enabled;
    }
    if (c->current_tab == static_cast<int32_t>(ProfileTab::account))
        t->change_shown = true;
}

void update_personal(ProfileTabControls* t, const ProfileRecord* r) {
    if (is_guest(r)) {
        disable_entry(&t->tag_line);
        t->core_contingency_value = widget_value::disabled;
        t->battle_tactics_value = widget_value::disabled;
        disable_entry(&t->forum_alias);
        disable_entry(&t->icq);
        t->display_email_alias_value = widget_value::disabled;
        return;
    }
    set_entry_text(&t->tag_line, r->tag_line);
    t->core_contingency_value = check_value(r->has_core_contingency);
    t->battle_tactics_value = check_value(r->has_battle_tactics);
    set_entry_text(&t->forum_alias, r->forum_alias);
    set_entry_text(&t->icq, r->icq);
    const bool has_email_alias = r->email_alias != nullptr && r->email_alias[0] != '\0';
    t->display_email_alias_value = check_value(has_email_alias);
}

void update_allegiance(ProfileTabControls* t, const ProfileRecord* r) {
    t->user_icon = r->user_icon;
    copy_bounded(t->user_name, sizeof t->user_name, r->name);
    if (is_guest(r)) {
        t->arm_value = widget_value::disabled;
        t->core_value = widget_value::disabled;
    } else {
        switch (r->allegiance) {
        case 'A':
            t->logo_frame = insignia_frame::arm;
            t->name_color = SideColor::arm;
            break;
        case 'C':
            t->logo_frame = insignia_frame::core;
            t->name_color = SideColor::core;
            break;
        default:
            t->logo_frame = insignia_frame::none;
            t->name_color = SideColor::no_side_tab;
            break;
        }
        if (r->allegiance == 'A' || r->allegiance == 'C') {
            t->side_name = profile_side_name(r);
            t->rank = rank_title(r->user_icon, true);
        }
        if (r->allegiance_locked > 0) {
            t->arm_value = widget_value::disabled;
            t->core_value = widget_value::disabled;
        } else if (profile_allegiance_open(r)) {
            if (r->allegiance == 'A') {
                t->arm_value = widget_value::selected;
                t->core_value = widget_value::enabled;
            } else if (r->allegiance == 'C') {
                t->arm_value = widget_value::enabled;
                t->core_value = widget_value::selected;
            } else {
                t->arm_value = widget_value::enabled;
                t->core_value = widget_value::enabled;
            }
        }
    }
    profile_allegiance_show(t, r);
    t->allegiance_html = profile_report_html(r->info_html);
}

void update_preferences(ProfileTabControls* t, const ProfileRecord* r) {
    if (is_guest(r)) {
        radio_clear(&t->skill);
        radio_clear(&t->players);
        radio_clear(&t->commander_death);
        disable_entry(&t->favorite_map);
        return;
    }
    radio_select_by_name(&t->skill, text_or_empty(r->preferred_skill));
    radio_select_by_name(&t->players, text_or_empty(r->preferred_players));
    radio_select_by_name(&t->commander_death, text_or_empty(r->preferred_death));
    set_entry_text(&t->favorite_map, r->favorite_map);
}

/// Shows a number in a profile edit field as signed decimal text.
///
/// @param[out] field edit field text
/// @param value number shown
void set_number_text(char (&field)[kProfileFieldCapacity], int32_t value) {
    std::snprintf(field, sizeof field, "%d", value);
}

void update_local_settings(ProfileConsole* c) {
    c->unit_limit = read_setting(c, kUnitLimitKey, kUnitLimitDefault);
    set_number_text(c->unit_limit_text, c->unit_limit);
    c->slide_steps = read_setting(c, kConsoleSlideKey, kSlideStepsDefault);
    set_number_text(c->slide_steps_text, c->slide_steps);
    c->sound_off = read_setting(c, kSoundOffKey, 0) != 0;
    c->sound_off_checked = c->sound_off;
}

} // namespace

void profile_record_init(ProfileRecord* r) {
    std::memset(r, 0, sizeof *r);
    r->allegiance = 'U';
    r->allegiance_locked = -1;
    r->status = static_cast<int32_t>(ProfileStatus::active);
}

const ProfileTemplates* profile_read_templates(const BoneyardsService* service) {
    if (service == nullptr || service->profile_templates == nullptr)
        return nullptr;
    return service->profile_templates(service->user);
}

const char* profile_status_label(const ProfileRecord* r) {
    if (is_guest(r))
        return "Guest";
    switch (static_cast<ProfileStatus>(r->status)) {
    case ProfileStatus::active:
        return "Active";
    case ProfileStatus::admin:
        return "Admin";
    case ProfileStatus::beta_tester:
        return "Beta Tester";
    }
    return "Unknown";
}

const char* rank_title(int32_t user_icon, bool short_form) {
    const RankTitle* entry = rank_entry(user_icon);
    if (entry == nullptr)
        return "";
    return short_form ? entry->short_form : entry->qualified;
}

void profile_entry_init(ProfileEntry* entry, int32_t max_length, bool masked) {
    std::memset(entry, 0, sizeof *entry);
    entry->max_length = max_length;
    entry->value = kEntryInitialValue;
    entry->shown = true;
    entry->mask = masked ? '*' : 0;
}

void profile_create_tab(ProfileConsole* c, int32_t tab) {
    if (tab < 0 || tab >= static_cast<int32_t>(kProfileTabCount) || c->tab_created[tab])
        return;
    c->tab_created[tab] = true;
    ProfileTabControls* t = &c->tabs;
    switch (static_cast<ProfileTab>(tab)) {
    case ProfileTab::account:
        profile_entry_init(&t->password, entry_length::password, true);
        profile_entry_init(&t->confirm, entry_length::password, true);
        t->change_value = widget_value::disabled;
        t->change_shown = true;
        break;
    case ProfileTab::personal:
        profile_entry_init(&t->tag_line, entry_length::tag_line, false);
        profile_entry_init(&t->forum_alias, entry_length::forum_alias, false);
        profile_entry_init(&t->icq, entry_length::icq, false);
        break;
    case ProfileTab::allegiance:
        t->logo_shown = true;
        t->side_shown = true;
        t->rank_shown = true;
        t->join_shown = true;
        t->radios_shown = true;
        break;
    case ProfileTab::game_preferences:
        build_radio(&t->skill, {"Novice", "Seasoned", "Expert", "Don't Know"});
        build_radio(&t->players, {"2", "3", "4", "Doesn't Matter"});
        build_radio(&t->commander_death, {"Ends", "Continues", "Deathmatch", "Doesn't Matter"});
        profile_entry_init(&t->favorite_map, entry_length::favorite_map, false);
        break;
    case ProfileTab::ladder:
    case ProfileTab::awards:
    case ProfileTab::local_settings:
        break;
    }
    profile_update_tab(c, tab);
}

void profile_update_tab(ProfileConsole* c, int32_t tab) {
    if (tab < 0 || tab >= static_cast<int32_t>(kProfileTabCount) || !c->tab_created[tab])
        return;
    const ProfileRecord* r = c->record;
    ++c->tab_revision[tab];
    switch (static_cast<ProfileTab>(tab)) {
    case ProfileTab::account:
        update_account(c, r);
        break;
    case ProfileTab::personal:
        update_personal(&c->tabs, r);
        break;
    case ProfileTab::allegiance:
        update_allegiance(&c->tabs, r);
        break;
    case ProfileTab::game_preferences:
        update_preferences(&c->tabs, r);
        break;
    case ProfileTab::ladder:
        c->tabs.ladder_html = profile_report_html(r->ladder_html);
        break;
    case ProfileTab::awards:
        c->tabs.awards_html = profile_report_html(r->awards_html);
        break;
    case ProfileTab::local_settings:
        update_local_settings(c);
        break;
    }
}

} // namespace oa::boneyards::screens

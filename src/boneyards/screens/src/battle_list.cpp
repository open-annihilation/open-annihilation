// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/screens/battle_list.hpp"

#include "text.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace oa::boneyards::screens {
namespace {

// A record's game id is matched over at most this many characters.
constexpr std::size_t kGameIdCompared = 0x24;
constexpr int32_t kChangeAdded = 0;
constexpr int32_t kChangeRemoved = 1;
constexpr int32_t kInProgress = 0;
constexpr int32_t kForming = 1;
constexpr int32_t kDefaultOnDeath = 2;
constexpr const char* kReportSeparator = ", ";
constexpr const char* kTitleSeparator = " ";
// Slots a growing table starts with.
constexpr std::size_t kFirstSlots = 8;

/// Makes room for at least `needed` items of `size` bytes in a heap table, doubling it; the new
/// items are zeroed.
///
/// @param[in,out] items the table; replaced when it moves
/// @param[in,out] capacity items the table holds
/// @param needed items it must hold
/// @param size bytes of one item
/// @param limit items the table may never exceed
/// @return false when `needed` is past `limit` or the table cannot grow; the table is then unchanged
bool reserve(
    void** items, std::size_t* capacity, std::size_t needed, std::size_t size, std::size_t limit
) {
    if (needed <= *capacity)
        return true;
    if (needed > limit)
        return false;
    std::size_t grown = *capacity != 0 ? *capacity : kFirstSlots;
    while (grown < needed)
        grown *= 2;
    if (grown > limit)
        grown = limit;
    void* moved = std::realloc(*items, grown * size);
    if (moved == nullptr)
        return false;
    std::memset(static_cast<char*>(moved) + *capacity * size, 0, (grown - *capacity) * size);
    *items = moved;
    *capacity = grown;
    return true;
}

void set_activity(const BattleList* list, bool running) {
    if (list->ui.activity != nullptr)
        list->ui.activity(list->ui.user, running);
}

// A request the service refuses gets no answer to stop the light.
void settle(const BattleList* list, ServiceResult result) {
    if (result != ServiceResult::ok && result != ServiceResult::pending)
        set_activity(list, false);
}

BattleEntry* entry_at(BattleList* list, int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= list->battle_slots ||
        !list->battles[index].used)
        return nullptr;
    return &list->battles[index];
}

void append_bounded(char* out, std::size_t capacity, const char* text) {
    const std::size_t used = std::strlen(out);
    copy_bounded(out + used, capacity - used, text);
}

/// Starts a battle record: forming, its host's side unknown, commander death 2, with no name,
/// intel, report rows or category.
///
/// @param[out] b record to start
/// @param game_id battle id
/// @param type game mode id
void battle_entry_init(BattleEntry* b, const char* game_id, int32_t type) {
    *b = BattleEntry{};
    b->used = true;
    b->category = kNoBattleCategory;
    b->type = type;
    b->forming = kForming;
    b->host_code = static_cast<int32_t>(BattleHost::unknown);
    copy_bounded(b->game_id, sizeof b->game_id, game_id);
    b->on_death = kDefaultOnDeath;
}

/// Sets a battle's name, forming flag and host side; the panels and the row showing it refresh.
///
/// @param[in,out] b battle record
/// @param name battle name
/// @param forming 1 while forming, 0 once in progress
/// @param host_code the host's side (BattleHost)
void battle_entry_set_info(BattleEntry* b, const char* name, int32_t forming, int32_t host_code) {
    copy_chars(b->name, sizeof b->name, name, kBattleNameCharacters);
    b->forming = forming;
    b->host_code = host_code;
}

/// Lists a record at the end of a category, which holds a reference on it.
///
/// @param[in,out] list battle list
/// @param category category index
/// @param battle record index
void category_link(BattleList* list, int32_t category, int32_t battle) {
    BattleCategory* c = &list->categories[category];
    void* items = c->battles;
    if (!reserve(
            &items, &c->battle_capacity, c->battle_count + 1, sizeof c->battles[0], kMaxBattles
        ))
        return;
    c->battles = static_cast<int32_t*>(items);
    c->battles[c->battle_count++] = battle;
    BattleEntry* b = &list->battles[battle];
    ++b->references;
    b->category = category;
}

/// Takes a record off the category listing it, dropping that reference.
///
/// @param[in,out] list battle list
/// @param battle record index
void category_unlink(BattleList* list, int32_t battle) {
    BattleEntry* b = &list->battles[battle];
    if (b->category == kNoBattleCategory)
        return;
    BattleCategory* c = &list->categories[b->category];
    for (std::size_t i = 0; i < c->battle_count; ++i) {
        if (c->battles[i] != battle)
            continue;
        --b->references;
        std::memmove(
            &c->battles[i], &c->battles[i + 1], (c->battle_count - i - 1) * sizeof c->battles[0]
        );
        --c->battle_count;
        break;
    }
    b->category = kNoBattleCategory;
}

// Drops one reference on a record and frees it when nothing holds it.
void release(BattleList* list, BattleEntry* b) {
    --b->references;
    char game_id[kBattleGameIdCapacity];
    copy_bounded(game_id, sizeof game_id, b->game_id);
    battle_list_remove(list, game_id);
}

/// Lets go of every record a category lists, freeing those nothing else holds, and empties it.
///
/// @param[in,out] list battle list
/// @param[in,out] c category to empty
void category_clear(BattleList* list, BattleCategory* c) {
    for (std::size_t i = 0; i < c->battle_count; ++i) {
        BattleEntry* b = &list->battles[c->battles[i]];
        b->category = kNoBattleCategory;
        release(list, b);
    }
    c->battle_count = 0;
}

/// Files a record under its game mode's category, listed for the current area.
///
/// A mode without a category leaves it unlisted.
///
/// @param[in,out] list battle list
/// @param area_id the session's current area
/// @param battle record index
void file_battle(BattleList* list, uint32_t area_id, int32_t battle) {
    BattleEntry* b = &list->battles[battle];
    b->area_id = area_id;
    for (std::size_t i = 0; i < list->category_count; ++i) {
        if (static_cast<int32_t>(list->categories[i].mode.mode) == b->type) {
            category_link(list, static_cast<int32_t>(i), battle);
            return;
        }
    }
}

/// Makes the record of a battle first reported and files it under its game mode.
///
/// @param[in,out] list battle list
/// @param area_id the session's current area
/// @param game_id battle id
/// @param type game mode id
/// The records grow as needed; the index of a free slot is reused.
///
/// @return the record's index, or kNoBattle when the list holds kMaxBattles records or
///     cannot grow
int32_t create_battle(BattleList* list, uint32_t area_id, const char* game_id, int32_t type) {
    std::size_t slot = 0;
    while (slot < list->battle_slots && list->battles[slot].used)
        ++slot;
    void* items = list->battles;
    if (!reserve(&items, &list->battle_slots, slot + 1, sizeof list->battles[0], kMaxBattles))
        return kNoBattle;
    list->battles = static_cast<BattleEntry*>(items);
    battle_entry_init(&list->battles[slot], game_id, type);
    file_battle(list, area_id, static_cast<int32_t>(slot));
    return static_cast<int32_t>(slot);
}

/// Takes a record off its category and marks it unlisted.
///
/// @param[in,out] list battle list
/// @param battle record index
void unfile_battle(BattleList* list, int32_t battle) {
    category_unlink(list, battle);
    list->battles[battle].area_id = 0;
}

/// Shows a battle on the Join summary or the Scoreboard.
///
/// The panel drops the reference it held, freeing that record unless
/// something else holds it, and takes one on the new record.
///
/// @param[in,out] list battle list
/// @param[in,out] panel_battle the panel's record index
/// @param battle record index to show
void hold_battle(BattleList* list, int32_t* panel_battle, int32_t battle) {
    if (BattleEntry* old = entry_at(list, *panel_battle)) {
        if (*panel_battle == battle)
            --old->references;
        else
            release(list, old);
    }
    *panel_battle = battle;
    if (BattleEntry* b = entry_at(list, battle))
        ++b->references;
}

/// Closes the Join summary or the Scoreboard, which lets go of its record.
///
/// @param[in,out] list battle list
/// @param[out] visible the panel's visible flag, cleared
/// @param[in,out] panel_battle the panel's record index, set to kNoBattle
void close_battle_panel(BattleList* list, bool* visible, int32_t* panel_battle) {
    *visible = false;
    if (BattleEntry* b = entry_at(list, *panel_battle))
        release(list, b);
    *panel_battle = kNoBattle;
}

// A shown panel's battle asks the service for its intel and report with the
// light running until they arrive.
void request_intel(const BattleList* list, const BattleEntry* b) {
    set_activity(list, true);
    settle(list, list->service->get_battle(list->service->user, b->area_id, b->game_id));
}

} // namespace

void battle_list_init(BattleList* list, const BoneyardsService* service, BattleUi ui) {
    battle_list_release(list);
    std::memset(list, 0, sizeof *list);
    list->service = service;
    list->ui = ui;
    list->join_battle = kNoBattle;
    list->scoreboard_battle = kNoBattle;
    GameModeEntry modes[kGameModeCount];
    list->category_count = game_mode_list(modes, kGameModeCount);
    for (std::size_t i = 0; i < list->category_count; ++i)
        battle_category_init(&list->categories[i], modes[i]);
    list->rows_dirty = true;
    battle_list_rebuild_rows(list);
}

void battle_list_release(BattleList* list) {
    std::free(list->battles);
    list->battles = nullptr;
    list->battle_slots = 0;
    for (BattleCategory& category : list->categories) {
        std::free(category.battles);
        category.battles = nullptr;
        category.battle_count = 0;
        category.battle_capacity = 0;
    }
    std::free(list->rows);
    list->rows = nullptr;
    list->row_count = 0;
    list->row_capacity = 0;
    list->join_battle = kNoBattle;
    list->scoreboard_battle = kNoBattle;
}

void battle_category_init(BattleCategory* category, const GameModeEntry& mode) {
    category->mode = mode;
    category->expanded = true;
    category->battle_count = 0;
}

void battle_list_open(BattleList* list, uint32_t area_id) {
    list->area_id = area_id;
    set_activity(list, true);
    settle(list, list->service->enter_battles(list->service->user, area_id));
}

void battle_list_close(BattleList* list, uint32_t area_id) {
    if (list->host_visible)
        list->host_visible = false;
    else if (list->scoreboard_visible)
        close_battle_panel(list, &list->scoreboard_visible, &list->scoreboard_battle);
    else if (list->join_visible)
        close_battle_panel(list, &list->join_visible, &list->join_battle);
    battle_list_reset_categories(list);
    list->rows_dirty = true;
    set_activity(list, true);
    settle(list, list->service->leave_battles(list->service->user, area_id));
}

int32_t battle_list_find(const BattleList* list, const char* game_id) {
    if (game_id == nullptr)
        return kNoBattle;
    for (std::size_t i = 0; i < list->battle_slots; ++i)
        if (list->battles[i].used &&
            std::strncmp(list->battles[i].game_id, game_id, kGameIdCompared) == 0)
            return static_cast<int32_t>(i);
    return kNoBattle;
}

void battle_list_remove(BattleList* list, const char* game_id) {
    BattleEntry* b = entry_at(list, battle_list_find(list, game_id));
    if (b == nullptr || b->references > 0)
        return;
    *b = BattleEntry{};
    b->category = kNoBattleCategory;
}

const BattleEntry* battle_list_entry(const BattleList* list, int32_t index) {
    return entry_at(const_cast<BattleList*>(list), index);
}

void battle_list_reset_categories(BattleList* list) {
    for (std::size_t i = 0; i < list->category_count; ++i)
        category_clear(list, &list->categories[i]);
}

void battle_list_rebuild_rows(BattleList* list) {
    if (!list->rows_dirty)
        return;
    std::size_t needed = list->category_count;
    for (std::size_t ci = 0; ci < list->category_count; ++ci)
        needed += list->categories[ci].battle_count;
    void* items = list->rows;
    if (!reserve(
            &items, &list->row_capacity, needed, sizeof list->rows[0], kGameModeCount + kMaxBattles
        ))
        return;
    list->rows = static_cast<BattleRow*>(items);
    list->row_count = 0;
    for (std::size_t ci = 0; ci < list->category_count; ++ci) {
        const BattleCategory* c = &list->categories[ci];
        list->rows[list->row_count++] = BattleRow{true, ci, kNoBattle};
        if (!c->expanded)
            continue;
        for (std::size_t bi = 0; bi < c->battle_count; ++bi)
            list->rows[list->row_count++] = BattleRow{false, ci, c->battles[bi]};
    }
    list->rows_dirty = false;
}

void battle_list_close_panels(BattleList* list) {
    if (list->host_visible)
        list->host_visible = false;
    if (list->join_visible)
        close_battle_panel(list, &list->join_visible, &list->join_battle);
    if (list->scoreboard_visible)
        close_battle_panel(list, &list->scoreboard_visible, &list->scoreboard_battle);
}

void battle_list_request_intel(BattleList* list, int32_t battle) {
    if (const BattleEntry* b = entry_at(list, battle))
        request_intel(list, b);
}

void battle_list_hold(BattleList* list, BattlePanel panel, int32_t battle) {
    if (panel == BattlePanel::join) {
        hold_battle(list, &list->join_battle, battle);
        list->join_visible = true;
    } else if (panel == BattlePanel::scoreboard) {
        hold_battle(list, &list->scoreboard_battle, battle);
        list->scoreboard_visible = true;
    }
}

void battle_list_close_panel(BattleList* list, BattlePanel panel) {
    switch (panel) {
    case BattlePanel::host:
        list->host_visible = false;
        break;
    case BattlePanel::join:
        close_battle_panel(list, &list->join_visible, &list->join_battle);
        break;
    case BattlePanel::scoreboard:
        close_battle_panel(list, &list->scoreboard_visible, &list->scoreboard_battle);
        break;
    }
}

bool battle_list_panel_shown(const BattleList* list, BattlePanel panel) {
    switch (panel) {
    case BattlePanel::host:
        return list->host_visible;
    case BattlePanel::join:
        return list->join_visible;
    case BattlePanel::scoreboard:
        return list->scoreboard_visible;
    }
    return false;
}

void battle_list_on_control(BattleList* list, BattleControl control, std::size_t row) {
    if (control == BattleControl::host_button) {
        const bool was_open = list->host_visible;
        battle_list_close_panels(list);
        if (!was_open) {
            list->host_visible = true;
            if (list->ui.open_host != nullptr)
                list->ui.open_host(list->ui.user);
        }
        return;
    }
    if (row >= list->row_count)
        return;
    const BattleRow r = list->rows[row];
    if (r.is_category) {
        list->rows_dirty = true;
        list->categories[r.category].expanded = !list->categories[r.category].expanded;
        return;
    }
    const BattleEntry* b = entry_at(list, r.battle);
    if (b == nullptr)
        return;
    request_intel(list, b);
    switch (b->forming) {
    case kInProgress:
        battle_list_close_panels(list);
        battle_list_hold(list, BattlePanel::scoreboard, r.battle);
        break;
    case kForming:
        battle_list_close_panels(list);
        battle_list_hold(list, BattlePanel::join, r.battle);
        break;
    default:
        break;
    }
}

void battle_list_on_done(BattleList* list) {
    set_activity(list, false);
}

void battle_list_on_battle_info(
    BattleList* list,
    uint32_t area_id,
    int32_t change,
    const char* game_id,
    const char* name,
    int32_t type,
    int32_t forming,
    int32_t host_code
) {
    int32_t battle = battle_list_find(list, game_id);
    list->rows_dirty = true;
    if (change == kChangeAdded) {
        if (battle == kNoBattle)
            battle = create_battle(list, area_id, game_id, type);
        else if (list->battles[battle].area_id != area_id)
            file_battle(list, area_id, battle);
        if (battle == kNoBattle)
            return;
        battle_entry_set_info(&list->battles[battle], name, forming, host_code);
    } else if (battle != kNoBattle) {
        unfile_battle(list, battle);
    }
    if (battle == kNoBattle)
        return;
    bool panels_changed = false;
    if (list->join_visible && list->join_battle == battle) {
        if (change == kChangeRemoved) {
            battle_list_close_panels(list);
            panels_changed = true;
        } else if (forming == kInProgress) {
            battle_list_close_panels(list);
            battle_list_hold(list, BattlePanel::scoreboard, battle);
            panels_changed = true;
        }
    }
    if (panels_changed && list->ui.panels_changed != nullptr)
        list->ui.panels_changed(list->ui.user);
    if (change == kChangeRemoved) {
        battle_list_remove(list, game_id);
        return;
    }
    if ((list->join_visible && list->join_battle == battle) ||
        (list->scoreboard_visible && list->scoreboard_battle == battle))
        request_intel(list, &list->battles[battle]);
}

void battle_list_on_intel(BattleList* list, const char* text) {
    if (list->ui.intel != nullptr)
        list->ui.intel(list->ui.user, text);
}

void battle_list_on_battle_intel(BattleList* list, const BattleIntel* intel) {
    set_activity(list, false);
    if (intel->failed)
        return;
    if (BattleEntry* b = entry_at(list, battle_list_find(list, intel->game_id)))
        battle_entry_set_intel(b, intel);
}

void battle_list_on_battle_report(BattleList* list, const BattleReport* report) {
    const int32_t battle = battle_list_find(list, report->game_id);
    BattleEntry* b = entry_at(list, battle);
    if (b == nullptr)
        return;
    if (report->message != nullptr && report->message[0] != '\0') {
        if (list->scoreboard_visible && list->scoreboard_battle == battle &&
            list->ui.report_message != nullptr)
            list->ui.report_message(list->ui.user, report->message);
        return;
    }
    battle_entry_add_report(b, report);
}

void battle_entry_set_intel(BattleEntry* b, const BattleIntel* intel) {
    b->host_user_id = intel->user_id;
    copy_chars(b->host_name, sizeof b->host_name, intel->user_name, kPlayerNameCharacters);
    b->has_password = intel->has_password;
    copy_bounded(b->mission, sizeof b->mission, intel->mission);
    b->report_count = 0;
    b->report_names[0] = '\0';
    b->skill = intel->skill;
    b->players = intel->players;
    b->on_death = intel->on_death;
}

void battle_entry_add_report(BattleEntry* b, const BattleReport* report) {
    BattlePlayer* row = nullptr;
    for (BattlePlayer& p : b->scores)
        if (row == nullptr && p.user_id == report->user_id)
            row = &p;
    for (BattlePlayer& p : b->scores)
        if (row == nullptr && p.user_id == 0) {
            row = &p;
            row->user_id = report->user_id;
        }
    if (row == nullptr)
        return;
    ++b->report_count;
    char* names = b->report_names;
    if (names[0] != '\0')
        append_bounded(names, sizeof b->report_names, kReportSeparator);
    const char* title = rank_title(report->icon, false);
    if (title[0] != '\0') {
        append_bounded(names, sizeof b->report_names, title);
        append_bounded(names, sizeof b->report_names, kTitleSeparator);
    }
    append_bounded(names, sizeof b->report_names, report->user_name);
    copy_chars(row->name, sizeof row->name, report->user_name, kPlayerNameCharacters);
    row->icon = report->icon;
    std::snprintf(row->kills, sizeof row->kills, "%d", report->kills);
    std::snprintf(row->losses, sizeof row->losses, "%d", report->losses);
    std::snprintf(row->metal, sizeof row->metal, "%d", report->metal);
    std::snprintf(row->energy, sizeof row->energy, "%d", report->energy);
    std::snprintf(row->score, sizeof row->score, "%d", report->score);
}

} // namespace oa::boneyards::screens

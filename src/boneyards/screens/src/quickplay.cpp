// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/screens/quickplay.hpp"

#include "oa/boneyards/shown_text.hpp"

#include <cstdio>
#include <cstring>

namespace oa::boneyards::screens {
namespace {

constexpr const char* kTypeNames[kQuickPlayTypeCount] = {
    "Duel",
    "Winner Takes All",
    "For Fun",
    "Galactic War",
    "War Training",
};

constexpr const char* kSkillNames[] = {"Novice", "Seasoned", "Expert"};

constexpr const char* kBlurbs[kQuickPlayTypeCount][kQuickPlayBlurbLines] = {
    {"Two commanders, one map,", "no allies. Ranked on the", "Duel ladder."},
    {"Three or four commanders,", "one winner. Ranked on the", "Winner Takes All ladder."},
    {"A battle that is not", "ranked. Any number of", "commanders may join."},
    {"Fight for the planet", "you selected. Only on a", "contested world."},
    {"Practice on a planet", "of your own side. The", "ladders do not count it."},
};

constexpr const char* kDefaultMap = "Lava Alley";

void copy_text(char* out, std::size_t capacity, const char* text) {
    if (out == nullptr || capacity == 0)
        return;
    std::size_t length = 0;
    if (text != nullptr)
        length = std::strlen(text);
    if (length >= capacity)
        length = capacity - 1;
    if (length > 0)
        std::memcpy(out, text, length);
    out[length] = '\0';
}

bool galactic_allowed(bool has_planet, char planet_side, char allegiance) {
    if (!has_planet)
        return false;
    if (planet_side != 'I' && planet_side != 'i')
        return false;
    return allegiance != 'U';
}

bool training_allowed(bool has_planet, char planet_side, char allegiance) {
    if (!has_planet)
        return false;
    if ((planet_side == 'A' || planet_side == 'a') && allegiance == 'A')
        return true;
    return (planet_side == 'C' || planet_side == 'c') && allegiance == 'C';
}

int32_t wire_game_type(int32_t index) {
    switch (index) {
    case kQuickPlayForFun:
        return 0;
    case kQuickPlayDuel:
        return 1;
    case kQuickPlayWinnerTakesAll:
        return 2;
    case kQuickPlayGalacticWar:
        return 4;
    case kQuickPlayWarTraining:
        return 7;
    default:
        return -1;
    }
}

const char* wire_type_name(int32_t game_type) {
    switch (game_type) {
    case 0:
        return "For Fun";
    case 1:
        return "Duel";
    case 2:
        return "Winner Takes All";
    case 4:
        return "Galactic War";
    case 7:
        return "War Training";
    default:
        return "";
    }
}

const char* skill_word(int32_t skill) {
    if (skill < 0 || skill > static_cast<int32_t>(QuickPlaySkill::expert))
        return "";
    return quickplay_skill_name(static_cast<QuickPlaySkill>(skill));
}

int32_t clamped_meter(int32_t meter) {
    if (meter < 0)
        return 0;
    if (meter > kQuickPlayMeterLast)
        return kQuickPlayMeterLast;
    return meter;
}

int32_t results_due(const QuickPlay* play) {
    if (play->elapsed_ms <= 0)
        return 0;
    const int32_t due = play->elapsed_ms / kQuickPlayLineIntervalMs;
    const int32_t stored = play->scan_dropped + play->scan_count;
    return due < stored ? due : stored;
}

void clear_offer(QuickPlay* play) {
    play->offer_open = false;
    play->offer_id[0] = '\0';
    play->offer_name[0] = '\0';
    play->offer_planet[0] = '\0';
    play->offer_game_id[0] = '\0';
    play->offer_game_known = false;
    play->offer_role = 0;
    play->offer_game_type = 0;
    play->offer_skill = 0;
    play->offer_seats = 0;
    play->offer_on_death = 0;
    play->offer_area = 0;
    play->offer_map[0] = '\0';
    play->offer_map_known = false;
}

void clear_scans(QuickPlay* play) {
    play->last_seq = 0;
    play->scan_count = 0;
    play->scan_dropped = 0;
}

void stop_search(QuickPlay* play) {
    play->searching = false;
    play->launch_pending = false;
    play->elapsed_ms = 0;
    play->awaiting_status = false;
    play->has_status = false;
    play->meter = kQuickPlayMeterFrame;
    play->planets = 0;
    play->fits = 0;
    play->declined_game_id[0] = '\0';
    play->withdrawn_status[0] = '\0';
    clear_scans(play);
    clear_offer(play);
}

int32_t withdrawn_notes(const QuickPlay* play) {
    return play->withdrawn_status[0] != '\0' ? 2 : 0;
}

int32_t result_room(const QuickPlay* play) {
    const int32_t room = kQuickPlayScanCapacity - 1 - withdrawn_notes(play);
    return room > 0 ? room : 0;
}

void remember_declined(QuickPlay* play) {
    if (!play->offer_game_known || play->offer_game_id[0] == '\0')
        return;
    copy_text(play->declined_game_id, sizeof play->declined_game_id, play->offer_game_id);
}

bool declined_battle(const QuickPlay* play, const QuickPlayFoundNotice* found) {
    if (found->role != 0 || play->declined_game_id[0] == '\0' || found->game_id[0] == '\0')
        return false;
    return std::strcmp(play->declined_game_id, found->game_id) == 0;
}

bool make_scan_room(QuickPlay* play) {
    if (play->scan_count < kQuickPlayScanStore)
        return true;
    const int32_t revealed = results_due(play);
    const int32_t keep_from =
        revealed > kQuickPlayResultLines ? revealed - kQuickPlayResultLines : 0;
    int32_t drop = keep_from - play->scan_dropped;
    if (drop <= 0)
        return false;
    if (drop > play->scan_count)
        drop = play->scan_count;
    const int32_t kept = play->scan_count - drop;
    if (kept > 0) {
        std::memmove(
            play->scans, play->scans + drop, sizeof(QuickPlayScan) * static_cast<std::size_t>(kept)
        );
    }
    play->scan_count = kept;
    play->scan_dropped += drop;
    return play->scan_count < kQuickPlayScanStore;
}

void compose_scan(const QuickPlayScan& scan, char* out, std::size_t capacity) {
    char pattern[96];
    char type_name[48];
    char skill_name[32];
    copy_shown(type_name, sizeof type_name, wire_type_name(scan.game_type));
    copy_shown(skill_name, sizeof skill_name, skill_word(scan.skill));
    if (scan.kind == 1) {
        copy_shown_pattern(pattern, sizeof pattern, "%s: no forming battles");
        std::snprintf(out, capacity, pattern, scan.planet);
        return;
    }
    if (scan.kind == 3) {
        copy_shown_pattern(pattern, sizeof pattern, "%s: %s, %d of %d seats, %s");
        std::snprintf(
            out, capacity, pattern, scan.planet, scan.battle, scan.seats, scan.seats_max, skill_name
        );
        return;
    }
    if (scan.reason == 0) {
        copy_shown_pattern(pattern, sizeof pattern, "%s: %s wants %s - skipped");
        std::snprintf(out, capacity, pattern, scan.planet, scan.battle, skill_name);
        return;
    }
    if (scan.reason == 1) {
        copy_shown_pattern(pattern, sizeof pattern, "%s: %s is %s - skipped");
        std::snprintf(out, capacity, pattern, scan.planet, scan.battle, type_name);
        return;
    }
    copy_shown_pattern(pattern, sizeof pattern, "%s: %s - skipped");
    std::snprintf(out, capacity, pattern, scan.planet, scan.battle);
}

} // namespace

void quickplay_init(QuickPlay* play) {
    if (play == nullptr)
        return;
    play->selected = kQuickPlayDuel;
    play->where = QuickPlayWhere::any_planet;
    play->skill = QuickPlaySkill::seasoned;
    play->side = QuickPlaySide::either;
    play->prefer_favorite = true;
    copy_text(play->favorite, sizeof play->favorite, kDefaultMap);
    play->requests_sent = 0;
    stop_search(play);
    quickplay_set_place(play, false, 0, 'U');
}

void quickplay_set_place(QuickPlay* play, bool has_planet, char planet_side, char allegiance) {
    if (play == nullptr)
        return;
    play->allowed[kQuickPlayDuel] = true;
    play->allowed[kQuickPlayWinnerTakesAll] = true;
    play->allowed[kQuickPlayForFun] = true;
    play->allowed[kQuickPlayGalacticWar] = galactic_allowed(has_planet, planet_side, allegiance);
    play->allowed[kQuickPlayWarTraining] = training_allowed(has_planet, planet_side, allegiance);
    if (play->selected < 0 || play->selected >= kQuickPlayTypeCount ||
        !play->allowed[play->selected])
        play->selected = kQuickPlayDuel;
}

void quickplay_set_favorite(QuickPlay* play, const char* map) {
    if (play == nullptr)
        return;
    if (map == nullptr || map[0] == '\0')
        copy_text(play->favorite, sizeof play->favorite, kDefaultMap);
    else
        copy_text(play->favorite, sizeof play->favorite, map);
}

bool quickplay_type_allowed(const QuickPlay* play, int32_t index) {
    if (play == nullptr || index < 0 || index >= kQuickPlayTypeCount)
        return false;
    return play->allowed[index];
}

bool quickplay_select_type(QuickPlay* play, int32_t index) {
    if (!quickplay_type_allowed(play, index))
        return false;
    play->selected = index;
    return true;
}

const char* quickplay_type_name(int32_t index) {
    if (index < 0 || index >= kQuickPlayTypeCount)
        return "";
    return kTypeNames[index];
}

const char* quickplay_skill_name(QuickPlaySkill skill) {
    const int32_t index = static_cast<int32_t>(skill);
    if (index < 0 || index >= static_cast<int32_t>(sizeof kSkillNames / sizeof kSkillNames[0]))
        return "";
    return kSkillNames[index];
}

const char* quickplay_blurb(int32_t index, int32_t line) {
    if (index < 0 || index >= kQuickPlayTypeCount || line < 0 || line >= kQuickPlayBlurbLines)
        return "";
    return kBlurbs[index][line];
}

void quickplay_favorite_caption(const QuickPlay* play, char* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0)
        return;
    const char* map = play != nullptr && play->favorite[0] != '\0' ? play->favorite : kDefaultMap;
    char pattern[64];
    copy_shown_pattern(pattern, sizeof pattern, "Prefer my favorite: %s");
    std::snprintf(out, capacity, pattern, map);
}

void quickplay_find(QuickPlay* play) {
    if (play == nullptr || play->searching)
        return;
    const int32_t sent = play->requests_sent;
    stop_search(play);
    play->requests_sent = sent;
    play->searching = true;
    play->awaiting_status = true;
}

void quickplay_cancel(QuickPlay* play) {
    if (play == nullptr)
        return;
    const int32_t sent = play->requests_sent;
    stop_search(play);
    play->requests_sent = sent;
}

void quickplay_advance(QuickPlay* play, int32_t ms) {
    if (play == nullptr || !play->searching || ms <= 0)
        return;
    if (play->elapsed_ms > INT32_MAX - ms)
        play->elapsed_ms = INT32_MAX;
    else
        play->elapsed_ms += ms;
}

int32_t quickplay_scanner_frame(const QuickPlay* play) {
    if (play == nullptr || !play->searching || play->elapsed_ms <= 0)
        return 0;
    return (play->elapsed_ms / kQuickPlayScannerMs) % kQuickPlayScannerFrames;
}

int32_t quickplay_visible_lines(const QuickPlay* play) {
    if (play == nullptr || !play->searching)
        return 0;
    int32_t shown = results_due(play);
    if (shown > kQuickPlayResultLines)
        shown = kQuickPlayResultLines;
    const int32_t room = result_room(play);
    if (shown > room)
        shown = room;
    return 1 + withdrawn_notes(play) + shown;
}

void quickplay_scan_line(const QuickPlay* play, int32_t index, char* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0)
        return;
    out[0] = '\0';
    if (play == nullptr || index < 0 || index >= quickplay_visible_lines(play))
        return;
    if (index == 0) {
        char pattern[128];
        char type_name[48];
        char skill_name[32];
        const bool fuel = play->where == QuickPlayWhere::fuel_dump;
        copy_shown_pattern(
            pattern,
            sizeof pattern,
            fuel ? "Scanning Fuel Dump for a %s at %s skill..."
                 : "Scanning %d planets for a %s at %s skill..."
        );
        copy_shown(type_name, sizeof type_name, quickplay_type_name(play->selected));
        copy_shown(skill_name, sizeof skill_name, quickplay_skill_name(play->skill));
        const int32_t planets = play->planets > 0 ? play->planets : 0;
        if (fuel)
            std::snprintf(out, capacity, pattern, type_name, skill_name);
        else
            std::snprintf(out, capacity, pattern, planets, type_name, skill_name);
        return;
    }
    const int32_t notes = withdrawn_notes(play);
    if (index <= notes) {
        copy_shown(
            out, capacity, index == 1 ? play->withdrawn_status : quickplay_searching_again_text()
        );
        return;
    }
    const int32_t result_index = index - notes;
    const int32_t revealed = results_due(play);
    const int32_t window = revealed > kQuickPlayResultLines ? revealed - kQuickPlayResultLines : 0;
    const int32_t window_lines =
        revealed > kQuickPlayResultLines ? kQuickPlayResultLines : revealed;
    const int32_t room = result_room(play);
    const int32_t shown_results = window_lines < room ? window_lines : room;
    const int32_t skipped = window_lines - shown_results;
    const int32_t logical = window + skipped + (result_index - 1);
    if (logical < play->scan_dropped || logical >= revealed)
        return;
    const int32_t at = logical - play->scan_dropped;
    if (at < 0 || at >= play->scan_count)
        return;
    compose_scan(play->scans[at], out, capacity);
}

void quickplay_timer(const QuickPlay* play, char* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0)
        return;
    int32_t seconds = 0;
    if (play != nullptr && play->elapsed_ms > 0)
        seconds = play->elapsed_ms / 1000;
    std::snprintf(out, capacity, "%d:%02d", seconds / 60, seconds % 60);
}

int32_t quickplay_meter_frame(const QuickPlay* play) {
    if (play == nullptr || !play->has_status)
        return kQuickPlayMeterFrame;
    return clamped_meter(play->meter);
}

void quickplay_query(
    const QuickPlay* play, uint32_t area_id, bool area_known, QuickPlayQuery* out
) {
    if (out == nullptr)
        return;
    *out = {};
    if (play == nullptr)
        return;
    out->game_type = wire_game_type(play->selected);
    out->where = static_cast<int32_t>(play->where);
    out->skill = static_cast<int32_t>(play->skill);
    out->side = static_cast<int32_t>(play->side);
    out->prefer_favorite = play->prefer_favorite ? 1 : 0;
    if (play->prefer_favorite)
        copy_text(out->favorite, sizeof out->favorite, play->favorite);
    out->area_id = area_id;
    out->area_known = area_known;
}

const char* quickplay_result_text(int32_t result) {
    switch (result) {
    case 1:
        return "QuickPlay is not open for this account.";
    case 2:
        return "That battle is not allowed on this planet.";
    case 4:
        return "You are already in a battle.";
    case 5:
        return "That battle has filled.";
    case 6:
        return "Fuel Dump is not in this sector.";
    case 7:
        return "The queue dropped this search.";
    case 8:
        return "Those settings cannot be queued.";
    default:
        return nullptr;
    }
}

const char* quickplay_unavailable_text() {
    return "The search could not be started.";
}

const char* quickplay_withdrawn_text(int32_t reason) {
    if (reason == 2)
        return "That battle was abandoned.";
    return "That battle has filled.";
}

const char* quickplay_searching_again_text() {
    return "Searching again…";
}

bool quickplay_status_overdue(const QuickPlay* play) {
    return play != nullptr && play->searching && play->awaiting_status &&
           play->elapsed_ms >= kQuickPlayStatusWaitMs;
}

QuickPlayEffect quickplay_apply_status(QuickPlay* play, const QuickPlayStatusNotice* status) {
    QuickPlayEffect effect{};
    if (play == nullptr || status == nullptr)
        return effect;
    if (!play->searching && !play->awaiting_status && !play->launch_pending)
        return effect;
    if (play->launch_pending) {
        // The battle Yes asked for was refused, and the search goes on.
        play->launch_pending = false;
        play->searching = true;
    }
    play->awaiting_status = false;
    play->has_status = true;
    play->meter = clamped_meter(status->meter);
    play->fits = status->fits;
    if (status->queued != 0)
        play->planets = status->planets > 0 ? status->planets : 0;
    if (status->result == 5) {
        play->searching = true;
        remember_declined(play);
        play->offer_open = false;
        effect.close_offer = true;
        effect.show_box = true;
        effect.box_result = 5;
        return effect;
    }
    if (status->queued != 0) {
        play->searching = true;
        return effect;
    }
    const bool was_searching = play->searching;
    if (was_searching || status->result != 0) {
        const int32_t sent = play->requests_sent;
        stop_search(play);
        play->requests_sent = sent;
        effect.leave_search = true;
        effect.close_offer = true;
        if (status->result != 0) {
            effect.show_box = true;
            effect.box_result = status->result;
        }
    }
    return effect;
}

void quickplay_apply_scan(QuickPlay* play, const QuickPlayScanNotice* scan) {
    if (play == nullptr || scan == nullptr || (!play->searching && !play->launch_pending))
        return;
    if (scan->seq <= play->last_seq)
        return;
    if (!make_scan_room(play))
        return;
    QuickPlayScan& stored = play->scans[play->scan_count];
    stored = {};
    stored.seq = scan->seq;
    stored.kind = scan->kind;
    stored.reason = scan->reason;
    stored.game_type = scan->game_type;
    stored.skill = scan->skill;
    stored.seats = scan->seats;
    stored.seats_max = scan->seats_max;
    copy_text(stored.planet, sizeof stored.planet, scan->planet);
    copy_text(stored.battle, sizeof stored.battle, scan->battle);
    ++play->scan_count;
    play->last_seq = scan->seq;
}

QuickPlayEffect quickplay_apply_found(QuickPlay* play, const QuickPlayFoundNotice* found) {
    QuickPlayEffect effect{};
    if (play == nullptr || found == nullptr || !play->searching)
        return effect;
    if (declined_battle(play, found))
        return effect;
    copy_text(play->offer_id, sizeof play->offer_id, found->offer_id);
    copy_text(play->offer_name, sizeof play->offer_name, found->name);
    copy_text(play->offer_planet, sizeof play->offer_planet, found->planet);
    copy_text(play->offer_game_id, sizeof play->offer_game_id, found->game_id);
    play->offer_game_known = found->game_known && found->game_id[0] != '\0';
    play->offer_role = found->role;
    play->offer_game_type = found->game_type;
    play->offer_skill = found->skill;
    play->offer_seats = found->seats;
    play->offer_on_death = found->on_death;
    play->offer_area = found->area_id;
    copy_text(play->offer_map, sizeof play->offer_map, found->map);
    play->offer_map_known = found->map_known && found->map[0] != '\0';
    play->offer_open = true;
    effect.show_offer = true;
    return effect;
}

QuickPlayEffect
quickplay_apply_withdrawn(QuickPlay* play, const QuickPlayWithdrawnNotice* withdrawn) {
    QuickPlayEffect effect{};
    if (play == nullptr || withdrawn == nullptr || !play->searching || !play->offer_open)
        return effect;
    if (withdrawn->offer_id[0] == '\0' || std::strcmp(play->offer_id, withdrawn->offer_id) != 0)
        return effect;
    copy_text(
        play->withdrawn_status,
        sizeof play->withdrawn_status,
        quickplay_withdrawn_text(withdrawn->reason)
    );
    clear_offer(play);
    effect.close_offer = true;
    return effect;
}

QuickPlayEffect quickplay_decide(QuickPlay* play, int32_t accept) {
    QuickPlayEffect effect{};
    if (play == nullptr || !play->offer_open)
        return effect;
    effect.close_offer = true;
    play->offer_open = false;
    if (accept == 0) {
        if (play->offer_role == 0)
            remember_declined(play);
        return effect;
    }
    if (play->offer_role == 1)
        effect.launch_host = true;
    else if (play->offer_game_known)
        effect.launch_join = true;
    if (effect.launch_host || effect.launch_join) {
        // The battle ends the search; the offer stays for the launch to read.
        play->searching = false;
        play->awaiting_status = false;
        play->launch_pending = true;
        effect.leave_search = true;
    }
    return effect;
}

void quickplay_offer_line(const QuickPlay* play, char* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0)
        return;
    out[0] = '\0';
    if (play == nullptr)
        return;
    char pattern[32];
    char type_name[48];
    char second[96];
    char third[96];
    copy_shown_pattern(pattern, sizeof pattern, "%s on %s");
    copy_shown(type_name, sizeof type_name, wire_type_name(play->offer_game_type));
    std::snprintf(second, sizeof second, pattern, type_name, play->offer_planet);
    copy_shown(
        third, sizeof third, play->offer_role == 1 ? "No games available, host one?" : "Join now?"
    );
    std::snprintf(out, capacity, "%s\n%s\n%s", play->offer_name, second, third);
}

int32_t quickplay_players_radio(int32_t seats) {
    switch (seats) {
    case 3:
        return 1;
    case 4:
        return 2;
    default:
        return 0;
    }
}

bool quickplay_host_request(const QuickPlay* play, HostGameRequest* out) {
    if (play == nullptr || out == nullptr || play->offer_role != 1)
        return false;
    out->area_id = play->offer_area;
    out->game_mode = play->offer_game_type;
    out->name = play->offer_name;
    out->password = nullptr;
    out->skill = play->offer_skill;
    out->players = quickplay_players_radio(play->offer_seats);
    out->commander_death = play->offer_on_death;
    return true;
}

} // namespace oa::boneyards::screens

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Matchmaking choices and the queue. The elapsed time, the scanner and the
// search lines stay here. The dialog sends the records and applies what
// comes back through matchmaking_apply_status, matchmaking_apply_found and
// matchmaking_apply_go.
#include "oa/boneyards/screens/matchmaking.hpp"

#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/screens/after_battle.hpp"
#include "oa/boneyards/screens/profile.hpp"
#include "oa/boneyards/shown_text.hpp"

#include <cstdio>

namespace oa::boneyards::screens {
namespace {

constexpr int32_t k_pulses_per_second = 30;
constexpr int32_t k_scanner_step = 3; // about ten steps a second
constexpr int32_t k_scanner_frames = 8;

const char* game_name(MatchGame game) {
    switch (game) {
    case MatchGame::winner_takes_all:
        return "Winner Takes All";
    case MatchGame::for_fun:
        return "For Fun";
    case MatchGame::duel:
        return "Duel";
    }
    return "Duel";
}

const char* skill_phrase(MatchSkill skill) {
    return skill == MatchSkill::any ? "at any skill" : "near Seasoned";
}

const char* side_phrase(MatchSide side) {
    switch (side) {
    case MatchSide::arm:
        return "as Arm";
    case MatchSide::either:
        return "either side";
    case MatchSide::core:
        return "as Core";
    }
    return "as Core";
}

int32_t wire_game(MatchGame game) {
    switch (game) {
    case MatchGame::for_fun:
        return static_cast<int32_t>(GameMode::for_fun);
    case MatchGame::winner_takes_all:
        return static_cast<int32_t>(GameMode::winner_takes_all);
    case MatchGame::duel:
        return static_cast<int32_t>(GameMode::duel);
    }
    return static_cast<int32_t>(GameMode::duel);
}

const char* type_word(int32_t game_type) {
    switch (game_type) {
    case static_cast<int32_t>(GameMode::for_fun):
        return "For Fun";
    case static_cast<int32_t>(GameMode::winner_takes_all):
        return "Winner Takes All";
    case static_cast<int32_t>(GameMode::galactic_war):
        return "Galactic War";
    case static_cast<int32_t>(GameMode::war_training):
        return "War Training";
    default:
        return "Duel";
    }
}

const char* skill_word(int32_t skill) {
    switch (skill) {
    case 0:
        return "Novice";
    case 2:
        return "Expert";
    default:
        return "Seasoned";
    }
}

void copy_text(char* out, std::size_t capacity, const char* text) {
    if (out == nullptr || capacity == 0)
        return;
    std::snprintf(out, capacity, "%s", text != nullptr ? text : "");
}

int32_t clamped_meter(int32_t meter) {
    if (meter < 0)
        return 0;
    if (meter > k_matchmaking_meter_last)
        return k_matchmaking_meter_last;
    return meter;
}

void leave_queue(MatchmakingChoices* choices) {
    choices->queued = false;
    choices->elapsed_pulses = 0;
    choices->offer_open = false;
    choices->awaiting_status = false;
    choices->status_wait = 0;
    choices->look_wait = 0;
}

} // namespace

void matchmaking_setup(MatchmakingChoices* choices) {
    if (choices == nullptr)
        return;
    *choices = MatchmakingChoices{};
}

void matchmaking_queue_activate(MatchmakingChoices* choices) {
    if (choices == nullptr || choices->queued)
        return;
    choices->queued = true;
    choices->look_wait = 0;
}

void matchmaking_queue_cancel(MatchmakingChoices* choices) {
    if (choices == nullptr)
        return;
    leave_queue(choices);
}

void matchmaking_queue_pulse(MatchmakingChoices* choices) {
    if (choices != nullptr && choices->queued)
        ++choices->elapsed_pulses;
}

void matchmaking_query(
    const MatchmakingChoices* choices, uint32_t area_id, bool area_known, MatchmakingQuery* out
) {
    if (choices == nullptr || out == nullptr)
        return;
    *out = MatchmakingQuery{};
    out->game_type = wire_game(choices->game);
    out->skill_mode = static_cast<int32_t>(choices->skill);
    out->side = static_cast<int32_t>(choices->side);
    out->planets = static_cast<int32_t>(choices->planets);
    out->area_id = area_id;
    out->area_known = area_known && choices->planets == MatchPlanets::this_planet;
}

int32_t matchmaking_scanner_frame(const MatchmakingChoices* choices) {
    if (choices == nullptr || !choices->queued)
        return 0;
    return (choices->elapsed_pulses / k_scanner_step) % k_scanner_frames;
}

int32_t matchmaking_queue_count(const MatchmakingChoices* choices) {
    return choices != nullptr ? choices->queue_count : 0;
}

int32_t matchmaking_meter_frame(const MatchmakingChoices* choices) {
    return choices != nullptr ? clamped_meter(choices->meter) : 0;
}

void matchmaking_queue_line(
    const MatchmakingChoices* choices, const char* planet, char* out, std::size_t capacity
) {
    if (choices == nullptr || out == nullptr || capacity == 0)
        return;
    if (choices->planets == MatchPlanets::any_planet) {
        copy_shown(out, capacity, "Commanders queued on any planet");
        return;
    }
    const char* name = planet != nullptr && planet[0] != '\0' ? planet : "Fuel Dump";
    char pattern[96];
    copy_shown_pattern(pattern, sizeof pattern, "Commanders queued on %s");
    std::snprintf(out, capacity, pattern, name);
}

void matchmaking_detail_line(const MatchmakingChoices* choices, char* out, std::size_t capacity) {
    if (choices == nullptr || out == nullptr || capacity == 0)
        return;
    char pattern[96];
    char game[48];
    char skill[48];
    char side[48];
    copy_shown_pattern(pattern, sizeof pattern, "Looking for a %s %s, %s.");
    copy_shown(game, sizeof game, game_name(choices->game));
    copy_shown(skill, sizeof skill, skill_phrase(choices->skill));
    copy_shown(side, sizeof side, side_phrase(choices->side));
    std::snprintf(out, capacity, pattern, game, skill, side);
}

void matchmaking_search_line(const MatchmakingChoices* choices, char* out, std::size_t capacity) {
    if (choices == nullptr || out == nullptr || capacity == 0)
        return;
    const int32_t seconds = choices->elapsed_pulses / k_pulses_per_second;
    char pattern[48];
    copy_shown_pattern(pattern, sizeof pattern, "SEARCHING  %d:%02d");
    std::snprintf(out, capacity, pattern, seconds / 60, seconds % 60);
}

void matchmaking_offer_line(const MatchmakingChoices* choices, char* out, std::size_t capacity) {
    if (out == nullptr || capacity == 0)
        return;
    out[0] = '\0';
    if (choices == nullptr)
        return;
    char found[96];
    char name_pattern[32];
    char join_pattern[96];
    char skill[32];
    char game[48];
    char named[96];
    char join[96];
    copy_shown(found, sizeof found, "Opponent found:");
    copy_shown_pattern(name_pattern, sizeof name_pattern, "%s (%s)");
    copy_shown_pattern(join_pattern, sizeof join_pattern, "Join the %s?");
    copy_shown(skill, sizeof skill, skill_word(choices->offer_skill));
    copy_shown(game, sizeof game, type_word(choices->offer_game_type));
    std::snprintf(named, sizeof named, name_pattern, choices->offer_name, skill);
    std::snprintf(join, sizeof join, join_pattern, game);
    std::snprintf(out, capacity, "%s\n%s\n%s", found, named, join);
}

const char* matchmaking_result_text(int32_t result) {
    switch (result) {
    case 1:
        return "Matchmaking is not open for this account.";
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

const char* matchmaking_unavailable_text() {
    return "The queue is unavailable.";
}

MatchmakingEffect
matchmaking_apply_status(MatchmakingChoices* choices, const MatchmakingStatusNotice* status) {
    MatchmakingEffect effect{};
    if (choices == nullptr || status == nullptr)
        return effect;
    choices->queue_count = status->count > 0 ? status->count : 0;
    choices->meter = clamped_meter(status->meter);
    choices->awaiting_status = false;
    choices->status_wait = 0;
    if (status->result == 5) {
        choices->queued = status->queued != 0;
        effect.show_box = true;
        effect.box_result = 5;
        return effect;
    }
    if (status->queued != 0) {
        choices->queued = true;
        return effect;
    }
    const bool was_queued = choices->queued;
    if (was_queued || status->result != 0) {
        leave_queue(choices);
        effect.close_offer = true;
        if (was_queued || status->result != 0)
            effect.face = MatchmakingFaceChange::settings;
        if (status->result != 0) {
            effect.show_box = true;
            effect.box_result = status->result;
        }
    }
    return effect;
}

MatchmakingEffect
matchmaking_apply_found(MatchmakingChoices* choices, const MatchmakingFoundNotice* found) {
    MatchmakingEffect effect{};
    if (choices == nullptr || found == nullptr)
        return effect;
    copy_text(choices->offer_id, sizeof choices->offer_id, found->offer_id);
    copy_text(choices->offer_name, sizeof choices->offer_name, found->name);
    choices->offer_skill = found->skill;
    choices->offer_game_type = found->game_type;
    choices->offer_role = found->role;
    choices->offer_side = found->side;
    choices->offer_area = found->area_id;
    choices->assigned_side = found->side;
    choices->side_known = true;
    effect.face = MatchmakingFaceChange::searching;
    effect.show_offer = true;
    return effect;
}

MatchmakingEffect matchmaking_apply_go(MatchmakingChoices* choices, const MatchmakingGoNotice* go) {
    MatchmakingEffect effect{};
    if (choices == nullptr || go == nullptr)
        return effect;
    effect.close_offer = true;
    choices->offer_open = false;
    if (go->side_known) {
        choices->assigned_side = go->side;
        choices->side_known = true;
    }
    if (go->result == 1) {
        choices->queued = true;
        effect.face = MatchmakingFaceChange::searching;
        return effect;
    }
    if (go->result != 0)
        return effect;
    copy_text(choices->offer_id, sizeof choices->offer_id, go->offer_id);
    if (go->game_known)
        copy_text(choices->game_id, sizeof choices->game_id, go->game_id);
    if (go->role == 1)
        effect.launch_host = true;
    else
        effect.launch_join = true;
    arm_after_battle(go->game_type);
    return effect;
}

int32_t matchmaking_players_radio(int32_t seats) {
    switch (seats) {
    case 3:
        return 1;
    case 4:
        return 2;
    default:
        return 0;
    }
}

bool matchmaking_host_request(const MatchmakingGoNotice* go, HostGameRequest* out) {
    if (go == nullptr || out == nullptr)
        return false;
    out->area_id = go->area_id;
    out->game_mode = go->game_type;
    out->name = go->name_known ? go->name : "";
    out->password = nullptr;
    out->skill = go->skill;
    out->players = matchmaking_players_radio(go->seats);
    out->commander_death = go->on_death;
    arm_after_battle(go->game_type);
    return true;
}

MessageBox* matchmaking_offer_create(const MatchmakingChoices* choices, Window* focus_return) {
    char text[192];
    matchmaking_offer_line(choices, text, sizeof text);
    return message_box_create(text, message_box_buttons::yes_no, nullptr, nullptr, focus_return);
}

} // namespace oa::boneyards::screens

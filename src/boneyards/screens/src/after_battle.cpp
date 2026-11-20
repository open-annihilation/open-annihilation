// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/boneyards/screens/after_battle.hpp"

#include "oa/boneyards/shown_text.hpp"
#include "oa/ui/screen_registry.hpp"
#include "oa/ui/screen_registry/kit.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace oa::boneyards::screens {
namespace {

struct AfterBattle {
    std::optional<int32_t> armed{};
    std::optional<int32_t> noted{};
    // The wire type of the notice or prompt last queued. For Fun is 0.
    std::optional<int32_t> shown_wire{};
    bool played = false;
    // A launch starts recorded. finished and left say otherwise.
    bool recorded = true;
    bool share_wanted = false;
    int shown = 0;
};

AfterBattle g_after;

void copy_line(char* out, std::size_t capacity, const char* text) {
    if (out == nullptr || capacity == 0)
        return;
    std::size_t written = 0;
    if (text != nullptr) {
        while (text[written] != '\0' && written + 1 < capacity) {
            out[written] = text[written];
            ++written;
        }
    }
    out[written] = '\0';
}

void on_share_answered(void* /*user*/, uint32_t /*id*/, int32_t button) {
    if (button == 0)
        g_after.share_wanted = true;
}

} // namespace

void arm_after_battle(int32_t wire_game_type) {
    g_after.armed = wire_game_type;
}

void begin_after_battle() {
    g_after.noted = g_after.armed;
    g_after.armed.reset();
    g_after.played = false;
    g_after.recorded = true;
}

void forget_after_battle() {
    g_after.armed.reset();
    g_after.noted.reset();
    g_after.played = false;
    g_after.recorded = true;
}

void mark_after_battle_played() {
    if (g_after.noted.has_value())
        g_after.played = true;
}

void mark_after_battle_recorded(bool kept) {
    g_after.recorded = kept;
}

int after_battle_shown() {
    return g_after.shown;
}

const char* after_battle_picture_name() {
    if (g_after.shown_wire == 0)
        return "notice-share";
    return "notice-ranked";
}

bool take_after_battle_share() {
    const bool wanted = g_after.share_wanted;
    g_after.share_wanted = false;
    return wanted;
}

std::string_view after_battle_sentence(int32_t wire_game_type) {
    switch (wire_game_type) {
    case 0:
        return kAfterBattleShare;
    case 1:
    case 2:
    case 4:
    case 7:
        return kAfterBattleRanked;
    default:
        return {};
    }
}

void show_after_battle_notice(const oa::app::ScreenServices* services, void* host) {
    if (!g_after.played) {
        forget_after_battle();
        return;
    }
    const std::optional<int32_t> wire = g_after.noted;
    const bool recorded = g_after.recorded;
    g_after.noted.reset();
    g_after.played = false;
    g_after.recorded = true;
    if (!wire.has_value() || services == nullptr)
        return;
    const bool for_fun = *wire == 0;
    const std::string_view sentence = for_fun && !recorded ? kAfterBattleUnrecorded
                                      : for_fun            ? kAfterBattleShare
                                                           : after_battle_sentence(*wire);
    if (sentence.empty())
        return;
    // shown_text keeps one buffer. Copy each line out before the next.
    static char title[256];
    static char body[256];
    copy_line(title, sizeof title, oa::boneyards::shown_text(kAfterBattleTitle.data()));
    copy_line(body, sizeof body, oa::boneyards::shown_text(sentence.data()));
    const char* paragraphs[] = {body};
    // An unrecorded For Fun battle is a statement, not the share question.
    // The picture name stays the notice, not notice-share.
    if (for_fun && !recorded) {
        if (services->show_notice == nullptr)
            return;
        oa::app::NoticeRequest request;
        request.title = title;
        request.paragraphs = paragraphs;
        request.paragraph_count = 1;
        const uint32_t id = services->show_notice(host, &request);
        if (id != 0) {
            g_after.shown_wire.reset();
            ++g_after.shown;
        }
        return;
    }
    if (for_fun) {
        if (services->ask_prompt == nullptr)
            return;
        static char share_caption[64];
        static char later_caption[64];
        copy_line(
            share_caption,
            sizeof share_caption,
            oa::boneyards::shown_text(kAfterBattleShareButton.data())
        );
        copy_line(
            later_caption, sizeof later_caption, oa::boneyards::shown_text(kAfterBattleLater.data())
        );
        const char* buttons[] = {share_caption, later_caption};
        const char* button_ids[] = {"share", "not-now"};
        oa::app::PromptRequest request;
        request.title = title;
        request.paragraphs = paragraphs;
        request.paragraph_count = 1;
        request.buttons = buttons;
        request.button_count = 2;
        request.button_ids = button_ids;
        request.cancel_button = 1;
        request.primary_button = 0;
        request.answered = on_share_answered;
        const uint32_t id = services->ask_prompt(host, &request);
        if (id != 0) {
            g_after.shown_wire = *wire;
            ++g_after.shown;
        }
        return;
    }
    if (services->show_notice == nullptr)
        return;
    oa::app::NoticeRequest request;
    request.title = title;
    request.paragraphs = paragraphs;
    request.paragraph_count = 1;
    const uint32_t id = services->show_notice(host, &request);
    if (id != 0) {
        g_after.shown_wire = *wire;
        ++g_after.shown;
    }
}

} // namespace oa::boneyards::screens

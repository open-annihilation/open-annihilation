// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/hud/kill_board.hpp"

#include "oa/present/colour_vision/colour_vision.hpp"
#include "oa/ui/hud/game_fields.hpp"
#include "oa/ui/hud/share_panel.hpp"

#include <cstdio>

namespace oa::ui::hud {
namespace {

constexpr uint8_t kFlashFade = 2;
constexpr int32_t kBoardMargin = 6;
constexpr int32_t kFirstRowBottom = kBoardTop + 0x34;
constexpr int32_t kLocalHighlight = kLocalRowLightLevel;
constexpr int32_t kLocalHighlightInner = kLocalRowInnerLightLevel;
constexpr int32_t kBoardShade = kBoardShadeLevel;

bool on_board(World& world, const Player& player) noexcept {
    if (!player_participating(player))
        return false;
    if (player.in_use == 0)
        return true;
    const auto* info = world_player_info(&world, &player);
    return info == nullptr || (info->options & OA_SETUP_OPTION_WATCHER) == 0;
}

const char* localize(const KillBoardSink& sink, const char* text) {
    const char* out = sink.localize != nullptr ? sink.localize(sink.user, text) : nullptr;
    return out != nullptr ? out : text;
}

int32_t width_of(const KillBoardSink& sink, const char* text) {
    return sink.text_width != nullptr ? sink.text_width(sink.user, text) : 0;
}

void text(
    const KillBoardSink& sink, const char* value, int32_t x, int32_t y, int32_t width, uint8_t flash
) {
    if (sink.text != nullptr)
        sink.text(sink.user, value, x, y, width, flash);
}

void shade(
    const KillBoardSink& sink, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t level
) {
    if (sink.shade != nullptr)
        sink.shade(sink.user, x0, y0, x1, y1, level);
}

void play(const KillBoardSink& sink, const char* name) {
    if (sink.play_sound != nullptr)
        sink.play_sound(sink.user, name);
}

/// Moves the slide a quarter of the way to its target; true when the board
/// has nothing to draw.
bool advance_slide(KillBoard& board, bool wanted, const KillBoardSink& sink) {
    if (!wanted) {
        if (board.slide < 1)
            return true;
        if (board.slide == kBoardWidth)
            play(sink, "Panel");
        const int32_t step = board.slide / 4 < 2 ? 1 : board.slide / 4;
        board.slide -= step;
        if (board.slide > 0)
            return false;
        board.slide = 0;
    } else {
        if (board.slide >= kBoardWidth)
            return false;
        if (board.slide == 0)
            play(sink, "Panel");
        const int32_t quarter = (kBoardWidth - board.slide) / 4;
        board.slide += quarter < 2 ? 1 : quarter;
        if (board.slide < kBoardWidth)
            return false;
        board.slide = kBoardWidth;
    }
    play(sink, "Options");
    return false;
}

} // namespace

void flash_kill(KillBoard& board, int32_t killer, int32_t victim) noexcept {
    if (killer >= 0 && killer < OA_PLAYER_COUNT)
        board.kill_flash[killer] = kBoardFlashTicks;
    if (victim >= 0 && victim < OA_PLAYER_COUNT)
        board.loss_flash[victim] = kBoardFlashTicks;
}

bool kill_board_wanted(const Game& game, bool hold_key, bool typing) noexcept {
    return (game.graphics_flags & kGraphicsBoardPinned) != 0 || (hold_key && !typing);
}

void draw_kill_board(
    World& world,
    KillBoard& board,
    uint32_t now,
    bool wanted,
    int32_t screen_width,
    const KillBoardSink& sink
) {
    Game& game = world.game;
    if (board.next_fade_tick < static_cast<int32_t>(now)) {
        board.next_fade_tick = static_cast<int32_t>(now) + 1;
        for (int32_t index = 0; index < OA_PLAYER_COUNT; ++index) {
            if (board.kill_flash[index] != 0)
                board.kill_flash[index] =
                    static_cast<uint8_t>(board.kill_flash[index] - kFlashFade);
            if (board.loss_flash[index] != 0)
                board.loss_flash[index] =
                    static_cast<uint8_t>(board.loss_flash[index] - kFlashFade);
        }
    }
    if (advance_slide(board, wanted, sink))
        return;
    const int32_t left = screen_width - board.slide;
    const int32_t right = left + kBoardWidth;
    shade(sink, left, kBoardTop, right, game.player_count * kBoardRowHeight + 0x2e, kBoardShade);
    const int32_t column = right - left - kBoardMargin;
    char label[100];
    std::snprintf(label, sizeof label, "%s", localize(sink, "Kills"));
    text(sink, label, left + 2, kBoardTop, column, 0);
    std::snprintf(label, sizeof label, "%s", localize(sink, "Losses"));
    text(sink, label, right - width_of(sink, label) - 2, kBoardTop, column, 0);
    const bool commanders = game.session_rules == kSessionRulesCommanderCounts;
    int32_t bottom = kFirstRowBottom;
    for (int32_t row = 0; row < game.player_count; ++row) {
        int32_t index = 0;
        for (; index < OA_PLAYER_COUNT; ++index) {
            Player& player = game.players[index];
            if (!on_board(world, player) || board_row(player) != row)
                continue;
            if (index == game.local_player_index) {
                shade(sink, left + 4, bottom - 0x26, right - 4, bottom + 1, kLocalHighlight);
                shade(sink, left + 4, bottom - 0x26, right - 4, bottom + 1, kLocalHighlightInner);
            }
            const int32_t x0 = left + 7;
            const int32_t top = bottom - 0x24;
            const int32_t x1 = left + column;
            if (sink.logo != nullptr)
                sink.logo(sink.user, player, x0, top, x1, bottom);
            // The marker sits in the wash, so the name starts past it and
            // the room it is given stops at the wash's right edge.
            const bool cues = oa::present::colour_vision::shape_cues_active();
            const int32_t name_x = cues ? x0 + 24 : x0 + 2;
            const int32_t name_room = cues ? (column > 22 ? column - 22 : 0) : column;
            text(sink, player.name, name_x, top + 5, name_room, 0);
            std::snprintf(
                label, sizeof label, "%d", commanders ? player.commanders_killed : player.kills
            );
            text(sink, label, x0 + 2, top + 0x14, column, board.kill_flash[index]);
            std::snprintf(
                label, sizeof label, "%d", commanders ? player.commanders_lost : player.losses
            );
            text(
                sink,
                label,
                x1 - width_of(sink, label) - 2,
                top + 0x14,
                column,
                board.loss_flash[index]
            );
            bottom += kBoardRowHeight;
            break;
        }
        if (index == OA_PLAYER_COUNT)
            for (auto& player : game.players)
                if (on_board(world, player) && row < board_row(player))
                    set_board_row(player, static_cast<uint8_t>(board_row(player) - 1));
    }
}

} // namespace oa::ui::hud

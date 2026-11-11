// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/hud/chat_panel.hpp"

#include "oa/ui/hud/game_fields.hpp"
#include "oa/ui/hud/ingame_menu.hpp"

#include "oa/sim/messages.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace oa::ui::hud {
namespace {

constexpr const char* kTalkControl = "TALK";
constexpr const char* kSendTo = "SENDTO";
constexpr const char* kSendType = "SENDTYPE";
constexpr const char* kLivePlayerPrefix = "LIVEPLYR";
constexpr size_t kLivePlayerPrefixBytes = 8;
/// Panel flags of TALK.GUI and of the team variant TALK2.GUI.
constexpr int32_t kTalkPanelFlags = 0x880;
constexpr int32_t kTeamTalkPanelFlags = 0x800;
/// Characters that end a target prefix ("a:", "e;", "3,").
constexpr const char* kTargetSeparators = ",:;";
/// Chat send modes the SENDTYPE control can select.
constexpr uint8_t kLastSendType = 4;
constexpr double kLogoScale = 0.8;
constexpr double kLogoGap = 1.5;

bool watcher(World& world, const Player& player) noexcept {
    const auto* info = world_player_info(&world, &player);
    return info != nullptr && (info->options & OA_SETUP_OPTION_WATCHER) != 0;
}

bool same_name(const char* a, const char* b, size_t limit = static_cast<size_t>(-1)) noexcept {
    for (size_t i = 0; i < limit; ++i) {
        const auto ca = std::tolower(static_cast<unsigned char>(a[i]));
        const auto cb = std::tolower(static_cast<unsigned char>(b[i]));
        if (ca != cb)
            return false;
        if (ca == 0)
            return true;
    }
    return true;
}

int32_t control_value(const PanelControls& controls, int32_t index) {
    return index != -1 && controls.value != nullptr ? controls.value(controls.user, index) : 0;
}

void set_group(const PanelControls& controls, const char* name, int32_t value) {
    const auto index = find_control(controls, name);
    if (index != -1 && controls.set_group_value != nullptr)
        controls.set_group_value(controls.user, index, value);
}

void set_control(const PanelControls& controls, const char* name, int32_t value) {
    const auto index = find_control(controls, name);
    if (index != -1 && controls.set_value != nullptr)
        controls.set_value(controls.user, index, value);
}

} // namespace

void send_chat_line(World& world, const char* typed, const ChatHost& host) {
    Game& game = world.game;
    char buffer[typed_text_bytes];
    std::snprintf(buffer, sizeof buffer, "%s", typed != nullptr ? typed : "");
    const char* text = buffer;
    while (*text == ' ')
        ++text;
    uint8_t mode = game.chat_mode;
    if (host.submit_command != nullptr)
        mode = host.submit_command(host.user, text, mode);
    if (std::strlen(text) == 0)
        return;
    const uint8_t saved_mode = game.chat_mode;
    uint8_t saved_targets[OA_PLAYER_COUNT];
    std::memcpy(saved_targets, game.chat_targets, sizeof saved_targets);
    const char* target = nullptr;
    const auto separator = static_cast<unsigned char>(text[1]);
    if (separator > ' ' && std::strchr(kTargetSeparators, separator) != nullptr) {
        const auto lead = static_cast<unsigned char>(text[0]);
        if (std::isdigit(lead) != 0) {
            const int32_t index = lead - '0';
            if (index < 0 || index > 9 || game.players[index].player_id == 0)
                return;
            text += 2;
            // Eleven bytes are cleared: the target array and the byte after it.
            std::memset(game.chat_targets, 0, sizeof game.chat_targets);
            game.chat_target_no_player = 0;
            mode = OA_CHAT_MODE_CHOSEN;
            game.chat_targets[index] = 1;
            target = game.players[index].name;
        } else {
            const auto letter = std::tolower(lead);
            if (letter == 'a') {
                mode = OA_CHAT_MODE_ALLIES;
                text += 2;
                target = "Allies";
            } else if (letter == 'e') {
                mode = OA_CHAT_MODE_ENEMIES;
                text += 2;
                target = "Enemies";
            }
        }
    }
    game.chat_mode = mode;
    if (host.post_chat != nullptr)
        host.post_chat(
            host.user,
            game.players[game.local_player_index % OA_PLAYER_COUNT],
            text,
            kMessageKindChat,
            target
        );
    std::memcpy(game.chat_targets, saved_targets, sizeof saved_targets);
    game.chat_mode = saved_mode;
}

bool open_chat_panel(
    World& world,
    oa::data::campaign::SessionKind session_kind,
    ChatDraft& draft,
    const PanelLoader& loader,
    const PanelControls& controls,
    const ChatHost& host
) {
    Game& game = world.game;
    const Player& local = game.players[game.local_player_index % OA_PLAYER_COUNT];
    if (watcher(world, local))
        return false;
    if (!draft.initialized) {
        std::memset(draft.text, 0, sizeof draft.text);
        draft.initialized = true;
    }
    if ((game.frame_flags & kFrameUnitInfoOpen) != 0)
        return false;
    const bool team =
        chat_to_team(game) && session_kind == oa::data::campaign::SessionKind::multiplayer;
    const char* layout = team ? "TALK2.GUI" : "TALK.GUI";
    if (loader.load == nullptr ||
        !loader.load(loader.user, layout, nullptr, team ? kTeamTalkPanelFlags : kTalkPanelFlags))
        return false;
    game.frame_flags |= kFrameChatOpen;
    const auto talk = find_control(controls, kTalkControl);
    if (talk != -1 && controls.set_text != nullptr)
        controls.set_text(controls.user, talk, draft.text);
    set_group(controls, kSendTo, team ? 1 : 0);
    if (session_kind == oa::data::campaign::SessionKind::multiplayer) {
        if (team) {
            set_group(controls, kSendType, game.chat_mode);
            if (host.refresh_player_slots != nullptr)
                host.refresh_player_slots(host.user);
            refresh_chat_targets(game, controls);
        }
    } else {
        set_control(controls, kSendTo, 0);
    }
    if (talk != -1 && controls.focus != nullptr)
        controls.focus(controls.user, talk);
    return true;
}

ChatClick chat_panel_click(
    World& world,
    oa::data::campaign::SessionKind session_kind,
    const char* name,
    int32_t control,
    ChatDraft& draft,
    const PanelLoader& loader,
    const PanelControls& controls,
    const ChatHost& host,
    const HudEvents& events
) {
    Game& game = world.game;
    if (name == nullptr) {
        game.frame_flags = static_cast<uint16_t>(game.frame_flags & ~kFrameChatOpen);
        return ChatClick::closed;
    }
    if (same_name(name, kLivePlayerPrefix, kLivePlayerPrefixBytes)) {
        play_sound(events, "SmallButton");
        game.chat_mode = OA_CHAT_MODE_CHOSEN;
        set_group(controls, kSendType, game.chat_mode);
        const auto index = std::atoi(name + kLivePlayerPrefixBytes);
        if (index >= 0 && index < OA_PLAYER_COUNT)
            game.chat_targets[index] = static_cast<uint8_t>(control_value(controls, control));
        return ChatClick::clear_selection;
    }
    if (same_name(name, kSendTo)) {
        play_sound(events, "SmallButton");
        set_chat_to_team(game, (control_value(controls, control) & 1) != 0);
        const auto talk = find_control(controls, kTalkControl);
        const char* typed =
            talk != -1 && controls.text != nullptr ? controls.text(controls.user, talk) : nullptr;
        std::snprintf(draft.text, sizeof draft.text, "%s", typed != nullptr ? typed : "");
        if (loader.close_to_root != nullptr)
            loader.close_to_root(loader.user);
        open_chat_panel(world, session_kind, draft, loader, controls, host);
        return ChatClick::reopened;
    }
    if (same_name(name, kSendType)) {
        play_sound(events, "SmallButton");
        game.chat_mode =
            static_cast<uint8_t>(control_value(controls, find_control(controls, kSendType)));
        if (game.chat_mode > kLastSendType)
            game.chat_mode = OA_CHAT_MODE_EVERYONE;
        refresh_chat_targets(game, controls);
        return ChatClick::clear_selection;
    }
    if (!same_name(name, kTalkControl))
        return ChatClick::focus_text;
    const auto talk = find_control(controls, kTalkControl);
    send_chat_line(
        world,
        talk != -1 && controls.text != nullptr ? controls.text(controls.user, talk) : nullptr,
        host
    );
    std::memset(draft.text, 0, sizeof draft.text);
    set_chat_to_team(game, false);
    return ChatClick::focus_text;
}

LogoBlit player_logo_blit(
    const int32_t rect[4], int32_t frame_width, int32_t frame_height, int32_t lift
) noexcept {
    const int32_t top = rect[1] + lift;
    const int32_t bottom = rect[3] + lift;
    return {
        {rect[0], top, rect[2], top, rect[2], bottom, rect[0], bottom},
        {0, 0, frame_width, 0, frame_width, frame_height, 0, frame_height},
    };
}

void draw_message_log(const World& world, const MessageLogSink& sink) {
    const Game& game = world.game;
    const int32_t capacity = message_lines(game);
    if (capacity == 0)
        return;
    uint32_t index = game.chat_head;
    for (int32_t shown = 1; shown < capacity && index != game.chat_tail; ++shown)
        index = index == 0 ? OA_CHAT_LINE_COUNT - 1 : index - 1;
    const int32_t line_height = sink.font_height != nullptr ? sink.font_height(sink.user) : 0;
    const int32_t filter = message_filter(game);

    // The lines the filter shows, oldest first, with where each one's text
    // starts and the rows it takes.
    struct Shown {
        sim::messages::MessageLine line{};
        int32_t x{};
        int32_t logo{}; ///< the logo's side; 0 for a line without a sender
        int32_t rows{};
    };

    std::array<Shown, OA_CHAT_LINE_COUNT> shown{};
    std::size_t count = 0;
    bool showing = true;
    while (game.chat_head != index && count < shown.size()) {
        Shown& entry = shown[count];
        std::memcpy(&entry.line, game.chat_lines[index], sizeof entry.line);
        index = index + 1 == OA_CHAT_LINE_COUNT ? 0 : index + 1;
        const uint8_t kind = entry.line.kind & 0x0f;
        bool visible = false;
        if (filter == 1) {
            if (kind != 2)
                showing = false;
            visible = showing;
        } else if (filter == 2) {
            visible = kind != 8;
        } else if (filter == 3) {
            visible = message_show_all(game) != 0 || kind == 1 || kind == 4 || kind == 8;
        }
        if (!visible)
            continue;
        if (sink.hide_recorded_chat) {
            const char* const text = entry.line.text;
            if (text[0] == '<' && std::strstr(text, "> ") != nullptr)
                continue;
        }
        entry.x = kMessageLogLeft;
        if (entry.line.sender != sim::messages::sender_none &&
            entry.line.sender < OA_PLAYER_COUNT) {
            entry.logo = static_cast<int32_t>(line_height * kLogoScale);
            entry.x = static_cast<int32_t>(entry.logo * kLogoGap + kMessageLogLeft);
        }
        entry.line.text[sizeof entry.line.text - 1] = '\0';
        entry.rows = sink.rows != nullptr
                         ? std::max(sink.rows(sink.user, entry.line.text, entry.x), int32_t{1})
                         : 1;
        ++count;
    }
    // Past the most rows, the oldest lines go first; the newest stays.
    std::size_t first = 0;
    if (sink.most_rows > 0) {
        int32_t rows = 0;
        for (std::size_t at = 0; at < count; ++at)
            rows += shown[at].rows;
        while (first + 1 < count && rows > sink.most_rows)
            rows -= shown[first++].rows;
    }
    int32_t y = kMessageLogTop;
    for (std::size_t at = first; at < count; ++at) {
        const Shown& entry = shown[at];
        if (sink.set_color != nullptr)
            sink.set_color(
                sink.user, game.ui_colors[(entry.line.kind & kMessageHighlight) != 0 ? 10 : 15]
            );
        if (entry.logo != 0 && sink.logo != nullptr)
            sink.logo(
                sink.user,
                game.players[entry.line.sender],
                kMessageLogLeft,
                y,
                kMessageLogLeft + entry.logo,
                y + entry.logo
            );
        if (sink.text != nullptr)
            sink.text(sink.user, entry.line.text, entry.x, y);
        y += line_height * entry.rows;
    }
}

} // namespace oa::ui::hud

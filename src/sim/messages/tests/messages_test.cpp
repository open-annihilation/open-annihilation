// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/messages.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace oa;
using namespace oa::sim::messages;

namespace {
int failures = 0;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                           \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

struct Recorder {
    int arrived = 0;
    int refreshed = 0;
    int shared = 0;
    int recorded = 0;
    int32_t kind = 3;
    uint32_t roll = 4;
    int centred = 0;
    int32_t camera[3]{};
};

Hooks hooks_for(Recorder& r) {
    Hooks h{};
    h.context = &r;
    h.play_sound = [](void* c, const char* name) {
        if (std::strcmp(name, "MessageArrived") == 0)
            ++static_cast<Recorder*>(c)->arrived;
    };
    h.refresh_panel = [](void* c) { ++static_cast<Recorder*>(c)->refreshed; };
    h.random = [](void* c) { return static_cast<Recorder*>(c)->roll; };
    h.share_chat = [](void* c, const char*) { ++static_cast<Recorder*>(c)->shared; };
    h.record_chat = [](void* c, const char*) { ++static_cast<Recorder*>(c)->recorded; };
    h.game_kind = [](void* c) { return static_cast<Recorder*>(c)->kind; };
    h.center_camera = [](void* c, int32_t x, int32_t y, int32_t glide) {
        auto& rec = *static_cast<Recorder*>(c);
        ++rec.centred;
        rec.camera[0] = x;
        rec.camera[1] = y;
        rec.camera[2] = glide;
    };
    return h;
}

void set_capacity(Game& g, int32_t lines) {
    g.text_lines = lines;
}

struct SeatCtx {
    Recorder rec{};
    int seat_shares = 0;
    int plain_shares = 0;
    uint8_t seat = 0;
    uint8_t mode = 0;
    uint8_t targets[OA_PLAYER_COUNT]{};
    bool saw_targets = false;
    char line[most_chat_line_bytes]{};
    std::size_t limit = 0;
    bool limit_hook = false;
};

Hooks seat_hooks(SeatCtx& ctx) {
    Hooks hooks{};
    hooks.context = &ctx;
    hooks.play_sound = [](void* c, const char* name) {
        if (std::strcmp(name, "MessageArrived") == 0)
            ++static_cast<SeatCtx*>(c)->rec.arrived;
    };
    hooks.share_chat = [](void* c, const char*) { ++static_cast<SeatCtx*>(c)->plain_shares; };
    hooks.record_chat = [](void* c, const char*) { ++static_cast<SeatCtx*>(c)->rec.recorded; };
    hooks.game_kind = [](void* c) { return static_cast<SeatCtx*>(c)->rec.kind; };
    hooks.share_seat_chat =
        [](void* c, uint8_t seat, const char* line, uint8_t mode, const uint8_t* targets) {
            auto& share = *static_cast<SeatCtx*>(c);
            ++share.seat_shares;
            share.seat = seat;
            share.mode = mode;
            share.saw_targets = targets != nullptr;
            if (targets != nullptr)
                std::memcpy(share.targets, targets, OA_PLAYER_COUNT);
            else
                std::memset(share.targets, 0, OA_PLAYER_COUNT);
            share.line[0] = '\0';
            if (line != nullptr)
                std::snprintf(share.line, sizeof share.line, "%s", line);
        };
    hooks.shared_chat_line_bytes = [](void* c) -> std::size_t {
        const auto& held = *static_cast<SeatCtx*>(c);
        return held.limit_hook ? held.limit : 0;
    };
    return hooks;
}

void name_player(Player& player, uint8_t index, const char* name) {
    player.index = index;
    player.in_use = 1;
    std::memcpy(player.second_name, name, std::strlen(name) + 1);
}

void seat_chat_checks() {
    const ChatLineParts aimed = split_chat_line("<Steve->Bob> hello");
    CHECK(aimed.speaker == "Steve" && aimed.target == "Bob" && aimed.words == "hello");
    const ChatLineParts plain = split_chat_line("<Steve> hello");
    CHECK(plain.speaker == "Steve" && plain.target.empty() && plain.words == "hello");
    const ChatLineParts bare = split_chat_line("hello");
    CHECK(bare.speaker.empty() && bare.target.empty() && bare.words == "hello");
    const ChatLineParts open = split_chat_line("<Steve hello");
    CHECK(open.speaker.empty() && open.target.empty() && open.words == "<Steve hello");
    const ChatLineParts empty = split_chat_line("");
    CHECK(empty.speaker.empty() && empty.target.empty() && empty.words.empty());

    CHECK(chat_mode_of_target("") == OA_CHAT_MODE_EVERYONE);
    CHECK(chat_mode_of_target("Allies") == OA_CHAT_MODE_ALLIES);
    CHECK(chat_mode_of_target("ALLIES") == OA_CHAT_MODE_ALLIES);
    CHECK(chat_mode_of_target("aLLieS") == OA_CHAT_MODE_ALLIES);
    CHECK(chat_mode_of_target("Enemies") == OA_CHAT_MODE_ENEMIES);
    CHECK(chat_mode_of_target("enemies") == OA_CHAT_MODE_ENEMIES);
    CHECK(chat_mode_of_target("Bob") == OA_CHAT_MODE_CHOSEN);
    CHECK(chat_mode_of_target("Ally") == OA_CHAT_MODE_CHOSEN);
    CHECK(chat_mode_of_target(" Allies") == OA_CHAT_MODE_CHOSEN);

    CHECK(chat_words_command("+x"));
    CHECK(chat_words_command(".x"));
    CHECK(chat_words_command("  +x"));
    CHECK(chat_words_command("  .x"));
    CHECK(!chat_words_command("x"));
    CHECK(!chat_words_command("  "));
    CHECK(!chat_words_command("x+"));
    CHECK(!chat_words_command("\t+x"));
    CHECK(!chat_words_command(""));

    World* world = world_create();
    if (world == nullptr) {
        ++failures;
        return;
    }
    Game& game = world->game;
    set_capacity(game, 30);
    game.local_player_index = 0;
    name_player(game.players[0], 0, "You");
    name_player(game.players[1], 1, "Steve");
    name_player(game.players[2], 2, "Bob");
    name_player(game.players[4], 4, "Ann");
    // Slot 1 allies with 2. Slot 2 does not ally back. Allies and enemies follow
    // the speaker's row, the same row the human chat path uses.
    game.players[1].alliance[2] = 1;
    game.players[2].alliance[1] = 0;

    CHECK(seat_hears(game, 1, 0, OA_CHAT_MODE_EVERYONE, nullptr));
    CHECK(seat_hears(game, 1, 2, OA_CHAT_MODE_EVERYONE, nullptr));
    CHECK(!seat_hears(game, 1, 1, OA_CHAT_MODE_EVERYONE, nullptr));
    CHECK(seat_hears(game, 1, 2, OA_CHAT_MODE_ALLIES, nullptr));
    CHECK(!seat_hears(game, 1, 0, OA_CHAT_MODE_ALLIES, nullptr));
    CHECK(!seat_hears(game, 1, 2, OA_CHAT_MODE_ENEMIES, nullptr));
    CHECK(seat_hears(game, 1, 0, OA_CHAT_MODE_ENEMIES, nullptr));
    CHECK(!seat_hears(game, 2, 1, OA_CHAT_MODE_ALLIES, nullptr));
    CHECK(seat_hears(game, 2, 1, OA_CHAT_MODE_ENEMIES, nullptr));
    uint8_t chosen[OA_PLAYER_COUNT]{};
    chosen[0] = 1;
    CHECK(seat_hears(game, 1, 0, OA_CHAT_MODE_CHOSEN, chosen));
    CHECK(!seat_hears(game, 1, 2, OA_CHAT_MODE_CHOSEN, chosen));
    CHECK(!seat_hears(game, 1, 0, OA_CHAT_MODE_CHOSEN, nullptr));
    CHECK(!seat_hears(game, 1, 1, OA_CHAT_MODE_CHOSEN, chosen));
    chosen[1] = 1;
    CHECK(!seat_hears(game, 1, 1, OA_CHAT_MODE_EVERYONE, chosen));
    game.players[1].alliance[1] = 1;
    CHECK(!seat_hears(game, 1, 1, OA_CHAT_MODE_ALLIES, nullptr));
    CHECK(!seat_hears(game, 1, 0, chat_mode_local_only, nullptr));
    CHECK(!seat_hears(game, 10, 0, OA_CHAT_MODE_EVERYONE, nullptr));
    CHECK(!seat_hears(game, 1, 10, OA_CHAT_MODE_EVERYONE, nullptr));
    CHECK(!seat_hears(game, 255, 0, OA_CHAT_MODE_EVERYONE, nullptr));
    CHECK(!seat_hears(game, 1, 255, OA_CHAT_MODE_EVERYONE, nullptr));

    SeatCtx ctx;
    ctx.rec.kind = 3;
    const Hooks hooks = seat_hooks(ctx);
    const Player& steve = game.players[1];
    post_chat(*world, steve, "hi", kind_status, nullptr, hooks);
    CHECK(ctx.plain_shares == 1 && ctx.seat_shares == 0);
    ctx.plain_shares = 0;
    ctx.rec.recorded = 0;
    ctx.rec.arrived = 0;
    clear_messages(game);

    post_seat_chat(*world, steve, "hi", OA_CHAT_MODE_EVERYONE, nullptr, hooks);
    CHECK(std::strcmp(ctx.line, "<Steve> hi") == 0);
    CHECK(ctx.seat == 1 && ctx.mode == OA_CHAT_MODE_EVERYONE && !ctx.saw_targets);
    CHECK(ctx.seat_shares == 1 && ctx.plain_shares == 0 && ctx.rec.recorded == 1);
    CHECK(game.chat_head == 1);
    const MessageLine* shown = message_line(game, 0);
    CHECK(std::strcmp(shown->text, "<Steve> hi") == 0);
    CHECK(shown->sender == 1 && (shown->kind & kind_mask) == kind_player_chat);
    CHECK(ctx.rec.arrived == 1);

    ctx.seat_shares = 0;
    ctx.rec.recorded = 0;
    ctx.rec.arrived = 0;
    clear_messages(game);
    post_seat_chat(*world, steve, "go", OA_CHAT_MODE_ALLIES, nullptr, hooks);
    CHECK(std::strcmp(ctx.line, "<Steve->Allies> go") == 0);
    CHECK(ctx.rec.recorded == 0 && game.chat_head == 0 && ctx.rec.arrived == 0);
    game.players[1].alliance[0] = 1;
    post_seat_chat(*world, steve, "go", OA_CHAT_MODE_ALLIES, nullptr, hooks);
    CHECK(game.chat_head == 1 && message_line(game, 0)->sender == 1);
    CHECK(std::strcmp(message_line(game, 0)->text, "<Steve->Allies> go") == 0);
    CHECK(ctx.rec.recorded == 0 && ctx.rec.arrived == 1);
    game.players[1].alliance[0] = 0;

    ctx.seat_shares = 0;
    ctx.rec.arrived = 0;
    clear_messages(game);
    post_seat_chat(*world, steve, "go", OA_CHAT_MODE_ENEMIES, nullptr, hooks);
    CHECK(std::strcmp(ctx.line, "<Steve->Enemies> go") == 0);
    CHECK(ctx.rec.recorded == 0 && game.chat_head == 1 && ctx.rec.arrived == 1);
    CHECK(message_line(game, 0)->sender == 1);

    uint8_t targets[OA_PLAYER_COUNT]{};
    targets[2] = 1;
    targets[4] = 1;
    ctx.seat_shares = 0;
    ctx.rec.arrived = 0;
    clear_messages(game);
    post_seat_chat(*world, steve, "go", OA_CHAT_MODE_CHOSEN, targets, hooks);
    CHECK(std::strcmp(ctx.line, "<Steve->Bob> go") == 0);
    CHECK(ctx.seat == 1 && ctx.mode == OA_CHAT_MODE_CHOSEN && ctx.saw_targets);
    CHECK(ctx.targets[2] == 1 && ctx.targets[4] == 1 && ctx.targets[0] == 0);
    CHECK(ctx.rec.recorded == 0 && game.chat_head == 0 && ctx.rec.arrived == 0);
    targets[0] = 1;
    post_seat_chat(*world, steve, "go", OA_CHAT_MODE_CHOSEN, targets, hooks);
    CHECK(std::strcmp(ctx.line, "<Steve->You> go") == 0);
    CHECK(game.chat_head == 1 && message_line(game, 0)->sender == 1);

    ctx.seat_shares = 0;
    ctx.rec.recorded = 0;
    ctx.rec.arrived = 0;
    clear_messages(game);
    post_seat_chat(*world, steve, "here", chat_mode_local_only, nullptr, hooks);
    CHECK(ctx.seat_shares == 0 && ctx.plain_shares == 0 && ctx.rec.recorded == 0);
    CHECK(game.chat_head == 0 && ctx.rec.arrived == 0);

    ctx.rec.kind = 0;
    ctx.seat_shares = 0;
    post_seat_chat(*world, steve, "hi", OA_CHAT_MODE_EVERYONE, nullptr, hooks);
    CHECK(ctx.rec.recorded == 0 && ctx.seat_shares == 1);
    ctx.rec.kind = 3;

    const std::string long_words(191, 'a');
    ctx.seat_shares = 0;
    post_seat_chat(*world, steve, long_words, OA_CHAT_MODE_EVERYONE, nullptr, hooks);
    CHECK(std::strlen(ctx.line) == 199);
    CHECK(std::strncmp(ctx.line, "<Steve> ", 8) == 0);
    std::string hanzi(190, 'a');
    hanzi += "\xe4\xbd\xa0";
    post_seat_chat(*world, steve, hanzi, OA_CHAT_MODE_EVERYONE, nullptr, hooks);
    CHECK(std::strlen(ctx.line) == 198);

    ctx.limit_hook = true;
    ctx.limit = 20;
    post_seat_chat(*world, steve, "abcdefghijklmnop", OA_CHAT_MODE_EVERYONE, nullptr, hooks);
    CHECK(std::strlen(ctx.line) == 19);
    CHECK(std::strcmp(ctx.line, "<Steve> abcdefghijk") == 0);
    ctx.limit_hook = false;

    const int shares = ctx.seat_shares;
    const int recorded = ctx.rec.recorded;
    const int arrived = ctx.rec.arrived;
    const uint32_t head = game.chat_head;
    post_seat_chat(*world, steve, "+metal", OA_CHAT_MODE_EVERYONE, nullptr, hooks);
    post_seat_chat(*world, steve, "  .take", OA_CHAT_MODE_EVERYONE, nullptr, hooks);
    CHECK(ctx.seat_shares == shares && ctx.plain_shares == 0 && ctx.rec.recorded == recorded);
    CHECK(ctx.rec.arrived == arrived && game.chat_head == head);

    world_destroy(world);
}

} // namespace

int main() {
    World* w = world_create();
    if (w == nullptr)
        return 1;
    Recorder r;
    const Hooks h = hooks_for(r);

    post_message(*w, "ignored", 1, 0, 0, h);
    CHECK(w->game.chat_head == 0);
    set_capacity(w->game, 5);
    w->game.tick = 77;
    post_message(*w, "hello", 3, 9, 2, h);
    const MessageLine* line = message_line(w->game, 0);
    CHECK(std::strcmp(line->text, "hello") == 0 && line->tick == 77 && line->value == 9);
    CHECK(line->sender == 2 && (line->kind & 0xf) == 3);
    CHECK(w->game.chat_head == 1 && w->game.chat_tail == 0 && r.arrived == 1 && r.refreshed == 1);
    for (int i = 0; i < 4; ++i)
        post_message(*w, "x", 0, 0, sender_none, h);
    // Capacity 5: the fifth insert pushes the tail.
    CHECK(w->game.chat_head == 5 && w->game.chat_tail == 1 && r.arrived == 1);
    char long_text[80];
    std::memset(long_text, 'a', sizeof long_text);
    long_text[79] = '\0';
    post_message(*w, long_text, 0, 0, sender_none, h);
    CHECK(std::strlen(message_line(w->game, 5)->text) == 0x3f);
    // A UTF-8 character the 63rd byte would split goes whole: here a
    // three-byte one from byte 62 on.
    std::memset(long_text, 'a', 61);
    std::memcpy(long_text + 61, "\xE4\xB8\xAD\xE6\x97\xA5", 6);
    long_text[67] = '\0';
    post_message(*w, long_text, 0, 0, sender_none, h);
    CHECK(std::strlen(message_line(w->game, 6)->text) == 61);
    // Bytes of an 8-bit code page are cut at the 63rd byte as before.
    std::memset(long_text, '\xE9', 70);
    long_text[70] = '\0';
    post_message(*w, long_text, 0, 0, sender_none, h);
    CHECK(std::strlen(message_line(w->game, 7)->text) == 0x3f);

    // Wrapping: a space within reach splits after it; otherwise 51 characters.
    World* v = world_create();
    set_capacity(v->game, 30);
    char text[128];
    std::memset(text, 'b', 70);
    text[60] = ' ';
    text[70] = '\0';
    post_notice(*v, text, h);
    CHECK(v->game.chat_head == 2);
    CHECK(
        std::strlen(message_line(v->game, 0)->text) == 61 &&
        (message_line(v->game, 0)->kind & 0xf) == 0
    );
    CHECK(std::strlen(message_line(v->game, 1)->text) == 9);
    std::memset(text, 'c', 100);
    text[100] = '\0';
    post_notice(*v, text, h);
    CHECK(v->game.chat_head == 4 && std::strlen(message_line(v->game, 2)->text) == 51);
    CHECK(std::strlen(message_line(v->game, 3)->text) == 49);
    post_notice(*v, "   ", h);
    CHECK(v->game.chat_head == 4);
    // A notice with characters of several bytes breaks where the line
    // breaker lets it: Korean at its spaces, each word whole, into lines of
    // at most 63 bytes.
    World* korean = world_create();
    set_capacity(korean->game, 30);
    post_notice(
        *korean,
        "스파이더 한 대는 반드시 살아남아야 합니다. 스파이더 전차 한 대를 기지로 데려오십시오.",
        h
    );
    CHECK(korean->game.chat_head == 2);
    CHECK(
        std::strcmp(
            message_line(korean->game, 0)->text, "스파이더 한 대는 반드시 살아남아야 합니다."
        ) == 0
    );
    CHECK(
        std::strcmp(
            message_line(korean->game, 1)->text, "스파이더 전차 한 대를 기지로 데려오십시오."
        ) == 0
    );
    world_destroy(korean);

    Player& p = v->game.players[1];
    p.index = 1;
    p.info = oa_ref_from_index(1);
    v->player_info[1].side = 0;
    post_elimination(*v, p, h);
    CHECK(std::strcmp(message_line(v->game, 4)->text, "Arm vermin have been exterminated") == 0);
    CHECK(
        message_line(v->game, 4)->sender == 1 &&
        (message_line(v->game, 4)->kind & 0xf) == kind_elimination
    );

    // A profile's ending replaces the one the roll picks, and is not
    // translated; a null one leaves that ending to the translation hook.
    struct Endings {
        const char* replaced = nullptr;
        uint32_t asked = 99;
    } endings;

    Hooks profiled = h;
    profiled.context = &endings;
    profiled.random = [](void*) -> uint32_t { return 5; };
    profiled.translate = [](void*, const char* text) -> const char* {
        return std::strcmp(text, elimination_messages[2]) == 0 ? "ist fort" : nullptr;
    };
    profiled.elimination_ending = [](void* c, uint32_t index) {
        auto& e = *static_cast<Endings*>(c);
        e.asked = index;
        return e.replaced;
    };
    post_elimination(*v, p, profiled);
    CHECK(endings.asked == 2 && std::strcmp(message_line(v->game, 5)->text, "Arm ist fort") == 0);
    endings.replaced = "has been made-up";
    post_elimination(*v, p, profiled);
    CHECK(std::strcmp(message_line(v->game, 6)->text, "Arm has been made-up") == 0);

    // The kills board's new leader: a status line from no player.
    std::memcpy(p.name, "Ada", 4);
    post_kill_lead(*v, p, 7, h);
    CHECK(std::strcmp(message_line(v->game, 7)->text, "Ada has taken the lead with 7 kills") == 0);
    CHECK(
        message_line(v->game, 7)->sender == sender_none &&
        (message_line(v->game, 7)->kind & 0xf) == kind_status
    );
    Hooks lead = h;
    lead.kill_lead_text = [](void*) -> const char* { return "%d kills: %s leads"; };
    post_kill_lead(*v, p, 12, lead);
    CHECK(std::strcmp(message_line(v->game, 8)->text, "12 kills: Ada leads") == 0);

    // The text is never a format: each of %s and %d is put in once, %% is
    // one %, and everything else is shown as written.
    char lead_line[64];
    format_kill_lead(lead_line, sizeof lead_line, "%s %s %d %d %x %% %", "Ada", -3);
    CHECK(std::strcmp(lead_line, "Ada %s -3 %d %x % %") == 0);
    format_kill_lead(lead_line, sizeof lead_line, "%s leads", "Ada", 3);
    CHECK(std::strcmp(lead_line, "Ada leads") == 0);
    format_kill_lead(lead_line, sizeof lead_line, "A new leader", "Ada", 3);
    CHECK(std::strcmp(lead_line, "A new leader") == 0);
    format_kill_lead(lead_line, 6, "%s has %d", "Adalbert", 3);
    CHECK(std::strcmp(lead_line, "Adalb") == 0);

    std::memcpy(p.second_name, "Steve", 6);
    v->game.chat_mode = OA_CHAT_MODE_EVERYONE;
    post_chat(*v, p, "gg", 2, nullptr, h);
    CHECK(std::strcmp(message_line(v->game, 9)->text, "<Steve> gg") == 0);
    CHECK(r.shared == 1 && r.recorded == 1);
    v->game.chat_mode = OA_CHAT_MODE_ALLIES;
    post_chat(*v, p, "hi", 2, "Bob", h);
    CHECK(std::strcmp(message_line(v->game, 10)->text, "<Steve->Bob> hi") == 0);
    CHECK(r.shared == 2 && r.recorded == 1);
    // A line longer than the formatter holds is cut between whole UTF-8
    // characters: "<Steve> " and 63 hanzi of 80, not a part of the 64th.
    {
        static std::string shared;
        Hooks cut = h;
        cut.share_chat = [](void*, const char* line) { shared = line; };
        std::string hanzi;
        for (int i = 0; i < 80; ++i)
            hanzi += "\xe4\xbd\xa0"; // U+4F60
        v->game.chat_mode = OA_CHAT_MODE_EVERYONE;
        post_chat(*v, p, hanzi.c_str(), 2, nullptr, cut);
        CHECK(shared == "<Steve> " + hanzi.substr(0, 63 * 3));
        // A line that may go out as several records holds what the hooks
        // ask, up to 255 bytes: "<Steve> " and 82 hanzi; 0 asks for none.
        cut.shared_chat_line_bytes = [](void*) -> std::size_t { return 0x100; };
        hanzi += hanzi;
        post_chat(*v, p, hanzi.c_str(), 2, nullptr, cut);
        CHECK(shared == "<Steve> " + hanzi.substr(0, 82 * 3));
        cut.shared_chat_line_bytes = [](void*) -> std::size_t { return 0x1000; };
        post_chat(*v, p, hanzi.c_str(), 2, nullptr, cut);
        CHECK(shared == "<Steve> " + hanzi.substr(0, 82 * 3));
        cut.shared_chat_line_bytes = [](void*) -> std::size_t { return 0; };
        post_chat(*v, p, hanzi.c_str(), 2, nullptr, cut);
        CHECK(shared == "<Steve> " + hanzi.substr(0, 63 * 3));
    }
    // A shared line that went out as two records shows as those two lines.
    {
        Hooks split = h;
        split.shared_chat_line = [](void*, const char* line, std::size_t index) -> const char* {
            static const char* const parts[] = {"<Steve> first half", "<Steve> second half"};
            return std::strcmp(line, "<Steve> first half second half") == 0 && index < 2
                       ? parts[index]
                       : nullptr;
        };
        const auto head = v->game.chat_head;
        post_chat(*v, p, "first half second half", 2, nullptr, split);
        CHECK(std::strcmp(message_line(v->game, head)->text, "<Steve> first half") == 0);
        CHECK(std::strcmp(message_line(v->game, head + 1)->text, "<Steve> second half") == 0);
        CHECK(v->game.chat_head == head + 2);
        // Kept on this machine only, it shows as typed.
        v->game.chat_mode = chat_mode_local_only;
        post_chat(*v, p, "first half second half", 2, nullptr, split);
        CHECK(
            std::strcmp(message_line(v->game, head + 2)->text, "<Steve> first half second half") ==
            0
        );
        v->game.chat_mode = OA_CHAT_MODE_EVERYONE;
    }

    // The alliance lines and the missing-map line go in English and show in
    // the language shown, the head and the names as they came: German,
    // Chinese, and English, which has no translation.
    {
        struct Phrases {
            const char* allied;
            const char* broke;
            const char* missing;
        };

        static const Phrases german{
            "Verb\xc3\xbcndet mit", "k\xc3\xbcndigt Allianz mit", "hat diese Karte nicht"
        };
        static const Phrases chinese{
            "\xe7\xbb\x93\xe7\x9b\x9f",
            "\xe8\xa7\xa3\xe9\x99\xa4\xe7\xbb\x93\xe7\x9b\x9f",
            "\xe6\xb2\xa1\xe6\x9c\x89\xe6\xad\xa4\xe5\x9c\xb0\xe5\x9b\xbe"
        };
        const auto translate = [](void* context, const char* text) -> const char* {
            const auto& phrases = *static_cast<const Phrases*>(context);
            if (std::strcmp(text, phrase_allied_with) == 0)
                return phrases.allied;
            if (std::strcmp(text, phrase_broke_alliance_with) == 0)
                return phrases.broke;
            if (std::strcmp(text, phrase_missing_map) == 0)
                return phrases.missing;
            return nullptr;
        };
        const auto shown = [&](const char* line, const Phrases* phrases, size_t size = 0x100) {
            char out[0x100];
            std::memset(out, 'x', sizeof out);
            format_shown_chat_line(
                out,
                size,
                line,
                phrases != nullptr ? +translate : nullptr,
                const_cast<Phrases*>(phrases)
            );
            return std::string(out);
        };
        for (const Phrases* phrases : {&german, &chinese}) {
            CHECK(
                shown("<Hans>  allied with Mary", phrases) ==
                std::string("<Hans>  ") + phrases->allied + " Mary"
            );
            CHECK(
                shown("<Hans>  broke alliance with Li Wei", phrases) ==
                std::string("<Hans>  ") + phrases->broke + " Li Wei"
            );
            CHECK(
                shown("<Hans> does not have this map", phrases) ==
                std::string("<Hans> ") + phrases->missing
            );
            // A line to chosen players keeps its head.
            CHECK(
                shown("<Hans->Mary>  allied with Mary", phrases) ==
                std::string("<Hans->Mary>  ") + phrases->allied + " Mary"
            );
            // Other lines show as they came: other words, one space before
            // the phrase, no name, more after the map line, no head.
            for (const char* other_line :
                 {"<Hans> gg",
                  "<Hans> allied with Mary",
                  "<Hans>  allied with",
                  "<Hans>  allied withMary",
                  "<Hans> does not have this map yet",
                  " allied with Mary",
                  "Hans>  allied with Mary",
                  ""})
                CHECK(shown(other_line, phrases) == other_line);
        }
        CHECK(shown("<Hans>  allied with Mary", nullptr) == "<Hans>  allied with Mary");
        CHECK(shown("<Hans> does not have this map", nullptr) == "<Hans> does not have this map");
        // A line that does not fit is cut between whole characters: the
        // second hanzi of the Chinese phrase would be split.
        CHECK(shown("<Hans>  allied with Mary", &chinese, 13) == "<Hans>  \xe7\xbb\x93");
        CHECK(shown("<Hans>  allied with Mary", &german, 6) == "<Hans");
        {
            char untouched[2] = {'x', 'y'};
            format_shown_chat_line(untouched, 0, "<Hans> gg", +translate, nullptr);
            CHECK(untouched[0] == 'x' && untouched[1] == 'y');
        }
        // The speaker's log shows the line so, while the other players get
        // it in English.
        static std::string shared_line;
        Hooks said{};
        said.context = const_cast<Phrases*>(&german);
        said.translate = translate;
        said.share_chat = [](void*, const char* line) { shared_line = line; };
        std::memcpy(p.second_name, "Hans", 5);
        v->game.chat_mode = OA_CHAT_MODE_EVERYONE;
        const auto head = v->game.chat_head;
        post_chat(*v, p, " allied with Mary", kind_player_chat, nullptr, said);
        CHECK(shared_line == "<Hans>  allied with Mary");
        CHECK(
            std::string(message_line(v->game, head)->text) ==
            std::string("<Hans>  ") + german.allied + " Mary"
        );
        std::memcpy(p.second_name, "Steve", 6);
    }

    // Expiry: the oldest line goes once it has been up for TextScroll + 1
    // seconds, one line per call.
    World* e = world_create();
    set_log_options(e->game, 10, 2, filter_session_start, 1);
    CHECK(line_capacity(e->game) == 10 && text_scroll(e->game) == 2);
    CHECK(!expire_oldest_message(e->game));
    e->game.tick = 100;
    post_message(*e, "first", 1, 0, sender_none, h);
    e->game.tick = 130;
    post_message(*e, "second", 1, 0, sender_none, h);
    e->game.tick = 190;
    CHECK(!expire_oldest_message(e->game));
    e->game.tick = 191;
    CHECK(expire_oldest_message(e->game) && e->game.chat_tail == 1);
    CHECK(!expire_oldest_message(e->game));
    e->game.tick = 400;
    CHECK(expire_oldest_message(e->game) && e->game.chat_tail == 2 && e->game.chat_head == 2);
    CHECK(!expire_oldest_message(e->game));
    post_message(*e, "third", 1, 0, sender_none, h);
    clear_messages(e->game);
    CHECK(e->game.chat_head == 0 && e->game.chat_tail == 0);
    world_destroy(e);

    // Reported units: lines are scanned from the
    // oldest; a line needs a unit id, no visited bit (0x10) and a unit with
    // OA_UNIT_FLAG_LIVE. The camera gets the high words of x and z.
    World* t = world_create();
    WorldCapacity capacity{8, 1, 0};
    if (t == nullptr || !world_alloc_tables(t, &capacity))
        return 1;
    set_log_options(t->game, 10, 2, filter_session_start, 1);
    t->units[3].flags = OA_UNIT_FLAG_LIVE;
    t->units[3].position = {
        static_cast<int32_t>(300u << 16) + 0x1234, 0, static_cast<int32_t>(200u << 16)
    };
    t->units[4].flags = OA_UNIT_FLAG_LIVE;
    t->units[4].position = {
        static_cast<int32_t>(400u << 16), 0, static_cast<int32_t>(0xfffbu << 16)
    };
    t->units[5].flags = OA_UNIT_FLAG_SELECTED;
    post_message(*t, "no unit", kind_unit_report, 0, sender_none, h);
    post_message(*t, "gone", kind_unit_report, 5, sender_none, h);
    post_message(*t, "three", kind_unit_report, 3, sender_none, h);
    post_message(*t, "four", kind_unit_report, 4, sender_none, h);
    r = Recorder{};
    cycle_reported_units(*t, h);
    CHECK(r.centred == 1 && r.camera[0] == 300 && r.camera[1] == 200 && r.camera[2] == 1);
    CHECK(
        message_line(t->game, 2)->kind ==
        (kind_unit_report | line_flag_visited | line_flag_highlight)
    );
    CHECK(message_line(t->game, 1)->kind == kind_unit_report);
    cycle_reported_units(*t, h);
    CHECK(r.centred == 2 && r.camera[0] == 400 && r.camera[1] == -5);
    CHECK(message_line(t->game, 2)->kind == (kind_unit_report | line_flag_visited));
    CHECK(
        message_line(t->game, 3)->kind ==
        (kind_unit_report | line_flag_visited | line_flag_highlight)
    );
    CHECK(!track_next_reported_unit(*t, h) && r.centred == 2);
    // All visited: the visits are forgotten and the oldest goes again.
    cycle_reported_units(*t, h);
    CHECK(r.centred == 3 && r.camera[0] == 300);
    CHECK(message_line(t->game, 3)->kind == kind_unit_report);
    CHECK(
        message_line(t->game, 2)->kind ==
        (kind_unit_report | line_flag_visited | line_flag_highlight)
    );
    // A head outside the ring ends the scan instead of running forever.
    t->game.chat_head = OA_CHAT_LINE_COUNT;
    CHECK(!track_next_reported_unit(*t, h));
    world_destroy(t);

    // A third side's elimination: the hook names it by its own index; without
    // the hook every side but the first is the Core.
    World* s = world_create();
    if (s == nullptr)
        return 1;
    set_capacity(s->game, 5);
    Player& third = s->game.players[2];
    third.index = 2;
    third.info = oa_ref_from_index(2);
    s->player_info[2].side = 2;
    Hooks by_side = h;
    by_side.side_name = [](void*, uint8_t side) { return side == 2 ? "East" : "Other"; };
    post_elimination(*s, third, by_side);
    CHECK(std::strncmp(message_line(s->game, 0)->text, "East ", 5) == 0);
    post_elimination(*s, third, h);
    CHECK(std::strncmp(message_line(s->game, 1)->text, "Core ", 5) == 0);
    world_destroy(s);

    world_destroy(v);
    world_destroy(w);
    seat_chat_checks();
    if (failures != 0)
        return 1;
    std::puts("sim-messages: ok");
    return 0;
}

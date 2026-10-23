// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Chat a computer seat reads and sends: what it may say, who hears a line,
// and the pace of one line per 30 ticks.

#include "oa/app/seat_chat.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace oa;
using namespace oa::app::seat_chat;
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

struct Share {
    uint8_t seat{};
    uint8_t mode{};
    std::string line;
};

struct Heard {
    uint8_t seat{};
    uint8_t from{};
    uint8_t mode{};
    std::string words;
};

struct Probe {
    std::vector<Share> shares;
};

Probe* probe_of(void* context) {
    return static_cast<Probe*>(context);
}

std::vector<Heard>* heard_of(void* context) {
    return static_cast<std::vector<Heard>*>(context);
}

Hooks share_hooks(Probe& probe) {
    Hooks hooks{};
    hooks.context = &probe;
    hooks.share_seat_chat =
        [](void* context, uint8_t seat, const char* line, uint8_t mode, const uint8_t*) {
            probe_of(context)->shares.push_back({seat, mode, line != nullptr ? line : ""});
        };
    hooks.play_sound = [](void*, const char*) {};
    return hooks;
}

Listener hear_listener(std::vector<Heard>& heard) {
    Listener listener{};
    listener.context = &heard;
    listener.heard =
        [](void* context, uint8_t seat, uint8_t from, uint8_t mode, std::string_view words) {
            heard_of(context)->push_back({seat, from, mode, std::string(words)});
        };
    return listener;
}

void name_player(Player& player, uint8_t index, uint8_t status, const char* name) {
    player.index = index;
    player.in_use = 1;
    player.status = status;
    std::memcpy(player.second_name, name, std::strlen(name) + 1);
}

World* make_world() {
    World* world = world_create();
    if (world == nullptr)
        return nullptr;
    const WorldCapacity capacity{8, 1, 0};
    if (world_alloc_tables(world, &capacity) == 0) {
        world_destroy(world);
        return nullptr;
    }
    name_player(world->game.players[0], 0, OA_PLAYER_STATUS_LOCAL, "You");
    name_player(world->game.players[1], 1, OA_PLAYER_STATUS_COMPUTER, "North");
    name_player(world->game.players[2], 2, OA_PLAYER_STATUS_COMPUTER, "South");
    name_player(world->game.players[3], 3, OA_PLAYER_STATUS_MIRRORED, "Far");
    world->game.local_player_index = 0;
    world->game.text_lines = 30;
    return world;
}

void say_refuses_and_paces(World& world) {
    SeatChat chat;
    CHECK(chat.say(world, 3, "hi", OA_CHAT_MODE_EVERYONE, 0) == Said::not_a_seat);
    CHECK(chat.say(world, 0, "+x", OA_CHAT_MODE_EVERYONE, 0) == Said::not_a_seat);
    CHECK(chat.say(world, 4, "hi", OA_CHAT_MODE_EVERYONE, 0) == Said::not_a_seat);
    CHECK(chat.say(world, 10, "hi", OA_CHAT_MODE_EVERYONE, 0) == Said::not_a_seat);
    CHECK(chat.say(world, 255, "hi", OA_CHAT_MODE_EVERYONE, 0) == Said::not_a_seat);
    CHECK(chat.pending() == 0);

    CHECK(chat.say(world, 1, "", OA_CHAT_MODE_EVERYONE, 0) == Said::empty);
    CHECK(chat.say(world, 1, "   ", OA_CHAT_MODE_EVERYONE, 0) == Said::empty);
    CHECK(chat.say(world, 1, "\t", OA_CHAT_MODE_EVERYONE, 0) == Said::empty);
    CHECK(chat.say(world, 1, "\x1f", OA_CHAT_MODE_EVERYONE, 0) == Said::empty);
    CHECK(chat.say(world, 1, "+", OA_CHAT_MODE_EVERYONE, 0) == Said::command);
    CHECK(chat.say(world, 1, ".x", OA_CHAT_MODE_EVERYONE, 0) == Said::command);
    CHECK(chat.say(world, 1, "  +metal", OA_CHAT_MODE_EVERYONE, 0) == Said::command);
    CHECK(chat.say(world, 1, "\x01+", OA_CHAT_MODE_EVERYONE, 0) == Said::command);
    CHECK(chat.pending() == 0);

    CHECK(chat.say(world, 1, "hi", chat_mode_local_only, 0) == Said::no_target);
    CHECK(chat.say(world, 1, "hi", 9, 0) == Said::no_target);
    CHECK(chat.say(world, 1, "hi", OA_CHAT_MODE_CHOSEN, 1) == Said::no_target);
    CHECK(chat.say(world, 1, "hi", OA_CHAT_MODE_CHOSEN, 4) == Said::no_target);
    CHECK(chat.say(world, 1, "hi", OA_CHAT_MODE_CHOSEN, 10) == Said::no_target);
    CHECK(chat.say(world, 1, "hi", OA_CHAT_MODE_CHOSEN, 0) == Said::queued);
    CHECK(chat.say(world, 1, "hi", OA_CHAT_MODE_CHOSEN, 3) == Said::too_soon);
    chat.reset();
    CHECK(chat.pending() == 0);

    world.game.tick = 100;
    CHECK(chat.say(world, 1, "one", OA_CHAT_MODE_EVERYONE, 0) == Said::queued);
    CHECK(chat.pending() == 1);
    world.game.tick = 129;
    CHECK(chat.say(world, 1, "two", OA_CHAT_MODE_EVERYONE, 0) == Said::too_soon);
    CHECK(chat.pending() == 1);
    world.game.tick = 130;
    CHECK(chat.say(world, 1, "two", OA_CHAT_MODE_EVERYONE, 0) == Said::queued);
    CHECK(chat.pending() == 2);

    chat.reset();
    world.game.tick = 50;
    CHECK(chat.say(world, 1, "north", OA_CHAT_MODE_EVERYONE, 0) == Said::queued);
    CHECK(chat.say(world, 2, "south", OA_CHAT_MODE_EVERYONE, 0) == Said::queued);
    CHECK(chat.pending() == 2);
    world.game.tick = 79;
    CHECK(chat.say(world, 1, "again", OA_CHAT_MODE_EVERYONE, 0) == Said::too_soon);
    CHECK(chat.say(world, 2, "again", OA_CHAT_MODE_EVERYONE, 0) == Said::too_soon);
    CHECK(chat.pending() == 2);
    world.game.tick = 80;
    CHECK(chat.say(world, 1, "again", OA_CHAT_MODE_EVERYONE, 0) == Said::queued);
    CHECK(chat.say(world, 2, "again", OA_CHAT_MODE_EVERYONE, 0) == Said::queued);

    std::vector<Heard> heard;
    chat.set_listener(hear_listener(heard));
    chat.reset();
    CHECK(chat.pending() == 0);
    world.game.tick = 80;
    CHECK(chat.say(world, 1, "back", OA_CHAT_MODE_EVERYONE, 0) == Said::queued);
    Probe probe;
    chat.flush(world, share_hooks(probe));
    CHECK(chat.pending() == 0);
    CHECK(heard.size() == 1 && heard[0].seat == 2 && heard[0].words == "back");
}

void flush_posts_in_order(World& world) {
    SeatChat chat;
    Probe probe;
    const Hooks hooks = share_hooks(probe);
    clear_messages(world.game);
    world.game.players[1].alliance[0] = 0;
    world.game.players[1].alliance[2] = 0;
    CHECK(chat.say(world, 1, "one", OA_CHAT_MODE_EVERYONE, 0) == Said::queued);
    CHECK(chat.say(world, 2, "two", OA_CHAT_MODE_EVERYONE, 0) == Said::queued);
    std::vector<Heard> heard;
    chat.set_listener(hear_listener(heard));
    chat.flush(world, hooks);
    CHECK(probe.shares.size() == 2);
    if (probe.shares.size() == 2) {
        CHECK(probe.shares[0].seat == 1 && probe.shares[0].mode == OA_CHAT_MODE_EVERYONE);
        CHECK(probe.shares[0].line == "<North> one");
        CHECK(probe.shares[1].seat == 2 && probe.shares[1].mode == OA_CHAT_MODE_EVERYONE);
        CHECK(probe.shares[1].line == "<South> two");
    }
    CHECK(world.game.chat_head == 2);
    CHECK(std::strcmp(message_line(world.game, 0)->text, "<North> one") == 0);
    CHECK(message_line(world.game, 0)->sender == 1);
    CHECK(std::strcmp(message_line(world.game, 1)->text, "<South> two") == 0);
    CHECK(message_line(world.game, 1)->sender == 2);
    CHECK(heard.size() == 2);
    if (heard.size() == 2) {
        CHECK(heard[0].seat == 2 && heard[0].from == 1 && heard[0].words == "one");
        CHECK(heard[1].seat == 1 && heard[1].from == 2 && heard[1].words == "two");
    }

    chat.reset();
    probe.shares.clear();
    heard.clear();
    clear_messages(world.game);
    world.game.players[1].alliance[0] = 1;
    world.game.players[1].alliance[2] = 0;
    CHECK(chat.say(world, 1, "ally", OA_CHAT_MODE_ALLIES, 0) == Said::queued);
    chat.flush(world, hooks);
    CHECK(probe.shares.size() == 1 && probe.shares[0].mode == OA_CHAT_MODE_ALLIES);
    CHECK(world.game.chat_head == 1);
    CHECK(std::strcmp(message_line(world.game, 0)->text, "<North->Allies> ally") == 0);
    CHECK(heard.empty());

    chat.reset();
    probe.shares.clear();
    clear_messages(world.game);
    CHECK(chat.say(world, 1, "foe", OA_CHAT_MODE_ENEMIES, 0) == Said::queued);
    chat.flush(world, hooks);
    CHECK(probe.shares.size() == 1 && probe.shares[0].mode == OA_CHAT_MODE_ENEMIES);
    CHECK(world.game.chat_head == 0);
    CHECK(heard.size() == 1 && heard[0].seat == 2 && heard[0].mode == OA_CHAT_MODE_ENEMIES);
    CHECK(heard[0].words == "foe");

    chat.reset();
    probe.shares.clear();
    heard.clear();
    CHECK(chat.say(world, 1, "south", OA_CHAT_MODE_CHOSEN, 2) == Said::queued);
    chat.flush(world, hooks);
    CHECK(probe.shares.size() == 1 && probe.shares[0].mode == OA_CHAT_MODE_CHOSEN);
    CHECK(probe.shares[0].line == "<North->South> south");
    CHECK(world.game.chat_head == 0);
    CHECK(heard.size() == 1 && heard[0].seat == 2 && heard[0].mode == OA_CHAT_MODE_CHOSEN);
    CHECK(heard[0].from == 1 && heard[0].words == "south");

    chat.reset();
    probe.shares.clear();
    heard.clear();
    clear_messages(world.game);
    CHECK(chat.say(world, 1, "you", OA_CHAT_MODE_CHOSEN, 0) == Said::queued);
    chat.flush(world, hooks);
    CHECK(world.game.chat_head == 1);
    CHECK(std::strcmp(message_line(world.game, 0)->text, "<North->You> you") == 0);
    CHECK(heard.empty());

    SeatChat quiet;
    probe.shares.clear();
    heard.clear();
    CHECK(quiet.say(world, 1, "quiet", OA_CHAT_MODE_EVERYONE, 0) == Said::queued);
    quiet.flush(world, hooks);
    CHECK(probe.shares.size() == 1 && heard.empty());
    quiet.set_listener(hear_listener(heard));
    quiet.reset();
    CHECK(quiet.say(world, 1, "loud", OA_CHAT_MODE_EVERYONE, 0) == Said::queued);
    quiet.flush(world, hooks);
    CHECK(heard.size() == 1 && heard[0].words == "loud");

    chat.reset();
    heard.clear();
    clear_messages(world.game);
    chat.set_listener(hear_listener(heard));
    CHECK(
        chat.say(
            world,
            1,
            "a\x01"
            "b",
            OA_CHAT_MODE_EVERYONE,
            0
        ) == Said::queued
    );
    CHECK(
        chat.say(
            world,
            1,
            "\x1f"
            "x",
            OA_CHAT_MODE_EVERYONE,
            0
        ) == Said::too_soon
    );
    chat.reset();
    CHECK(
        chat.say(
            world,
            1,
            "a\x01"
            "b",
            OA_CHAT_MODE_EVERYONE,
            0
        ) == Said::queued
    );
    chat.flush(world, hooks);
    CHECK(heard.size() == 1 && heard[0].words == "a b");
    CHECK(std::strcmp(message_line(world.game, 0)->text, "<North> a b") == 0);
    chat.reset();
    heard.clear();
    clear_messages(world.game);
    CHECK(
        chat.say(
            world,
            1,
            "\x1f"
            "x",
            OA_CHAT_MODE_EVERYONE,
            0
        ) == Said::queued
    );
    chat.flush(world, hooks);
    CHECK(heard.size() == 1 && heard[0].words == " x");
    CHECK(std::strcmp(message_line(world.game, 0)->text, "<North>  x") == 0);

    chat.reset();
    heard.clear();
    CHECK(chat.say(world, 1, "  hi", OA_CHAT_MODE_EVERYONE, 0) == Said::queued);
    chat.flush(world, hooks);
    CHECK(heard.size() == 1 && heard[0].words == "  hi");
}

void hear_local_follows_each_mode(World& world) {
    SeatChat chat;
    std::vector<Heard> heard;
    chat.set_listener(hear_listener(heard));
    world.game.players[1].alliance[0] = 0;
    world.game.players[1].alliance[2] = 0;

    chat.hear_local(world, 1, "+metal", OA_CHAT_MODE_EVERYONE, nullptr);
    chat.hear_local(world, 1, ".take", OA_CHAT_MODE_EVERYONE, nullptr);
    chat.hear_local(world, 1, "  +metal", OA_CHAT_MODE_EVERYONE, nullptr);
    CHECK(heard.empty());

    chat.hear_local(world, 1, "all", OA_CHAT_MODE_EVERYONE, nullptr);
    CHECK(heard.size() == 1 && heard[0].seat == 2 && heard[0].from == 1);
    CHECK(heard[0].mode == OA_CHAT_MODE_EVERYONE && heard[0].words == "all");

    heard.clear();
    chat.hear_local(world, 0, "both", OA_CHAT_MODE_EVERYONE, nullptr);
    CHECK(heard.size() == 2);
    if (heard.size() == 2) {
        CHECK(heard[0].seat == 1 && heard[1].seat == 2);
        CHECK(heard[0].words == "both" && heard[1].words == "both");
    }

    heard.clear();
    world.game.players[1].alliance[2] = 1;
    chat.hear_local(world, 1, "ally", OA_CHAT_MODE_ALLIES, nullptr);
    CHECK(heard.size() == 1 && heard[0].seat == 2 && heard[0].mode == OA_CHAT_MODE_ALLIES);
    world.game.players[1].alliance[2] = 0;
    heard.clear();
    chat.hear_local(world, 1, "ally", OA_CHAT_MODE_ALLIES, nullptr);
    CHECK(heard.empty());

    chat.hear_local(world, 1, "foe", OA_CHAT_MODE_ENEMIES, nullptr);
    CHECK(heard.size() == 1 && heard[0].seat == 2 && heard[0].mode == OA_CHAT_MODE_ENEMIES);
    world.game.players[1].alliance[2] = 1;
    heard.clear();
    chat.hear_local(world, 1, "foe", OA_CHAT_MODE_ENEMIES, nullptr);
    CHECK(heard.empty());
    world.game.players[1].alliance[2] = 0;

    uint8_t targets[OA_PLAYER_COUNT]{};
    targets[2] = 1;
    chat.hear_local(world, 1, "south", OA_CHAT_MODE_CHOSEN, targets);
    CHECK(heard.size() == 1 && heard[0].seat == 2 && heard[0].mode == OA_CHAT_MODE_CHOSEN);
    CHECK(heard[0].words == "south");
    heard.clear();
    chat.hear_local(world, 1, "south", OA_CHAT_MODE_CHOSEN, nullptr);
    CHECK(heard.empty());
    chat.hear_local(world, 255, "nope", OA_CHAT_MODE_EVERYONE, nullptr);
    CHECK(heard.empty());
}

void hear_remote_reaches_the_addressee(World& world) {
    SeatChat chat;
    std::vector<Heard> heard;
    chat.set_listener(hear_listener(heard));

    chat.hear_remote(world, 7, 0, true, "<Ann->Allies> hello");
    CHECK(heard.size() == 2);
    if (heard.size() == 2) {
        CHECK(heard[0].seat == 1 && heard[1].seat == 2);
        CHECK(heard[0].from == 7 && heard[1].from == 7);
        CHECK(heard[0].mode == OA_CHAT_MODE_EVERYONE && heard[1].mode == OA_CHAT_MODE_EVERYONE);
        CHECK(heard[0].words == "hello" && heard[1].words == "hello");
        CHECK(heard[0].words.find('<') == std::string::npos);
    }

    heard.clear();
    chat.hear_remote(world, 255, 0, true, "<Ann> hello");
    CHECK(heard.size() == 2 && heard[0].from == 255 && heard[0].words == "hello");

    heard.clear();
    chat.hear_remote(world, 7, 2, false, "<Ann->Allies> hello");
    CHECK(heard.size() == 1 && heard[0].seat == 2 && heard[0].from == 7);
    CHECK(heard[0].mode == OA_CHAT_MODE_ALLIES && heard[0].words == "hello");

    heard.clear();
    chat.hear_remote(world, 7, 2, false, "<Ann->Bob> hello");
    CHECK(heard.size() == 1 && heard[0].seat == 2 && heard[0].mode == OA_CHAT_MODE_CHOSEN);
    CHECK(heard[0].words == "hello");

    heard.clear();
    chat.hear_remote(world, 7, 0, false, "<Ann->You> hello");
    CHECK(heard.empty());
    chat.hear_remote(world, 7, 1, true, "<Ann> +metal");
    chat.hear_remote(world, 7, 2, false, "<Ann> .take");
    chat.hear_remote(world, 7, 2, false, "<Ann>  +metal");
    CHECK(heard.empty());
}

} // namespace

int main() {
    World* world = make_world();
    if (world == nullptr) {
        std::fputs("app-seat-chat: world_create failed\n", stderr);
        return 1;
    }
    say_refuses_and_paces(*world);
    world->game.tick = 0;
    flush_posts_in_order(*world);
    hear_local_follows_each_mode(*world);
    hear_remote_reaches_the_addressee(*world);
    world_destroy(world);
    if (failures != 0)
        return 1;
    std::puts("app-seat-chat: ok");
    return 0;
}

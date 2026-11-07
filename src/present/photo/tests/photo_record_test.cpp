// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The text a photo carries: the chunk list for a fixed moment, with player
// names and without, the JSON record, and the facts a photo must not keep.
#include "oa/present/photo/photo_texts.hpp"

#include "oa/formats/json.hpp"

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

using oa::formats::json::Json;
using oa::formats::json::JsonError;
using oa::formats::json::JsonType;
using oa::formats::json::parse_json;
using oa::present::photo::CameraFact;
using oa::present::photo::ChunkKind;
using oa::present::photo::GameKind;
using oa::present::photo::LookFact;
using oa::present::photo::PhotoFacts;
using oa::present::photo::PhotoText;
using oa::present::photo::PlayerFact;
using oa::present::photo::ReplayFact;
using oa::present::photo::SizeFact;
using oa::present::photo::UtcMoment;
using oa::present::photo::photo_texts;

constexpr const char* kJson =
    "{\"photo\":1,\"engine\":\"0.9.0\",\"mod\":{\"id\":\"ridge\",\"version\":\"28\"},"
    "\"map\":{\"name\":\"Canal Crossing\"},"
    "\"game\":{\"kind\":\"skirmish\",\"tick\":6630,\"time\":\"3:41\",\"speed\":10},"
    "\"players\":[{\"name\":\"Stève\",\"side\":\"Arm\",\"colour\":0,\"team\":1},"
    "{\"name\":\"Core AI\",\"side\":\"Core\",\"colour\":1,\"team\":2}],"
    "\"camera\":{\"x\":2620,\"z\":1480,\"zoom\":1.5,\"yaw\":0,\"pitch\":0},"
    "\"size\":{\"width\":2560,\"height\":1440,\"smooth\":1}}";

constexpr const char* kJsonUnnamed =
    "{\"photo\":1,\"engine\":\"0.9.0\",\"mod\":{\"id\":\"ridge\",\"version\":\"28\"},"
    "\"map\":{\"name\":\"Canal Crossing\"},"
    "\"game\":{\"kind\":\"skirmish\",\"tick\":6630,\"time\":\"3:41\",\"speed\":10},"
    "\"players\":[{\"side\":\"Arm\",\"colour\":0,\"team\":1},"
    "{\"side\":\"Core\",\"colour\":1,\"team\":2}],"
    "\"camera\":{\"x\":2620,\"z\":1480,\"zoom\":1.5,\"yaw\":0,\"pitch\":0},"
    "\"size\":{\"width\":2560,\"height\":1440,\"smooth\":1}}";

constexpr const char* kJsonNoTeam =
    "{\"photo\":1,\"engine\":\"0.9.0\",\"mod\":{\"id\":\"ridge\",\"version\":\"28\"},"
    "\"map\":{\"name\":\"Canal Crossing\"},"
    "\"game\":{\"kind\":\"skirmish\",\"tick\":6630,\"time\":\"3:41\",\"speed\":10},"
    "\"players\":[{\"name\":\"Player\",\"side\":\"Arm\",\"colour\":0,\"team\":null},"
    "{\"name\":\"Core\",\"side\":\"Core\",\"colour\":1,\"team\":null}],"
    "\"camera\":{\"x\":2620,\"z\":1480,\"zoom\":1.5,\"yaw\":0,\"pitch\":0},"
    "\"size\":{\"width\":2560,\"height\":1440,\"smooth\":1}}";

constexpr const char* kJsonNoTeamUnnamed =
    "{\"photo\":1,\"engine\":\"0.9.0\",\"mod\":{\"id\":\"ridge\",\"version\":\"28\"},"
    "\"map\":{\"name\":\"Canal Crossing\"},"
    "\"game\":{\"kind\":\"skirmish\",\"tick\":6630,\"time\":\"3:41\",\"speed\":10},"
    "\"players\":[{\"side\":\"Arm\",\"colour\":0,\"team\":null},"
    "{\"side\":\"Core\",\"colour\":1,\"team\":null}],"
    "\"camera\":{\"x\":2620,\"z\":1480,\"zoom\":1.5,\"yaw\":0,\"pitch\":0},"
    "\"size\":{\"width\":2560,\"height\":1440,\"smooth\":1}}";

/// The pinned chunk list: kind, keyword and text, one chunk a group.
///
/// @param chunks the chunks
/// @return the bytes
std::string pinned_bytes(const std::vector<PhotoText>& chunks) {
    std::string pinned;
    for (const PhotoText& chunk : chunks) {
        pinned.push_back(chunk.kind == ChunkKind::latin1 ? 'L' : 'U');
        pinned.push_back('\n');
        pinned += chunk.keyword;
        pinned.push_back('\n');
        pinned += chunk.text;
        pinned.push_back('\n');
    }
    return pinned;
}

/// True when none of the chunks holds `needle`.
///
/// @param chunks the chunks
/// @param needle the text
/// @return true when it is absent
bool absent(const std::vector<PhotoText>& chunks, std::string_view needle) {
    for (const PhotoText& chunk : chunks) {
        if (chunk.keyword.find(needle) != std::string::npos)
            return false;
        if (chunk.text.find(needle) != std::string::npos)
            return false;
    }
    return true;
}

/// The fixed photo: two players, two teams, a UTF-8 name.
///
/// @param names whether the players are named
/// @return the facts
PhotoFacts sample(bool names) {
    PhotoFacts facts;
    facts.map = "Canal Crossing";
    facts.engine = "0.9.0";
    facts.mod_id = "ridge";
    facts.mod_version = "28";
    facts.kind = GameKind::skirmish;
    facts.tick = 6630;
    facts.speed = 10;
    facts.players.push_back(PlayerFact{"Stève", "Arm", 0, 1});
    facts.players.push_back(PlayerFact{"Core AI", "Core", 1, 2});
    facts.camera = CameraFact{2620, 1480, 1.5, 0, 0};
    facts.size = SizeFact{2560, 1440, 1};
    facts.created = UtcMoment{1994, 11, 6, 8, 49, 37};
    facts.player_names = names;
    facts.shots_folder = "/home/secret/Screenshots/default";
    facts.account = "acct-771";
    facts.host = "203.0.113.9";
    return facts;
}

/// Compares a chunk list with the pinned bytes, and prints both when they differ.
///
/// @param chunks the chunks
/// @param expected the pinned bytes
void check_pin(const std::vector<PhotoText>& chunks, std::string_view expected) {
    const std::string pinned = pinned_bytes(chunks);
    if (pinned != expected) {
        std::fputs("--- got ---\n", stderr);
        std::fwrite(pinned.data(), 1, pinned.size(), stderr);
        std::fputs("--- expected ---\n", stderr);
        std::fwrite(expected.data(), 1, expected.size(), stderr);
    }
    CHECK(pinned == expected);
}

} // namespace

int main() {
    const std::string names_on = std::string("U\nTitle\nCanal Crossing, 3:41\n") +
                                 "U\nDescription\nStève (Arm, team 1) vs Core AI (Core, team 2)\n"
                                 "L\nSoftware\nOpen Annihilation 0.9.0\n"
                                 "L\nCreation Time\nSun, 06 Nov 1994 08:49:37 GMT\n"
                                 "L\nSource\nSkirmish\n"
                                 "U\nOpenAnnihilation\n" +
                                 std::string(kJson) + "\n";
    const std::vector<PhotoText> named = photo_texts(sample(true));
    check_pin(named, names_on);
    CHECK(absent(named, "/home/secret/Screenshots/default"));
    CHECK(absent(named, "acct-771"));
    CHECK(absent(named, "203.0.113.9"));

    const std::string names_off = std::string("U\nTitle\nCanal Crossing, 3:41\n") +
                                  "U\nDescription\nArm, team 1 vs Core, team 2\n"
                                  "L\nSoftware\nOpen Annihilation 0.9.0\n"
                                  "L\nCreation Time\nSun, 06 Nov 1994 08:49:37 GMT\n"
                                  "L\nSource\nSkirmish\n"
                                  "U\nOpenAnnihilation\n" +
                                  std::string(kJsonUnnamed) + "\n";
    const std::vector<PhotoText> unnamed = photo_texts(sample(false));
    check_pin(unnamed, names_off);
    CHECK(absent(unnamed, "Stève"));
    CHECK(absent(unnamed, "Core AI"));
    CHECK(absent(unnamed, "/home/secret/Screenshots/default"));
    CHECK(absent(unnamed, "acct-771"));
    CHECK(absent(unnamed, "203.0.113.9"));

    PhotoFacts no_team = sample(true);
    no_team.players.clear();
    no_team.players.push_back(PlayerFact{"Player", "Arm", 0, std::nullopt});
    no_team.players.push_back(PlayerFact{"Core", "Core", 1, std::nullopt});
    const std::string no_team_on = std::string("U\nTitle\nCanal Crossing, 3:41\n") +
                                   "U\nDescription\nPlayer (Arm) vs Core (Core)\n"
                                   "L\nSoftware\nOpen Annihilation 0.9.0\n"
                                   "L\nCreation Time\nSun, 06 Nov 1994 08:49:37 GMT\n"
                                   "L\nSource\nSkirmish\n"
                                   "U\nOpenAnnihilation\n" +
                                   std::string(kJsonNoTeam) + "\n";
    const std::vector<PhotoText> open = photo_texts(no_team);
    check_pin(open, no_team_on);
    no_team.player_names = false;
    const std::string no_team_off = std::string("U\nTitle\nCanal Crossing, 3:41\n") +
                                    "U\nDescription\nArm vs Core\n"
                                    "L\nSoftware\nOpen Annihilation 0.9.0\n"
                                    "L\nCreation Time\nSun, 06 Nov 1994 08:49:37 GMT\n"
                                    "L\nSource\nSkirmish\n"
                                    "U\nOpenAnnihilation\n" +
                                    std::string(kJsonNoTeamUnnamed) + "\n";
    const std::vector<PhotoText> open_hidden = photo_texts(no_team);
    check_pin(open_hidden, no_team_off);
    CHECK(absent(open_hidden, "Player"));
    CHECK(open_hidden[1].text.find("team") == std::string::npos);

    JsonError error;
    const auto record = parse_json(named.size() == 6 ? named[5].text : "", error);
    CHECK(record.has_value());
    if (record.has_value()) {
        const Json* version = record->find("photo");
        CHECK(version != nullptr && version->integer().has_value() && *version->integer() == 1);
        const auto order = record->names();
        const char* expected_order[] = {
            "photo", "engine", "mod", "map", "game", "players", "camera", "size"
        };
        CHECK(order.size() == 8);
        for (std::size_t at = 0; at < order.size() && at < 8; ++at)
            CHECK(order[at] == expected_order[at]);
        const Json* players = record->find("players");
        CHECK(players != nullptr && players->type() == JsonType::array);
        if (players != nullptr && players->elements().size() == 2) {
            const Json* first = players->elements()[0].find("name");
            CHECK(first != nullptr && first->string() != nullptr && *first->string() == "Stève");
        }
    }
    const auto hidden = parse_json(unnamed.size() == 6 ? unnamed[5].text : "", error);
    CHECK(hidden.has_value());
    if (hidden.has_value()) {
        const Json* players = hidden->find("players");
        CHECK(players != nullptr);
        if (players != nullptr) {
            for (const Json& element : players->elements())
                CHECK(element.find("name") == nullptr);
        }
    }

    PhotoFacts later = sample(true);
    later.tick = 114630;
    const std::vector<PhotoText> hour = photo_texts(later);
    CHECK(hour.size() == 6 && hour[0].text == "Canal Crossing, 1:03:41");

    PhotoFacts other = sample(true);
    other.kind = GameKind::campaign;
    CHECK(photo_texts(other)[4].text == "Campaign");
    other.kind = GameKind::replay;
    CHECK(photo_texts(other)[4].text == "Replay");
    other.kind = GameKind::multiplayer;
    CHECK(photo_texts(other)[4].text == "Multiplayer");

    PhotoFacts code_page = sample(true);
    code_page.players[0].name = "caf";
    code_page.players[0].name.push_back(static_cast<char>(0xE9));
    const std::vector<PhotoText> decoded = photo_texts(code_page);
    std::string cafe = "caf";
    cafe.push_back(static_cast<char>(0xC3));
    cafe.push_back(static_cast<char>(0xA9));
    CHECK(decoded.size() == 6);
    if (decoded.size() == 6) {
        CHECK(decoded[1].text.find(cafe) != std::string::npos);
        CHECK(decoded[5].text.find(cafe) != std::string::npos);
        CHECK(absent(decoded, std::string(1, static_cast<char>(0xE9))));
    }

    PhotoFacts watched = sample(true);
    ReplayFact replay;
    replay.file = "/home/secret/replays/2026-11-04 Canal Crossing.oademo";
    replay.sha256 = "abc";
    replay.tick = 6630;
    watched.replay = replay;
    LookFact look;
    look.preset = "dusk";
    look.vignette = 40;
    look.grain = 20;
    watched.look = look;
    const std::vector<PhotoText> full = photo_texts(watched);
    CHECK(full.size() == 6);
    CHECK(absent(full, "/home/secret"));
    const auto full_record = parse_json(full.size() == 6 ? full[5].text : "", error);
    CHECK(full_record.has_value());
    if (full_record.has_value()) {
        const auto order = full_record->names();
        const char* expected_order[] = {
            "photo", "engine", "mod", "map", "game", "players", "replay", "camera", "look", "size"
        };
        CHECK(order.size() == 10);
        for (std::size_t at = 0; at < order.size() && at < 10; ++at)
            CHECK(order[at] == expected_order[at]);
        const Json* file = full_record->find("replay");
        CHECK(file != nullptr && file->find("file") != nullptr);
        if (file != nullptr && file->find("file") != nullptr &&
            file->find("file")->string() != nullptr)
            CHECK(*file->find("file")->string() == "2026-11-04 Canal Crossing.oademo");
        const Json* tilt = full_record->find("look");
        CHECK(tilt != nullptr && tilt->find("tilt_shift") != nullptr);
        if (tilt != nullptr && tilt->find("tilt_shift") != nullptr)
            CHECK(tilt->find("tilt_shift")->type() == JsonType::null);
    }

    PhotoFacts plain = sample(true);
    const auto plain_record = parse_json(photo_texts(plain)[5].text, error);
    CHECK(plain_record.has_value());
    if (plain_record.has_value()) {
        CHECK(plain_record->find("replay") == nullptr);
        CHECK(plain_record->find("look") == nullptr);
    }

    PhotoFacts hidden_replay = sample(false);
    ReplayFact named_file;
    named_file.file = "884 - 17.2.2009 - Caldera's Rim - Stève, Core AI.tad";
    named_file.sha256 = "abc";
    named_file.tick = 6630;
    hidden_replay.replay = named_file;
    const std::vector<PhotoText> quiet = photo_texts(hidden_replay);
    CHECK(absent(quiet, "Stève"));
    CHECK(absent(quiet, "Core AI"));
    CHECK(absent(quiet, "Caldera"));
    const auto quiet_record = parse_json(quiet.size() == 6 ? quiet[5].text : "", error);
    CHECK(quiet_record.has_value());
    if (quiet_record.has_value()) {
        const Json* played = quiet_record->find("replay");
        CHECK(played != nullptr && played->find("file") == nullptr);
        if (played != nullptr) {
            const auto replay_names = played->names();
            CHECK(replay_names.size() == 2);
            if (replay_names.size() == 2) {
                CHECK(replay_names[0] == "sha256");
                CHECK(replay_names[1] == "tick");
            }
            const Json* digest = played->find("sha256");
            const Json* replay_tick = played->find("tick");
            CHECK(digest != nullptr && digest->string() != nullptr && *digest->string() == "abc");
            CHECK(
                replay_tick != nullptr && replay_tick->integer().has_value() &&
                *replay_tick->integer() == 6630
            );
        }
    }

    if (failures != 0) {
        std::fprintf(stderr, "%d photo record checks failed\n", failures);
        return 1;
    }
    std::printf("photo record: pinned\n");
    return 0;
}

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The made-up brain profile-reader: AI profile lines, build lists and the
// seat's strategic strengths. --data checks every installed ai/*.txt file.
#include "match_fixture.hpp"

#include "internal.hpp"

#include "oa/data/defs/layout.hpp"
#include "oa/data/limits.hpp"
#include "oa/sim/ai.hpp"
#include "oa/sim/brains/check.hpp"
#include "oa/sim/brains/seat.hpp"
#include "oa/test/game_assets.hpp"

#include "lua.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace host = oa::data::script_host;
namespace brains = oa::sim::brains;

namespace {

struct HandLine {
    uint32_t number{};
    std::vector<std::string> words{};
    std::string command{};
    std::string kind{};
    bool any{};
    bool easy{};
    bool medium{};
    bool hard{};
    std::string name{};
    std::string third{};
    std::string unit{};
};

std::string sample_profile() {
    std::string text;
    text += "weight ARMSOLAR 0.1\n";
    text += "plan easy any\n";
    text += "weight all\n";
    text += "plan any easy\n";
    text += "limit core 3x\n";
    text += "plan\n";
    text += "limit ARMRELAY -1\n";
    text += "PLAN Hard\n";
    text += "weight #x 2\n";
    text += "// comment\n";
    text.append(140, 'Q');
    text += '\n';
    text += "a b c d e f g h i j k l m n o p q r s t u v w x y\n";
    text += "wei#ght 1";
    return text;
}

HandLine line_of(uint32_t number, std::vector<std::string> words, std::string command) {
    HandLine line;
    line.number = number;
    line.words = std::move(words);
    line.command = std::move(command);
    return line;
}

std::vector<HandLine> hand_lines() {
    std::vector<std::string> letters;
    for (int letter = 0; letter < 20; ++letter)
        letters.emplace_back(1, static_cast<char>('a' + letter));
    std::vector<HandLine> lines;
    lines.push_back(line_of(1, {"weight", "ARMSOLAR", "0.1"}, "weight"));
    lines.back().kind = "weight";
    lines.back().name = "ARMSOLAR";
    lines.back().third = "0.1";
    lines.back().unit = "true";
    lines.push_back(line_of(2, {"plan", "easy", "any"}, "plan"));
    lines.back().kind = "plan";
    lines.back().easy = true;
    lines.push_back(line_of(3, {"weight", "all"}, "weight"));
    lines.back().kind = "weight";
    lines.back().name = "all";
    lines.back().unit = "false";
    lines.push_back(line_of(4, {"plan", "any", "easy"}, "plan"));
    lines.back().kind = "plan";
    lines.back().any = true;
    lines.back().easy = true;
    lines.push_back(line_of(5, {"limit", "core", "3x"}, "limit"));
    lines.back().kind = "limit";
    lines.back().name = "core";
    lines.back().third = "3x";
    lines.back().unit = "false";
    lines.push_back(line_of(6, {"plan"}, "plan"));
    lines.back().kind = "plan";
    lines.push_back(line_of(7, {"limit", "ARMRELAY", "-1"}, "limit"));
    lines.back().kind = "limit";
    lines.back().name = "ARMRELAY";
    lines.back().third = "-1";
    lines.back().unit = "true";
    lines.push_back(line_of(8, {"PLAN", "Hard"}, "plan"));
    lines.back().kind = "plan";
    lines.back().hard = true;
    lines.push_back(line_of(9, {"weight"}, "weight"));
    lines.back().kind = "weight";
    lines.back().unit = "false";
    lines.push_back(line_of(10, {"//", "comment"}, "//"));
    lines.push_back(line_of(11, {std::string(126, 'Q')}, std::string(126, 'q')));
    lines.push_back(line_of(12, std::move(letters), "a"));
    lines.push_back(line_of(13, {"wei"}, "wei"));
    return lines;
}

lua_Integer bits_of(const char* word) {
    const float value = static_cast<float>(std::strtod(word == nullptr ? "" : word, nullptr));
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    return static_cast<lua_Integer>(bits);
}

int32_t count_of(const char* word) {
    return static_cast<int32_t>(std::strtol(word == nullptr ? "" : word, nullptr, 10));
}

std::string flag_text(bool value) {
    return value ? "true" : "false";
}

std::string expected_dump(const std::vector<HandLine>& lines) {
    std::string out;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index != 0)
            out += '\n';
        const HandLine& line = lines[index];
        out += std::to_string(line.number);
        out += '|';
        for (std::size_t word = 0; word < line.words.size(); ++word) {
            if (word != 0)
                out += '\t';
            out += line.words[word];
        }
        out += '|';
        out += line.command;
        if (line.kind == "plan") {
            out += "|any=" + flag_text(line.any) + "|easy=" + flag_text(line.easy) +
                   "|medium=" + flag_text(line.medium) + "|hard=" + flag_text(line.hard);
        } else if (line.kind == "weight") {
            out += "|name=" + line.name + "|factor=" + std::to_string(bits_of(line.third.c_str())) +
                   "|unit=" + line.unit;
        } else if (line.kind == "limit") {
            out += "|name=" + line.name + "|count=" + std::to_string(count_of(line.third.c_str())) +
                   "|unit=" + line.unit;
        }
    }
    return out;
}

struct Gathered {
    std::vector<uint32_t> numbers{};
    std::vector<std::vector<std::string>> words{};
};

void gather_line(void* context, const oa::sim::ai::ProfileLine& line) {
    auto* gathered = static_cast<Gathered*>(context);
    gathered->numbers.push_back(line.number);
    std::vector<std::string> words;
    for (uint32_t index = 0; index < line.count; ++index) {
        const char* word = line.words != nullptr ? line.words[index] : nullptr;
        words.emplace_back(word == nullptr ? "" : word);
    }
    gathered->words.push_back(std::move(words));
}

void require_sample_lines(const std::string& text, const std::vector<HandLine>& lines) {
    Gathered gathered;
    oa::sim::ai::computer_profile_lines(text, gather_line, &gathered);
    CHECK(gathered.numbers.size() == lines.size());
    for (std::size_t index = 0; index < lines.size(); ++index) {
        CHECK(gathered.numbers[index] == lines[index].number);
        CHECK(gathered.words[index] == lines[index].words);
        if (lines[index].kind == "weight") {
            const char* third = lines[index].words.size() >= 3 ? lines[index].words[2].c_str() : "";
            CHECK(bits_of(third) == bits_of(lines[index].third.c_str()));
        }
        if (lines[index].kind == "limit") {
            const char* third = lines[index].words.size() >= 3 ? lines[index].words[2].c_str() : "";
            CHECK(count_of(third) == count_of(lines[index].third.c_str()));
        }
    }
    CHECK(count_of("3x") == 3);
    CHECK(count_of("-1") == -1);
    CHECK(count_of("") == 0);
}

std::string load_text(const char* name) {
    const std::string path = std::string(OA_PROFILE_READER_DIR) + "/" + name;
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("missing " + path);
    std::string text;
    std::string line;
    while (std::getline(input, line)) {
        text.append(line);
        text.push_back('\n');
    }
    CHECK(!text.empty());
    return text;
}

brains::Seat* seat_for(Fixture& fixture, uint8_t which) {
    const auto& held = fixture.match->seat_state();
    CHECK(static_cast<bool>(held));
    auto* seated = static_cast<brains::Brains*>(held.get());
    CHECK(seated != nullptr);
    CHECK(seated->seats[which] != nullptr);
    return seated->seats[which].get();
}

brains::SeatSpec spec_of() {
    brains::SeatSpec spec;
    spec.player = computer;
    spec.id = "profile-reader";
    spec.name = "Profile Reader";
    spec.version = "1";
    spec.entry = "brain.lua";
    spec.level = "steady";
    spec.difficulty = OA_DIFFICULTY_MEDIUM;
    return spec;
}

void fill_files(
    const std::string& script, const std::string& manifest, brains::PackageFile* files
) {
    files[0].path = "brain.lua";
    files[0].bytes =
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(script.data()), script.size()};
    files[1].path = "oabrain.yaml";
    files[1].bytes = std::span<const uint8_t>{
        reinterpret_cast<const uint8_t*>(manifest.data()), manifest.size()
    };
}

void seat_reader(Fixture& fixture, const std::string& script, const std::string& manifest) {
    brains::PackageFile files[2];
    fill_files(script, manifest, files);
    brains::SeatSpec spec = spec_of();
    spec.files = files;
    const brains::SeatOutcome outcome = brains::seat_brain(*fixture.match, spec);
    if (outcome.result != brains::SeatResult::seated) {
        std::string why(outcome.reason.data());
        if (!outcome.error.message.empty())
            why += outcome.error.message;
        throw std::runtime_error(why.empty() ? "seat failed" : why);
    }
}

oa::data::brain_facts::GameFacts
facts_of(const std::string& profile_text, const std::string& report) {
    oa::data::brain_facts::GameFacts facts;
    facts.ai_profile.path = "ai/Sample.txt";
    facts.ai_profile.text = profile_text;
    facts.ai_profile_report_path = report;
    oa::data::brain_facts::ProfileFile alpha;
    alpha.path = "ai/alpha.txt";
    alpha.text = "limit ARMRELAY 2\n";
    oa::data::brain_facts::ProfileFile mid;
    mid.path = "ai/mid.txt";
    mid.text = "weight core 4\n";
    mid.text.append(65537, 'x');
    oa::data::brain_facts::ProfileFile zeta;
    zeta.path = "ai/zeta.txt";
    zeta.text = "plan any\n";
    facts.ai_profiles.push_back(std::move(alpha));
    facts.ai_profiles.push_back(std::move(mid));
    facts.ai_profiles.push_back(std::move(zeta));
    return facts;
}

std::string global_string(lua_State* lua, const char* name) {
    lua_getglobal(lua, name);
    std::size_t length = 0;
    const char* text = lua_tolstring(lua, -1, &length);
    std::string out = text != nullptr ? std::string(text, length) : std::string();
    lua_pop(lua, 1);
    return out;
}

lua_Integer global_integer(lua_State* lua, const char* name) {
    lua_getglobal(lua, name);
    CHECK(lua_isinteger(lua, -1));
    const lua_Integer value = lua_tointeger(lua, -1);
    lua_pop(lua, 1);
    return value;
}

void require_start(
    lua_State* lua,
    const std::string& profile_text,
    const std::string& report,
    lua_Integer cap,
    const char* downloads,
    const std::string& dump
) {
    CHECK(global_string(lua, "seen_text") == profile_text);
    CHECK(global_string(lua, "seen_name") == "Sample");
    CHECK(global_string(lua, "seen_path") == "ai/Sample.txt");
    CHECK(global_string(lua, "seen_report") == report);
    CHECK(global_integer(lua, "seen_cap") == cap);
    CHECK(global_string(lua, "seen_downloads") == downloads);
    CHECK(global_string(lua, "seen_lines") == dump);
    CHECK(global_string(lua, "seen_medium_w") == "1");
    CHECK(global_string(lua, "seen_medium_l") == "5");
    CHECK(global_string(lua, "seen_easy_w") == "1,3");
    CHECK(global_string(lua, "seen_easy_l") == "5");
    CHECK(global_string(lua, "seen_hard_w") == "1,9");
    CHECK(global_string(lua, "seen_hard_l") == "5");
    CHECK(global_string(lua, "seen_parsed_name") == "core");
    CHECK(global_integer(lua, "seen_parsed_bits") == bits_of("0.5"));
    CHECK(global_string(lua, "seen_parsed_unit") == "false");
    CHECK(global_string(lua, "seen_parsed_path") == "nil");
    CHECK(global_string(lua, "seen_before") == "nil");
    CHECK(global_integer(lua, "seen_profile_count") == 3);
    CHECK(global_string(lua, "seen_p1") == "ai/alpha.txt|alpha|limit");
    CHECK(global_string(lua, "seen_p2_large") == "true");
    CHECK(global_string(lua, "seen_p2_text") == "nil");
    CHECK(global_string(lua, "seen_p2_name") == "core");
    CHECK(global_string(lua, "seen_p3") == "zeta");
}

int push_after(void* context, lua_State* lua) {
    auto* seat = static_cast<brains::Seat*>(context);
    if (seat == nullptr)
        return luaL_error(lua, "missing seat");
    lua_rawgeti(lua, LUA_REGISTRYINDEX, seat->hooks_ref);
    lua_getfield(lua, -1, "after");
    lua_remove(lua, -2);
    return 1;
}

void call_after(Fixture& fixture) {
    brains::Seat* seat = seat_for(fixture, computer);
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    host::ScriptError error;
    host::CallHooks hooks;
    hooks.context = seat;
    hooks.push = push_after;
    if (!sandbox->call("after", hooks, error))
        throw std::runtime_error(error.message.empty() ? "after failed" : error.message);
}

std::string list_text(const oa::sim::ai::ComputerPlayers* players) {
    std::string out;
    for (uint16_t type = 1; type < type_count; ++type) {
        if (type != 1)
            out += ';';
        out += std::to_string(type);
        out += '=';
        const oa::sim::ai::ComputerType& record = players->types[type];
        for (uint32_t index = 0; index < record.build_count; ++index) {
            if (index != 0)
                out += ',';
            out += std::to_string(record.build_ids[index]);
        }
    }
    return out;
}

std::string strength_text(const Fixture& fixture) {
    const auto& table = fixture.match->strategic_state(computer).strengths;
    CHECK(table.size() > collector);
    std::string out;
    for (std::size_t index = 0; index < 3; ++index) {
        if (index != 0)
            out += ',';
        const auto stored = static_cast<int8_t>(table[collector][index]);
        out += std::to_string(static_cast<int>(stored));
    }
    return out;
}

void append_run(std::vector<uint16_t>& runs, const std::vector<uint16_t>& ids) {
    runs.push_back(static_cast<uint16_t>(ids.size()));
    runs.insert(runs.end(), ids.begin(), ids.end());
}

void play(
    const std::string& script,
    const std::string& manifest,
    const std::string& profile_text,
    const std::string& dump,
    const std::vector<uint16_t>& runs,
    const data::limits::Limits& limits,
    bool campaign,
    const std::string& report,
    lua_Integer cap,
    const char* downloads,
    uint32_t vehicle_kept
) {
    Fixture fixture(0, true, {}, 0, limits);
    fixture.spawn(computer, vehicle, 40, 40);
    std::snprintf(
        fixture.match->state().unit_defs[collector].ai_weight,
        sizeof fixture.match->state().unit_defs[collector].ai_weight,
        "weight core 0.5\n"
    );
    CHECK(
        oa::sim::ai::configure_match_computer_players(*fixture.match, profile_text, runs, campaign)
    );
    const oa::sim::ai::ComputerPlayers* players =
        oa::sim::ai::match_computer_players(*fixture.match);
    CHECK(players->profile_text != nullptr);
    CHECK(players->profile_length == profile_text.size());
    CHECK(std::memcmp(players->profile_text, profile_text.data(), profile_text.size()) == 0);
    CHECK(fixture.match->strategic_state(computer).strengths.empty());
    brains::set_match_game_facts(*fixture.match, facts_of(profile_text, report));
    seat_reader(fixture, script, manifest);
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    require_start(sandbox->state(), profile_text, report, cap, downloads, dump);
    fixture.match->tick();
    call_after(fixture);
    players = oa::sim::ai::match_computer_players(*fixture.match);
    CHECK(players->types[vehicle].build_count == vehicle_kept);
    CHECK(global_string(sandbox->state(), "seen_lists") == list_text(players));
    CHECK(global_string(sandbox->state(), "seen_strength") == strength_text(fixture));
    fixture.match->refresh_strategic_state(computer);
    call_after(fixture);
    players = oa::sim::ai::match_computer_players(*fixture.match);
    CHECK(global_string(sandbox->state(), "seen_lists") == list_text(players));
    CHECK(global_string(sandbox->state(), "seen_strength") == strength_text(fixture));
}

void check_outside(
    const std::string& script, const std::string& manifest, const std::string& profile_text
) {
    brains::PackageFile files[2];
    fill_files(script, manifest, files);
    brains::SeatSpec spec = spec_of();
    spec.files = files;
    const oa::data::brain_facts::GameFacts facts = facts_of(profile_text, "");
    const brains::SeatOutcome outcome = brains::check_brain(spec, facts);
    if (outcome.result != brains::SeatResult::seated) {
        std::string why(outcome.reason.data());
        if (!outcome.error.message.empty())
            why += outcome.error.message;
        throw std::runtime_error(why.empty() ? "outside fit failed" : why);
    }
}

bool same_ascii(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        unsigned char a = static_cast<unsigned char>(left[index]);
        unsigned char b = static_cast<unsigned char>(right[index]);
        if (a >= 'A' && a <= 'Z')
            a = static_cast<unsigned char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = static_cast<unsigned char>(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

bool is_space(char letter) {
    return letter == ' ' || letter == '\t' || letter == '\r' || letter == '\n' || letter == '\v' ||
           letter == '\f';
}

/// The test's own restatement of the profile storage rules, so an installed
/// file is not checked by asking the walker to agree with itself.
std::vector<std::string> split_words(std::string_view line) {
    constexpr std::size_t word_cap = 20;
    constexpr std::size_t byte_cap = 0x7e;
    constexpr std::size_t storage_cap = byte_cap + word_cap;
    std::vector<std::string> words;
    std::string storage;
    std::size_t index = 0;
    while (index < line.size()) {
        while (index < line.size() && is_space(line[index]))
            ++index;
        if (index >= line.size() || line[index] == '#')
            break;
        std::string word;
        while (index < line.size() && !is_space(line[index]) && line[index] != '#' &&
               storage.size() < byte_cap) {
            word.push_back(line[index]);
            storage.push_back(line[index]);
            ++index;
        }
        while (index < line.size() && !is_space(line[index]) && line[index] != '#')
            ++index;
        if (words.size() < word_cap)
            words.push_back(std::move(word));
        storage.push_back('\0');
        if (storage.size() >= storage_cap)
            break;
    }
    return words;
}

struct RawLine {
    uint32_t number{};
    std::vector<std::string> words{};
};

std::vector<RawLine> split_profile(std::string_view text) {
    std::vector<RawLine> lines;
    if (text.empty())
        return lines;
    const char* cursor = text.data();
    const char* const end = text.data() + text.size();
    uint32_t number = 1;
    for (;;) {
        const char* line_end = cursor;
        while (line_end < end && *line_end != '\n')
            ++line_end;
        std::vector<std::string> words =
            split_words(std::string_view(cursor, static_cast<std::size_t>(line_end - cursor)));
        if (!words.empty())
            lines.push_back(RawLine{number, std::move(words)});
        if (line_end == end)
            break;
        cursor = line_end + 1;
        ++number;
    }
    return lines;
}

struct Walked {
    std::vector<RawLine> lines{};
};

void walk_installed(void* context, const oa::sim::ai::ProfileLine& line) {
    auto* walked = static_cast<Walked*>(context);
    RawLine stored;
    stored.number = line.number;
    for (uint32_t index = 0; index < line.count; ++index) {
        const char* word = line.words != nullptr ? line.words[index] : nullptr;
        stored.words.emplace_back(word == nullptr ? "" : word);
    }
    walked->lines.push_back(std::move(stored));
}

void check_installed() {
    const oa::AssetStore assets = oa::test::require_game_assets("the installed AI profiles");
    const char* folder = oa::data::defs::directory_name(oa::data::defs::DataDirectory::ai);
    CHECK(folder != nullptr);
    const std::vector<std::string> names = assets.list_effective(folder, ".txt");
    bool saw_default = false;
    int weight_checks = 0;
    int limit_checks = 0;
    for (const std::string& name : names) {
        const std::size_t slash = name.find_last_of("/\\");
        const std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
        if (same_ascii(base, "default.txt"))
            saw_default = true;
        const std::optional<std::vector<uint8_t>> bytes = assets.load_file_contents(name);
        if (!bytes)
            continue;
        const std::string_view text{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
        const std::vector<RawLine> raw = split_profile(text);
        Walked walked;
        oa::sim::ai::computer_profile_lines(text, walk_installed, &walked);
        CHECK(walked.lines.size() == raw.size());
        for (std::size_t index = 0; index < raw.size(); ++index) {
            CHECK(walked.lines[index].number == raw[index].number);
            CHECK(walked.lines[index].words == raw[index].words);
            const bool weight =
                !raw[index].words.empty() && same_ascii(raw[index].words[0], "weight");
            const bool limit =
                !raw[index].words.empty() && same_ascii(raw[index].words[0], "limit");
            if (!weight && !limit)
                continue;
            static const std::string no_word;
            const std::string& third = raw[index].words.size() >= 3 ? raw[index].words[2] : no_word;
            const std::string& stored =
                walked.lines[index].words.size() >= 3 ? walked.lines[index].words[2] : no_word;
            CHECK(stored == third);
            if (same_ascii(raw[index].words[0], "weight")) {
                const float from_raw = static_cast<float>(std::strtod(third.c_str(), nullptr));
                const float from_stored = static_cast<float>(std::strtod(stored.c_str(), nullptr));
                CHECK(from_raw == from_stored);
                ++weight_checks;
            } else {
                const auto from_raw = static_cast<int32_t>(std::strtol(third.c_str(), nullptr, 10));
                const auto from_stored =
                    static_cast<int32_t>(std::strtol(stored.c_str(), nullptr, 10));
                CHECK(from_raw == from_stored);
                ++limit_checks;
            }
        }
    }
    CHECK(saw_default);
    CHECK(weight_checks > 0);
    CHECK(limit_checks > 0);
}

} // namespace

int main(int argc, char** argv) {
    bool data = false;
    for (int index = 1; index < argc; ++index) {
        if (argv[index] != nullptr && std::strcmp(argv[index], "--data") == 0)
            data = true;
    }
    if (data) {
        check_installed();
        return 0;
    }
    const std::string profile_text = sample_profile();
    const std::vector<HandLine> lines = hand_lines();
    require_sample_lines(profile_text, lines);
    const std::string dump = expected_dump(lines);
    const std::string script = load_text("brain.lua");
    const std::string manifest = load_text("oabrain.yaml");
    check_outside(script, manifest, profile_text);

    std::vector<uint16_t> short_runs;
    append_run(short_runs, {});
    append_run(short_runs, std::vector<uint16_t>(40, collector));
    append_run(short_runs, {});
    append_run(short_runs, {});
    play(
        script, manifest, profile_text, dump, short_runs, {}, true, "ai/Hover.txt", 31, "false", 31
    );

    data::limits::Limits raised;
    raised.build_lists.copy = 40;
    std::vector<uint16_t> full_runs = short_runs;
    append_run(full_runs, {missile});
    append_run(full_runs, {});
    play(script, manifest, profile_text, dump, full_runs, raised, false, "", 41, "true", 40);
    return 0;
}

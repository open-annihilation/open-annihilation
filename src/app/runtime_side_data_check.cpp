// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Each match reads gamedata/sidedata.tdf again. A file added under --mod-dir
// between matches changes the commanders that match plays; the menus keep
// the sides the run started with.
#include "oa/app/runtime.hpp"

#include "oa/ui/frontend_dialogs.hpp"

#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("side data per match check: " + what);
}

std::string field_text(const char* field, std::size_t size) {
    return {field, strnlen(field, size)};
}

bool same_name(std::string_view known, std::string_view candidate) {
    if (known.size() != candidate.size())
        return false;
    for (std::size_t at = 0; at < known.size(); ++at) {
        const auto known_letter = static_cast<unsigned char>(known[at]);
        const auto candidate_letter = static_cast<unsigned char>(candidate[at]);
        if (std::tolower(known_letter) != std::tolower(candidate_letter))
            return false;
    }
    return true;
}

bool side_header_at(std::string_view text, std::size_t at, std::optional<uint32_t> side) {
    constexpr std::string_view token = "[side";
    if (at + token.size() >= text.size())
        return false;
    for (std::size_t i = 0; i < token.size(); ++i) {
        const auto letter = static_cast<unsigned char>(text[at + i]);
        if (std::tolower(letter) != static_cast<unsigned char>(token[i]))
            return false;
    }
    std::size_t cursor = at + token.size();
    if (cursor >= text.size() || std::isdigit(static_cast<unsigned char>(text[cursor])) == 0)
        return false;
    uint32_t value = 0;
    while (cursor < text.size() && std::isdigit(static_cast<unsigned char>(text[cursor])) != 0) {
        value = value * 10U + static_cast<uint32_t>(text[cursor] - '0');
        ++cursor;
    }
    if (cursor >= text.size() || text[cursor] != ']')
        return false;
    return !side || *side == value;
}

/// Replaces one side's commander value and leaves every other byte as it was.
std::string replace_commander(std::string text, uint32_t side, std::string_view commander) {
    std::size_t section = std::string::npos;
    for (std::size_t at = 0; at < text.size(); ++at) {
        if (side_header_at(text, at, side)) {
            section = at;
            break;
        }
    }
    if (section == std::string::npos)
        fail("gamedata/sidedata.tdf has no section for side " + std::to_string(side));
    std::size_t next = text.size();
    for (std::size_t at = section + 1; at < text.size(); ++at) {
        if (side_header_at(text, at, std::nullopt)) {
            next = at;
            break;
        }
    }
    constexpr std::string_view key = "commander";
    for (std::size_t at = section; at + key.size() < next; ++at) {
        bool key_here = true;
        for (std::size_t i = 0; i < key.size(); ++i) {
            const auto letter = static_cast<unsigned char>(text[at + i]);
            if (std::tolower(letter) != static_cast<unsigned char>(key[i]))
                key_here = false;
        }
        if (!key_here)
            continue;
        if (at > section) {
            const auto before = static_cast<unsigned char>(text[at - 1]);
            if (std::isalnum(before) != 0 || before == '_')
                continue;
        }
        std::size_t equals = at + key.size();
        while (equals < next && std::isspace(static_cast<unsigned char>(text[equals])) != 0)
            ++equals;
        if (equals >= next || text[equals] != '=')
            continue;
        ++equals;
        while (equals < next && std::isspace(static_cast<unsigned char>(text[equals])) != 0)
            ++equals;
        std::size_t end = equals;
        while (end < next) {
            const auto letter = static_cast<unsigned char>(text[end]);
            if (text[end] == ';' || text[end] == '}' || text[end] == '\n' || text[end] == '\r' ||
                std::isspace(letter) != 0)
                break;
            ++end;
        }
        text.replace(equals, end - equals, commander);
        return text;
    }
    fail("side " + std::to_string(side) + " names no commander");
}

/// The side data with every [SIDEn] section past SIDE0 left out.
std::string side0_only(std::string_view text) {
    std::string kept;
    kept.reserve(text.size());
    std::size_t cursor = 0;
    while (cursor < text.size()) {
        std::size_t header = std::string::npos;
        for (std::size_t at = cursor; at < text.size(); ++at) {
            if (side_header_at(text, at, std::nullopt)) {
                header = at;
                break;
            }
        }
        if (header == std::string::npos) {
            kept.append(text.substr(cursor));
            break;
        }
        std::size_t next = text.size();
        for (std::size_t at = header + 1; at < text.size(); ++at) {
            if (side_header_at(text, at, std::nullopt)) {
                next = at;
                break;
            }
        }
        if (side_header_at(text, header, 0))
            kept.append(text.substr(cursor, next - cursor));
        else
            kept.append(text.substr(cursor, header - cursor));
        cursor = next;
    }
    return kept;
}

void write_text(const fs::path& path, std::string_view text) {
    std::error_code io_error;
    fs::create_directories(path.parent_path(), io_error);
    if (io_error)
        fail("could not make the mod folder's gamedata directory");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        fail("could not write gamedata/sidedata.tdf");
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out)
        fail("could not write gamedata/sidedata.tdf");
}

/// Removes the side data file the check wrote, and the gamedata folder it made,
/// however the check ends, so a failed run leaves the mod folder as it found it.
class WrittenSideData {
  public:

    WrittenSideData(fs::path file, bool made_folder)
        : file_(std::move(file)), made_folder_(made_folder) {}

    WrittenSideData(const WrittenSideData&) = delete;
    WrittenSideData& operator=(const WrittenSideData&) = delete;

    ~WrittenSideData() {
        std::error_code ignored;
        fs::remove(file_, ignored);
        if (made_folder_ && fs::is_empty(file_.parent_path(), ignored))
            fs::remove(file_.parent_path(), ignored);
    }

  private:

    fs::path file_;
    bool made_folder_;
};

} // namespace

void Runtime::check_side_data_per_match() {
    if (options_.mod_dir.empty())
        fail("needs --mod-dir");
    const fs::path mod_sidedata = options_.mod_dir / "gamedata" / "sidedata.tdf";
    std::error_code io_error;
    if (fs::exists(mod_sidedata, io_error))
        fail("needs a mod folder with no gamedata/sidedata.tdf");
    if (io_error)
        fail("could not look in the mod folder");
    // The check writes into the mod folder, never into the game folder it layers over.
    std::size_t roots_at_mod_dir = 0;
    for (const auto& root : assets_.loose_roots()) {
        std::error_code same_error;
        if (fs::equivalent(root, options_.mod_dir, same_error))
            ++roots_at_mod_dir;
    }
    std::error_code game_error;
    if (roots_at_mod_dir > 1 || (!options_.game_dir.empty() &&
                                 fs::equivalent(options_.game_dir, options_.mod_dir, game_error)))
        fail("needs a mod folder that is not the game folder");
    std::error_code folder_error;
    const bool made_folder = !fs::exists(mod_sidedata.parent_path(), folder_error);
    const WrittenSideData cleanup(mod_sidedata, made_folder);
    if (side_table_.count < 2)
        fail("the run lists fewer than two sides");

    const auto commander_of = [](const oa::data::defs::SideTable& table, uint32_t side_index) {
        const auto& name = table.sides[side_index].commander;
        return field_text(name, sizeof name);
    };
    std::vector<std::string> run_commanders;
    run_commanders.reserve(side_table_.count);
    for (uint32_t side_index = 0; side_index < side_table_.count; ++side_index)
        run_commanders.push_back(commander_of(side_table_, side_index));
    const int32_t setup_side_count = skirmish_ui_.side_count;
    const auto played_slot = skirmish_settings_.slots[0].side;
    if (played_slot < 0 || static_cast<uint32_t>(played_slot) >= run_commanders.size())
        fail("slot 0 plays no side");
    const auto played_side = static_cast<uint32_t>(played_slot);
    std::optional<uint32_t> other_side;
    for (uint32_t side_index = 0; side_index < run_commanders.size(); ++side_index) {
        if (side_index != played_side &&
            run_commanders[side_index] != run_commanders[played_side]) {
            other_side = side_index;
            break;
        }
    }
    if (!other_side)
        fail("needs another side with a different commander");

    const auto run_table_in_place = [&](std::string_view where) {
        if (run_side_table_)
            fail(std::string(where) + ": a match is still playing other sides");
        if (skirmish_ui_.side_count != setup_side_count)
            fail(std::string(where) + ": the skirmish setup's side count changed");
        for (uint32_t side_index = 0; side_index < run_commanders.size(); ++side_index) {
            if (commander_of(side_table_, side_index) != run_commanders[side_index])
                fail(
                    std::string(where) + ": side " + std::to_string(side_index) +
                    " no longer names " + run_commanders[side_index]
                );
        }
    };
    const auto player_commander = [&] {
        if (!match_)
            fail("no match is playing");
        oa::World& world = match_->state();
        for (const auto& slot : match_->world().slots) {
            if (slot.unit == nullptr || slot.record.type_index == 0 || slot.record.owner_index != 0)
                continue;
            const oa::UnitDef* definition = oa::world_unit_def_of(&world, &slot.record);
            if (definition == nullptr)
                continue;
            return field_text(definition->unit_name, sizeof definition->unit_name);
        }
        fail("player 0 has no commander");
    };
    const auto game_commander = [&](uint32_t side_index) {
        const auto& name = match_->state().game.sides[side_index].commander;
        return field_text(name, sizeof name);
    };
    const auto leave_for_menu = [&] {
        while (oa::ui::frontend_dialogs::dialog_count() != 0)
            oa::ui::frontend_dialogs::close_dialog();
        if (match_)
            leave_match();
        load(Screen::main_menu);
    };
    const auto expect_refused = [&](std::string_view part) {
        status_.clear();
        std::string thrown;
        bool entered = false;
        try {
            start_benchmark_skirmish();
            entered = true;
        } catch (const std::runtime_error& error) {
            thrown = error.what();
        }
        const bool said =
            status_.find(part) != std::string::npos || thrown.find(part) != std::string::npos;
        if (entered || !said) {
            fail(std::string(part) + " did not refuse the start (" + thrown + "; " + status_ + ")");
        }
        run_table_in_place(part);
        leave_for_menu();
    };

    start_benchmark_skirmish();
    if (run_side_table_)
        fail("the first match parsed a new side table");
    if (game_commander(played_side) != run_commanders[played_side] ||
        !same_name(player_commander(), run_commanders[played_side]))
        fail("the first match did not play the run's commander");
    leave_for_menu();
    run_table_in_place("after the first match");

    const auto run_bytes = assets_.read(side_data_path());
    const std::string run_text(
        reinterpret_cast<const char*>(run_bytes.bytes.data()), run_bytes.bytes.size()
    );
    write_text(mod_sidedata, replace_commander(run_text, played_side, run_commanders[*other_side]));
    assets_.mark_loose_shadows();
    start_benchmark_skirmish();
    if (!run_side_table_ ||
        commander_of(*run_side_table_, played_side) != run_commanders[played_side])
        fail("the second match did not keep the run's sides aside");
    if (game_commander(played_side) != run_commanders[*other_side] ||
        commander_of(side_table_, played_side) != run_commanders[*other_side] ||
        !same_name(player_commander(), run_commanders[*other_side]))
        fail("the second match did not play the other side's commander");
    if (skirmish_ui_.side_count != setup_side_count)
        fail("the second match changed the skirmish setup's side count");
    leave_for_menu();
    run_table_in_place("after the second match");

    write_text(mod_sidedata, "[NOTE]\n{\ninfo=none;\n}\n");
    assets_.mark_loose_shadows();
    expect_refused("contains no side definitions");

    write_text(mod_sidedata, replace_commander(run_text, played_side, "NOTACOMMANDER"));
    assets_.mark_loose_shadows();
    expect_refused("side commander is absent from unit catalog");

    std::array<int32_t, entry::skirmish_slot_capacity> saved_sides{};
    for (std::size_t index = 0; index < saved_sides.size(); ++index)
        saved_sides[index] = skirmish_settings_.slots[index].side;
    skirmish_settings_.slots[0].side = 1;
    skirmish_settings_.slots[1].side = 0;
    write_text(mod_sidedata, side0_only(run_text));
    assets_.mark_loose_shadows();
    start_benchmark_skirmish();
    if (side_table_.count != 1)
        fail("the one-side match did not load a single side");
    if (skirmish_settings_.slots[0].side != 0)
        fail("slot 0 stayed on a side the match does not have");
    if (!same_name(player_commander(), run_commanders[0]))
        fail("player 0 did not play the first side's commander");
    leave_for_menu();
    run_table_in_place("after a side past the match's side data");
    for (std::size_t index = 0; index < saved_sides.size(); ++index)
        skirmish_settings_.slots[index].side = saved_sides[index];

    io_error.clear();
    if (!fs::remove(mod_sidedata, io_error) || io_error)
        fail("could not remove gamedata/sidedata.tdf");
    assets_.mark_loose_shadows();
    start_benchmark_skirmish();
    if (run_side_table_)
        fail("the last match parsed a new side table");
    if (game_commander(played_side) != run_commanders[played_side] ||
        commander_of(side_table_, played_side) != run_commanders[played_side] ||
        !same_name(player_commander(), run_commanders[played_side]))
        fail("the last match did not play the run's commander");
    leave_for_menu();
    run_table_in_place("after the last match");
    std::cout << "side data per match check: passed\n";
}

} // namespace oa::app

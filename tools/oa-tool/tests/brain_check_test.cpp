// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// oa-tool brain check, run in-process. Without a game: usage, a file of
// another kind, a package the file checks refuse, and a missing game folder.
// With the installed game: seating, a decline, a start that fails, a start
// that runs past its allowance, a mod the manifest names, the level, the
// JSON keys, and the same report from a folder and its package.

#include "command.hpp"

#include "oa/formats/zip.hpp"
#include "oa/test/check.hpp"
#include "oa/test/game_data.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace zip = oa::formats::zip;

struct Scratch {
    fs::path path;

    Scratch() : path(oa::test::make_scratch_directory("T01-oa-tool-brain-check")) {}

    ~Scratch() {
        std::error_code error;
        fs::remove_all(path, error);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

struct Captured {
    int status = 0;
    std::string out;
    std::string err;
};

Captured run(std::vector<std::string> arguments) {
    std::ostringstream out;
    std::ostringstream err;
    oa::tool::Output output{out, err};
    const int status = oa::tool::run_tool(arguments, output);
    return {status, out.str(), err.str()};
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

void write_text(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    OA_CHECK(static_cast<bool>(out));
}

void write_bytes(const fs::path& path, std::span<const uint8_t> bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    OA_CHECK(static_cast<bool>(out));
}

std::string manifest_text(std::string_view id, std::string_view works) {
    return std::string(
               "oabrain: 1\n"
               "id: "
           ) +
           std::string(id) +
           "\n"
           "name: \"Steady Commander\"\n"
           "version: \"1.2\"\n"
           "author:\n"
           "  name: \"A. Maker\"\n"
           "description: \"Holds the line.\"\n"
           "tags: [\"classic\"]\n"
           "works-with: [" +
           std::string(works) +
           "]\n"
           "levels:\n"
           "  - id: firm\n"
           "    name: \"Firm\"\n"
           "    difficulty: medium\n"
           "entry: \"brain.lua\"\n";
}

fs::path write_brain(
    const fs::path& scratch, std::string_view id, std::string_view works, std::string_view lua
) {
    const fs::path folder = scratch / std::string(id);
    write_text(folder / "oabrain.yaml", manifest_text(id, works));
    write_text(folder / "brain.lua", lua);
    return folder;
}

void zip_folder(const fs::path& folder, const fs::path& file) {
    std::vector<std::string> names;
    std::vector<std::vector<uint8_t>> blobs;
    for (fs::recursive_directory_iterator cursor(folder), end; cursor != end; ++cursor) {
        if (cursor->is_directory())
            continue;
        names.push_back(cursor->path().lexically_relative(folder).generic_string());
        std::ifstream in(cursor->path(), std::ios::binary);
        blobs.emplace_back(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    std::vector<zip::NewEntry> entries;
    entries.reserve(names.size());
    for (std::size_t index = 0; index < names.size(); ++index)
        entries.push_back(zip::NewEntry{names[index], blobs[index]});
    std::vector<uint8_t> archive;
    zip::ZipError error{};
    OA_CHECK(zip::write_archive(entries, archive, error));
    write_bytes(file, archive);
}

/// The number after a label such as `start: failed, `.
bool count_after(std::string_view text, std::string_view label, uint64_t& count) {
    const std::size_t at = text.find(label);
    if (at == std::string_view::npos)
        return false;
    std::size_t index = at + label.size();
    if (index >= text.size() || text[index] < '0' || text[index] > '9')
        return false;
    uint64_t value = 0;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
        value = value * 10u + static_cast<uint64_t>(text[index] - '0');
        ++index;
    }
    count = value;
    return true;
}

void keys_in_order(std::string_view json) {
    const std::string_view keys[] = {
        "\"check\"",
        "\"ok\"",
        "\"problems\"",
        "\"warnings\"",
        "\"id\"",
        "\"version\"",
        "\"level\"",
        "\"game\"",
        "\"mod\"",
        "\"load\"",
        "\"fits\"",
        "\"start\"",
        "\"error\"",
        "\"lua_bytes\"",
        "\"network_bytes\"",
    };
    std::size_t from = 0;
    for (const std::string_view key : keys) {
        const std::size_t found = json.find(key, from);
        OA_CHECK(found != std::string_view::npos);
        if (found == std::string_view::npos)
            return;
        from = found + key.size();
    }
}

void usage_without_a_game(const Scratch& scratch) {
    const Captured help = run({"help", "brain", "check"});
    OA_CHECK(help.status == 0);
    OA_CHECK(contains(help.out, "flat"));
    OA_CHECK(contains(help.out, "tick"));
    const Captured group = run({"help", "brain"});
    OA_CHECK(group.status == 0);
    OA_CHECK(contains(group.out, "check"));

    const Captured missing = run({"brain", "check", "T01-absent.oabrain"});
    OA_CHECK(missing.status == 2);
    OA_CHECK(contains(missing.err, "option '--game-dir' is required"));
    OA_CHECK(contains(missing.err, "run 'oa-tool help brain check'"));

    const fs::path other = scratch.path / "T01-other.oamut";
    write_text(other, "not a brain\n");
    const Captured kind =
        run({"brain", "check", other.string(), "--game-dir", scratch.path.string()});
    OA_CHECK(kind.status == 2);
    OA_CHECK(contains(kind.err, "the file is not a .oabrain"));

    const fs::path broken =
        write_brain(scratch.path, "broken-file", "\"ta-3.1c\"", "function start(\n");
    const fs::path package = scratch.path / "broken-file.oabrain";
    zip_folder(broken, package);
    const std::string absent = (scratch.path / "T01-do-not-mount").string();
    const Captured refused = run({"brain", "check", package.string(), "--game-dir", absent});
    OA_CHECK(refused.status == 1);
    const std::string refused_text = refused.out + refused.err;
    OA_CHECK(contains(refused_text, "brain.lua"));
    OA_CHECK(!contains(refused_text, "T01-do-not-mount"));

    const fs::path steady = write_brain(
        scratch.path, "steady-commander", "\"ta-3.1c\"", "return { start = function() end }\n"
    );
    const std::string missing_game = (scratch.path / "T01-missing-game").string();
    const Captured gone = run({"brain", "check", steady.string(), "--game-dir", missing_game});
    OA_CHECK(gone.status == 1);
    OA_CHECK(contains(gone.out + gone.err, "T01-missing-game"));
}

void against_the_game(const Scratch& scratch, const fs::path& game) {
    const fs::path steady = write_brain(
        scratch.path, "steady-commander", "\"ta-3.1c\"", "return { start = function() end }\n"
    );
    const Captured seated =
        run({"brain", "check", steady.string(), "--game-dir", game.string(), "--level", "firm"});
    OA_CHECK(seated.status == 0);
    OA_CHECK(contains(seated.out, "brain: steady-commander 1.2 level firm\n"));
    OA_CHECK(contains(seated.out, "game: totala\n"));
    OA_CHECK(contains(seated.out, "mod: none\n"));
    OA_CHECK(contains(seated.out, "load: ok, "));
    OA_CHECK(contains(seated.out, "fits: none, "));
    OA_CHECK(contains(seated.out, "start: ok, "));
    OA_CHECK(contains(seated.out, "instructions\n"));
    OA_CHECK(contains(seated.out, "result: ok\n"));

    const Captured json = run(
        {"brain",
         "check",
         steady.string(),
         "--game-dir",
         game.string(),
         "--level",
         "firm",
         "--json"}
    );
    OA_CHECK(json.status == 0);
    keys_in_order(json.out);
    OA_CHECK(contains(json.out, "\"check\":\"brain\""));
    OA_CHECK(contains(json.out, "\"ok\":true"));

    const fs::path packed = scratch.path / "steady-commander-1.2.oabrain";
    const Captured pack =
        run({"brain", "pack", steady.string(), "--out", packed.string(), "--force"});
    OA_CHECK(pack.status == 0);
    const Captured from_package =
        run({"brain", "check", packed.string(), "--game-dir", game.string(), "--level", "firm"});
    OA_CHECK(from_package.status == 0);
    OA_CHECK(from_package.out == seated.out);

    const fs::path picky = write_brain(
        scratch.path,
        "picky-planner",
        "\"ta-3.1c\"",
        "return {\n"
        "  fits = function()\n"
        "    return \"No build list for this mod's third side\"\n"
        "  end\n"
        "}\n"
    );
    const Captured declined = run({"brain", "check", picky.string(), "--game-dir", game.string()});
    OA_CHECK(declined.status == 1);
    OA_CHECK(
        contains(declined.out, "fits: does not fit: No build list for this mod's third side, ")
    );
    OA_CHECK(!contains(declined.out, "\nerror:"));
    OA_CHECK(contains(declined.out, "result: refused\n"));

    const fs::path broken = write_brain(
        scratch.path,
        "broken-planner",
        "\"ta-3.1c\"",
        "return {\n"
        "  start = function()\n"
        "    error(\"broken on purpose\")\n"
        "  end\n"
        "}\n"
    );
    const Captured failed = run({"brain", "check", broken.string(), "--game-dir", game.string()});
    OA_CHECK(failed.status == 1);
    OA_CHECK(contains(failed.out, "start: failed, "));
    OA_CHECK(contains(failed.out, "brain.lua"));
    OA_CHECK(contains(failed.out, "broken on purpose"));

    const fs::path loop = write_brain(
        scratch.path,
        "loop-planner",
        "\"ta-3.1c\"",
        "return { start = function() while true do end end }\n"
    );
    const Captured looped = run({"brain", "check", loop.string(), "--game-dir", game.string()});
    OA_CHECK(looped.status == 1);
    uint64_t used = 0;
    OA_CHECK(count_after(looped.out, "start: failed, ", used));
    OA_CHECK(used >= 50000000ull);
    OA_CHECK(used <= 50000000ull + 1000ull);
    OA_CHECK(contains(looped.out, "the script ran past its budget of 50000000 instructions"));

    const fs::path raider = write_brain(
        scratch.path, "ridge-raider", "\"ridge >= 28\"", "return { start = function() end }\n"
    );
    const Captured unfit = run({"brain", "check", raider.string(), "--game-dir", game.string()});
    OA_CHECK(unfit.status == 1);
    OA_CHECK(contains(
        unfit.out, "fits: does not fit: works-with names ridge >= 28; this game is ta-3.1c, "
    ));
    OA_CHECK(contains(unfit.out, "load: not run, 0 instructions\n"));
    OA_CHECK(contains(unfit.out, "start: not run, 0 instructions\n"));

    const fs::path ridge = scratch.path / "ridge";
    write_text(
        ridge / "oamod.yaml",
        "oamod: 1\n"
        "id: ridge\n"
        "name: \"Ridge\"\n"
        "version: \"28\"\n"
        "requires: {base: ta-3.1c, catalogue: 1}\n"
        "author: {name: unknown}\n"
        "packaging: {revision: 1, date: 2026-10-04, packager: t}\n"
    );
    const Captured fitted = run(
        {"brain", "check", raider.string(), "--game-dir", game.string(), "--mod", ridge.string()}
    );
    OA_CHECK(fitted.status == 0);
    OA_CHECK(contains(fitted.out, "mod: ridge 28\n"));
    OA_CHECK(contains(fitted.out, "result: ok\n"));

    const std::string bogus = (scratch.path / "T01-do-not-mount-level").string();
    const Captured level =
        run({"brain", "check", steady.string(), "--game-dir", bogus, "--level", "nowhere"});
    OA_CHECK(level.status == 1);
    const std::string level_text = level.out + level.err;
    OA_CHECK(contains(level_text, "unknown level nowhere"));
    OA_CHECK(!contains(level_text, "T01-do-not-mount-level"));
}

} // namespace

int main(int argc, char** argv) {
    Scratch scratch;
    usage_without_a_game(scratch);
    if (oa::test::game_data_requested(argc, argv)) {
        const fs::path game = oa::test::require_game_directory("brain check");
        against_the_game(scratch, game);
    }
    return oa::test::check_exit_status();
}

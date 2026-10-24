// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// oa-tool's commands, run in-process: an archive's listing and files, a loose
// file chosen over that archive, a PCX written as PPM and PNG, help, usage
// failures, the option parser and a group that the tool does not ship.

#include "arguments.hpp"
#include "command.hpp"

#include "oa/formats/hpi.hpp"
#include "oa/formats/png.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Scratch {
    std::filesystem::path path;

    Scratch() : path(oa::test::make_scratch_directory("oa-tool")) {}

    ~Scratch() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

struct Captured {
    int status = 0;
    std::string out;
    std::string err;
};

Captured run(std::span<const oa::tool::Command> table, std::vector<std::string> arguments) {
    std::ostringstream out;
    std::ostringstream err;
    oa::tool::Output output{out, err};
    const int status = oa::tool::run_tool(
        table, std::span<const std::string>(arguments.data(), arguments.size()), output
    );
    return {status, out.str(), err.str()};
}

Captured run(std::vector<std::string> arguments) {
    return run(oa::tool::commands(), std::move(arguments));
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

std::vector<uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (!in || size < 0)
        return {};
    in.seekg(0);
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    if (size > 0)
        in.read(reinterpret_cast<char*>(bytes.data()), size);
    return bytes;
}

void write_bytes(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
}

/// A 2 by 1 indexed PCX: red, then green.
std::vector<uint8_t> pcx_2x1() {
    std::vector<uint8_t> bytes(128 + 2 + 769, 0);
    bytes[0] = 0x0A;
    bytes[1] = 5;
    bytes[2] = 1;
    bytes[3] = 8;
    bytes[8] = 1;
    bytes[65] = 1;
    bytes[66] = 2;
    bytes[128] = 0;
    bytes[129] = 1;
    bytes[130] = 0x0C;
    bytes[131] = 255;
    bytes[135] = 255;
    return bytes;
}

std::vector<uint8_t> ppm_2x1() {
    const std::string header = "P6\n2 1\n255\n";
    std::vector<uint8_t> bytes(header.begin(), header.end());
    const uint8_t pixels[] = {255, 0, 0, 0, 255, 0};
    bytes.insert(bytes.end(), pixels, pixels + sizeof pixels);
    return bytes;
}

void check_png_2x1(const std::filesystem::path& path) {
    const auto bytes = read_bytes(path);
    oa::formats::png::Messages messages{};
    oa::formats::png::Info info{};
    OA_CHECK(oa::formats::png::read_info(bytes, messages, &info));
    OA_CHECK(info.header.width == 2);
    OA_CHECK(info.header.height == 1);
    OA_CHECK(info.header.color_type == oa::formats::png::ColorType::rgb);
    const auto row = oa::formats::png::row_bytes(info.header, {});
    std::vector<uint8_t> rows(static_cast<std::size_t>(info.header.height) * row);
    const auto progress = oa::formats::png::read_image(bytes, info, {}, messages, rows);
    OA_CHECK(progress == oa::formats::png::Progress::end);
    const uint8_t expected[] = {255, 0, 0, 0, 255, 0};
    OA_CHECK(rows == std::vector<uint8_t>(expected, expected + 6));
}

void test_commands(const Scratch& scratch) {
    const std::string tdf_text = "UnitName=armcom;\n";
    const std::vector<uint8_t> tdf(tdf_text.begin(), tdf_text.end());
    const std::vector<uint8_t> pcx = pcx_2x1();
    const auto decoded = oa::decode_pcx(pcx);
    OA_CHECK(decoded.ok());
    if (decoded.ok())
        OA_CHECK(decoded.value->width == 2 && decoded.value->height == 1);

    const oa::HpiWriteFile files[] = {
        {"gamedata/a.tdf", tdf},
        {"anims/b.pcx", pcx},
    };
    const auto archive_path = scratch.path / "pack.hpi";
    write_bytes(archive_path, oa::write_hpi(files));

    const auto listed = run({"list", archive_path.string()});
    const std::string expected_list = "gamedata/a.tdf\t" + std::to_string(tdf.size()) +
                                      "\nanims/b.pcx\t" + std::to_string(pcx.size()) + "\n";
    OA_CHECK(listed.status == oa::tool::exit_done);
    OA_CHECK(listed.out == expected_list);
    OA_CHECK(listed.err.empty());

    const auto extracted_path = scratch.path / "a.tdf";
    const auto extracted =
        run({"extract", archive_path.string(), "gamedata/a.tdf", extracted_path.string()});
    OA_CHECK(extracted.status == oa::tool::exit_done);
    OA_CHECK(extracted.out.empty());
    OA_CHECK(read_bytes(extracted_path) == tdf);

    const std::string loose_text = "loose copy\n";
    const std::vector<uint8_t> loose(loose_text.begin(), loose_text.end());
    const auto loose_path = scratch.path / "root" / "gamedata" / "a.tdf";
    write_bytes(loose_path, loose);
    const auto loose_out = scratch.path / "from-loose.tdf";
    const auto from_loose = run(
        {"asset-extract",
         (scratch.path / "root").string(),
         "gamedata/a.tdf",
         loose_out.string(),
         archive_path.string()}
    );
    const std::string loose_line =
        "loose: " + loose_path.string() + " (" + std::to_string(loose.size()) + " bytes)\n";
    OA_CHECK(from_loose.status == oa::tool::exit_done);
    OA_CHECK(from_loose.out == loose_line);
    OA_CHECK(read_bytes(loose_out) == loose);

    const auto archive_out = scratch.path / "from-archive.pcx";
    const auto from_archive = run(
        {"asset-extract",
         (scratch.path / "root").string(),
         "anims/b.pcx",
         archive_out.string(),
         archive_path.string()}
    );
    const std::string archive_line =
        "archive: " + std::filesystem::weakly_canonical(archive_path).string() + " (" +
        std::to_string(pcx.size()) + " bytes)\n";
    OA_CHECK(from_archive.status == oa::tool::exit_done);
    OA_CHECK(from_archive.out == archive_line);
    OA_CHECK(read_bytes(archive_out) == pcx);

    const auto ppm_path = scratch.path / "image.ppm";
    const auto png_path = scratch.path / "image.png";
    const auto preview_ppm =
        run({"preview", archive_path.string(), "anims/b.pcx", ppm_path.string()});
    const auto preview_png =
        run({"preview", archive_path.string(), "anims/b.pcx", png_path.string()});
    OA_CHECK(preview_ppm.status == oa::tool::exit_done);
    OA_CHECK(preview_ppm.out == "2x1 RGB -> " + ppm_path.string() + "\n");
    OA_CHECK(read_bytes(ppm_path) == ppm_2x1());
    OA_CHECK(preview_png.status == oa::tool::exit_done);
    OA_CHECK(preview_png.out == "2x1 RGB -> " + png_path.string() + "\n");
    check_png_2x1(png_path);

    const auto pcx_path = scratch.path / "b.pcx";
    write_bytes(pcx_path, pcx);
    const auto decoded_ppm = scratch.path / "decoded.ppm";
    const auto decoded_png = scratch.path / "decoded.png";
    const auto decode_ppm = run({"decode-pcx", pcx_path.string(), decoded_ppm.string()});
    const auto decode_png = run({"decode-pcx", pcx_path.string(), decoded_png.string()});
    OA_CHECK(decode_ppm.status == oa::tool::exit_done);
    OA_CHECK(decode_ppm.out == "2x1 RGB -> " + decoded_ppm.string() + "\n");
    OA_CHECK(read_bytes(decoded_ppm) == ppm_2x1());
    OA_CHECK(decode_png.status == oa::tool::exit_done);
    OA_CHECK(decode_png.out == "2x1 RGB -> " + decoded_png.string() + "\n");
    check_png_2x1(decoded_png);
}

void test_help() {
    const auto table = oa::tool::commands();
    OA_CHECK(table.size() == 12);
    OA_CHECK(table[0].name == "list");
    OA_CHECK(table[1].name == "extract");
    OA_CHECK(table[2].name == "asset-extract");
    OA_CHECK(table[3].name == "preview");
    OA_CHECK(table[4].name == "decode-pcx");
    OA_CHECK(table[5].name == "pack");
    OA_CHECK(table[6].name == "check");
    OA_CHECK(table[7].name == "mutator");
    OA_CHECK(table[8].name == "brain");
    OA_CHECK(table[9].name == "catalogue");
    OA_CHECK(table[10].name == "registry");
    OA_CHECK(table[11].name == "interface-words");
    // mutator diff is the fourth subcommand. tools-oa-tool checks this list.
    OA_CHECK(table[7].subcommands.size() == 4);
    OA_CHECK(table[7].subcommands[0].name == "pack");
    OA_CHECK(table[7].subcommands[1].name == "check");
    OA_CHECK(table[7].subcommands[2].name == "import");
    OA_CHECK(table[7].subcommands[3].name == "diff");
    OA_CHECK(table[8].subcommands.size() == 3);
    OA_CHECK(table[8].subcommands[0].name == "pack");
    OA_CHECK(table[8].subcommands[1].name == "check");
    OA_CHECK(table[8].subcommands[2].name == "net");
    OA_CHECK(table[10].subcommands.size() == 2);
    OA_CHECK(table[9].subcommands.size() == 4);
    OA_CHECK(table[9].subcommands[0].name == "keygen");
    OA_CHECK(table[9].subcommands[1].name == "public");
    OA_CHECK(table[9].subcommands[2].name == "sign");
    OA_CHECK(table[9].subcommands[3].name == "verify");

    const auto bare = run({});
    OA_CHECK(bare.status == oa::tool::exit_usage);
    OA_CHECK(bare.out.empty());
    const auto listed = run({"help"});
    OA_CHECK(listed.status == oa::tool::exit_done);
    OA_CHECK(listed.err.empty());
    OA_CHECK(listed.out == bare.err);
    for (const char* line :
         {"oa-tool list ARCHIVE\n",
          "oa-tool extract ARCHIVE ENTRY OUTPUT\n",
          "oa-tool asset-extract ROOT ENTRY OUTPUT [ARCHIVE...]\n",
          "oa-tool preview ARCHIVE PCX_ENTRY OUTPUT.ppm|OUTPUT.png\n",
          "oa-tool decode-pcx INPUT.pcx OUTPUT.ppm|OUTPUT.png\n",
          "oa-tool pack FOLDER [--out FILE] [--force] [--game-dir DIR]\n",
          "oa-tool check FILE [--game-dir DIR] [--mod FOLDER[=KEY]]... "
          "[--accept-unimplemented-hacks] [--json]\n"}) {
        OA_CHECK(contains(listed.out, line));
    }

    const auto help_list = run({"help", "list"});
    const auto list_help = run({"list", "--help"});
    const auto list_short = run({"list", "-h"});
    OA_CHECK(help_list.status == oa::tool::exit_done);
    OA_CHECK(help_list.out == list_help.out);
    OA_CHECK(list_help.out == list_short.out);
    OA_CHECK(contains(help_list.out, "oa-tool list ARCHIVE\n"));
    OA_CHECK(contains(help_list.out, "Reads an HPI archive"));
    OA_CHECK(help_list.err.empty());

    const auto unknown = run({"nosuch"});
    OA_CHECK(unknown.status == oa::tool::exit_usage);
    OA_CHECK(contains(unknown.err, "oa-tool nosuch: unknown command\n"));
    OA_CHECK(contains(unknown.err, "run 'oa-tool help nosuch'\n"));

    const auto no_archive = run({"list"});
    OA_CHECK(no_archive.status == oa::tool::exit_usage);
    OA_CHECK(contains(no_archive.err, "oa-tool list: "));
    OA_CHECK(contains(no_archive.err, "run 'oa-tool help list'\n"));

    const auto extra = run({"extract", "a", "b", "c", "d"});
    OA_CHECK(extra.status == oa::tool::exit_usage);
    OA_CHECK(contains(extra.err, "oa-tool extract: "));
    OA_CHECK(contains(extra.err, "run 'oa-tool help extract'\n"));

    const auto missing = run({"list", "no-such-archive.hpi"});
    OA_CHECK(missing.status == oa::tool::exit_failed);
    OA_CHECK(contains(missing.err, "oa-tool list:"));
    OA_CHECK(missing.out.empty());

    const auto words = run({"interface-words", "--json"});
    OA_CHECK(words.status == oa::tool::exit_done);
    OA_CHECK(words.err.empty());
    OA_CHECK(!words.out.empty() && words.out.front() == '[' && words.out.back() == '\n');
    OA_CHECK(words.out[words.out.size() - 2] == ']');
    OA_CHECK(contains(words.out, "\"The download failed\""));
    const auto words_help = run({"help", "interface-words"});
    OA_CHECK(words_help.status == oa::tool::exit_done);
    OA_CHECK(contains(words_help.out, "oa-tool interface-words --json\n"));
    const auto words_bare = run({"interface-words"});
    OA_CHECK(words_bare.status == oa::tool::exit_usage);
    OA_CHECK(words_bare.out.empty());
    OA_CHECK(contains(words_bare.err, "run 'oa-tool help interface-words'\n"));
}

const oa::tool::OptionSpec parser_options[] = {
    {"key", true, true},
    {"json", false, false},
    {"force", false, false},
    {"out", true, false},
};

void check_parsed(std::span<const std::string> arguments, std::vector<std::string> positional) {
    std::string problem = "unchanged";
    const auto parsed = oa::tool::parse_arguments(arguments, parser_options, problem);
    OA_CHECK(parsed.has_value());
    OA_CHECK(problem.empty());
    if (!parsed)
        return;
    OA_CHECK(parsed->positional == positional);
}

void test_parse() {
    const std::string key_value[] = {"--key", "k"};
    std::string problem;
    const auto separate = oa::tool::parse_arguments(key_value, parser_options, problem);
    OA_CHECK(separate.has_value());
    OA_CHECK(separate && separate->value("key") != nullptr && *separate->value("key") == "k");

    const std::string key_attached[] = {"--key=k"};
    const auto attached = oa::tool::parse_arguments(key_attached, parser_options, problem);
    OA_CHECK(attached && attached->value("key") != nullptr && *attached->value("key") == "k");

    const std::string force_flag[] = {"--force"};
    const auto force = oa::tool::parse_arguments(force_flag, parser_options, problem);
    OA_CHECK(force && force->flags.contains("force"));
    OA_CHECK(force && force->value("force") == nullptr);

    const std::string around[] = {"--json", "left.hpi", "--key", "a", "right.hpi", "--key=b"};
    const auto mixed = oa::tool::parse_arguments(around, parser_options, problem);
    OA_CHECK(mixed.has_value());
    if (mixed) {
        OA_CHECK(mixed->positional == std::vector<std::string>({"left.hpi", "right.hpi"}));
        OA_CHECK(mixed->flags.contains("json"));
        OA_CHECK(mixed->values.at("key") == std::vector<std::string>({"a", "b"}));
        OA_CHECK(mixed->value("key") != nullptr && *mixed->value("key") == "a");
    }

    const std::string after[] = {"file.hpi", "--json"};
    check_parsed(after, {"file.hpi"});
    const auto after_parsed = oa::tool::parse_arguments(after, parser_options, problem);
    OA_CHECK(after_parsed && after_parsed->flags.contains("json"));

    const std::string ended[] = {"file.hpi", "--", "--force", "--key"};
    const auto stopped = oa::tool::parse_arguments(ended, parser_options, problem);
    OA_CHECK(stopped.has_value());
    if (stopped) {
        OA_CHECK(stopped->positional == std::vector<std::string>({"file.hpi", "--force", "--key"}));
        OA_CHECK(stopped->flags.empty());
        OA_CHECK(stopped->values.empty());
    }

    const std::string unknown[] = {"--nope"};
    OA_CHECK(!oa::tool::parse_arguments(unknown, parser_options, problem));
    OA_CHECK(problem == "unknown option '--nope'");

    const std::string missing[] = {"--out"};
    OA_CHECK(!oa::tool::parse_arguments(missing, parser_options, problem));
    OA_CHECK(problem == "option '--out' needs a value");

    const std::string twice[] = {"--out", "a", "--out", "b"};
    OA_CHECK(!oa::tool::parse_arguments(twice, parser_options, problem));
    OA_CHECK(problem == "option '--out' was given twice");

    const std::string flag_twice[] = {"--force", "--force"};
    OA_CHECK(!oa::tool::parse_arguments(flag_twice, parser_options, problem));
    OA_CHECK(problem == "option '--force' was given twice");
}

int run_note(std::span<const std::string> arguments, oa::tool::Output& output) {
    output.out << "noted " << arguments[0] << '\n';
    return oa::tool::exit_done;
}

void test_group() {
    static const oa::tool::Command children[] = {
        {"note", "FILE", "Note a file.", "Reads FILE and prints its name.", 1, 1, run_note, {}},
    };
    const oa::tool::Command table[] = {
        {"kit",
         "COMMAND",
         "Group a sample command.",
         "A group used only by this test. It reads nothing and writes nothing.",
         0,
         0,
         nullptr,
         children},
    };

    const auto listed = run(table, {});
    OA_CHECK(listed.status == oa::tool::exit_usage);
    OA_CHECK(contains(listed.err, "oa-tool kit COMMAND\n"));
    OA_CHECK(contains(listed.err, "oa-tool kit note FILE\n"));

    const auto group = run(table, {"kit"});
    OA_CHECK(group.status == oa::tool::exit_usage);
    OA_CHECK(contains(group.err, "oa-tool kit note FILE\n"));
    OA_CHECK(group.out.empty());

    const auto noted = run(table, {"kit", "note", "hello"});
    OA_CHECK(noted.status == oa::tool::exit_done);
    OA_CHECK(noted.out == "noted hello\n");

    const auto help = run(table, {"help", "kit", "note"});
    OA_CHECK(help.status == oa::tool::exit_done);
    OA_CHECK(contains(help.out, "oa-tool kit note FILE\n"));
    OA_CHECK(contains(help.out, "Reads FILE and prints its name."));

    const auto note_help = run(table, {"kit", "note", "--help"});
    OA_CHECK(note_help.status == oa::tool::exit_done);
    OA_CHECK(note_help.out == help.out);

    const auto missing = run(table, {"kit", "missing"});
    OA_CHECK(missing.status == oa::tool::exit_usage);
    OA_CHECK(contains(missing.err, "run 'oa-tool help kit missing'\n"));
}

} // namespace

int main() {
    const Scratch scratch;
    test_commands(scratch);
    test_help();
    test_parse();
    test_group();
    return oa::test::check_exit_status();
}

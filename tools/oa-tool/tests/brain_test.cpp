// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// oa-tool pack and check for a made-up brain, run in-process. The folder
// packs to a .oabrain in a fixed order, brain pack and pack write the same
// bytes, a second pack after the files' times change is the same bytes, and
// check reports the catalogue facts. A broken manifest, script, layout,
// network or icon is refused by pack and by check.

#include "command.hpp"

#include "oa/base/sha256.hpp"
#include "oa/formats/json.hpp"
#include "oa/formats/png.hpp"
#include "oa/formats/zip.hpp"
#include "oa/formats/zip/stream.hpp"
#include "oa/platform/files.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace json = oa::formats::json;
namespace png = oa::formats::png;
namespace sha256 = oa::base::sha256;
namespace zip = oa::formats::zip;

/// What is wrong with a made-up brain, or nothing.
enum class Fault : uint8_t {
    none,
    manifest,
    lua,
    precompiled,
    layout,
    network,
    icon,
};

struct Scratch {
    fs::path path;

    Scratch() : path(oa::test::make_scratch_directory("P05-oa-tool-brain")) {}

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

/// A manifest that names every key. HASH is 64 hex digits of the network.
constexpr std::string_view kManifest = "oabrain: 1\n"
                                       "id: steady-commander\n"
                                       "name: \"Steady Commander\"\n"
                                       "version: \"2.0\"\n"
                                       "author:\n"
                                       "  name: \"A. Maker\"\n"
                                       "  email: \"maker@example.com\"\n"
                                       "description: \"Holds the line.\\nNever rushes.\"\n"
                                       "homepage: \"https://example.com/steady\"\n"
                                       "icon: \"icon.png\"\n"
                                       "tags: [\"classic\", \"economy\"]\n"
                                       "works-with: [\"ta-3.1c\", \"ridge >= 28\"]\n"
                                       "requires:\n"
                                       "  engine: \">= 0.8.0\"\n"
                                       "  expansions: [\"core-contingency\"]\n"
                                       "levels:\n"
                                       "  - id: steady\n"
                                       "    name: \"Steady\"\n"
                                       "    difficulty: easy\n"
                                       "  - id: bold\n"
                                       "    name: \"Bold\"\n"
                                       "    difficulty: hard\n"
                                       "fullAwareness: true\n"
                                       "networks:\n"
                                       "  - id: threat\n"
                                       "    file: \"nets/threat.oann\"\n"
                                       "    sha256: \"HASH\"\n"
                                       "options:\n"
                                       "  waves:\n"
                                       "    type: \"number\"\n"
                                       "    label: \"Waves\"\n"
                                       "    min: 1\n"
                                       "    max: 8\n"
                                       "    step: 1\n"
                                       "    default: 3\n"
                                       "  stance:\n"
                                       "    type: \"choice\"\n"
                                       "    label: \"Stance\"\n"
                                       "    values: [\"hold\", \"push\"]\n"
                                       "    default: \"hold\"\n"
                                       "entry: \"brain.lua\"\n";

constexpr std::string_view kHashMark = "HASH";

const std::vector<uint8_t> kNetwork{
    'o', 'a', 'n', 'n', '-', 's', 't', 'a', 'n', 'd', '-', 'i', 'n', '\n'
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

std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Writes a stored PNG.
///
/// @param side its width and height
/// @return the file; empty when it could not be written
std::vector<uint8_t> png_image(uint32_t side) {
    png::Header header{};
    header.width = side;
    header.height = side;
    header.bit_depth = 8;
    header.color_type = png::ColorType::grey;
    const std::size_t row = png::row_bytes(header, {});
    const std::vector<uint8_t> rows(row * static_cast<std::size_t>(side));
    png::Image image{};
    image.header = header;
    image.rows = rows;
    std::vector<uint8_t> out;
    OA_CHECK(png::write_stored(image, &out));
    return out;
}

std::string hex_of(std::span<const uint8_t> bytes) {
    const auto hex = sha256::to_hex(sha256::digest_of(bytes));
    return {hex.data(), hex.size()};
}

/// Writes the made-up brain, with one fault when `fault` names one.
///
/// @param folder the folder
/// @param fault what is broken
/// @param icon the icon's bytes, so a check can hash the same file
void make_brain(const fs::path& folder, Fault fault, const std::vector<uint8_t>& icon) {
    const std::string sha = hex_of(kNetwork);
    std::string manifest{kManifest};
    const std::size_t mark = manifest.find(kHashMark);
    OA_CHECK(mark != std::string::npos);
    if (mark != std::string::npos)
        manifest.replace(mark, kHashMark.size(), sha);
    if (fault == Fault::manifest) {
        manifest.replace(
            manifest.find("version: \"2.0\""),
            std::string("version: \"2.0\"").size(),
            "version: \"nope\""
        );
    }
    write_text(folder / "oabrain.yaml", manifest);
    write_text(folder / "README.md", "A made-up brain.\n");
    write_text(folder / "LICENSE", "Made-up licence.\n");
    write_text(folder / "data" / "build_orders.csv", "unit,count\narmcom,1\n");
    const uint32_t side = fault == Fault::icon ? 300 : 64;
    const std::vector<uint8_t> picture = side == 64 ? icon : png_image(side);
    write_bytes(folder / "icon.png", picture);
    // A case-insensitive volume keeps the first spelling of a directory. The
    // capital spelling is outside a brain's layout, so it is created before
    // lua/tasks. Otherwise the file lands in lua/tasks and is a legal module.
    if (fault == Fault::layout)
        write_text(folder / "lua" / "Tasks" / "x.lua", "return 1\n");
    if (fault == Fault::lua)
        write_text(folder / "lua" / "tasks" / "land_strike.lua", "local strike = 1\nreturn (\n");
    else
        write_text(
            folder / "lua" / "tasks" / "land_strike.lua", "local strike = 1\nreturn strike\n"
        );
    if (fault == Fault::precompiled) {
        const uint8_t binary[] = {0x1B, 'L', 'u', 'a'};
        write_bytes(folder / "brain.lua", binary);
    } else {
        write_text(folder / "brain.lua", "local value = 1\nreturn value\n");
    }
    if (fault == Fault::network) {
        std::vector<uint8_t> changed = kNetwork;
        changed.push_back(static_cast<uint8_t>('x'));
        write_bytes(folder / "nets" / "threat.oann", changed);
    } else {
        write_bytes(folder / "nets" / "threat.oann", kNetwork);
    }
}

struct OpenFile {
    std::FILE* stream{};
    oa::platform::Files files{};

    OpenFile() = default;
    OpenFile(const OpenFile&) = delete;
    OpenFile& operator=(const OpenFile&) = delete;

    ~OpenFile() {
        if (stream != nullptr)
            std::fclose(stream);
    }
};

bool read_at(void* context, uint64_t offset, std::span<uint8_t> bytes) {
    auto& file = *static_cast<OpenFile*>(context);
    auto* handle = reinterpret_cast<oa::platform::FileHandle*>(file.stream);
    if (file.stream == nullptr ||
        file.files.seek(
            nullptr, handle, static_cast<int64_t>(offset), oa::platform::SeekOrigin::begin
        ) != 0)
        return false;
    std::size_t done = 0;
    while (done < bytes.size()) {
        const std::size_t got =
            std::fread(bytes.data() + done, 1, bytes.size() - done, file.stream);
        if (got == 0)
            return false;
        done += got;
    }
    return true;
}

bool read_directory(const fs::path& path, zip::StreamDirectory& directory) {
    OpenFile file;
    file.stream = oa::platform::open_file(path, "rb");
    if (file.stream == nullptr)
        return false;
    file.files = oa::platform::stdio_files();
    auto* handle = reinterpret_cast<oa::platform::FileHandle*>(file.stream);
    if (file.files.seek(nullptr, handle, 0, oa::platform::SeekOrigin::end) != 0)
        return false;
    const int64_t end = file.files.tell(nullptr, handle);
    if (end < 0)
        return false;
    zip::ZipError error{};
    return zip::read_stream_directory(
        {&file, read_at}, static_cast<uint64_t>(end), {}, directory, error
    );
}

/// Packs the folder's files into a stored zip so check can read a refusal
/// pack itself will not leave behind.
///
/// @param folder the folder
/// @param file the zip
void zip_folder(const fs::path& folder, const fs::path& file) {
    std::vector<std::string> names;
    std::vector<std::vector<uint8_t>> blobs;
    for (fs::recursive_directory_iterator cursor(folder), end; cursor != end; ++cursor) {
        if (cursor->is_directory())
            continue;
        names.push_back(cursor->path().lexically_relative(folder).generic_string());
        blobs.push_back(read_bytes(cursor->path()));
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

const json::Json* member(const json::Json& object, std::string_view name) {
    return object.find(name);
}

std::string text_of(const json::Json* value) {
    if (value == nullptr || value->string() == nullptr)
        return {};
    return *value->string();
}

void touch_tree(const fs::path& folder) {
    const auto when = fs::file_time_type::clock::now() + std::chrono::hours(48);
    std::error_code error;
    fs::last_write_time(folder, when, error);
    for (fs::recursive_directory_iterator cursor(folder), end; cursor != end; ++cursor)
        fs::last_write_time(cursor->path(), when, error);
}

/// The keys a brain's report carries, in the order check writes them.
constexpr std::string_view kKeys[] = {
    "check",       "ok",       "problems",       "warnings", "file",
    "kind",        "id",       "name",           "version",  "revision",
    "size",        "sha256",   "unpacked_size",  "files",    "summary",
    "description", "homepage", "tags",           "author",   "requires",
    "works_with",  "levels",   "full_awareness", "networks", "network_bytes",
    "options",     "icon",     "entry",          "scripts",  "data",
};

bool number_is(const json::Json* value, std::string_view text) {
    return value != nullptr && value->number_text() != nullptr && *value->number_text() == text;
}

void expect_facts(
    const json::Json& report, const std::vector<uint8_t>& icon, std::string_view file
) {
    const std::span<const std::string> names = report.names();
    OA_CHECK(names.size() == std::size(kKeys));
    if (names.size() == std::size(kKeys)) {
        for (std::size_t index = 0; index < names.size(); ++index)
            OA_CHECK(names[index] == kKeys[index]);
    }
    OA_CHECK(member(report, "ok") != nullptr && member(report, "ok")->boolean() == true);
    OA_CHECK(text_of(member(report, "kind")) == "oabrain");
    OA_CHECK(
        member(report, "revision") != nullptr &&
        member(report, "revision")->type() == json::JsonType::null
    );
    OA_CHECK(text_of(member(report, "id")) == "steady-commander");
    OA_CHECK(text_of(member(report, "name")) == "Steady Commander");
    OA_CHECK(text_of(member(report, "version")) == "2.0");
    OA_CHECK(text_of(member(report, "file")) == file);
    OA_CHECK(text_of(member(report, "summary")) == "Holds the line.");
    OA_CHECK(text_of(member(report, "description")) == "Holds the line.\nNever rushes.");
    OA_CHECK(text_of(member(report, "homepage")) == "https://example.com/steady");
    OA_CHECK(text_of(member(report, "author")) == "A. Maker");
    OA_CHECK(text_of(member(report, "entry")) == "brain.lua");
    OA_CHECK(member(report, "scripts") != nullptr && member(report, "scripts")->integer() == 2);
    OA_CHECK(
        member(report, "full_awareness") != nullptr &&
        member(report, "full_awareness")->boolean() == true
    );
    OA_CHECK(
        member(report, "network_bytes") != nullptr &&
        member(report, "network_bytes")->integer() == static_cast<int64_t>(kNetwork.size())
    );

    const json::Json* tags = member(report, "tags");
    OA_CHECK(tags != nullptr && tags->elements().size() == 2);
    if (tags != nullptr && tags->elements().size() == 2) {
        OA_CHECK(text_of(&tags->elements()[0]) == "classic");
        OA_CHECK(text_of(&tags->elements()[1]) == "economy");
    }

    const json::Json* needed = member(report, "requires");
    OA_CHECK(needed != nullptr);
    if (needed != nullptr) {
        OA_CHECK(text_of(needed->find("engine")) == ">= 0.8.0");
        const json::Json* expansions = needed->find("expansions");
        OA_CHECK(expansions != nullptr && expansions->elements().size() == 1);
        if (expansions != nullptr && !expansions->elements().empty())
            OA_CHECK(text_of(&expansions->elements()[0]) == "core-contingency");
    }

    const json::Json* works = member(report, "works_with");
    OA_CHECK(works != nullptr && works->elements().size() == 2);
    if (works != nullptr && works->elements().size() == 2) {
        OA_CHECK(text_of(&works->elements()[0]) == "ta-3.1c");
        OA_CHECK(text_of(&works->elements()[1]) == "ridge >= 28");
    }

    const json::Json* levels = member(report, "levels");
    OA_CHECK(levels != nullptr && levels->elements().size() == 2);
    if (levels != nullptr && levels->elements().size() == 2) {
        OA_CHECK(text_of(levels->elements()[0].find("id")) == "steady");
        OA_CHECK(text_of(levels->elements()[0].find("name")) == "Steady");
        OA_CHECK(text_of(levels->elements()[0].find("difficulty")) == "easy");
        OA_CHECK(text_of(levels->elements()[1].find("id")) == "bold");
        OA_CHECK(text_of(levels->elements()[1].find("name")) == "Bold");
        OA_CHECK(text_of(levels->elements()[1].find("difficulty")) == "hard");
    }

    const json::Json* networks = member(report, "networks");
    OA_CHECK(networks != nullptr && networks->elements().size() == 1);
    if (networks != nullptr && !networks->elements().empty()) {
        const json::Json& network = networks->elements()[0];
        OA_CHECK(text_of(network.find("id")) == "threat");
        OA_CHECK(text_of(network.find("file")) == "nets/threat.oann");
        OA_CHECK(text_of(network.find("sha256")) == hex_of(kNetwork));
        OA_CHECK(
            network.find("size") != nullptr &&
            network.find("size")->integer() == static_cast<int64_t>(kNetwork.size())
        );
    }

    const json::Json* options = member(report, "options");
    OA_CHECK(options != nullptr && options->elements().size() == 2);
    if (options != nullptr && options->elements().size() == 2) {
        const json::Json& waves = options->elements()[0];
        OA_CHECK(text_of(waves.find("id")) == "waves");
        OA_CHECK(text_of(waves.find("label")) == "Waves");
        OA_CHECK(text_of(waves.find("type")) == "number");
        OA_CHECK(number_is(waves.find("default"), "3"));
        OA_CHECK(number_is(waves.find("min"), "1"));
        OA_CHECK(number_is(waves.find("max"), "8"));
        OA_CHECK(number_is(waves.find("step"), "1"));
        const json::Json& stance = options->elements()[1];
        OA_CHECK(text_of(stance.find("id")) == "stance");
        OA_CHECK(text_of(stance.find("type")) == "choice");
        OA_CHECK(text_of(stance.find("default")) == "hold");
    }

    const json::Json* icon_fact = member(report, "icon");
    OA_CHECK(icon_fact != nullptr);
    if (icon_fact != nullptr) {
        OA_CHECK(text_of(icon_fact->find("path")) == "icon.png");
        OA_CHECK(text_of(icon_fact->find("sha256")) == hex_of(icon));
        OA_CHECK(
            icon_fact->find("size") != nullptr &&
            icon_fact->find("size")->integer() == static_cast<int64_t>(icon.size())
        );
        OA_CHECK(icon_fact->find("width") != nullptr && icon_fact->find("width")->integer() == 64);
        OA_CHECK(
            icon_fact->find("height") != nullptr && icon_fact->find("height")->integer() == 64
        );
    }

    const json::Json* tables = member(report, "data");
    OA_CHECK(tables != nullptr && tables->elements().size() == 1);
    if (tables != nullptr && !tables->elements().empty())
        OA_CHECK(text_of(&tables->elements()[0]) == "data/build_orders.csv");
}

json::Json parse_report(std::string_view text) {
    json::JsonError error{};
    const std::optional<json::Json> parsed = json::parse_json(text, error);
    OA_CHECK(parsed.has_value() && parsed->type() == json::JsonType::object);
    if (!parsed)
        return json::Json{};
    return *parsed;
}

void test_pack_and_check(const fs::path& scratch) {
    const std::vector<uint8_t> icon = png_image(64);
    const fs::path folder = scratch / "steady-commander";
    make_brain(folder, Fault::none, icon);
    const fs::path out = scratch / "steady-commander-2.0.oabrain";
    const fs::path grouped = scratch / "grouped.oabrain";
    const Captured packed = run({"brain", "pack", "--out", grouped.string(), folder.string()});
    OA_CHECK(packed.status == oa::tool::exit_done);
    if (packed.status != oa::tool::exit_done)
        std::fprintf(stderr, "brain pack: %s\n", packed.err.c_str());
    const Captured plain = run({"pack", "--out", out.string(), folder.string()});
    OA_CHECK(plain.status == oa::tool::exit_done);
    if (plain.status != oa::tool::exit_done)
        std::fprintf(stderr, "pack: %s\n", plain.err.c_str());
    OA_CHECK(packed.err.empty());
    OA_CHECK(read_bytes(grouped) == read_bytes(out));
    OA_CHECK(!fs::exists(fs::path(out.string() + ".part")));

    zip::StreamDirectory directory;
    OA_CHECK(read_directory(out, directory));
    const std::pair<std::string_view, zip::Method> expect[] = {
        {"oabrain.yaml", zip::Method::deflated},
        {"LICENSE", zip::Method::deflated},
        {"README.md", zip::Method::deflated},
        {"brain.lua", zip::Method::deflated},
        {"data/build_orders.csv", zip::Method::deflated},
        {"icon.png", zip::Method::stored},
        {"lua/tasks/land_strike.lua", zip::Method::deflated},
        {"nets/threat.oann", zip::Method::deflated},
    };
    OA_CHECK(directory.entries.size() == 8);
    if (directory.entries.size() == 8) {
        for (std::size_t index = 0; index < 8; ++index) {
            OA_CHECK(directory.entries[index].name == expect[index].first);
            OA_CHECK(directory.entries[index].method == expect[index].second);
        }
    }

    touch_tree(folder);
    const fs::path again = scratch / "steady-again.oabrain";
    const Captured repacked = run({"pack", folder.string(), "--out", again.string()});
    OA_CHECK(repacked.status == oa::tool::exit_done);
    OA_CHECK(read_bytes(out) == read_bytes(again));

    const Captured checked = run({"check", out.string(), "--json"});
    OA_CHECK(checked.status == oa::tool::exit_done);
    if (checked.status != oa::tool::exit_done)
        std::fprintf(stderr, "check: %s\n%s\n", checked.out.c_str(), checked.err.c_str());
    expect_facts(parse_report(checked.out), icon, out.filename().string());

    const Captured text = run({"check", out.string()});
    OA_CHECK(text.status == oa::tool::exit_done);
    OA_CHECK(text.out.size() >= std::string("result: ok\n").size());
    if (text.out.size() >= std::string("result: ok\n").size())
        OA_CHECK(text.out.ends_with("result: ok\n"));

    const fs::path wrong = scratch / "brain.zip";
    const Captured refused_name = run({"pack", folder.string(), "--out", wrong.string()});
    OA_CHECK(refused_name.status == oa::tool::exit_failed);
    OA_CHECK(contains(refused_name.err, ".oabrain"));
    OA_CHECK(!fs::exists(wrong));

    const fs::path cwd = scratch / "cwd";
    fs::create_directories(cwd);
    {
        const fs::path previous = fs::current_path();
        fs::current_path(cwd);
        const Captured named = run({"brain", "pack", folder.string()});
        fs::current_path(previous);
        OA_CHECK(named.status == oa::tool::exit_done);
        OA_CHECK(contains(named.out, "packed steady-commander-2.0.oabrain: "));
    }
    OA_CHECK(fs::exists(cwd / "steady-commander-2.0.oabrain"));

    const Captured game_dir = run({"check", out.string(), "--game-dir", scratch.string()});
    OA_CHECK(game_dir.status == oa::tool::exit_usage);
    OA_CHECK(contains(game_dir.err, "option '--game-dir' is not for a brain"));
}

void test_refusal(const fs::path& scratch, Fault fault, std::string_view snippet) {
    const std::vector<uint8_t> icon = png_image(64);
    const fs::path folder = scratch / ("fault-" + std::to_string(static_cast<unsigned>(fault)));
    make_brain(folder, fault, icon);
    const fs::path out = scratch / (folder.filename().string() + ".oabrain");
    const Captured packed = run({"pack", folder.string(), "--out", out.string()});
    OA_CHECK(packed.status == oa::tool::exit_failed);
    OA_CHECK(contains(packed.err, snippet));
    OA_CHECK(!fs::exists(out));
    OA_CHECK(!fs::exists(fs::path(out.string() + ".part")));

    const fs::path zip = scratch / (folder.filename().string() + "-stored.oabrain");
    zip_folder(folder, zip);
    const Captured checked = run({"check", zip.string()});
    OA_CHECK(checked.status == oa::tool::exit_failed);
    OA_CHECK(contains(checked.out + checked.err, snippet));
    const Captured grouped = run({"brain", "pack", folder.string(), "--out", out.string()});
    OA_CHECK(grouped.status == oa::tool::exit_failed);
    OA_CHECK(contains(grouped.err, snippet));
}

void test_refusals(const fs::path& scratch) {
    test_refusal(scratch, Fault::manifest, "oabrain.yaml:");
    test_refusal(scratch, Fault::lua, "lua/tasks/land_strike.lua:3:");
    test_refusal(scratch, Fault::precompiled, "precompiled");
    test_refusal(scratch, Fault::layout, "lua/Tasks/x.lua");
    test_refusal(scratch, Fault::network, "its SHA-256 is");
    test_refusal(scratch, Fault::icon, "square PNG");
}

void test_brain_usage(const fs::path& scratch) {
    const fs::path mod = scratch / "mod";
    write_text(mod / "oamod.yaml", "oamod: 1\n");
    const Captured packed = run({"brain", "pack", mod.string()});
    OA_CHECK(packed.status == oa::tool::exit_usage);
    OA_CHECK(contains(packed.err, "oabrain.yaml"));
    OA_CHECK(contains(packed.err, "run 'oa-tool help brain pack'\n"));
}

} // namespace

int main() {
    const Scratch scratch;
    const auto run_case = [](const char* name, auto&& function) {
        try {
            function();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "%s: %s\n", name, error.what());
            ++oa::test::failed_checks();
        }
    };
    run_case("test_pack_and_check", [&] { test_pack_and_check(scratch.path); });
    run_case("test_refusals", [&] { test_refusals(scratch.path); });
    run_case("test_brain_usage", [&] { test_brain_usage(scratch.path); });
    return oa::test::check_exit_status();
}

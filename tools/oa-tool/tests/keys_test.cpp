// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// oa-tool pack and check for a made-up key layout, run in-process. The folder
// packs to a .oakeys in a fixed order, keys pack and pack write the same
// bytes, a second pack after the files' times change is the same bytes, and
// check reports the catalogue facts. A broken chord, an outside file, an icon
// or a second manifest is refused by pack and by check. Primary beside Ctrl or
// Meta on two actions is a clash that names the platforms it holds on. The
// picture is a PNG of the keyboard, and show prints the bindings after the base.

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

/// A layout that names the facts check reports.
constexpr std::string_view kManifest = "oakeys: 1\n"
                                       "id: grid-test\n"
                                       "name: \"Grid test\"\n"
                                       "version: \"1.0\"\n"
                                       "author: {name: \"A. Maker\"}\n"
                                       "description: \"Orders on the left.\\nThe rest stays.\"\n"
                                       "homepage: \"https://example.com/grid-test\"\n"
                                       "icon: \"icon.png\"\n"
                                       "tags: [grid, test]\n"
                                       "base: oa-default\n"
                                       "keyboard: any\n"
                                       "pointer: right-click\n"
                                       "requires:\n"
                                       "  engine: \">= 0.8.0\"\n"
                                       "works-with: [\"ridge >= 28\"]\n"
                                       "bindings:\n"
                                       "  order.attack: [\"Q\"]\n"
                                       "  order.move: [\"@KeyW\"]\n"
                                       "helpers:\n"
                                       "  sticky-modifiers: true\n"
                                       "  pointer-keys: false\n";

/// The Left hand starter layout's bindings, as it writes them. Its keys
/// overlay and Settings are Primary+5 and Primary+6, beside Ctrl+5 and Ctrl+6
/// on two group keys.
constexpr std::string_view kLeftHand = R"(oakeys: 1
id: "left-hand"
name: "Left hand"
version: "1.0"
author:
  name: "OA team"
description: "Every key a match needs, within reach of the left hand."
tags: ["left-hand", "accessibility"]
base: oa-default
bindings:
  build.page-1: ["@Digit1"]
  build.page-2: ["@Digit2"]
  build.page-3: ["@Digit3"]
  build.page-4: ["@Digit4"]
  build.page-5: ["@Digit5"]
  build.page-6: ["@Digit6"]
  build.page-7: ["Ctrl+@KeyE"]
  build.page-8: ["Ctrl+@KeyR"]
  build.page-9: ["Ctrl+@KeyT"]
  build.page-next: ["Ctrl+@KeyW"]
  build.page-previous: ["Ctrl+@KeyQ"]
  camera.bookmark-go-3: [Shift+F5]
  camera.bookmark-go-4: [Shift+F6]
  camera.bookmark-set-3: [Ctrl+Shift+F5]
  camera.bookmark-set-4: [Ctrl+Shift+F6]
  camera.centre: ["Alt+@KeyC"]
  camera.next-unit: ["Shift+@KeyC"]
  camera.pan-down: ["Alt+@KeyS"]
  camera.pan-left: ["Alt+@KeyA"]
  camera.pan-right: ["Alt+@KeyD"]
  camera.pan-up: ["Alt+@KeyW"]
  camera.unlocked: ["Alt+@KeyQ"]
  chat.open: ["Ctrl+@KeyG"]
  chat.send: ["Ctrl+Shift+@KeyG"]
  game.clear-messages: [Shift+F3]
  game.health-bars-mode-3: ["Shift+@Digit5"]
  game.pause: ["Ctrl+Shift+@KeyT"]
  game.repeat-command: ["Ctrl+@KeyX"]
  game.screenshot: [Ctrl+Shift+F3]
  game.share-panel: ["@KeyZ"]
  game.speed-down: ["Shift+@Digit2"]
  game.speed-up: ["Shift+@Digit4"]
  group.add-1: ["Alt+Shift+@Digit1"]
  group.add-2: ["Alt+Shift+@Digit2"]
  group.add-3: ["Alt+Shift+@Digit3"]
  group.add-4: ["Alt+Shift+@Digit4"]
  group.add-5: ["Alt+Shift+@Digit5"]
  group.add-6: ["Alt+Shift+@Digit6"]
  group.add-7: ["Alt+Shift+@KeyZ"]
  group.add-8: ["Alt+Shift+@KeyX"]
  group.add-9: ["Alt+Shift+@KeyC"]
  group.assign-1: ["Ctrl+@Digit1"]
  group.assign-2: ["Ctrl+@Digit2"]
  group.assign-3: ["Ctrl+@Digit3"]
  group.assign-4: ["Ctrl+@Digit4"]
  group.assign-5: ["Ctrl+@Digit5"]
  group.assign-6: ["Ctrl+@Digit6"]
  group.assign-7: ["Ctrl+Shift+@KeyQ"]
  group.assign-8: ["Ctrl+Shift+@KeyW"]
  group.assign-9: ["Ctrl+Shift+@KeyE"]
  group.select-1: ["Alt+@Digit1"]
  group.select-2: ["Alt+@Digit2"]
  group.select-3: ["Alt+@Digit3"]
  group.select-4: ["Alt+@Digit4"]
  group.select-5: ["Alt+@Digit5"]
  group.select-6: ["Alt+@Digit6"]
  group.select-7: ["Alt+@KeyZ"]
  group.select-8: ["Alt+@KeyX"]
  group.select-9: ["Alt+@KeyV"]
  order.cloak: ["Shift+@KeyQ"]
  order.load: ["Shift+@KeyW"]
  order.move: ["@KeyQ"]
  order.patrol: ["@KeyW"]
  order.unload: ["Shift+@KeyE"]
  panel.orders-tab: ["Shift+@KeyZ"]
  pointer.click-left: ["Ctrl+Alt+@KeyS"]
  pointer.click-right: ["Ctrl+Alt+@KeyV"]
  pointer.hold-left: ["Ctrl+Alt+@KeyB"]
  pointer.let-go: ["Ctrl+Alt+@KeyG"]
  pointer.move-down: ["Ctrl+Alt+@KeyX"]
  pointer.move-down-left: ["Ctrl+Alt+@KeyZ"]
  pointer.move-down-right: ["Ctrl+Alt+@KeyC"]
  pointer.move-left: ["Ctrl+Alt+@KeyA"]
  pointer.move-right: ["Ctrl+Alt+@KeyD"]
  pointer.move-up: ["Ctrl+Alt+@KeyW"]
  pointer.move-up-left: ["Ctrl+Alt+@KeyQ"]
  pointer.move-up-right: ["Ctrl+Alt+@KeyE"]
  ui.full-screen: ["Alt+@KeyF"]
  ui.keys-overlay: ["Primary+@Digit5"]
  ui.menu-accept: ["Alt+@KeyG"]
  ui.menu-focus-left: ["Shift+@KeyA"]
  ui.menu-focus-right: ["Shift+@KeyD"]
  ui.menu-list-down: [WheelDown]
  ui.menu-list-up: [WheelUp]
  ui.settings: ["Primary+@Digit6"]
helpers:
  sticky-modifiers: true
)";

struct Scratch {
    fs::path path;

    Scratch() : path(oa::test::make_scratch_directory("K44-oa-tool-keys")) {}

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

/// Puts the process back in the folder it left.
struct WorkingDirectory {
    fs::path previous;

    explicit WorkingDirectory(const fs::path& next) : previous(fs::current_path()) {
        fs::current_path(next);
    }

    ~WorkingDirectory() {
        std::error_code error;
        fs::current_path(previous, error);
    }

    WorkingDirectory(const WorkingDirectory&) = delete;
    WorkingDirectory& operator=(const WorkingDirectory&) = delete;
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

/// Writes a stored grey PNG.
///
/// @param side its width and height
/// @return the file
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

void touch_tree(const fs::path& folder) {
    const auto when = fs::file_time_type::clock::now() + std::chrono::hours(48);
    std::error_code error;
    fs::last_write_time(folder, when, error);
    for (fs::recursive_directory_iterator cursor(folder), end; cursor != end; ++cursor)
        fs::last_write_time(cursor->path(), when, error);
}

const json::Json* member(const json::Json& object, std::string_view name) {
    return object.find(name);
}

std::string text_of(const json::Json* value) {
    if (value == nullptr || value->string() == nullptr)
        return {};
    return *value->string();
}

json::Json parse_report(std::string_view text) {
    json::JsonError error{};
    const std::optional<json::Json> parsed = json::parse_json(text, error);
    OA_CHECK(parsed.has_value() && parsed->type() == json::JsonType::object);
    if (!parsed)
        return json::Json{};
    return *parsed;
}

json::Json parse_array(std::string_view text) {
    json::JsonError error{};
    const std::optional<json::Json> parsed = json::parse_json(text, error);
    OA_CHECK(parsed.has_value() && parsed->type() == json::JsonType::array);
    if (!parsed)
        return json::Json{};
    return *parsed;
}

/// The keys a layout's report carries, in the order check writes them.
constexpr std::string_view kKeys[] = {
    "check",   "ok",          "problems", "warnings", "file",    "kind",          "id",
    "name",    "version",     "revision", "size",     "sha256",  "unpacked_size", "files",
    "summary", "description", "homepage", "tags",     "author",  "requires",      "works_with",
    "base",    "keyboard",    "pointer",  "helpers",  "changes", "triggers",      "unknown_actions",
    "clashes", "icon",
};

/// The PNG's width and height, from its IHDR.
bool png_size(std::span<const uint8_t> bytes, uint32_t& width, uint32_t& height) {
    if (bytes.size() < 24)
        return false;
    const bool signature = bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' &&
                           bytes[3] == 'G' && bytes[4] == 0x0D && bytes[5] == 0x0A &&
                           bytes[6] == 0x1A && bytes[7] == 0x0A;
    if (!signature || bytes[12] != 'I' || bytes[13] != 'H' || bytes[14] != 'D' || bytes[15] != 'R')
        return false;
    width = (uint32_t{bytes[16]} << 24) | (uint32_t{bytes[17]} << 16) | (uint32_t{bytes[18]} << 8) |
            uint32_t{bytes[19]};
    height = (uint32_t{bytes[20]} << 24) | (uint32_t{bytes[21]} << 16) |
             (uint32_t{bytes[22]} << 8) | uint32_t{bytes[23]};
    return true;
}

std::vector<std::string> strings_of(const json::Json* value) {
    std::vector<std::string> out;
    if (value == nullptr || value->type() != json::JsonType::array)
        return out;
    out.reserve(value->elements().size());
    for (const json::Json& element : value->elements())
        out.push_back(text_of(&element));
    return out;
}

void expect_facts(const json::Json& report, std::string_view file) {
    const std::span<const std::string> names = report.names();
    OA_CHECK(names.size() == std::size(kKeys));
    if (names.size() == std::size(kKeys)) {
        for (std::size_t index = 0; index < names.size(); ++index)
            OA_CHECK(names[index] == kKeys[index]);
    }
    OA_CHECK(member(report, "ok") != nullptr && member(report, "ok")->boolean() == true);
    OA_CHECK(text_of(member(report, "kind")) == "oakeys");
    OA_CHECK(
        member(report, "revision") != nullptr &&
        member(report, "revision")->type() == json::JsonType::null
    );
    OA_CHECK(text_of(member(report, "id")) == "grid-test");
    OA_CHECK(text_of(member(report, "name")) == "Grid test");
    OA_CHECK(text_of(member(report, "version")) == "1.0");
    OA_CHECK(text_of(member(report, "file")) == file);
    OA_CHECK(text_of(member(report, "summary")) == "Orders on the left.");
    OA_CHECK(text_of(member(report, "description")) == "Orders on the left.\nThe rest stays.");
    OA_CHECK(text_of(member(report, "homepage")) == "https://example.com/grid-test");
    OA_CHECK(text_of(member(report, "author")) == "A. Maker");
    OA_CHECK(text_of(member(report, "base")) == "oa-default");
    OA_CHECK(text_of(member(report, "keyboard")) == "any");
    OA_CHECK(text_of(member(report, "pointer")) == "right-click");
    OA_CHECK(
        strings_of(member(report, "helpers")) ==
        std::vector<std::string>({"sticky-modifiers", "pointer-keys"})
    );
    OA_CHECK(member(report, "changes") != nullptr && member(report, "changes")->integer() == 2);
    OA_CHECK(strings_of(member(report, "triggers")) == std::vector<std::string>({"Q", "@KeyW"}));
    OA_CHECK(
        member(report, "unknown_actions") != nullptr &&
        member(report, "unknown_actions")->integer() == 0
    );
    OA_CHECK(member(report, "clashes") != nullptr && member(report, "clashes")->integer() == 0);
    OA_CHECK(strings_of(member(report, "works_with")) == std::vector<std::string>({"ridge >= 28"}));
    OA_CHECK(strings_of(member(report, "tags")) == std::vector<std::string>({"grid", "test"}));
    const json::Json* requirement = member(report, "requires");
    OA_CHECK(requirement != nullptr && text_of(requirement->find("engine")) == ">= 0.8.0");
    const json::Json* icon = member(report, "icon");
    OA_CHECK(icon != nullptr && text_of(icon->find("path")) == "icon.png");
    OA_CHECK(
        icon != nullptr && icon->find("width") != nullptr && icon->find("width")->integer() == 64
    );
}

/// The chord `order.attack` is bound to, or empty when the action is absent.
std::string attack_chord(const json::Json& rows) {
    for (const json::Json& row : rows.elements()) {
        if (text_of(row.find("action")) != "order.attack")
            continue;
        OA_CHECK(text_of(row.find("context")) == "play");
        const std::vector<std::string> chords = strings_of(row.find("chords"));
        return chords.size() == 1 ? chords[0] : std::string{};
    }
    return {};
}

void test_pack_and_check(const fs::path& scratch) {
    const std::vector<uint8_t> icon = png_image(64);
    const fs::path folder = scratch / "grid-test";
    write_text(folder / "oakeys.yaml", kManifest);
    write_text(folder / "README.md", "A made-up layout.\n");
    write_bytes(folder / "icon.png", icon);

    const fs::path out = scratch / "grid-test-1.0.oakeys";
    const fs::path grouped = scratch / "grouped.oakeys";
    const Captured packed = run({"keys", "pack", "--out", grouped.string(), folder.string()});
    OA_CHECK(packed.status == oa::tool::exit_done);
    if (packed.status != oa::tool::exit_done)
        std::fprintf(stderr, "keys pack: %s\n", packed.err.c_str());
    const Captured plain = run({"pack", "--out", out.string(), folder.string()});
    OA_CHECK(plain.status == oa::tool::exit_done);
    if (plain.status != oa::tool::exit_done)
        std::fprintf(stderr, "pack: %s\n", plain.err.c_str());
    OA_CHECK(read_bytes(grouped) == read_bytes(out));
    OA_CHECK(!fs::exists(fs::path(out.string() + ".part")));

    zip::StreamDirectory directory;
    OA_CHECK(read_directory(out, directory));
    const std::pair<std::string_view, zip::Method> expect[] = {
        {"oakeys.yaml", zip::Method::deflated},
        {"README.md", zip::Method::deflated},
        {"icon.png", zip::Method::stored},
    };
    OA_CHECK(directory.entries.size() == 3);
    if (directory.entries.size() == 3) {
        for (std::size_t index = 0; index < 3; ++index) {
            OA_CHECK(directory.entries[index].name == expect[index].first);
            OA_CHECK(directory.entries[index].method == expect[index].second);
        }
    }

    touch_tree(folder);
    const fs::path again = scratch / "grid-again.oakeys";
    const Captured repacked = run({"pack", folder.string(), "--out", again.string()});
    OA_CHECK(repacked.status == oa::tool::exit_done);
    OA_CHECK(read_bytes(out) == read_bytes(again));

    const Captured checked = run({"check", out.string(), "--json"});
    OA_CHECK(checked.status == oa::tool::exit_done);
    if (checked.status != oa::tool::exit_done)
        std::fprintf(stderr, "check: %s\n%s\n", checked.out.c_str(), checked.err.c_str());
    expect_facts(parse_report(checked.out), out.filename().string());

    const Captured folder_check = run({"keys", "check", folder.string(), "--json"});
    OA_CHECK(folder_check.status == oa::tool::exit_done);
    if (folder_check.status != oa::tool::exit_done)
        std::fprintf(
            stderr, "keys check: %s\n%s\n", folder_check.out.c_str(), folder_check.err.c_str()
        );
    expect_facts(parse_report(folder_check.out), folder.filename().string());

    const Captured text = run({"check", out.string()});
    OA_CHECK(text.status == oa::tool::exit_done);
    OA_CHECK(text.out.ends_with("result: ok\n"));

    const fs::path wrong = scratch / "layout.zip";
    const Captured refused_name = run({"pack", folder.string(), "--out", wrong.string()});
    OA_CHECK(refused_name.status == oa::tool::exit_failed);
    OA_CHECK(contains(refused_name.err, ".oakeys"));
    OA_CHECK(!fs::exists(wrong));

    const fs::path cwd = scratch / "cwd";
    fs::create_directories(cwd);
    {
        const WorkingDirectory here(cwd);
        const Captured named = run({"keys", "pack", folder.string()});
        OA_CHECK(named.status == oa::tool::exit_done);
        OA_CHECK(contains(named.out, "packed grid-test-1.0.oakeys: "));
    }
    OA_CHECK(fs::exists(cwd / "grid-test-1.0.oakeys"));

    const Captured game_dir = run({"keys", "check", out.string(), "--game-dir", scratch.string()});
    OA_CHECK(game_dir.status == oa::tool::exit_usage);
    OA_CHECK(contains(game_dir.err, "option '--game-dir' is not for a key layout"));
}

void test_unknown_and_strict(const fs::path& scratch) {
    const fs::path folder = scratch / "grid-future";
    write_text(
        folder / "oakeys.yaml",
        "oakeys: 1\n"
        "id: grid-future\n"
        "name: \"Grid future\"\n"
        "version: \"1.0\"\n"
        "author: {name: \"A. Maker\"}\n"
        "tags: [grid]\n"
        "base: oa-default\n"
        "bindings:\n"
        "  order.not-yet: [\"Q\"]\n"
    );
    const fs::path out = scratch / "grid-future-1.0.oakeys";
    const Captured packed = run({"pack", folder.string(), "--out", out.string()});
    OA_CHECK(packed.status == oa::tool::exit_done);
    if (packed.status != oa::tool::exit_done)
        std::fprintf(stderr, "future pack: %s\n", packed.err.c_str());

    const Captured checked = run({"check", out.string(), "--json"});
    OA_CHECK(checked.status == oa::tool::exit_done);
    const json::Json report = parse_report(checked.out);
    OA_CHECK(member(report, "ok") != nullptr && member(report, "ok")->boolean() == true);
    OA_CHECK(
        member(report, "unknown_actions") != nullptr &&
        member(report, "unknown_actions")->integer() == 1
    );
    OA_CHECK(contains(checked.out, "Not in this version of Open Annihilation"));

    const Captured strict = run({"keys", "check", out.string(), "--strict", "--json"});
    OA_CHECK(strict.status == oa::tool::exit_failed);
    const json::Json refused = parse_report(strict.out);
    OA_CHECK(member(refused, "ok") != nullptr && member(refused, "ok")->boolean() == false);
    OA_CHECK(contains(strict.out, "Not in this version of Open Annihilation"));
    const Captured strict_text = run({"keys", "check", folder.string(), "--strict"});
    OA_CHECK(strict_text.status == oa::tool::exit_failed);
    OA_CHECK(strict_text.out.ends_with("result: refused\n"));
}

/// Checks a layout folder as JSON.
///
/// @param folder the folder
/// @param strict refuse a warning
/// @return the run
Captured check_folder(const fs::path& folder, bool strict) {
    std::vector<std::string> arguments{"keys", "check", folder.string(), "--json"};
    if (strict)
        arguments.emplace_back("--strict");
    return run(std::move(arguments));
}

/// The clash count a report gives, or -1 when it gives none.
///
/// @param report the report
/// @return the count
int64_t clash_count(const json::Json& report) {
    const json::Json* clashes = member(report, "clashes");
    if (clashes == nullptr)
        return -1;
    return clashes->integer().value_or(-1);
}

/// True when one of the report's warnings holds the words.
///
/// @param report the report
/// @param words the words
/// @return true when a warning holds them
bool warns(const json::Json& report, std::string_view words) {
    for (const std::string& line : strings_of(member(report, "warnings"))) {
        if (contains(line, words))
            return true;
    }
    return false;
}

/// Primary and Ctrl, or Primary and Meta, on two actions are a clash on the
/// platforms that press them as one key, and the warning names those
/// platforms. Two chords of one action are not a clash, and neither are
/// Find on Open Annihilation's screens and Select factories in a match.
void test_platform_clashes(const fs::path& scratch) {
    const fs::path left = scratch / "left-hand";
    write_text(left / "oakeys.yaml", kLeftHand);
    const Captured checked = check_folder(left, false);
    OA_CHECK(checked.status == oa::tool::exit_done);
    const json::Json report = parse_report(checked.out);
    if (clash_count(report) != 2)
        std::fprintf(stderr, "left hand: %s\n%s\n", checked.out.c_str(), checked.err.c_str());
    OA_CHECK(member(report, "ok") != nullptr && member(report, "ok")->boolean() == true);
    OA_CHECK(clash_count(report) == 2);
    OA_CHECK(strings_of(member(report, "warnings")).size() == 2);
    OA_CHECK(warns(
        report,
        "bindings.ui.keys-overlay[0]: ui.keys-overlay and group.assign-5 both bind "
        "Ctrl+@Digit5 on Windows, Linux and the Steam Deck"
    ));
    OA_CHECK(warns(
        report,
        "bindings.ui.settings[0]: ui.settings and group.assign-6 both bind Ctrl+@Digit6 on "
        "Windows, Linux and the Steam Deck"
    ));
    const Captured strict = check_folder(left, true);
    OA_CHECK(strict.status == oa::tool::exit_failed);
    const Captured text = run({"keys", "check", left.string()});
    OA_CHECK(text.status == oa::tool::exit_done);
    OA_CHECK(contains(text.out, "clashes: 2"));
    OA_CHECK(contains(text.out, "on Windows, Linux and the Steam Deck"));

    const fs::path command = scratch / "command-pair";
    write_text(
        command / "oakeys.yaml",
        "oakeys: 1\n"
        "id: command-pair\n"
        "name: \"Command pair\"\n"
        "version: \"1.0\"\n"
        "author: {name: \"A. Maker\"}\n"
        "tags: [test]\n"
        "base: oa-default\n"
        "bindings:\n"
        "  group.assign-5: [\"Meta+@Digit5\"]\n"
        "  ui.keys-overlay: [\"Primary+@Digit5\"]\n"
    );
    const Captured command_check = check_folder(command, false);
    OA_CHECK(command_check.status == oa::tool::exit_done);
    const json::Json command_report = parse_report(command_check.out);
    OA_CHECK(clash_count(command_report) == 1);
    OA_CHECK(
        warns(command_report, "ui.keys-overlay and group.assign-5 both bind Meta+@Digit5 on macOS")
    );

    const fs::path paste = scratch / "paste-pair";
    write_text(
        paste / "oakeys.yaml",
        "oakeys: 1\n"
        "id: paste-pair\n"
        "name: \"Paste pair\"\n"
        "version: \"1.0\"\n"
        "author: {name: \"A. Maker\"}\n"
        "tags: [test]\n"
        "base: oa-default\n"
        "bindings:\n"
        "  ui.screen-paste: [\"Primary+V\", \"Ctrl+V\"]\n"
        "  select.factories: [\"Ctrl+F\"]\n"
        "  ui.screen-find: [\"Primary+F\"]\n"
        "  ui.keys-overlay: [\"Primary+Ctrl+Meta+@Digit5\"]\n"
        "  group.assign-5: [\"Ctrl+Meta+@Digit5\"]\n"
    );
    const Captured paste_check = check_folder(paste, false);
    OA_CHECK(paste_check.status == oa::tool::exit_done);
    const json::Json paste_report = parse_report(paste_check.out);
    if (clash_count(paste_report) != 1)
        std::fprintf(stderr, "paste pair: %s\n", paste_check.out.c_str());
    OA_CHECK(clash_count(paste_report) == 1);
    OA_CHECK(!warns(paste_report, "ui.screen-paste and ui.screen-paste"));
    OA_CHECK(!warns(paste_report, "ui.screen-find and select.factories"));
    OA_CHECK(warns(
        paste_report,
        "group.assign-5 and ui.keys-overlay both bind Ctrl+Meta+@Digit5 on Windows, Linux, the "
        "Steam Deck and macOS"
    ));
}

void refuse_both(const fs::path& folder, const fs::path& out, std::string_view snippet) {
    const Captured packed = run({"pack", folder.string(), "--out", out.string()});
    OA_CHECK(packed.status == oa::tool::exit_failed);
    OA_CHECK(contains(packed.err, snippet));
    OA_CHECK(!fs::exists(out));
    const Captured grouped = run({"keys", "pack", folder.string(), "--out", out.string()});
    OA_CHECK(grouped.status == oa::tool::exit_failed);
    OA_CHECK(contains(grouped.err, snippet));
    const Captured checked = run({"keys", "check", folder.string(), "--json"});
    OA_CHECK(checked.status == oa::tool::exit_failed);
    OA_CHECK(contains(checked.out + checked.err, snippet));
}

void test_refusals(const fs::path& scratch) {
    const fs::path chord = scratch / "bad-chord";
    write_text(
        chord / "oakeys.yaml",
        "oakeys: 1\n"
        "id: bad-chord\n"
        "name: \"Bad chord\"\n"
        "version: \"1.0\"\n"
        "author: {name: \"A. Maker\"}\n"
        "tags: [grid]\n"
        "base: oa-default\n"
        "bindings:\n"
        "  order.attack: [\"???\"]\n"
    );
    refuse_both(chord, scratch / "bad-chord.oakeys", "oakeys.yaml:");
    const Captured chord_check = run({"check", chord.string()});
    OA_CHECK(contains(chord_check.out + chord_check.err, "not a trigger"));

    const fs::path outside = scratch / "outside";
    write_text(outside / "oakeys.yaml", kManifest);
    write_bytes(outside / "icon.png", png_image(64));
    write_text(outside / "extra.lua", "return 1\n");
    refuse_both(
        outside, scratch / "outside.oakeys", "extra.lua: outside the files a key layout may hold"
    );

    const fs::path huge = scratch / "huge-icon";
    write_text(huge / "oakeys.yaml", kManifest);
    write_bytes(huge / "icon.png", png_image(300));
    refuse_both(
        huge, scratch / "huge-icon.oakeys", "not a square PNG of at most 256 pixels and 256 KiB"
    );

    const fs::path both = scratch / "both";
    write_text(both / "oakeys.yaml", kManifest);
    write_text(both / "oamut.yaml", "oamut: 1\n");
    refuse_both(both, scratch / "both.oakeys", "the folder holds both oakeys.yaml and oamut.yaml");

    const fs::path mod = scratch / "mod";
    write_text(mod / "oamod.yaml", "oamod: 1\n");
    const Captured mod_pack = run({"keys", "pack", mod.string()});
    OA_CHECK(mod_pack.status == oa::tool::exit_usage);
    OA_CHECK(contains(mod_pack.err, "the folder holds no oakeys.yaml"));
}

void test_picture_and_show(const fs::path& scratch) {
    const fs::path folder = scratch / "grid-test";
    const fs::path picture = scratch / "grid.png";
    const fs::path again = scratch / "grid-again.png";
    const Captured drawn = run({"keys", "picture", folder.string(), "--out", picture.string()});
    OA_CHECK(drawn.status == oa::tool::exit_done);
    if (drawn.status != oa::tool::exit_done)
        std::fprintf(stderr, "picture: %s\n", drawn.err.c_str());
    const Captured redrawn = run({"keys", "picture", "--out", again.string(), folder.string()});
    OA_CHECK(redrawn.status == oa::tool::exit_done);
    const std::vector<uint8_t> first = read_bytes(picture);
    const std::vector<uint8_t> second = read_bytes(again);
    OA_CHECK(first == second);
    OA_CHECK(first.size() < 256u * 1024u);
    uint32_t width = 0;
    uint32_t height = 0;
    OA_CHECK(png_size(first, width, height));
    OA_CHECK(width == 1200);
    OA_CHECK(height == 325);

    const Captured base = run({"keys", "show", "oa-default", "--json"});
    OA_CHECK(base.status == oa::tool::exit_done);
    if (base.status != oa::tool::exit_done)
        std::fprintf(stderr, "show default: %s\n", base.err.c_str());
    OA_CHECK(attack_chord(parse_array(base.out)) == "A");

    const Captured shown = run({"keys", "show", folder.string(), "--json"});
    OA_CHECK(shown.status == oa::tool::exit_done);
    if (shown.status != oa::tool::exit_done)
        std::fprintf(stderr, "show layout: %s\n", shown.err.c_str());
    OA_CHECK(attack_chord(parse_array(shown.out)) == "Q");
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
    run_case("test_unknown_and_strict", [&] { test_unknown_and_strict(scratch.path); });
    run_case("test_refusals", [&] { test_refusals(scratch.path); });
    run_case("test_platform_clashes", [&] { test_platform_clashes(scratch.path); });
    run_case("test_picture_and_show", [&] { test_picture_and_show(scratch.path); });
    return oa::test::check_exit_status();
}

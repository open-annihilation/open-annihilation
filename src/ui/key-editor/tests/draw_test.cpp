// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The key editor drawn twice into the viewport: Compact and Large at 1x,
// Regular at 2x, Compact's details page, and Large over a paused match.
// The two drawings of a scene are byte for byte the same. The dialog sits
// at the canvas origin; every pixel of it is painted, and nothing is drawn
// outside it, so the margin around the dialog keeps the colour the canvas
// started as. The picture is the whole viewport.
//
// --frames DIR writes key-editor-compact.png, key-editor-large.png,
// key-editor-regular.png, key-editor-compact-details.png and
// key-editor-large-match.png at 640x480, 1280x720, 1920x1080, 640x480 and
// 1280x720. --record merges this group's hashes into the frames table and
// leaves the other group. --data draws in the installed game's fonts.
// A scene with no hash fails unless --record is set.
//
//   oa-ui-key-editor-draw-test [--data] [--record] [--frames DIR]

#include "oa/base/sha256.hpp"
#include "oa/formats/png.hpp"
#include "oa/test/check.hpp"
#include "oa/test/game_assets.hpp"
#include "oa/test/game_data.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/key_editor/model.hpp"
#include "oa/ui/key_editor/screen.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/theme.hpp"
#include "screen_fixture.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef OA_KEY_EDITOR_FRAMES
#define OA_KEY_EDITOR_FRAMES "src/ui/key-editor/tests/frames.txt"
#endif

namespace {

namespace kit = oa::ui::kit;
namespace editor = oa::ui::key_editor;
namespace renderer = oa::ui::frontend_renderer;
namespace png = oa::formats::png;
namespace sha256 = oa::base::sha256;
using key_editor_fixture::editor_fixture;
using key_editor_fixture::fonts;
using key_editor_fixture::frame_for;

/// The colour the picture starts as. Only this test names it.
constexpr kit::Colour untouched{0x12, 0x34, 0x56};

/// One picture.
struct Scene {
    const char* name{};       ///< the file's name after "key-editor-"
    int32_t width{};          ///< the canvas width, in pixels
    int32_t height{};         ///< the canvas height, in pixels
    int32_t scale_percent{};  ///< the scale, as a percentage
    kit::Point window{};      ///< the dialog, in points
    bool details{false};      ///< Compact's details page
    bool in_match{false};     ///< the header says the game is paused
};

/// A drawing of a scene.
struct Drawing {
    renderer::Surface surface{}; ///< the viewport
    kit::Point window{};         ///< the dialog, in points
    int32_t scale{1};            ///< pixels a point
};

/// The five pictures, in table order.
const Scene kScenes[] = {
    {"compact", 640, 480, 100, {480, 324}, false, false},
    {"large", 1280, 720, 100, {960, 600}, false, false},
    {"regular", 1920, 1080, 200, {720, 486}, false, false},
    {"compact-details", 640, 480, 100, {480, 324}, true, false},
    {"large-match", 1280, 720, 100, {960, 600}, false, true},
};

/// True when a text is 64 hexadecimal digits.
///
/// @param text the text
/// @return true when it is a digest
bool hex_digits(std::string_view text) {
    if (text.size() != sha256::hex_size)
        return false;
    for (const char digit : text) {
        const bool hex = (digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f');
        if (!hex)
            return false;
    }
    return true;
}

/// The digest of a picture: four little-endian bytes of width, four of
/// height, then the RGB bytes.
///
/// @param surface the picture
/// @return the hex digest
std::string digest_of(const renderer::Surface& surface) {
    std::array<uint8_t, 8> header{};
    const auto store = [&header](uint32_t value, std::size_t offset) {
        header[offset] = static_cast<uint8_t>(value);
        header[offset + 1] = static_cast<uint8_t>(value >> 8);
        header[offset + 2] = static_cast<uint8_t>(value >> 16);
        header[offset + 3] = static_cast<uint8_t>(value >> 24);
    };
    store(surface.width, 0);
    store(surface.height, 4);
    sha256::Hasher hasher;
    sha256::update(hasher, header);
    sha256::update(hasher, surface.rgb);
    const auto hex = sha256::to_hex(sha256::finish(hasher));
    return std::string(hex.begin(), hex.end());
}

/// Lays one scene out and draws it at the canvas origin.
///
/// @param scene the scene
/// @param measured the fonts
/// @return the drawing
Drawing draw_scene(const Scene& scene, const kit::Fonts& measured) {
    const editor::Editor model(editor_fixture());
    editor::ScreenState state;
    state.selected = "order.attack";
    state.details_page = scene.details;
    state.in_match = scene.in_match;
    const kit::Frame frame = frame_for(scene.width, scene.height, scene.scale_percent);
    Drawing drawing;
    drawing.scale = frame.scale_percent / 100;
    drawing.window = editor::window_size(frame);
    const kit::DisplayList list = editor::editor_layout(model, state, frame, measured);
    drawing.surface.width = static_cast<uint32_t>(scene.width);
    drawing.surface.height = static_cast<uint32_t>(scene.height);
    drawing.surface.rgb.assign(
        static_cast<std::size_t>(scene.width) * static_cast<std::size_t>(scene.height) * 3, 0
    );
    for (std::size_t at = 0; at < drawing.surface.rgb.size(); at += 3) {
        drawing.surface.rgb[at] = untouched.r;
        drawing.surface.rgb[at + 1] = untouched.g;
        drawing.surface.rgb[at + 2] = untouched.b;
    }
    kit::Canvas canvas;
    canvas.surface = &drawing.surface;
    canvas.placement = {0, 0, drawing.scale, {}};
    canvas.fonts = &measured;
    editor::draw_editor(canvas, list, drawing.window);
    return drawing;
}

/// True when a pixel still has the colour the canvas started as.
///
/// @param drawing the drawing
/// @param x the column
/// @param y the row
/// @return true when nothing drew there
bool kept(const Drawing& drawing, int32_t x, int32_t y) {
    const std::size_t at =
        (static_cast<std::size_t>(y) * drawing.surface.width + static_cast<std::size_t>(x)) * 3;
    return drawing.surface.rgb[at] == untouched.r && drawing.surface.rgb[at + 1] == untouched.g &&
           drawing.surface.rgb[at + 2] == untouched.b;
}

/// Checks the dialog size, that every dialog pixel was painted, and that
/// nothing was drawn outside the dialog.
///
/// @param scene the scene
/// @param drawing its drawing
void check_dialog(const Scene& scene, const Drawing& drawing) {
    if (drawing.window.x != scene.window.x || drawing.window.y != scene.window.y) {
        std::fprintf(
            stderr,
            "%s: window %d x %d\n",
            scene.name,
            static_cast<int>(drawing.window.x),
            static_cast<int>(drawing.window.y)
        );
    }
    OA_CHECK(drawing.window.x == scene.window.x && drawing.window.y == scene.window.y);
    OA_CHECK(drawing.scale >= 1);
    const int32_t right = drawing.window.x * drawing.scale;
    const int32_t bottom = drawing.window.y * drawing.scale;
    OA_CHECK(right > 0 && bottom > 0);
    OA_CHECK(right <= static_cast<int32_t>(drawing.surface.width));
    OA_CHECK(bottom <= static_cast<int32_t>(drawing.surface.height));
    std::size_t outside = 0;
    std::size_t blank = 0;
    for (int32_t y = 0; y < static_cast<int32_t>(drawing.surface.height); ++y) {
        for (int32_t x = 0; x < static_cast<int32_t>(drawing.surface.width); ++x) {
            const bool in_dialog = x < right && y < bottom;
            const bool untouched_here = kept(drawing, x, y);
            if (!in_dialog && !untouched_here)
                ++outside;
            if (in_dialog && untouched_here)
                ++blank;
        }
    }
    if (outside != 0)
        std::fprintf(stderr, "%s: %zu pixels drawn outside the dialog\n", scene.name, outside);
    if (blank != 0)
        std::fprintf(stderr, "%s: %zu dialog pixels not drawn\n", scene.name, blank);
    OA_CHECK(outside == 0);
    OA_CHECK(blank == 0);
}

/// Writes the whole viewport as a PNG.
///
/// @param drawing the drawing
/// @param path the file
/// @return true when it was written
bool write_canvas(const Drawing& drawing, const std::filesystem::path& path) {
    png::Header header;
    header.width = drawing.surface.width;
    header.height = drawing.surface.height;
    header.bit_depth = 8;
    header.color_type = png::ColorType::rgb;
    std::vector<uint8_t> file;
    if (!png::write(png::Image{header, {}, drawing.surface.rgb}, &file))
        return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size())
    );
    return static_cast<bool>(output);
}

/// One stored line of the frames table.
struct TableLine {
    std::string text{};  ///< the line, without its newline
    bool scene{false};   ///< a group/scene hash
    std::string name{};  ///< group/scene, when scene
    std::string hex{};   ///< the hash, when scene
};

/// Reads the frames table. A missing file is an empty table and a failed check.
///
/// @param path the table
/// @return the lines
std::vector<TableLine> read_table(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::vector<TableLine> lines;
    if (!input) {
        std::fprintf(stderr, "draw: cannot read %s\n", path.string().c_str());
        OA_CHECK(false);
        return lines;
    }
    std::string text;
    std::size_t number = 0;
    while (std::getline(input, text)) {
        ++number;
        if (!text.empty() && text.back() == '\r')
            text.pop_back();
        TableLine line;
        line.text = text;
        if (text.empty() || text.starts_with("//") || text.starts_with("#")) {
            lines.push_back(std::move(line));
            continue;
        }
        const std::size_t space = text.find(' ');
        const bool one_space = space != std::string::npos && text.find(' ', space + 1) == std::string::npos;
        if (one_space) {
            line.name = text.substr(0, space);
            line.hex = text.substr(space + 1);
            line.scene = !line.name.empty() && line.name.find('/') != std::string::npos && hex_digits(line.hex);
        }
        if (!line.scene) {
            std::fprintf(stderr, "draw: %s:%zu is not a scene line\n", path.string().c_str(), number);
            OA_CHECK(false);
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

/// Writes the table back, replacing only one group's scene lines.
///
/// @param path the table
/// @param lines the lines that were read
/// @param group the group to replace
/// @param digests scene name to hash, without the group
/// @return true when the file was written
bool write_merged(
    const std::filesystem::path& path,
    const std::vector<TableLine>& lines,
    std::string_view group,
    const std::vector<std::pair<std::string, std::string>>& digests
) {
    const std::string prefix = std::string(group) + "/";
    std::string fresh;
    for (const auto& [scene, hex] : digests)
        fresh += prefix + scene + " " + hex + "\n";
    std::string out;
    bool wrote = false;
    for (const TableLine& line : lines) {
        if (line.scene && line.name.starts_with(prefix)) {
            if (!wrote) {
                out += fresh;
                wrote = true;
            }
            continue;
        }
        out += line.text;
        out += '\n';
    }
    if (!wrote)
        out += fresh;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(out.data(), static_cast<std::streamsize>(out.size()));
    return static_cast<bool>(output);
}

} // namespace

int main(int argc, char** argv) {
    bool record = false;
    bool data = false;
    std::optional<std::filesystem::path> frames;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--record")
            record = true;
        else if (argument == "--frames" && index + 1 < argc)
            frames = std::filesystem::path(argv[++index]);
        else if (argument == oa::test::kGameDataSwitch)
            data = true;
        else {
            std::fprintf(stderr, "draw: unexpected argument %s\n", argv[index]);
            return 2;
        }
    }

    kit::Fonts measured = fonts();
    if (data) {
        oa::AssetStore assets = oa::test::require_game_assets("the key editor's pictures");
        measured = kit::load_game_fonts(assets);
    }
    const std::string group = data ? "game" : "block";
    if (frames) {
        std::error_code directory_error;
        std::filesystem::create_directories(*frames, directory_error);
        if (directory_error) {
            std::fprintf(stderr, "draw: cannot make %s\n", frames->string().c_str());
            return 1;
        }
    }

    std::vector<std::pair<std::string, std::string>> digests;
    for (const Scene& scene : kScenes) {
        const Drawing first = draw_scene(scene, measured);
        const Drawing second = draw_scene(scene, measured);
        if (first.surface.width != static_cast<uint32_t>(scene.width) ||
            first.surface.height != static_cast<uint32_t>(scene.height)) {
            std::fprintf(
                stderr,
                "%s: canvas %u x %u\n",
                scene.name,
                first.surface.width,
                first.surface.height
            );
        }
        OA_CHECK(first.surface.width == static_cast<uint32_t>(scene.width));
        OA_CHECK(first.surface.height == static_cast<uint32_t>(scene.height));
        if (first.surface.rgb != second.surface.rgb)
            std::fprintf(stderr, "%s: two drawings differ\n", scene.name);
        OA_CHECK(first.surface.rgb == second.surface.rgb);
        check_dialog(scene, first);
        digests.emplace_back(scene.name, digest_of(first.surface));
        if (frames) {
            const std::filesystem::path path = *frames / (std::string("key-editor-") + scene.name + ".png");
            const bool written = write_canvas(first, path);
            if (!written)
                std::fprintf(stderr, "draw: cannot write %s\n", path.string().c_str());
            OA_CHECK(written);
        }
    }

    const std::filesystem::path table = OA_KEY_EDITOR_FRAMES;
    const std::vector<TableLine> lines = read_table(table);
    const std::string prefix = group + "/";
    for (const auto& [scene, hex] : digests) {
        const std::string name = prefix + scene;
        const TableLine* found = nullptr;
        for (const TableLine& line : lines) {
            if (line.scene && line.name == name)
                found = &line;
        }
        if (found == nullptr) {
            if (!record) {
                std::fprintf(stderr, "draw: %s is missing from %s\n", name.c_str(), table.string().c_str());
                OA_CHECK(false);
            }
            continue;
        }
        if (!record && found->hex != hex) {
            std::fprintf(stderr, "draw: %s differs\n", name.c_str());
            OA_CHECK(false);
        }
    }
    if (record) {
        const bool written = write_merged(table, lines, group, digests);
        if (!written)
            std::fprintf(stderr, "draw: cannot write %s\n", table.string().c_str());
        OA_CHECK(written);
    }
    return oa::test::failed_checks() == 0 ? 0 : 1;
}

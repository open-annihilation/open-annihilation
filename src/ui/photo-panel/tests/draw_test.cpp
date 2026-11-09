// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The photo panel drawn twice: a skirmish, a replay and a shared match at
// 640x480, 1280x720 and 1920x1080, in the classic look and the modern look.
// The two drawings of a scene are byte for byte the same. Focus is left
// unset, so the pictures have no focus ring. --frames DIR writes
// photo-panel-<scene>.png. --record merges this group's hashes into the
// frames table and leaves the other group. --data draws in the installed
// game's fonts.
//
//   oa-ui-photo-panel-draw-test [--data] [--record] [--frames DIR]

#include "oa/base/sha256.hpp"
#include "oa/formats/png.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/test/check.hpp"
#include "oa/test/game_assets.hpp"
#include "oa/test/game_data.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/photo_panel/crop.hpp"
#include "oa/ui/photo_panel/draw.hpp"
#include "block_font.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef OA_PHOTO_PANEL_FRAMES
#define OA_PHOTO_PANEL_FRAMES "src/ui/photo-panel/tests/frames.txt"
#endif

namespace {

namespace kit = oa::ui::kit;
namespace panel = oa::ui::photo_panel;
namespace renderer = oa::ui::frontend_renderer;
namespace png = oa::formats::png;
namespace sha256 = oa::base::sha256;

/// The battlefield this test fills before the panel is drawn.
constexpr kit::Colour kSand{0xc4, 0xa8, 0x74};

/// One picture.
struct Scene {
    std::string name{};
    int32_t width{};
    int32_t height{};
    panel::Visit visit{panel::Visit::skirmish};
    bool modern{false};
    bool shown{true};
    panel::Guide guide{panel::Guide::off};
    bool toast{false};
};

/// A line of the frames table.
struct TableLine {
    std::string text{};
    std::string name{};
    std::string hex{};
    bool scene{false};
};

/// The scenes the pins cover.
std::vector<Scene> all_scenes() {
    std::vector<Scene> scenes;
    const panel::Visit visits[] = {
        panel::Visit::skirmish, panel::Visit::replay, panel::Visit::shared
    };
    const char* visit_names[] = {"skirmish", "replay", "shared"};
    const int32_t widths[] = {640, 1280, 1920};
    const int32_t heights[] = {480, 720, 1080};
    const char* looks[] = {"classic", "modern"};
    for (int which_visit = 0; which_visit < 3; ++which_visit) {
        for (int size = 0; size < 3; ++size) {
            for (int look = 0; look < 2; ++look) {
                Scene scene;
                scene.name = std::string(visit_names[which_visit]) + "-" +
                             std::to_string(widths[size]) + "x" + std::to_string(heights[size]) +
                             "-" + looks[look];
                scene.width = widths[size];
                scene.height = heights[size];
                scene.visit = visits[which_visit];
                scene.modern = look == 1;
                scenes.push_back(scene);
            }
        }
    }
    Scene hidden;
    hidden.name = "hidden-1280x720-classic";
    hidden.width = 1280;
    hidden.height = 720;
    hidden.shown = false;
    scenes.push_back(hidden);
    Scene guides;
    guides.name = "guides-1280x720-classic";
    guides.width = 1280;
    guides.height = 720;
    guides.guide = panel::Guide::thirds;
    scenes.push_back(guides);
    Scene toast;
    toast.name = "toast-1280x720-classic";
    toast.width = 1280;
    toast.height = 720;
    toast.toast = true;
    scenes.push_back(toast);
    Scene crop;
    crop.name = "crop-1280x720-classic";
    crop.width = 1280;
    crop.height = 720;
    crop.guide = panel::Guide::crop_1_1;
    scenes.push_back(crop);
    return scenes;
}

/// The panel a scene draws. Strings are fixed, so the picture does not
/// depend on a clock.
panel::Model model_for(const Scene& scene) {
    panel::Model model;
    model.width = scene.width;
    model.height = scene.height;
    model.size_class = kit::size_class_of(scene.width, scene.height);
    model.visit = scene.visit;
    model.shown = scene.shown;
    model.guide = scene.guide;
    model.zoom_value = "1.0  Medium";
    model.turn_value = "0";
    model.tilt_value = "0";
    model.turn_lock = "Needs Full acceleration";
    model.tilt_lock = "Needs Full acceleration";
    model.speed_value = "30%";
    model.between_value = "8";
    model.clock = "12:34";
    model.file_name = "Photo 2026-11-05 13.12.08 Canal Crossing.png";
    model.head = "F9 or Esc leaves";
    model.hint_label = "H shows the panel";
    model.freeze_caption = "Freeze";
    model.time_mode = 2;
    const std::vector<float> zooms = panel::zoom_stops(0.083F, 8.F);
    model.zoom_stops = static_cast<int32_t>(zooms.size());
    model.zoom_stop = panel::zoom_stop_nearest(zooms, 1.F);
    model.key_line = "F9 Leave   Enter Take photo   Space Freeze   H Hide panel   G Guides";
    model.guide_value = std::string(panel::guide_name(scene.guide));
    if (scene.visit == panel::Visit::shared) {
        model.freeze_caption = "Hold picture";
        model.slow_lock = "Shared game";
        model.speed_lock = "Shared game";
        model.step_lock = "Shared game";
        model.between_lock = "Shared game";
        model.time_mode = 0;
        model.chips[0] = "Live";
        model.live_amber = true;
        model.alerts = "Commander is under attack  2 chat lines";
        model.key_line =
            "F9 Leave   Enter Take photo   Space Hold picture   H Hide panel   G Guides";
    } else if (scene.visit == panel::Visit::replay) {
        model.time_mode = 0;
        model.chips[0] = "Replay paused";
        model.chips[1] = "12:34";
    } else {
        model.chips[0] = "Photo mode";
        model.chips[1] = "Frozen";
        model.chips[2] = "12:34";
    }
    if (scene.toast) {
        model.toast = true;
        model.toast_name = model.file_name;
        model.toast_note = "Copied";
        model.toast_columns = 4;
        model.toast_rows = 2;
        model.toast_rgb.assign(4U * 2U * 3U, 0x3a);
    }
    return model;
}

/// Fills the picture with the stand-in battlefield.
void fill_sand(renderer::Surface& surface) {
    for (std::size_t index = 0; index + 2 < surface.rgb.size(); index += 3) {
        surface.rgb[index] = kSand.r;
        surface.rgb[index + 1] = kSand.g;
        surface.rgb[index + 2] = kSand.b;
    }
}

/// Draws one scene.
renderer::Surface draw_scene(const Scene& scene, kit::Fonts fonts) {
    panel::Model model = model_for(scene);
    fonts.size_class = model.size_class;
    const panel::Layout placed = panel::layout_of(model, fonts);
    kit::Interaction interaction;
    const kit::DisplayList list = panel::shown_list(placed, interaction);
    renderer::Surface surface;
    surface.width = static_cast<uint32_t>(scene.width);
    surface.height = static_cast<uint32_t>(scene.height);
    surface.rgb.assign(
        static_cast<std::size_t>(scene.width) * static_cast<std::size_t>(scene.height) * 3U, 0
    );
    fill_sand(surface);
    kit::Canvas canvas;
    canvas.surface = &surface;
    canvas.placement = {0, 0, 1, {}};
    canvas.fonts = &fonts;
    panel::draw_panel(canvas, model, placed, list);
    return surface;
}

/// Tells whether a text is a digest.
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

/// Writes the picture.
bool write_canvas(const renderer::Surface& surface, const std::filesystem::path& path) {
    png::Header header;
    header.width = surface.width;
    header.height = surface.height;
    header.bit_depth = 8;
    header.color_type = png::ColorType::rgb;
    std::vector<uint8_t> file;
    if (!png::write(png::Image{header, {}, surface.rgb}, &file))
        return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size())
    );
    return static_cast<bool>(output);
}

/// Reads the frames table. A missing file is an empty table.
std::vector<TableLine> read_table(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input)
        return {};
    std::vector<TableLine> lines;
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
        const bool one_space =
            space != std::string::npos && text.find(' ', space + 1) == std::string::npos;
        if (one_space) {
            line.name = text.substr(0, space);
            line.hex = text.substr(space + 1);
            line.scene = !line.name.empty() && line.name.find('/') != std::string::npos &&
                         hex_digits(line.hex);
        }
        if (!line.scene) {
            std::fprintf(
                stderr, "draw: %s:%zu is not a scene line\n", path.string().c_str(), number
            );
            OA_CHECK(false);
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

/// Writes the table back, replacing only one group's scene lines.
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

    auto stack =
        oa::platform::text_font::FontStack::open(oa::platform::text_font::bundled_font_directory());
    if (!stack) {
        std::fprintf(stderr, "draw: the modern fonts did not open\n");
        return 1;
    }

    kit::Fonts measured = photo_panel_test::block_fonts();
    if (data) {
        oa::AssetStore assets = oa::test::require_game_assets("the photo panel's pictures");
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
    for (const Scene& scene : all_scenes()) {
        kit::Fonts fonts = measured;
        if (scene.modern)
            fonts.modern = &*stack;
        const renderer::Surface first = draw_scene(scene, fonts);
        const renderer::Surface second = draw_scene(scene, fonts);
        if (first.rgb != second.rgb)
            std::fprintf(stderr, "%s: two drawings differ\n", scene.name.c_str());
        OA_CHECK(first.rgb == second.rgb);
        OA_CHECK(first.width == static_cast<uint32_t>(scene.width));
        OA_CHECK(first.height == static_cast<uint32_t>(scene.height));
        bool painted = false;
        for (std::size_t index = 0; index + 2 < first.rgb.size(); index += 3) {
            if (first.rgb[index] != kSand.r || first.rgb[index + 1] != kSand.g ||
                first.rgb[index + 2] != kSand.b) {
                painted = true;
                break;
            }
        }
        if (!painted)
            std::fprintf(stderr, "%s: nothing was drawn\n", scene.name.c_str());
        OA_CHECK(painted);
        digests.emplace_back(scene.name, digest_of(first));
        if (frames) {
            const std::filesystem::path path = *frames / ("photo-panel-" + scene.name + ".png");
            if (!write_canvas(first, path)) {
                std::fprintf(stderr, "draw: cannot write %s\n", path.string().c_str());
                OA_CHECK(false);
            }
        }
    }

    const std::filesystem::path table = OA_PHOTO_PANEL_FRAMES;
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
                std::fprintf(
                    stderr, "draw: %s is missing from %s\n", name.c_str(), table.string().c_str()
                );
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
    return oa::test::check_exit_status();
}

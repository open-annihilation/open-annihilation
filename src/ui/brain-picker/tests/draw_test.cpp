// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The brain picker drawn in the installed game's fonts, over made-up
// brains: Compact in the 640x480 window at 1x, Regular in 1920x1080 and
// Large in 2560x1440 at 2x, Compact's details page, the built-in player
// chosen with the Difficulty note at Regular, and Compact in the catalogue's
// French. Each is laid out and drawn twice into a picture with a margin
// round the window: the two drawings are byte for byte the same, the margin
// keeps its colour, and every pixel of the window is drawn. The characters
// the game's fonts lack are drawn in the modern fonts beside the test, as
// the game draws them. Given --frames DIR, it writes brain-picker-compact.png,
// brain-picker-regular.png, brain-picker-large.png,
// brain-picker-details-compact.png, brain-picker-built-in-regular.png and
// brain-picker-fr-compact.png.
//
//   oa-ui-brain-picker-draw-test --data INTERFACE_TDF [--frames DIR]

#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/formats/png.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/present/game_text.hpp"
#include "oa/test/check.hpp"
#include "oa/test/game_assets.hpp"
#include "oa/test/game_data.hpp"
#include "oa/ui/brain_picker/model.hpp"
#include "oa/ui/brain_picker/screen.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/library/library.hpp"
#include "screen_fixture.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace kit = oa::ui::kit;
namespace picker = oa::ui::brain_picker;
namespace lib = oa::ui::library;
namespace languages = oa::data::languages;
namespace renderer = oa::ui::frontend_renderer;
namespace png = oa::formats::png;
namespace text_font = oa::platform::text_font;
using brain_fixture::opened;
using brain_fixture::opened_note;
using brain_fixture::outside_window;
using brain_fixture::texts_not_fitting;

/// The catalogue the French picture reads. Null shows English.
const languages::InterfaceText* catalogue = nullptr;

/// The points of margin drawn round the window, which nothing may touch.
constexpr int32_t margin_points = 4;
/// The colour the picture starts as. Only this test names it; the screen uses kit tokens.
constexpr kit::Colour untouched{0x12, 0x34, 0x56};

/// Draws a line in the modern fonts, as the game does for the characters its
/// fonts lack.
///
/// @param context the font stack
/// @param text the line, in UTF-8
/// @param face the game font it stands in for
/// @param scale screen pixels to a game pixel
/// @param size the text size, in percent
/// @return the line's mask; null when the fonts cannot draw it
std::shared_ptr<const oa::present::TextMask> draw_modern(
    void* context, std::string_view text, oa::present::TextFace face, int32_t scale, int32_t size
) {
    auto& stack = *static_cast<text_font::FontStack*>(context);
    if (text.empty() || text.size() > text_font::max_text_bytes)
        return nullptr;
    const bool label = face == oa::present::TextFace::label;
    const int32_t face_pixels =
        face == oa::present::TextFace::message  ? text_font::message_log_pixel_size
        : face == oa::present::TextFace::status ? text_font::status_readout_pixel_size
                                                : text_font::label_pixel_size;
    text_font::Style style;
    style.pixel_size = std::clamp(
        oa::present::face_pixel_size(face_pixels, scale, size),
        int32_t{1},
        text_font::max_pixel_size
    );
    style.weight = label ? text_font::Weight::regular : text_font::Weight::bold;
    style.rendering = text_font::Rendering::mono;
    const auto drawn = stack.draw(text, style);
    const auto placements = stack.layout(text, style);
    const auto characters = text_font::decode_utf8(text);
    if (!drawn || !placements || !characters)
        return nullptr;
    auto mask = std::make_shared<oa::present::TextMask>();
    mask->width = drawn->width;
    mask->height = drawn->height;
    mask->baseline = drawn->baseline;
    mask->origin = drawn->origin;
    mask->advance = drawn->advance;
    mask->alpha = drawn->alpha;
    std::size_t placed = 0;
    int32_t pen = 0;
    for (const char32_t character : *characters) {
        if (!text_font::is_invisible(character) && placed < placements->size()) {
            const auto& placement = (*placements)[placed++];
            pen = placement.pen + placement.advance;
        }
        mask->character_ends.push_back(pen);
    }
    return mask;
}

/// Looks a word up in the installed interface catalogue.
///
/// @param context unused
/// @param english the English word
/// @return the catalogue's text; the view stays valid until the language changes
std::string_view catalogue_shown(void* /*context*/, std::string_view english) {
    return languages::interface_text(english);
}

/// Reads a file whole.
///
/// @param path the file
/// @return its bytes; empty when it cannot be read
std::string read_file(const char* path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

/// Which picture is drawn.
enum class SceneKind : uint8_t {
    list,     ///< the list and, at Regular and Large, the details
    details,  ///< Compact's details page
    built_in, ///< the built-in player, with the Difficulty note
    french,   ///< Compact in French
};

/// One picture.
struct Scene {
    const char* name{};              ///< the file's name after "brain-picker-"
    kit::Viewport window{};          ///< the window it is laid out for
    SceneKind kind{SceneKind::list}; ///< which picture
};

/// A drawing of a scene.
struct Drawing {
    renderer::Surface surface{}; ///< the picture, the margin round the window
    int32_t scale{1};            ///< pixels a point
    kit::Point window{};         ///< the window's size, in points
    kit::DisplayList list{};     ///< what was drawn
};

/// Returns a window's viewport at a scale.
///
/// @param width the canvas's width, in pixels
/// @param height the canvas's height, in pixels
/// @param percent the scale, as a percentage
/// @return the viewport
kit::Viewport window_of(int32_t width, int32_t height, int32_t percent) {
    kit::Viewport viewport;
    viewport.width = width;
    viewport.height = height;
    viewport.scale_percent = percent;
    return viewport;
}

/// Lays a scene out.
///
/// @param scene the scene
/// @param measured the fonts
/// @return the display list
kit::DisplayList layout_scene(const Scene& scene, const kit::Fonts& measured) {
    lib::TextHooks text;
    if (scene.kind == SceneKind::french && catalogue != nullptr) {
        if (const languages::Language* french = languages::find_by_tag("fr")) {
            languages::set_interface_language(catalogue, *french);
            text.shown = catalogue_shown;
        }
    }
    const picker::Picker model =
        scene.kind == SceneKind::built_in ? opened_note(text) : opened(text);
    const kit::Frame frame = kit::frame_of(scene.window);
    picker::ScreenState state;
    if (scene.kind == SceneKind::details)
        state.details_page = true;
    return picker::picker_layout(model, state, frame, measured);
}

/// Draws a scene once.
///
/// @param scene the scene
/// @param measured the fonts
/// @return the drawing
Drawing draw_scene(const Scene& scene, const kit::Fonts& measured) {
    const kit::Frame frame = kit::frame_of(scene.window);
    Drawing drawing;
    drawing.scale = frame.scale_percent / 100;
    drawing.window = picker::window_size(frame);
    drawing.list = layout_scene(scene, measured);
    const int32_t width = (drawing.window.x + 2 * margin_points) * drawing.scale;
    const int32_t height = (drawing.window.y + 2 * margin_points) * drawing.scale;
    drawing.surface.width = static_cast<uint32_t>(width);
    drawing.surface.height = static_cast<uint32_t>(height);
    drawing.surface.rgb.resize(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3
    );
    for (std::size_t at = 0; at < drawing.surface.rgb.size(); at += 3) {
        drawing.surface.rgb[at] = untouched.r;
        drawing.surface.rgb[at + 1] = untouched.g;
        drawing.surface.rgb[at + 2] = untouched.b;
    }
    kit::Canvas canvas;
    canvas.surface = &drawing.surface;
    canvas.placement = {
        margin_points * drawing.scale, margin_points * drawing.scale, drawing.scale, {}
    };
    canvas.fonts = &measured;
    picker::draw_picker(canvas, drawing.list, drawing.window);
    return drawing;
}

/// Tells whether a pixel keeps the colour it started as.
///
/// @param drawing the drawing
/// @param x the column, in pixels
/// @param y the row, in pixels
/// @return true when nothing drew there
bool kept(const Drawing& drawing, int32_t x, int32_t y) {
    const std::size_t at =
        (static_cast<std::size_t>(y) * drawing.surface.width + static_cast<std::size_t>(x)) * 3;
    return drawing.surface.rgb[at] == untouched.r && drawing.surface.rgb[at + 1] == untouched.g &&
           drawing.surface.rgb[at + 2] == untouched.b;
}

/// Checks that the margin keeps its colour and that every pixel of the window was drawn.
///
/// @param scene the scene
/// @param drawing its drawing
void check_edges(const Scene& scene, const Drawing& drawing) {
    const int32_t column = margin_points * drawing.scale;
    const int32_t right = column + drawing.window.x * drawing.scale;
    const int32_t top = margin_points * drawing.scale;
    const int32_t bottom = top + drawing.window.y * drawing.scale;
    std::size_t outside = 0;
    std::size_t blank = 0;
    for (int32_t y = 0; y < static_cast<int32_t>(drawing.surface.height); ++y) {
        for (int32_t x = 0; x < static_cast<int32_t>(drawing.surface.width); ++x) {
            const bool in_window = x >= column && x < right && y >= top && y < bottom;
            const bool untouched_here = kept(drawing, x, y);
            if (!in_window && !untouched_here)
                ++outside;
            if (in_window && untouched_here)
                ++blank;
        }
    }
    if (outside != 0)
        std::fprintf(stderr, "%s: %zu pixels drawn outside the window\n", scene.name, outside);
    if (blank != 0)
        std::fprintf(stderr, "%s: %zu pixels of the window not drawn\n", scene.name, blank);
    OA_CHECK(outside == 0);
    OA_CHECK(blank == 0);
}

/// Writes a drawing's window as a PNG file.
///
/// @param drawing the drawing
/// @param path the file
/// @return true when it was written
bool write_window(const Drawing& drawing, const std::filesystem::path& path) {
    const auto width = static_cast<std::size_t>(drawing.window.x * drawing.scale);
    const auto height = static_cast<std::size_t>(drawing.window.y * drawing.scale);
    const auto margin = static_cast<std::size_t>(margin_points * drawing.scale);
    std::vector<uint8_t> rows;
    rows.reserve(width * height * 3);
    for (std::size_t y = 0; y < height; ++y) {
        const std::size_t start = ((y + margin) * drawing.surface.width + margin) * 3;
        rows.insert(
            rows.end(),
            drawing.surface.rgb.begin() + static_cast<std::ptrdiff_t>(start),
            drawing.surface.rgb.begin() + static_cast<std::ptrdiff_t>(start + width * 3)
        );
    }
    png::Header header;
    header.width = static_cast<uint32_t>(width);
    header.height = static_cast<uint32_t>(height);
    header.bit_depth = 8;
    header.color_type = png::ColorType::rgb;
    std::vector<uint8_t> file;
    if (!png::write(png::Image{header, {}, rows}, &file))
        return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size())
    );
    return static_cast<bool>(output);
}

} // namespace

int main(int argc, char** argv) {
    std::optional<std::filesystem::path> frames;
    std::optional<std::filesystem::path> catalogue_path;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--frames" && index + 1 < argc)
            frames = std::filesystem::path(argv[++index]);
        else if (argument == oa::test::kGameDataSwitch)
            continue;
        else if (!argument.empty() && argument.front() != '-')
            catalogue_path = std::filesystem::path(argv[index]);
        else {
            std::fprintf(stderr, "draw: unexpected argument %s\n", argv[index]);
            return 2;
        }
    }
    if (!catalogue_path) {
        std::fprintf(
            stderr, "usage: oa-ui-brain-picker-draw-test --data INTERFACE_TDF [--frames DIR]\n"
        );
        return 2;
    }
    languages::InterfaceText loaded;
    std::string error;
    const bool read = loaded.add(read_file(catalogue_path->string().c_str()), &error);
    if (!read)
        std::fprintf(stderr, "%s: %s\n", catalogue_path->string().c_str(), error.c_str());
    OA_CHECK(read);
    if (!read)
        return oa::test::check_exit_status();
    catalogue = &loaded;
    oa::AssetStore assets = oa::test::require_game_assets("the brain picker's pictures");
    const kit::Fonts measured = kit::load_game_fonts(assets);
    const std::unique_ptr<text_font::FontStack> modern =
        text_font::FontStack::open(text_font::bundled_font_directory());
    if (modern) {
        oa::present::GameTextHooks hooks;
        hooks.context = modern.get();
        hooks.draw = draw_modern;
        oa::present::set_game_text_hooks(hooks);
    } else {
        std::printf(
            "draw: no modern fonts beside the test; the characters the game's fonts lack are not "
            "drawn\n"
        );
    }
    if (frames) {
        std::error_code directory_error;
        std::filesystem::create_directories(*frames, directory_error);
        if (directory_error) {
            std::fprintf(stderr, "draw: cannot make %s\n", frames->string().c_str());
            return 1;
        }
    }

    const Scene scenes[] = {
        {"compact", window_of(640, 480, 100), SceneKind::list},
        {"regular", window_of(1920, 1080, 200), SceneKind::list},
        {"large", window_of(2560, 1440, 200), SceneKind::list},
        {"details-compact", window_of(640, 480, 100), SceneKind::details},
        {"built-in-regular", window_of(1920, 1080, 200), SceneKind::built_in},
        {"fr-compact", window_of(640, 480, 100), SceneKind::french},
    };
    for (const Scene& scene : scenes) {
        const Drawing first = draw_scene(scene, measured);
        const Drawing second = draw_scene(scene, measured);
        if (first.surface.rgb != second.surface.rgb)
            std::fprintf(stderr, "%s: two drawings differ\n", scene.name);
        OA_CHECK(first.surface.rgb == second.surface.rgb);
        check_edges(scene, first);
        for (const std::string& problem : outside_window(first.list, first.window)) {
            std::fprintf(stderr, "%s: %s lies outside the window\n", scene.name, problem.c_str());
            ++oa::test::failed_checks();
        }
        for (const std::string& problem : texts_not_fitting(first.list, measured)) {
            std::fprintf(stderr, "%s: %s does not fit\n", scene.name, problem.c_str());
            ++oa::test::failed_checks();
        }
        if (frames) {
            const std::filesystem::path path =
                *frames / (std::string("brain-picker-") + scene.name + ".png");
            const bool written = write_window(first, path);
            if (!written)
                std::fprintf(stderr, "draw: cannot write %s\n", path.string().c_str());
            OA_CHECK(written);
        }
    }
    oa::present::set_game_text_hooks({});
    languages::set_interface_language(nullptr, languages::english());
    return oa::test::check_exit_status();
}

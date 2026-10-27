// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The key editor drawn twice into the viewport: the list at Compact, Large
// and Regular, Compact's details page, Large over a paused match, the
// keyboard and the mouse at each class, the ISO and JIS keyboards at Large,
// and the capture (waiting, complete, clash and refused), the clash prompt
// and the card at each class. The keys overlay is drawn at each class on an
// 800 by 480 room, to the right of a 128-point column. The two drawings of a
// scene are byte for byte the same. The dialog sits at the canvas origin;
// every pixel of it is painted, and nothing is drawn outside it, so the
// margin around the dialog keeps the colour the canvas started as. The
// overlay's panel is the only part of its picture that is painted. The
// picture is the whole viewport. Focus is left unset, so the pictures have
// no focus ring.
//
// --frames DIR writes key-editor-<scene>.png, including key-editor-capture-*.png,
// key-editor-clash-*.png, key-editor-card-regular.png, key-editor-share-*.png,
// key-editor-paste-*.png and key-editor-import-*.png, and
// keys-overlay-<class>.png with no key-editor- prefix. --record merges this
// group's hashes into the frames table and leaves the other group. --data
// draws in the installed game's fonts. A scene with no hash fails unless
// --record is set.
//
//   oa-ui-key-editor-draw-test [--data] [--record] [--frames DIR]

#include "oa/base/sha256.hpp"
#include "oa/data/input_actions/actions.hpp"
#include "oa/formats/png.hpp"
#include "oa/test/check.hpp"
#include "oa/test/game_assets.hpp"
#include "oa/test/game_data.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/key_bindings/layout.hpp"
#include "oa/ui/key_editor/model.hpp"
#include "oa/ui/key_editor/overlay.hpp"
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
namespace keys = oa::ui::key_bindings;
namespace renderer = oa::ui::frontend_renderer;
namespace acts = oa::data::input_actions;
namespace png = oa::formats::png;
namespace sha256 = oa::base::sha256;
using key_editor_fixture::editor_fixture;
using key_editor_fixture::fonts;
using key_editor_fixture::frame_for;

/// The colour the picture starts as. Only this test names it.
constexpr kit::Colour untouched{0x12, 0x34, 0x56};

/// Which screen a picture shows.
enum class Shown : uint8_t {
    list,      ///< the list, or Compact's details page
    waiting,   ///< the capture, before a key
    complete,  ///< the capture, with a free chord
    taken,     ///< the capture, with a clash
    refused,   ///< the capture, with a fixed chord
    question,  ///< the clash prompt
    card,      ///< the draft card
    share,     ///< Share your keys
    pasted,    ///< Paste keys
    importing, ///< Import keys from an older setup
};

/// One picture.
struct Scene {
    const char* name{};       ///< the file's name after "key-editor-"
    int32_t width{};          ///< the canvas width, in pixels
    int32_t height{};         ///< the canvas height, in pixels
    int32_t scale_percent{};  ///< the scale, as a percentage
    kit::Point window{};      ///< the dialog, in points
    bool details{false};      ///< Compact's details page
    bool in_match{false};     ///< the header says the game is paused
    Shown shown{Shown::list}; ///< which screen
    /// The picture, when the screen is not a prompt. The list scenes leave this.
    editor::View view{editor::View::list};
    /// The keyboard the picture draws. ANSI follows the system in the drop-down.
    kit::KeyboardShape shape{kit::KeyboardShape::ansi};
    /// The keys overlay, beside a 128-point column. Its dialog check is its own.
    bool overlay{false};
    /// The class the overlay is laid out at.
    kit::SizeClass overlay_class{kit::SizeClass::compact};
};

/// A drawing of a scene.
struct Drawing {
    renderer::Surface surface{}; ///< the viewport
    kit::Point window{};         ///< the dialog, in points
    int32_t scale{1};            ///< pixels a point
    kit::Rect panel{};           ///< the overlay's panel, in canvas pixels
    bool overlay{false};         ///< the picture is the keys overlay
};

/// A letter chord with Primary held.
///
/// @param character the letter
/// @return the chord
keys::Chord primary_letter(char32_t character) {
    keys::Chord chord;
    chord.modifiers = keys::primary;
    chord.kind = keys::TriggerKind::character;
    chord.character = character;
    return chord;
}

/// One picture at a size class.
///
/// @param name the file's name after "key-editor-"
/// @param width the canvas width, in pixels
/// @param height the canvas height, in pixels
/// @param scale the scale, as a percentage
/// @param window the dialog, in points
/// @param details Compact's details page
/// @param in_match the header says the game is paused
/// @param shown which screen
/// @return the scene
Scene picture(
    const char* name,
    int32_t width,
    int32_t height,
    int32_t scale,
    kit::Point window,
    bool details,
    bool in_match,
    Shown shown
) {
    Scene scene;
    scene.name = name;
    scene.width = width;
    scene.height = height;
    scene.scale_percent = scale;
    scene.window = window;
    scene.details = details;
    scene.in_match = in_match;
    scene.shown = shown;
    return scene;
}

/// One keyboard or mouse picture. ANSI reads as following the system.
///
/// @param name the file's name after "key-editor-"
/// @param width the canvas width, in pixels
/// @param height the canvas height, in pixels
/// @param scale the scale, as a percentage
/// @param window the dialog, in points
/// @param view which picture
/// @param shape the keyboard, when the picture is the keyboard
/// @return the scene
Scene shaped(
    const char* name,
    int32_t width,
    int32_t height,
    int32_t scale,
    kit::Point window,
    editor::View view,
    kit::KeyboardShape shape
) {
    Scene scene = picture(name, width, height, scale, window, false, false, Shown::list);
    scene.view = view;
    scene.shape = shape;
    return scene;
}

/// The keys overlay at one class, on an 800 by 480 room.
///
/// @param name the scene, keys-overlay- and the class
/// @param size_class the class
/// @return the scene
Scene overlay_picture(const char* name, kit::SizeClass size_class) {
    Scene scene = picture(name, 800, 480, 100, {0, 0}, false, false, Shown::list);
    scene.overlay = true;
    scene.overlay_class = size_class;
    return scene;
}

/// The list, then the capture, the clash prompt and the card at each class.
///
/// @return the scenes, in table order
std::vector<Scene> all_scenes() {
    const kit::Point compact{480, 324};
    const kit::Point regular{720, 486};
    const kit::Point large{960, 600};
    return {
        picture("compact", 640, 480, 100, compact, false, false, Shown::list),
        picture("large", 1280, 720, 100, large, false, false, Shown::list),
        picture("regular", 1920, 1080, 200, regular, false, false, Shown::list),
        picture("compact-details", 640, 480, 100, compact, true, false, Shown::list),
        picture("large-match", 1280, 720, 100, large, false, true, Shown::list),
        picture("capture-waiting-compact", 640, 480, 100, compact, false, false, Shown::waiting),
        picture("capture-waiting-regular", 1920, 1080, 200, regular, false, false, Shown::waiting),
        picture("capture-waiting-large", 1280, 720, 100, large, false, false, Shown::waiting),
        picture("capture-complete-compact", 640, 480, 100, compact, false, false, Shown::complete),
        picture(
            "capture-complete-regular", 1920, 1080, 200, regular, false, false, Shown::complete
        ),
        picture("capture-complete-large", 1280, 720, 100, large, false, false, Shown::complete),
        picture("capture-clash-compact", 640, 480, 100, compact, false, false, Shown::taken),
        picture("capture-clash-regular", 1920, 1080, 200, regular, false, false, Shown::taken),
        picture("capture-clash-large", 1280, 720, 100, large, false, false, Shown::taken),
        picture("capture-refused-compact", 640, 480, 100, compact, false, false, Shown::refused),
        picture("capture-refused-regular", 1920, 1080, 200, regular, false, false, Shown::refused),
        picture("capture-refused-large", 1280, 720, 100, large, false, false, Shown::refused),
        picture("clash-compact", 640, 480, 100, compact, false, false, Shown::question),
        picture("clash-regular", 1920, 1080, 200, regular, false, false, Shown::question),
        picture("clash-large", 1280, 720, 100, large, false, false, Shown::question),
        picture("card-compact", 640, 480, 100, compact, true, false, Shown::card),
        picture("card-regular", 1920, 1080, 200, regular, false, false, Shown::card),
        picture("card-large", 1280, 720, 100, large, false, false, Shown::card),
        picture("share-compact", 640, 480, 100, compact, false, false, Shown::share),
        picture("share-regular", 1920, 1080, 200, regular, false, false, Shown::share),
        picture("share-large", 1280, 720, 100, large, false, false, Shown::share),
        picture("paste-compact", 640, 480, 100, compact, false, false, Shown::pasted),
        picture("paste-regular", 1920, 1080, 200, regular, false, false, Shown::pasted),
        picture("paste-large", 1280, 720, 100, large, false, false, Shown::pasted),
        picture("import-compact", 640, 480, 100, compact, false, false, Shown::importing),
        picture("import-regular", 1920, 1080, 200, regular, false, false, Shown::importing),
        picture("import-large", 1280, 720, 100, large, false, false, Shown::importing),
        shaped(
            "keyboard-compact",
            640,
            480,
            100,
            compact,
            editor::View::keyboard,
            kit::KeyboardShape::ansi
        ),
        shaped(
            "keyboard-large",
            1280,
            720,
            100,
            large,
            editor::View::keyboard,
            kit::KeyboardShape::ansi
        ),
        shaped(
            "keyboard-regular",
            1920,
            1080,
            200,
            regular,
            editor::View::keyboard,
            kit::KeyboardShape::ansi
        ),
        shaped(
            "mouse-compact", 640, 480, 100, compact, editor::View::mouse, kit::KeyboardShape::ansi
        ),
        shaped("mouse-large", 1280, 720, 100, large, editor::View::mouse, kit::KeyboardShape::ansi),
        shaped(
            "mouse-regular", 1920, 1080, 200, regular, editor::View::mouse, kit::KeyboardShape::ansi
        ),
        shaped(
            "keyboard-iso-large",
            1280,
            720,
            100,
            large,
            editor::View::keyboard,
            kit::KeyboardShape::iso
        ),
        shaped(
            "keyboard-jis-large",
            1280,
            720,
            100,
            large,
            editor::View::keyboard,
            kit::KeyboardShape::jis
        ),
        overlay_picture("keys-overlay-compact", kit::SizeClass::compact),
        overlay_picture("keys-overlay-regular", kit::SizeClass::regular),
        overlay_picture("keys-overlay-large", kit::SizeClass::large),
    };
}

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

/// Puts the scene's prompt or card on the screen. Focus stays unset.
///
/// @param[in,out] model the editor; the clash prompt returns Select all to Ctrl+A
/// @param[out] state the screen
/// @param scene the scene
void arm_scene(editor::Editor& model, editor::ScreenState& state, const Scene& scene) {
    state.details_page = scene.details;
    state.in_match = scene.in_match;
    state.view = scene.view;
    state.shape = scene.shape;
    state.shape_from_system = scene.shape == kit::KeyboardShape::ansi;
    state.selected = "order.attack";
    state.capture.action = "order.attack";
    state.capture.means = oa::data::input_actions::By::letter;
    state.capture.starts = keys::Gesture::press;
    if (scene.shown == Shown::list)
        return;
    if (scene.shown == Shown::card) {
        state.selected = "select.all";
        state.filter = editor::Filter::clashes;
        return;
    }
    if (scene.shown == Shown::share) {
        state.prompt = editor::Prompt::share;
        state.share_field.text = "Grid, left hand";
        state.share_field.caret = static_cast<int32_t>(state.share_field.text.size());
        model.set_share_name(state.share_field.text);
        return;
    }
    if (scene.shown == Shown::pasted) {
        state.prompt = editor::Prompt::paste;
        constexpr std::string_view text = "# oakeys bindings, base oa-default\n"
                                          "order.move: [\"@KeyQ\"]\n"
                                          "select.all: [Ctrl+B]\n"
                                          "camera.orbit-left: [Q]\n";
        const auto* bytes = reinterpret_cast<const uint8_t*>(text.data());
        state.paste = editor::paste_preview(model, {bytes, text.size()});
        return;
    }
    if (scene.shown == Shown::importing) {
        state.prompt = editor::Prompt::import;
        state.import_chosen = 0;
        editor::ImportOffer offer;
        offer.id = "tdraw";
        offer.tool = "tdraw";
        offer.file = "Settings.ini";
        offer.caption = "tdraw";
        state.import_offers.push_back(offer);
        keys::LegacyRead read;
        keys::LegacyKey mapped;
        mapped.action = "megamap.toggle";
        keys::ChordError error{};
        if (const std::optional<keys::Chord> chord = keys::parse_chord("F4", error))
            mapped.chords.push_back(*chord);
        mapped.line = 1;
        read.keys.push_back(std::move(mapped));
        state.importing = editor::import_preview(model, read, "tdraw", "Settings.ini");
        state.import_date = "2026-10-27";
        return;
    }
    if (scene.shown == Shown::question) {
        model.reset_action("select.all");
        state.selected = "select.all";
        state.prompt = editor::Prompt::clash;
        state.capture.action = "select.all";
        state.capture.replacing = 0;
        state.capture.chord = primary_letter(U'B');
        state.capture.state = editor::CaptureState::clash;
        state.pending_clash.a = "select.all";
        state.pending_clash.b = "select.builders";
        state.pending_clash.chord = *state.capture.chord;
        return;
    }
    state.prompt = editor::Prompt::capture;
    if (scene.shown == Shown::waiting)
        return;
    if (scene.shown == Shown::complete) {
        state.capture.chord = primary_letter(U'Q');
        state.capture.state = editor::CaptureState::complete;
        return;
    }
    if (scene.shown == Shown::taken) {
        state.capture.chord = primary_letter(U'B');
        state.capture.state = editor::CaptureState::clash;
        return;
    }
    state.selected = "chat.send";
    state.capture.action = "chat.send";
    state.capture.means = oa::data::input_actions::By::place;
    state.capture.state = editor::CaptureState::refused;
    keys::Chord escape;
    escape.kind = keys::TriggerKind::named;
    escape.named = static_cast<uint8_t>(keys::NamedKey::escape);
    state.capture.chord = escape;
}

/// The bit of a context.
///
/// @param name the context id
/// @return the bit, or 0 when the name is absent
uint32_t context_bit(std::string_view name) {
    uint32_t bit = 1;
    for (const acts::Context& context : acts::contexts()) {
        if (context.name == name)
            return bit;
        if (bit >= 0x80000000U)
            return 0;
        bit <<= 1;
    }
    return 0;
}

/// Fills a canvas with the colour a picture starts as.
///
/// @param surface the picture
void fill_untouched(renderer::Surface& surface) {
    for (std::size_t at = 0; at < surface.rgb.size(); at += 3) {
        surface.rgb[at] = untouched.r;
        surface.rgb[at + 1] = untouched.g;
        surface.rgb[at + 2] = untouched.b;
    }
}

/// Draws the keys overlay for the contexts active while playing, with a
/// latched QUEUE in its On line.
///
/// Hacks are off. The panel sits in an 800 by 480 room, to the right of a
/// 128-point column, and the canvas outside that panel stays untouched.
///
/// @param scene the scene
/// @param measured the fonts
/// @return the drawing
Drawing draw_overlay_scene(const Scene& scene, const kit::Fonts& measured) {
    keys::KeyboardMap keyboard = keys::KeyboardMap::us();
    keys::ResolveInput input;
    input.base = keys::Base::oa_default;
    input.keyboard = &keyboard;
    editor::OverlayInputs inputs;
    inputs.resolved = keys::resolve(input);
    inputs.contexts.active = context_bit("global") | context_bit("match") | context_bit("play");
    inputs.keyboard = keyboard;
    inputs.layout_name = "OA default";
    inputs.on.push_back({"QUEUE · latched", true});
    const std::vector<editor::OverlayGroup> groups = editor::overlay_groups(inputs);
    const editor::OverlayRoom room{{0, 0, scene.width, scene.height}, 128, scene.overlay_class};
    const editor::OverlayLayout laid = editor::overlay_layout(inputs, groups, room, measured, 0);

    Drawing drawing;
    drawing.overlay = true;
    drawing.scale = 1;
    drawing.panel = laid.panel;
    drawing.surface.width = static_cast<uint32_t>(scene.width);
    drawing.surface.height = static_cast<uint32_t>(scene.height);
    drawing.surface.rgb.assign(
        static_cast<std::size_t>(scene.width) * static_cast<std::size_t>(scene.height) * 3, 0
    );
    fill_untouched(drawing.surface);
    kit::Canvas canvas;
    canvas.surface = &drawing.surface;
    canvas.placement = {laid.panel.x, laid.panel.y, drawing.scale, {}};
    canvas.fonts = &measured;
    editor::draw_overlay(canvas, laid);
    return drawing;
}

/// Lays one scene out and draws it at the canvas origin.
///
/// @param scene the scene
/// @param measured the fonts
/// @return the drawing
Drawing draw_scene(const Scene& scene, const kit::Fonts& measured) {
    if (scene.overlay)
        return draw_overlay_scene(scene, measured);
    editor::Editor model(editor_fixture());
    editor::ScreenState state;
    arm_scene(model, state, scene);
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
    fill_untouched(drawing.surface);
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

/// Checks that the overlay's panel was painted and the room around it was not.
///
/// @param scene the scene
/// @param drawing its drawing
void check_overlay(const Scene& scene, const Drawing& drawing) {
    const int32_t left = drawing.panel.x;
    const int32_t top = drawing.panel.y;
    const int32_t right = drawing.panel.x + drawing.panel.width;
    const int32_t bottom = drawing.panel.y + drawing.panel.height;
    OA_CHECK(drawing.scale == 1);
    OA_CHECK(left >= 128);
    OA_CHECK(top >= 0);
    OA_CHECK(right <= static_cast<int32_t>(drawing.surface.width));
    OA_CHECK(bottom <= static_cast<int32_t>(drawing.surface.height));
    OA_CHECK(drawing.panel.width > 0 && drawing.panel.height > 0);
    std::size_t outside = 0;
    std::size_t painted = 0;
    for (int32_t y = 0; y < static_cast<int32_t>(drawing.surface.height); ++y) {
        for (int32_t x = 0; x < static_cast<int32_t>(drawing.surface.width); ++x) {
            const bool in_panel = x >= left && y >= top && x < right && y < bottom;
            const bool untouched_here = kept(drawing, x, y);
            if (!in_panel && !untouched_here)
                ++outside;
            if (in_panel && !untouched_here)
                ++painted;
        }
    }
    if (outside != 0)
        std::fprintf(stderr, "%s: %zu pixels drawn outside the panel\n", scene.name, outside);
    if (painted == 0)
        std::fprintf(stderr, "%s: the panel was not painted\n", scene.name);
    OA_CHECK(outside == 0);
    OA_CHECK(painted > 0);
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
    std::string text{}; ///< the line, without its newline
    bool scene{false};  ///< a group/scene hash
    std::string name{}; ///< group/scene, when scene
    std::string hex{};  ///< the hash, when scene
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
    const std::vector<Scene> scenes = all_scenes();
    for (const Scene& scene : scenes) {
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
        if (scene.overlay)
            check_overlay(scene, first);
        else
            check_dialog(scene, first);
        digests.emplace_back(scene.name, digest_of(first.surface));
        if (frames) {
            const bool keys_overlay = std::string_view(scene.name).starts_with("keys-overlay");
            const std::string stem =
                (keys_overlay ? std::string{} : std::string("key-editor-")) + scene.name;
            const std::filesystem::path path = *frames / (stem + ".png");
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
    return oa::test::failed_checks() == 0 ? 0 : 1;
}

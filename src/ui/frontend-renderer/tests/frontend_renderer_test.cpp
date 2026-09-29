// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/frontend_renderer.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <variant>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);     \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

oa::ui::gui_layout::Gadget
button(std::string name, int16_t x, int16_t y, int16_t width, int16_t height) {
    oa::ui::gui_layout::Gadget result;
    result.common.type = oa::ui::gui_layout::GadgetType::button;
    result.common.name = std::move(name);
    result.common.x = x;
    result.common.y = y;
    result.common.width = width;
    result.common.height = height;
    result.common.active = 1;
    result.fields = oa::ui::gui_layout::ButtonFields{};
    return result;
}

uint8_t red_at(const oa::ui::frontend_renderer::Surface& surface, std::size_t x, std::size_t y) {
    return surface.rgb[(y * surface.width + x) * 3];
}

// A grayed-out art button shows the frame two past its normal one, or the
// last frame of a shorter sequence, darkened through shade table row 12.
void test_grayed_art_frame() {
    oa::ui::frontend_renderer::ScreenResources resources;
    resources.background.width = 4;
    resources.background.height = 4;
    resources.background.rgb.assign(4U * 4U * 3U, 99);
    for (std::size_t index = 0; index < 256; ++index)
        resources.gui_palette[index * 4] = static_cast<uint8_t>(index);
    // The screen's bitmap carries the palette the shade table indexes.
    resources.background.palette = resources.gui_palette;
    resources.layout.gadgets.push_back(button("ART", 1, 1, 1, 1));
    oa::formats::gaf::Sequence sequence;
    sequence.name = "ART";
    for (uint8_t value : {10, 11, 12, 13}) {
        oa::formats::gaf::Frame frame;
        frame.width = 1;
        frame.height = 1;
        frame.pixels = {value};
        frame.coverage = {1};
        sequence.frames.push_back(frame);
    }
    resources.sprites.sequences.push_back(sequence);
    resources.shade_table.resize(32U * 256U);
    for (std::size_t row = 0; row < 32; ++row)
        for (std::size_t index = 0; index < 256; ++index)
            resources.shade_table[row * 256 + index] = static_cast<uint8_t>(index);
    resources.shade_table[12U * 256U + 12U] = 50;
    const oa::ui::frontend_renderer::ButtonPresentation grayed{
        "ART",
        oa::ui::frontend_renderer::ButtonCondition::disabled,
        std::nullopt,
        std::nullopt,
        std::nullopt
    };
    CHECK(red_at(oa::ui::frontend_renderer::render_screen(resources, {&grayed, 1}), 1, 1) == 50);
    resources.sprites.sequences.back().frames.resize(2);
    CHECK(red_at(oa::ui::frontend_renderer::render_screen(resources, {&grayed, 1}), 1, 1) == 11);
    // Drawn in the GUI palette alone, the grayed frame is darkened in the game
    // palette, and left undarkened without one.
    resources.sprites.sequences.back().frames.push_back(
        resources.sprites.sequences.back().frames[0]
    );
    resources.sprites.sequences.back().frames.back().pixels = {12};
    resources.background.palette.reset();
    resources.game_palette = resources.gui_palette;
    CHECK(red_at(oa::ui::frontend_renderer::render_screen(resources, {&grayed, 1}), 1, 1) == 50);
    resources.game_palette.reset();
    CHECK(red_at(oa::ui::frontend_renderer::render_screen(resources, {&grayed, 1}), 1, 1) == 12);
}

// Six grayed-out 64x64 art buttons drawn in all 256 colours of a palette
// whose colours all differ, as a builder's page of empty build slots is:
// every pixel is darkened to the shade table's colour for its own, and the
// screen costs at most twice what it costs with the buttons drawn normally.
void test_grayed_art_cost() {
    namespace renderer = oa::ui::frontend_renderer;
    constexpr int screen_width = 640;
    constexpr int screen_height = 480;
    constexpr int button_side = 64;
    constexpr int button_count = 6;
    constexpr std::size_t shade_row = 12;
    renderer::ScreenResources resources;
    resources.background.width = screen_width;
    resources.background.height = screen_height;
    resources.background.rgb.assign(std::size_t{screen_width} * screen_height * 3U, 0);
    for (std::size_t index = 0; index < 256; ++index) {
        resources.gui_palette[index * 4] = static_cast<uint8_t>(index);
        resources.gui_palette[index * 4 + 1] = static_cast<uint8_t>(index * 37U);
        resources.gui_palette[index * 4 + 2] = static_cast<uint8_t>(index * 101U);
    }
    resources.background.palette = resources.gui_palette;
    resources.shade_table.resize(32U * 256U);
    for (std::size_t row = 0; row < 32; ++row)
        for (std::size_t index = 0; index < 256; ++index)
            resources.shade_table[row * 256 + index] = static_cast<uint8_t>(255U - index);
    oa::formats::gaf::Frame frame;
    frame.width = button_side;
    frame.height = button_side;
    frame.pixels.resize(std::size_t{button_side} * button_side);
    frame.coverage.assign(frame.pixels.size(), 1);
    for (std::size_t pixel = 0; pixel < frame.pixels.size(); ++pixel)
        frame.pixels[pixel] = static_cast<uint8_t>(pixel * 7U);
    std::vector<std::string> names;
    for (int slot = 0; slot < button_count; ++slot)
        names.push_back("SLOT" + std::to_string(slot));
    std::vector<renderer::ButtonPresentation> grayed;
    std::vector<renderer::ButtonPresentation> normal;
    for (int slot = 0; slot < button_count; ++slot) {
        auto gadget = button(
            names[static_cast<std::size_t>(slot)],
            static_cast<int16_t>(10 + slot * (button_side + 10)),
            20,
            button_side,
            button_side
        );
        std::get<oa::ui::gui_layout::ButtonFields>(gadget.fields).text.clear();
        resources.layout.gadgets.push_back(gadget);
        grayed.push_back(
            {names[static_cast<std::size_t>(slot)],
             renderer::ButtonCondition::disabled,
             std::nullopt,
             std::nullopt,
             std::nullopt}
        );
        normal.push_back(
            {names[static_cast<std::size_t>(slot)],
             renderer::ButtonCondition::normal,
             std::nullopt,
             std::nullopt,
             std::nullopt}
        );
    }
    // Each button draws the art sequence of its own name: the same frame
    // three times, so the grayed-out frame two past the normal one is it too.
    for (const auto& name : names) {
        oa::formats::gaf::Sequence sequence;
        sequence.name = name;
        sequence.frames.assign(3, frame);
        resources.sprites.sequences.push_back(sequence);
    }

    const auto shaded = renderer::render_screen(resources, grayed);
    bool every_pixel_shaded = true;
    for (int slot = 0; slot < button_count; ++slot)
        for (int y = 0; y < button_side; ++y)
            for (int x = 0; x < button_side; ++x) {
                const auto source = frame.pixels[static_cast<std::size_t>(y * button_side + x)];
                const std::size_t row = source >= 0x80 ? shade_row - 1 : shade_row;
                const auto darkened = resources.shade_table[row * 256 + source];
                const auto at = (static_cast<std::size_t>(20 + y) * screen_width +
                                 static_cast<std::size_t>(10 + slot * (button_side + 10) + x)) *
                                3U;
                for (std::size_t channel = 0; channel < 3; ++channel)
                    if (shaded.rgb[at + channel] != resources.gui_palette[darkened * 4U + channel])
                        every_pixel_shaded = false;
            }
    CHECK(every_pixel_shaded);

    // The quickest of several interleaved renders of each, so a busy
    // machine's pauses count against neither.
    constexpr int rounds = 7;
    auto quickest_grayed = std::chrono::steady_clock::duration::max();
    auto quickest_normal = std::chrono::steady_clock::duration::max();
    for (int round = 0; round < rounds; ++round) {
        auto start = std::chrono::steady_clock::now();
        const auto grayed_surface = renderer::render_screen(resources, grayed);
        quickest_grayed = std::min(quickest_grayed, std::chrono::steady_clock::now() - start);
        start = std::chrono::steady_clock::now();
        const auto normal_surface = renderer::render_screen(resources, normal);
        quickest_normal = std::min(quickest_normal, std::chrono::steady_clock::now() - start);
        CHECK(grayed_surface.rgb.size() == normal_surface.rgb.size());
    }
    CHECK(quickest_grayed <= 2 * quickest_normal);
    if (quickest_grayed > 2 * quickest_normal)
        std::fprintf(
            stderr,
            "grayed buttons took %lld us, normal ones %lld us\n",
            static_cast<long long>(
                std::chrono::duration_cast<std::chrono::microseconds>(quickest_grayed).count()
            ),
            static_cast<long long>(
                std::chrono::duration_cast<std::chrono::microseconds>(quickest_normal).count()
            )
        );
}

} // namespace

int main() {
    using oa::ui::frontend_renderer::staged_caption;
    assert(staged_caption("Easy|Medium|Hard", 0) == "Easy");
    assert(staged_caption("Easy|Medium|Hard", 1) == "Medium");
    assert(staged_caption("Easy|Medium|Hard", 2) == "Hard");
    assert(staged_caption("Easy|Medium|Hard", 3).empty());
    assert(staged_caption("Select Map", 2) == "Select Map");
    assert(staged_caption("", 1).empty());

    oa::ui::frontend_renderer::MainMenuResources resources;
    resources.background.width = 8;
    resources.background.height = 8;
    resources.background.rgb.assign(8U * 8U * 3U, 99);
    for (std::size_t index = 0; index < 256; ++index) {
        resources.gui_palette[index * 4] = static_cast<uint8_t>(index);
    }
    resources.layout.gadgets.push_back(button("PLAIN", 1, 1, 5, 5));

    const auto raised = oa::ui::frontend_renderer::render_main_menu(resources);
    assert(red_at(raised, 1, 1) == 17);
    assert(red_at(raised, 5, 5) == 0);
    assert(red_at(raised, 3, 3) == 20);

    const oa::ui::frontend_renderer::ButtonPresentation pressed{
        "plain",
        oa::ui::frontend_renderer::ButtonCondition::pressed,
        std::nullopt,
        std::nullopt,
        std::nullopt
    };
    const auto sunken = oa::ui::frontend_renderer::render_main_menu(resources, {&pressed, 1});
    assert(red_at(sunken, 1, 1) == 0);
    assert(red_at(sunken, 5, 5) == 17);

    const oa::ui::frontend_renderer::ButtonPresentation disabled{
        "PLAIN",
        oa::ui::frontend_renderer::ButtonCondition::disabled,
        std::nullopt,
        std::nullopt,
        std::nullopt
    };
    const auto grayed = oa::ui::frontend_renderer::render_main_menu(resources, {&disabled, 1});
    assert(red_at(grayed, 1, 1) == 0);
    assert(red_at(grayed, 5, 5) == 19);
    assert(red_at(grayed, 3, 3) == 19);

    oa::formats::gaf::Sequence sequence;
    sequence.name = "PLAIN";
    oa::formats::gaf::Frame frame;
    frame.width = 2;
    frame.height = 1;
    frame.transparency_index = 7;
    frame.pixels = {7, 42};
    frame.coverage = {1, 0};
    sequence.frames.push_back(frame);
    resources.sprites.sequences.push_back(sequence);
    const auto sprite = oa::ui::frontend_renderer::render_main_menu(resources);
    // A covered literal equal to the transparency index must still draw, and
    // an uncovered non-transparent value must not draw.
    assert(red_at(sprite, 1, 1) == 7);
    assert(red_at(sprite, 2, 1) == 99);

    oa::ui::frontend_renderer::ScreenResources controls;
    controls.background.width = 24;
    controls.background.height = 16;
    controls.background.rgb.assign(24U * 16U * 3U, 99);
    for (std::size_t index = 0; index < 256; ++index)
        controls.gui_palette[index * 4] = static_cast<uint8_t>(index);
    oa::formats::fnt::Glyph glyph;
    glyph.width = 2;
    glyph.height = 2;
    glyph.origin_y = -1;
    glyph.pixels.assign(4, 42);
    glyph.coverage.assign(4, 1);
    controls.font.glyphs['A'] = glyph;
    controls.font.glyphs['I'] = glyph;
    controls.light_table.resize(32U * 256U);
    for (std::size_t level = 0; level < 32; ++level)
        for (std::size_t index = 0; index < 256; ++index)
            controls.light_table[level * 256 + index] = static_cast<uint8_t>(index);
    controls.light_table[0x1eU * 256U + 42U] = 77;

    oa::ui::gui_layout::Gadget label;
    label.common.type = oa::ui::gui_layout::GadgetType::label;
    label.common.name = "DESCRIPTION";
    label.common.x = 1;
    label.common.y = 1;
    label.common.width = 2; // clips the second A completely
    label.common.height = 3;
    label.common.attributes = 1;
    label.common.active = 1;
    label.fields = oa::ui::gui_layout::LabelFields{"", "AA", ""};
    controls.layout.gadgets.push_back(label);

    oa::ui::gui_layout::Gadget list;
    list.common.type = oa::ui::gui_layout::GadgetType::list_box;
    list.common.name = "MAPNAMES";
    list.common.x = 8;
    list.common.y = 1;
    list.common.width = 8;
    list.common.height = 13;
    list.common.attributes = 1;
    list.common.active = 1;
    list.fields = oa::ui::gui_layout::ListBoxFields{0};
    controls.layout.gadgets.push_back(list);
    const std::vector<std::string> names{"A", "AA"};
    const oa::ui::frontend_renderer::ListPresentation list_data{"mapnames", names, 0, 1};
    const auto controls_rendered =
        oa::ui::frontend_renderer::render_screen(controls, {}, {&list_data, 1});
    assert(red_at(controls_rendered, 1, 2) == 42);
    assert(red_at(controls_rendered, 3, 2) == 99); // label clip

    // A single-line label authored with no height is limited by width only.
    auto flat_controls = controls;
    flat_controls.layout.gadgets[0].common.height = 0;
    const auto flat_rendered =
        oa::ui::frontend_renderer::render_screen(flat_controls, {}, {&list_data, 1});
    assert(red_at(flat_rendered, 1, 2) == 42);
    assert(red_at(flat_rendered, 3, 2) == 99);
    assert(red_at(controls_rendered, 10, 4) == 42);
    // line_height is glyph-I height+2; default item spacing adds one.
    assert(red_at(controls_rendered, 10, 9) == 77); // selected row lit at level 0x1E

    // The list drawing stops when the remaining gadget height falls below one font
    // line, even if another row's top would still lie inside the rectangle.
    auto short_controls = controls;
    auto& short_list = short_controls.layout.gadgets[1];
    short_list.common.height = 8; // line height 4, item height 5: one row only
    const auto short_rendered =
        oa::ui::frontend_renderer::render_screen(short_controls, {}, {&list_data, 1});
    CHECK(red_at(short_rendered, 10, 4) == 42);
    CHECK(red_at(short_rendered, 10, 9) == 99);

    // A button or label left with an authored foreground colour (a merged
    // sub-panel's records keep theirs) draws lit through that light-table row,
    // as 3.1c draws it with a colour table; colour 0 draws it plain.
    oa::formats::gaf::Sequence transport;
    transport.name = "CDPLAY";
    oa::formats::gaf::Frame transport_frame;
    transport_frame.width = 1;
    transport_frame.height = 1;
    transport_frame.pixels = {60};
    transport_frame.coverage = {1};
    transport.frames.push_back(transport_frame);
    controls.sprites.sequences.push_back(transport);
    controls.light_table[5U * 256U + 60U] = 120;
    auto lit_button = button("CDPLAY", 20, 10, 1, 1);
    lit_button.common.foreground_color = 5;
    controls.layout.gadgets.push_back(lit_button);
    const auto lit_button_rendered = oa::ui::frontend_renderer::render_screen(controls);
    CHECK(red_at(lit_button_rendered, 20, 10) == 120);
    controls.layout.gadgets.back().common.foreground_color = 0;
    const auto plain_button_rendered = oa::ui::frontend_renderer::render_screen(controls);
    CHECK(red_at(plain_button_rendered, 20, 10) == 60);

    controls.light_table[5U * 256U + 42U] = 150;
    oa::ui::gui_layout::Gadget lit_label;
    lit_label.common.type = oa::ui::gui_layout::GadgetType::label;
    lit_label.common.name = "TRACKNUM";
    lit_label.common.x = 20;
    lit_label.common.y = 12;
    lit_label.common.width = 2;
    lit_label.common.height = 3;
    lit_label.common.attributes = 1;
    lit_label.common.foreground_color = 5;
    lit_label.common.active = 1;
    lit_label.fields = oa::ui::gui_layout::LabelFields{"", "I", ""};
    controls.layout.gadgets.push_back(lit_label);
    const auto lit_label_rendered = oa::ui::frontend_renderer::render_screen(controls);
    CHECK(red_at(lit_label_rendered, 20, 13) == 150);

    oa::ui::gui_layout::Gadget runtime_image;
    runtime_image.common.type = oa::ui::gui_layout::GadgetType::hot_surface;
    runtime_image.common.name = "Color0";
    runtime_image.common.x = 20;
    runtime_image.common.y = 1;
    runtime_image.common.active = 1;
    controls.layout.gadgets.push_back(runtime_image);
    oa::formats::gaf::Sequence logos;
    logos.name = "32xlogos";
    oa::formats::gaf::Frame logo;
    logo.width = 1;
    logo.height = 1;
    logo.pixels = {55};
    logo.coverage = {1};
    logos.frames.push_back(logo);
    controls.global_sprites.sequences.push_back(logos);
    const oa::ui::frontend_renderer::ButtonPresentation image_state{
        "Color0",
        oa::ui::frontend_renderer::ButtonCondition::normal,
        0,
        std::nullopt,
        oa::ui::frontend_renderer::SpriteOverride{
            oa::ui::frontend_renderer::SpriteArchive::global, "32xlogos"
        }
    };
    const auto image_rendered =
        oa::ui::frontend_renderer::render_screen(controls, {&image_state, 1});
    assert(red_at(image_rendered, 20, 1) == 55);
    // Without a frame the hot surface is left to whoever draws it.
    auto unframed_state = image_state;
    unframed_state.gaf_frame.reset();
    const auto unframed = oa::ui::frontend_renderer::render_screen(controls, {&unframed_state, 1});
    assert(red_at(unframed, 20, 1) != 55);

    oa::ui::frontend_renderer::ScreenResources setup_screen = controls;
    setup_screen.background.width = 80;
    setup_screen.background.height = 40;
    setup_screen.background.rgb.assign(80U * 40U * 3U, 99);
    oa::ui::gui_layout::Gadget color_patch;
    color_patch.common.type = oa::ui::gui_layout::GadgetType::hot_surface;
    color_patch.common.name = "Color1";
    color_patch.common.x = 8;
    color_patch.common.y = 8;
    color_patch.common.width = 20;
    color_patch.common.height = 20;
    color_patch.common.active = 1;
    setup_screen.layout.gadgets = {color_patch};
    oa::formats::gaf::Frame logo32;
    logo32.width = 32;
    logo32.height = 32;
    logo32.pixels.assign(32U * 32U, 77);
    logo32.coverage.assign(32U * 32U, 1);
    logos.frames[0] = std::move(logo32);
    setup_screen.global_sprites.sequences.back() = logos;
    const oa::ui::frontend_renderer::ButtonPresentation patch_state{
        "Color1",
        oa::ui::frontend_renderer::ButtonCondition::normal,
        0,
        std::nullopt,
        oa::ui::frontend_renderer::SpriteOverride{
            oa::ui::frontend_renderer::SpriteArchive::global, "32xlogos"
        }
    };
    const auto patch_rendered =
        oa::ui::frontend_renderer::render_screen(setup_screen, {&patch_state, 1});
    assert(red_at(patch_rendered, 8, 8) == 77);
    assert(red_at(patch_rendered, 27, 27) == 77);
    assert(red_at(patch_rendered, 28, 8) == 99);
    assert(red_at(patch_rendered, 8, 28) == 99);

    oa::Image circuit;
    circuit.width = oa::ui::frontend_renderer::menu_spark_width;
    circuit.height = oa::ui::frontend_renderer::menu_spark_height;
    const auto pixels = static_cast<std::size_t>(circuit.width) * circuit.height;
    circuit.rgb.assign(pixels * 3U, 0);
    circuit.indices.assign(pixels, 0);
    circuit.palette.emplace();
    (*circuit.palette)[oa::ui::frontend_renderer::menu_spark_pixel * 4] = 250;
    (*circuit.palette)[oa::ui::frontend_renderer::menu_spark_pixel * 4 + 1] = 250;
    (*circuit.palette)[oa::ui::frontend_renderer::menu_spark_pixel * 4 + 2] = 250;
    for (int x = 0; x < oa::ui::frontend_renderer::menu_spark_width; ++x)
        circuit.indices[10U * circuit.width + static_cast<std::size_t>(x)] = 0xdf;
    oa::ui::frontend_renderer::Surface spark_surface;
    spark_surface.width = circuit.width;
    spark_surface.height = circuit.height;
    spark_surface.rgb = circuit.rgb;
    oa::ui::frontend_renderer::MenuSparks sparks;
    oa::ui::frontend_renderer::reset_menu_sparks(sparks, circuit);
    assert(sparks.sparks.size() == oa::ui::frontend_renderer::menu_spark_count);
    bool drew = false;
    for (int step = 0; step < 400 && !drew; ++step) {
        spark_surface.rgb.assign(pixels * 3U, 0);
        oa::ui::frontend_renderer::step_menu_sparks(sparks, spark_surface, circuit);
        for (const auto& spark : sparks.sparks) {
            if (spark.active == 0)
                continue;
            assert(spark.x >= 0 && spark.x < oa::ui::frontend_renderer::menu_spark_width);
            assert(spark.y >= 0 && spark.y < oa::ui::frontend_renderer::menu_spark_height);
            if (spark.y == 10) {
                const auto offset = (static_cast<std::size_t>(spark.y) * spark_surface.width +
                                     static_cast<std::size_t>(spark.x)) *
                                    3U;
                if (spark_surface.rgb[offset] == 250)
                    drew = true;
            }
        }
    }
    assert(drew);

    // A spark spawned on a vertical course moves down from an even row and
    // up from an odd one; a horizontal one right from an even column.
    oa::Image traces = circuit;
    traces.indices.assign(pixels, 0xdf);
    oa::ui::frontend_renderer::MenuSparks born;
    oa::ui::frontend_renderer::reset_menu_sparks(born, traces);
    oa::ui::frontend_renderer::step_menu_sparks(born, spark_surface, traces);
    int vertical = 0;
    for (const auto& spark : born.sparks) {
        assert(spark.active == 1);
        if (spark.dx == 0) {
            assert(spark.dy == ((spark.y & 1) == 0 ? 3 : -3));
            ++vertical;
        } else {
            assert(spark.dx == ((spark.x & 1) == 0 ? 3 : -3));
        }
    }
    assert(vertical > 0);

    test_grayed_art_frame();
    test_grayed_art_cost();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}

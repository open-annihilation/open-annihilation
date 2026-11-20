// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Shape cues on the war map, the planet summary and the chat: the side
// symbols, the dashed Arm links, the win-count words, the role tags, the
// latency bars and the lock and mute marks. The mode is put back to off.
#include "galaxy_graphics.hpp"

#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/screens/lobby_widgets.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/languages/translation.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/platform/system.hpp"
#include "oa/present/colour_vision/colour_vision.hpp"
#include "oa/present/surface.hpp"
#include "oa/test/netgame_installs.hpp"

#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace oa::boneyards::console;
using namespace oa::boneyards::screens;
using oa::present::colour_vision::Mode;
using oa::present::colour_vision::set_active;

namespace {

int g_failures = 0;

void check(bool condition, const char* what, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(expr) check((expr), #expr, __LINE__)

constexpr uint8_t kFill = 0xfe;

struct Canvas {
    oa::present::SurfaceBuffer buffer;

    Canvas(int32_t width, int32_t height) : buffer(oa::present::create_surface(width, height)) {
        std::fill(buffer.pixels.begin(), buffer.pixels.end(), kFill);
    }

    oa::Surface* surface() { return &buffer.surface; }

    uint8_t at(int32_t x, int32_t y) const {
        return buffer.pixels
            [static_cast<std::size_t>(y) * static_cast<std::size_t>(buffer.surface.pitch) +
             static_cast<std::size_t>(x)];
    }
};

struct RestoreMode {
    ~RestoreMode() { set_active(Mode::off, false); }
};

struct RestoreLanguage {
    ~RestoreLanguage() {
        oa::data::languages::set_interface_language(nullptr, oa::data::languages::english());
        oa::data::languages::set_translation_hooks({});
    }
};

void expect_line(const char* got, const char* want) {
    if (got == nullptr || want == nullptr || std::strcmp(got, want) != 0) {
        std::fprintf(
            stderr,
            "FAIL line text \"%s\", wanted \"%s\"\n",
            got != nullptr ? got : "(null)",
            want != nullptr ? want : "(null)"
        );
        ++g_failures;
    }
}

void test_symbols() {
    CHECK(held_side_symbol('A') == SideSymbol::arm);
    CHECK(held_side_symbol('c') == SideSymbol::core);
    CHECK(held_side_symbol('I') == SideSymbol::none);
    CHECK(founded_side_symbol(false, 'A') == SideSymbol::none);
    CHECK(founded_side_symbol(true, 'C') == SideSymbol::core);
    CHECK(founded_side_symbol(true, 'a') == SideSymbol::arm);
    CHECK(founded_side_symbol(true, 'I') == SideSymbol::none);
    CHECK(side_symbol_reach(20, 0) == 6);
    CHECK(side_symbol_reach(20, 3) == 26);
    const SideSymbolPlace held = side_symbol_place(40, 50, 6, false);
    const SideSymbolPlace founded = side_symbol_place(40, 50, 6, true);
    CHECK(held.left == 48 && held.top == 36);
    CHECK(founded.left == 26 && founded.top == 58);

    CHECK(side_symbol_covers(SideSymbol::arm, 3, 0));
    CHECK(!side_symbol_covers(SideSymbol::arm, 0, 0));
    CHECK(side_symbol_covers(SideSymbol::arm, 0, 6));
    CHECK(side_symbol_covers(SideSymbol::core, 1, 1));
    CHECK(side_symbol_covers(SideSymbol::core, 5, 5));
    CHECK(!side_symbol_covers(SideSymbol::core, 0, 0));
    CHECK(!side_symbol_covers(SideSymbol::none, 3, 3));

    Canvas arm(k_side_symbol_side, k_side_symbol_side);
    draw_side_symbol(arm.surface(), 0, 0, SideSymbol::arm);
    Canvas core(k_side_symbol_side, k_side_symbol_side);
    draw_side_symbol(core.surface(), 0, 0, SideSymbol::core);
    for (int32_t y = 0; y < k_side_symbol_side; ++y) {
        for (int32_t x = 0; x < k_side_symbol_side; ++x) {
            const bool arm_on = side_symbol_covers(SideSymbol::arm, x, y);
            const bool core_on = side_symbol_covers(SideSymbol::core, x, y);
            CHECK(arm_on == (arm.at(x, y) != kFill));
            CHECK(core_on == (core.at(x, y) != kFill));
            if (arm_on)
                CHECK(arm.at(x, y) == k_side_symbol_ink || arm.at(x, y) == k_side_symbol_outline);
            if (core_on)
                CHECK(core.at(x, y) == k_side_symbol_ink || core.at(x, y) == k_side_symbol_outline);
        }
    }
    CHECK(arm.at(3, 0) == k_side_symbol_outline);
    CHECK(arm.at(3, 3) == k_side_symbol_ink);
    CHECK(core.at(1, 1) == k_side_symbol_outline);
    CHECK(core.at(3, 3) == k_side_symbol_ink);
    Canvas blank(k_side_symbol_side, k_side_symbol_side);
    draw_side_symbol(blank.surface(), 0, 0, SideSymbol::none);
    CHECK(blank.at(3, 3) == kFill);
    Canvas hollow(k_side_symbol_side, k_side_symbol_side);
    draw_side_symbol(hollow.surface(), 0, 0, SideSymbol::core, true);
    CHECK(hollow.at(1, 1) == k_side_symbol_ink);
    CHECK(hollow.at(3, 3) == kFill);
    CHECK(hollow.at(0, 0) == kFill);
}

bool same_pixels(const Canvas& left, const Canvas& right) {
    return left.buffer.pixels == right.buffer.pixels;
}

void test_lines() {
    Canvas solid(16, 12);
    galaxy_draw_line(solid.surface(), 2, 5, 14, 5, 0xe5, false);
    Canvas dashed(16, 12);
    galaxy_draw_line(dashed.surface(), 2, 5, 14, 5, 0xe5, true);
    int solid_count = 0;
    int dashed_count = 0;
    for (int32_t x = 0; x < 16; ++x) {
        if (solid.at(x, 5) == 0xe5)
            ++solid_count;
        if (dashed.at(x, 5) == 0xe5) {
            ++dashed_count;
            CHECK(solid.at(x, 5) == 0xe5);
            CHECK(((x - 2) & 1) == 0);
        } else if (x >= 2 && x < 14) {
            CHECK(dashed.at(x, 5) == kFill);
        }
    }
    CHECK(solid_count == 12);
    CHECK(dashed_count == 6);
    Canvas other(16, 12);
    galaxy_draw_line(other.surface(), 2, 5, 14, 5, 0xe5);
    CHECK(same_pixels(solid, other));
}

void prepare_map(GalaxyView& view) {
    galaxy_init(&view);
    view.loading = false;
    view.has_data = true;
    view.planet_count = 2;
    view.planets[0].visible = true;
    view.planets[0].side = 'C';
    view.planets[0].capitol = true;
    view.planets[0].initial_side = 'A';
    view.planets[0].size_class = 0;
    view.planets[0].view_x = 20;
    view.planets[0].view_y = 30;
    view.planets[1].visible = true;
    view.planets[1].side = 'I';
    view.planets[1].capitol = true;
    view.planets[1].initial_side = 'c';
    view.planets[1].size_class = 0;
    view.planets[1].view_x = 40;
    view.planets[1].view_y = 30;
    view.connector_count = 2;
    view.connectors[0].visible = true;
    view.connectors[0].color = connector_color::arm;
    view.connectors[0].view_ax = 2;
    view.connectors[0].view_ay = 8;
    view.connectors[0].view_bx = 14;
    view.connectors[0].view_by = 8;
    view.connectors[1].visible = true;
    view.connectors[1].color = connector_color::core;
    view.connectors[1].view_ax = 2;
    view.connectors[1].view_ay = 10;
    view.connectors[1].view_bx = 14;
    view.connectors[1].view_by = 10;
}

void test_map() {
    auto view = std::make_unique<GalaxyView>();
    prepare_map(*view);
    GalaxyGraphics graphics;
    const int32_t reach = side_symbol_reach(view->planets[0].radius, view->scale);
    const SideSymbolPlace held =
        side_symbol_place(view->planets[0].view_x, view->planets[0].view_y, reach, false);
    const SideSymbolPlace founded =
        side_symbol_place(view->planets[0].view_x, view->planets[0].view_y, reach, true);
    const SideSymbolPlace unheld =
        side_symbol_place(view->planets[1].view_x, view->planets[1].view_y, reach, false);
    const SideSymbolPlace alone =
        side_symbol_place(view->planets[1].view_x, view->planets[1].view_y, reach, true);

    set_active(Mode::off, false);
    Canvas plain(64, 48);
    galaxy_draw(graphics, *view, plain.surface());
    CHECK(plain.at(held.left + 3, held.top + 3) == kFill);
    CHECK(plain.at(founded.left + 3, founded.top) == kFill);
    CHECK(plain.at(alone.left + 1, alone.top + 1) == kFill);
    CHECK(plain.at(2, 8) == static_cast<uint8_t>(connector_color::arm));
    CHECK(plain.at(3, 8) == static_cast<uint8_t>(connector_color::arm));
    CHECK(plain.at(3, 10) == static_cast<uint8_t>(connector_color::core));

    set_active(Mode::off, true);
    Canvas marked(64, 48);
    galaxy_draw(graphics, *view, marked.surface());
    // Core holds an Arm capitol: a filled square, and a hollow triangle on the other side.
    CHECK(marked.at(held.left + 3, held.top + 3) == k_side_symbol_ink);
    CHECK(marked.at(held.left + 1, held.top + 1) == k_side_symbol_outline);
    CHECK(marked.at(held.left, held.top) == kFill);
    CHECK(marked.at(founded.left + 3, founded.top) == k_side_symbol_ink);
    CHECK(marked.at(founded.left + 3, founded.top + 3) == kFill);
    // A Core capitol nobody holds carries the hollow square alone.
    CHECK(marked.at(unheld.left + 3, unheld.top + 3) == kFill);
    CHECK(marked.at(alone.left + 1, alone.top + 1) == k_side_symbol_ink);
    CHECK(marked.at(alone.left + 3, alone.top + 3) == kFill);
    CHECK(marked.at(2, 8) == static_cast<uint8_t>(connector_color::arm));
    CHECK(marked.at(3, 8) == static_cast<uint8_t>(connector_color::arm));

    set_active(Mode::protanopia, false);
    Canvas protan(64, 48);
    galaxy_draw(graphics, *view, protan.surface());
    CHECK(protan.at(held.left + 3, held.top + 3) == kFill);
    CHECK(protan.at(founded.left + 3, founded.top) == kFill);
    CHECK(protan.at(3, 8) == static_cast<uint8_t>(connector_color::arm));

    set_active(Mode::achromatopsia, false);
    CHECK(oa::present::colour_vision::shape_cues_active());
    Canvas grey(64, 48);
    galaxy_draw(graphics, *view, grey.surface());
    CHECK(grey.at(held.left + 3, held.top + 3) == k_side_symbol_ink);
    CHECK(grey.at(founded.left + 3, founded.top) == k_side_symbol_ink);
    CHECK(grey.at(alone.left + 1, alone.top + 1) == k_side_symbol_ink);
    CHECK(grey.at(2, 8) == static_cast<uint8_t>(connector_color::arm));
    CHECK(grey.at(3, 8) == kFill);
    CHECK(grey.at(4, 8) == static_cast<uint8_t>(connector_color::arm));
    CHECK(grey.at(2, 10) == static_cast<uint8_t>(connector_color::core));
    CHECK(grey.at(3, 10) == static_cast<uint8_t>(connector_color::core));
    set_active(Mode::off, false);
}

int ink_columns(const uint8_t* pixels, int32_t width, int32_t pitch, int32_t row) {
    int count = 0;
    bool in_bar = false;
    const uint8_t* line = pixels + static_cast<std::ptrdiff_t>(row) * pitch;
    for (int32_t x = 0; x < width; ++x) {
        const bool ink = line[x] == kCueInk;
        if (ink && !in_bar)
            ++count;
        in_bar = ink;
    }
    return count;
}

void test_bars_and_marks() {
    constexpr int32_t side = kByRowIconSize;
    std::vector<uint8_t> picture(static_cast<std::size_t>(side * side), 1);
    paint_latency_bars(picture.data(), side, side, side, 0, kCueInk, kCueGround);
    CHECK(picture[0] == 1);
    paint_latency_bars(picture.data(), side, side, side, 4, kCueInk, kCueGround);
    CHECK(picture[0] == 1);
    for (int grade = 1; grade <= 3; ++grade) {
        paint_latency_bars(picture.data(), side, side, side, grade, kCueInk, kCueGround);
        CHECK(ink_columns(picture.data(), side, side, 5) == grade);
        CHECK(picture[static_cast<std::size_t>(5 * side + 0)] == kCueGround);
    }

    std::vector<uint8_t> mark(static_cast<std::size_t>(kNameMarkSide * kNameMarkSide), 4);
    paint_name_mark(
        mark.data(), kNameMarkSide, kNameMarkSide, kNameMarkSide, NameMark::none, kCueInk, 9
    );
    CHECK(mark[0] == 4);
    paint_name_mark(
        mark.data(), kNameMarkSide, kNameMarkSide, kNameMarkSide, NameMark::lock, kCueInk, 9
    );
    CHECK(mark[3] == kCueInk);
    CHECK(mark[0] == 9);
    CHECK(mark[static_cast<std::size_t>(4 * kNameMarkSide + 3)] == kCueInk);
    paint_name_mark(
        mark.data(), kNameMarkSide, kNameMarkSide, kNameMarkSide, NameMark::mute, kCueInk, 9
    );
    CHECK(mark[0] == kCueInk);
    CHECK(mark[1] == 9);
    CHECK(mark[static_cast<std::size_t>(3 * kNameMarkSide + 3)] == kCueInk);
    CHECK(mark[static_cast<std::size_t>(1 * kNameMarkSide + 1)] == kCueInk);
}

void test_words() {
    set_active(Mode::off, false);
    char line[64];
    summary_side_count(line, sizeof line, true, "5");
    expect_line(line, "5");
    summary_side_count(line, sizeof line, false, "7");
    expect_line(line, "7");
    write_role_line(line, sizeof line, "hold", ChatRole::system, false);
    expect_line(line, "hold");

    set_active(Mode::off, true);
    summary_side_count(line, sizeof line, true, "5");
    expect_line(line, "Arm 5");
    summary_side_count(line, sizeof line, false, nullptr);
    expect_line(line, "Core");
    CHECK(chat_role_for_handles(10, 10, 11, 12, 13) == ChatRole::system);
    CHECK(chat_role_for_handles(11, 10, 11, 12, 13) == ChatRole::news);
    CHECK(chat_role_for_handles(12, 10, 11, 12, 13) == ChatRole::arm);
    CHECK(chat_role_for_handles(13, 10, 11, 12, 13) == ChatRole::core);
    CHECK(chat_role_for_handles(12, 12, 12, 12, 12) == ChatRole::system);
    CHECK(chat_role_for_handles(-1, -1, 11, 12, 13) == ChatRole::none);
    write_role_line(line, sizeof line, "hold", ChatRole::system, true);
    expect_line(line, "[SYS] hold");
    write_role_line(line, sizeof line, "wire", ChatRole::news, true);
    expect_line(line, "[NEWS] wire");
    write_role_line(line, sizeof line, "east", ChatRole::arm, true);
    expect_line(line, "[ARM] east");
    write_role_line(line, sizeof line, "west", ChatRole::core, true);
    expect_line(line, "[CORE] west");
    write_role_line(line, sizeof line, "", ChatRole::news, true);
    expect_line(line, "[NEWS]");
    write_role_line(line, sizeof line, "plain", ChatRole::none, true);
    expect_line(line, "plain");

    set_active(Mode::achromatopsia, false);
    summary_side_count(line, sizeof line, false, "2");
    expect_line(line, "Core 2");
    set_active(Mode::off, false);
}

void test_translated_words() {
    oa::data::languages::InterfaceText catalogue;
    CHECK(catalogue.add(
        "[SYS]\n\t{\n\tit=[SIS];\n\t}\n"
        "[NEWS]\n\t{\n\tit=[ACTU];\n\t}\n"
        "[ARM]\n\t{\n\tit=[ARM];\n\t}\n"
        "[CORE]\n\t{\n\tit=[CORE];\n\t}\n"
        "[Arm]\n\t{\n\tit=Arm;\n\t}\n"
        "[Core]\n\t{\n\tit=Core;\n\t}\n"
    ));
    const oa::data::languages::Language* italian = oa::data::languages::find_by_tag("it");
    CHECK(italian != nullptr);
    if (italian == nullptr)
        return;
    oa::data::languages::set_interface_language(&catalogue, *italian);
    set_active(Mode::off, true);
    char line[64];
    write_role_line(line, sizeof line, "hold", ChatRole::system, true);
    expect_line(line, "[SIS] hold");
    write_role_line(line, sizeof line, "wire", ChatRole::news, true);
    expect_line(line, "[ACTU] wire");
    write_role_line(line, sizeof line, "east", ChatRole::arm, true);
    expect_line(line, "[ARM] east");
    summary_side_count(line, sizeof line, true, "4");
    expect_line(line, "Arm 4");
    summary_side_count(line, sizeof line, false, "9");
    expect_line(line, "Core 9");
    set_active(Mode::off, false);
}

const char* stub_side_name(void*, const char* text) {
    if (text == nullptr)
        return nullptr;
    if (std::strcmp(text, "ARM") == 0)
        return "GLAN";
    if (std::strcmp(text, "CORE") == 0)
        return "NUCLEO";
    return nullptr;
}

bool starts_with_tag(const char* line) {
    return line != nullptr && line[0] == '[';
}

void test_stub_translation() {
    oa::data::languages::InterfaceText catalogue;
    CHECK(catalogue.add(
        "[SYS]\n\t{\n\tde=[SYS];\n\tes=[SIS];\n\t}\n"
        "[NEWS]\n\t{\n\tde=[NEWS];\n\tes=[NOTICIAS];\n\t}\n"
        "[ARM]\n\t{\n\tde=[ARM];\n\tes=[GLAN];\n\t}\n"
        "[CORE]\n\t{\n\tde=[CORE];\n\tes=[NÚCLEO];\n\t}\n"
    ));
    oa::data::languages::TranslationHooks hook{};
    hook.translate = stub_side_name;
    oa::data::languages::set_translation_hooks(hook);
    set_active(Mode::off, true);
    char line[64];
    const oa::data::languages::Language* german = oa::data::languages::find_by_tag("de");
    const oa::data::languages::Language* spanish = oa::data::languages::find_by_tag("es");
    CHECK(german != nullptr && spanish != nullptr);
    if (german != nullptr) {
        oa::data::languages::set_interface_language(&catalogue, *german);
        write_role_line(line, sizeof line, "east", ChatRole::arm, true);
        expect_line(line, "[ARM] east");
        CHECK(starts_with_tag(line));
        write_role_line(line, sizeof line, "west", ChatRole::core, true);
        expect_line(line, "[CORE] west");
        CHECK(starts_with_tag(line));
    }
    if (spanish != nullptr) {
        oa::data::languages::set_interface_language(&catalogue, *spanish);
        write_role_line(line, sizeof line, "east", ChatRole::arm, true);
        expect_line(line, "[GLAN] east");
        CHECK(starts_with_tag(line));
        write_role_line(line, sizeof line, "west", ChatRole::core, true);
        expect_line(line, "[NÚCLEO] west");
        CHECK(starts_with_tag(line));
        write_role_line(line, sizeof line, "wire", ChatRole::news, true);
        expect_line(line, "[NOTICIAS] wire");
    }
    oa::data::languages::set_translation_hooks({});
    set_active(Mode::off, false);
}

bool symbol_edge_at(SideSymbol symbol, int32_t x, int32_t y) {
    if (!side_symbol_covers(symbol, x, y))
        return false;
    return !side_symbol_covers(symbol, x - 1, y) || !side_symbol_covers(symbol, x + 1, y) ||
           !side_symbol_covers(symbol, x, y - 1) || !side_symbol_covers(symbol, x, y + 1);
}

// A symbol pixel may land only where its planet left the viewer blank. That blank is a key pixel
// of the aura, the capitol marker or the sprite.
void expect_mark_clear(
    const Canvas& plain,
    const Canvas& marked,
    SideSymbol symbol,
    SideSymbolPlace at,
    bool hollow,
    int32_t width,
    int32_t height
) {
    for (int32_t y = 0; y < k_side_symbol_side; ++y) {
        for (int32_t x = 0; x < k_side_symbol_side; ++x) {
            if (!side_symbol_covers(symbol, x, y))
                continue;
            const bool edge = symbol_edge_at(symbol, x, y);
            if (hollow && !edge)
                continue;
            const int32_t column = at.left + x;
            const int32_t row = at.top + y;
            CHECK(column >= 0 && row >= 0 && column < width && row < height);
            if (column < 0 || row < 0 || column >= width || row >= height)
                continue;
            if (plain.at(column, row) != kFill) {
                std::fprintf(
                    stderr,
                    "FAIL mark covers planet pixel at %d,%d (was %u)\n",
                    column,
                    row,
                    plain.at(column, row)
                );
                ++g_failures;
            }
            const uint8_t ink = edge && !hollow ? k_side_symbol_outline : k_side_symbol_ink;
            CHECK(marked.at(column, row) == ink);
        }
    }
}

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

const oa::formats::gaf::Sequence*
sequence_named(const oa::formats::gaf::Archive& archive, const std::string& name) {
    return galaxy_find_sequence(archive, name.c_str());
}

// The install's planet pictures, one size class: the four magnifications share a base name.
struct SpriteSet {
    std::array<const oa::formats::gaf::Sequence*, 4> sizes{};
};

bool read_install(void* user, const char* path, std::vector<uint8_t>* out) {
    try {
        *out = static_cast<oa::AssetStore*>(user)->read(path).bytes;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// Every planet picture of one size class that the install has at all four magnifications.
std::vector<SpriteSet> sprite_sets(const oa::formats::gaf::Archive& planets, const char* klass) {
    std::vector<SpriteSet> sets;
    const char* magnifications[] = {"25", "50", "75", "100"};
    for (const oa::formats::gaf::Sequence& sequence : planets.sequences) {
        const std::string suffix = std::string(magnifications[0]) + klass;
        if (!ends_with(sequence.name, suffix))
            continue;
        const std::string base = sequence.name.substr(0, sequence.name.size() - suffix.size());
        SpriteSet set;
        bool complete = true;
        for (std::size_t size = 0; size < 4; ++size) {
            const std::string name = base + magnifications[size] + klass;
            set.sizes[size] = sequence_named(planets, name);
            complete = complete && set.sizes[size] != nullptr && !set.sizes[size]->frames.empty();
        }
        if (complete)
            sets.push_back(set);
    }
    return sets;
}

void write_mark_pictures(GalaxyGraphics& graphics, GalaxyView& view, const SpriteSet& large);

// At each magnification, size class and planet picture, for every holder and founder, neither
// mark covers a painted pixel of its planet: its aura, its capitol marker or its sprite.
void test_installed_marks() {
    const std::filesystem::path root = oa::boneyards_test::by_install();
    if (root.empty()) {
        std::printf("side-mark overlap not checked: OA_GAME_DIR names no install\n");
        return;
    }
    oa::AssetStore assets{root};
    assets.mount(root / "by.ccx");
    const FileReader files{&assets, read_install};
    auto graphics = std::make_unique<GalaxyGraphics>();
    if (!galaxy_load_graphics(*graphics, files) || !graphics->planets) {
        std::fprintf(stderr, "FAIL: the install's war-map graphics did not load\n");
        ++g_failures;
        return;
    }
    const char* classes[] = {"_L", "_M", "_S"};
    constexpr int32_t radii[] = {0x14, 0x10, 0x0c};
    std::vector<SpriteSet> sprites[3];
    for (int klass = 0; klass < 3; ++klass) {
        sprites[klass] = sprite_sets(*graphics->planets, classes[klass]);
        if (sprites[klass].empty()) {
            std::fprintf(stderr, "FAIL: no planet sprite for size class %s\n", classes[klass]);
            ++g_failures;
            return;
        }
    }
    constexpr int32_t width = kGalaxyViewerWidth;
    constexpr int32_t height = kGalaxyViewerHeight;
    auto view = std::make_unique<GalaxyView>();
    galaxy_init(view.get());
    view->loading = false;
    view->has_data = true;
    view->planet_count = 1;
    GalaxyPlanet& planet = view->planets[0];
    planet.visible = true;
    planet.capitol = true;
    planet.view_x = 240;
    planet.view_y = 160;

    // Each capitol held by its founder, captured by the other side, and held by nobody. The
    // triangle reaches its box's bottom-left corner and the square does not, so both sides hold.
    struct Holding {
        char side;
        char initial_side;
    };

    constexpr Holding holdings[] = {
        {'A', 'A'}, {'C', 'C'}, {'A', 'C'}, {'C', 'A'}, {'I', 'A'}, {'I', 'C'}
    };
    for (int32_t scale = 0; scale < kGalaxyScaleCount; ++scale) {
        view->scale = scale;
        for (int klass = 0; klass < 3; ++klass) {
            planet.size_class = klass;
            planet.radius = radii[klass];
            for (const SpriteSet& set : sprites[klass]) {
                graphics->planet_sequences[0][static_cast<std::size_t>(scale)] =
                    set.sizes[static_cast<std::size_t>(scale)];
                for (const Holding& holding : holdings) {
                    planet.side = holding.side;
                    planet.initial_side = holding.initial_side;
                    set_active(Mode::off, false);
                    Canvas plain(width, height);
                    galaxy_draw(*graphics, *view, plain.surface());
                    set_active(Mode::off, true);
                    Canvas marked(width, height);
                    galaxy_draw(*graphics, *view, marked.surface());
                    const int32_t reach = side_symbol_reach(planet.radius, scale);
                    const SideSymbol held = held_side_symbol(planet.side);
                    const SideSymbol founded = founded_side_symbol(true, planet.initial_side);
                    const int failures = g_failures;
                    if (held != SideSymbol::none)
                        expect_mark_clear(
                            plain,
                            marked,
                            held,
                            side_symbol_place(planet.view_x, planet.view_y, reach, false),
                            false,
                            width,
                            height
                        );
                    expect_mark_clear(
                        plain,
                        marked,
                        founded,
                        side_symbol_place(planet.view_x, planet.view_y, reach, true),
                        true,
                        width,
                        height
                    );
                    if (g_failures != failures)
                        std::fprintf(
                            stderr,
                            "  at magnification %d, size class %s, held by %c, founded by %c\n",
                            static_cast<int>(scale),
                            classes[klass],
                            holding.side,
                            holding.initial_side
                        );
                }
            }
        }
    }
    planet.side = 'C';
    planet.initial_side = 'A';
    write_mark_pictures(*graphics, *view, sprites[0].front());
    set_active(Mode::off, false);
}

void append_base64(std::string& out, const uint8_t* bytes, std::size_t count) {
    static constexpr char kDigits[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::size_t index = 0;
    while (index + 3 <= count) {
        const uint32_t chunk = (static_cast<uint32_t>(bytes[index]) << 16) |
                               (static_cast<uint32_t>(bytes[index + 1]) << 8) |
                               static_cast<uint32_t>(bytes[index + 2]);
        out.push_back(kDigits[(chunk >> 18) & 63]);
        out.push_back(kDigits[(chunk >> 12) & 63]);
        out.push_back(kDigits[(chunk >> 6) & 63]);
        out.push_back(kDigits[chunk & 63]);
        index += 3;
    }
    if (index < count) {
        uint32_t chunk = static_cast<uint32_t>(bytes[index]) << 16;
        out.push_back(kDigits[(chunk >> 18) & 63]);
        if (index + 1 < count) {
            chunk |= static_cast<uint32_t>(bytes[index + 1]) << 8;
            out.push_back(kDigits[(chunk >> 12) & 63]);
            out.push_back(kDigits[(chunk >> 6) & 63]);
        } else {
            out.push_back(kDigits[(chunk >> 12) & 63]);
            out.push_back('=');
        }
        out.push_back('=');
    }
}

// Writes a cropped PPM of the captured capitol at one magnification, as base64, when
// OA_SHAPE_CUE_MARK_PICTURES is set. The studio run prints it so the pictures can be handed back.
void write_mark_pictures(GalaxyGraphics& graphics, GalaxyView& view, const SpriteSet& large) {
    const auto asked = oa::platform::environment_value("OA_SHAPE_CUE_MARK_PICTURES");
    if (!asked || asked->empty())
        return;
    const std::filesystem::path root = oa::boneyards_test::by_install();
    const std::vector<uint8_t> frontend = oa::test::netgame::read_archived(
        root, oa::test::netgame::kBaseArchive, "bitmaps/frontendx.pcx"
    );
    constexpr std::size_t kPaletteBytes = 256 * 3;
    if (frontend.size() < kPaletteBytes)
        return;
    const uint8_t* palette = frontend.data() + frontend.size() - kPaletteBytes;
    constexpr int32_t scales[] = {0, 3};
    for (const int32_t scale : scales) {
        view.scale = scale;
        view.planets[0].size_class = 0;
        view.planets[0].radius = 0x14;
        graphics.planet_sequences[0][static_cast<std::size_t>(scale)] =
            large.sizes[static_cast<std::size_t>(scale)];
        set_active(Mode::off, true);
        oa::present::SurfaceBuffer image =
            oa::present::create_surface(kGalaxyViewerWidth, kGalaxyViewerHeight);
        std::fill(image.pixels.begin(), image.pixels.end(), static_cast<uint8_t>(0));
        galaxy_draw(graphics, view, &image.surface);
        const int32_t reach = side_symbol_reach(view.planets[0].radius, scale);
        const int32_t half = reach + k_side_symbol_side + 24;
        int32_t left = view.planets[0].view_x - half;
        int32_t top = view.planets[0].view_y - half;
        int32_t right = view.planets[0].view_x + half;
        int32_t bottom = view.planets[0].view_y + half;
        left = std::max(left, 0);
        top = std::max(top, 0);
        right = std::min(right, image.surface.width);
        bottom = std::min(bottom, image.surface.height);
        const int32_t crop_w = right - left;
        const int32_t crop_h = bottom - top;
        std::vector<uint8_t> rgb(static_cast<std::size_t>(crop_w * crop_h * 3));
        for (int32_t y = 0; y < crop_h; ++y) {
            for (int32_t x = 0; x < crop_w; ++x) {
                const uint8_t index = image.pixels[static_cast<std::size_t>(
                    (top + y) * image.surface.pitch + (left + x)
                )];
                const std::size_t at = static_cast<std::size_t>((y * crop_w + x) * 3);
                rgb[at] = palette[static_cast<std::size_t>(index) * 3];
                rgb[at + 1] = palette[static_cast<std::size_t>(index) * 3 + 1];
                rgb[at + 2] = palette[static_cast<std::size_t>(index) * 3 + 2];
            }
        }
        std::string header =
            "P6\n" + std::to_string(crop_w) + " " + std::to_string(crop_h) + "\n255\n";
        std::string payload = header;
        payload.append(reinterpret_cast<const char*>(rgb.data()), rgb.size());
        std::string encoded;
        append_base64(encoded, reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
        std::printf("MARK-PICTURE mag%d %s\n", scale, encoded.c_str());
    }
}

} // namespace

int main() {
    const RestoreMode mode;
    const RestoreLanguage language;
    test_symbols();
    test_lines();
    test_map();
    test_bars_and_marks();
    test_words();
    test_translated_words();
    test_stub_translation();
    test_installed_marks();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("shape cue tests passed\n");
    return 0;
}

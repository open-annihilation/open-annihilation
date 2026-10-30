// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// ui.megamap's pieces: the fitted layout and its two-way mapping, the
// downscaled terrain snapped or dithered, by red, green and blue or as it
// looks among the map's own colours; the features it draws, their pictures
// shrunk, placed and laid over the terrain, and the marks of those without
// one; the icon file, the icon chosen for a unit and its recoloured pixels,
// and the rings with their minimums.
#include "oa/ui/hud/megamap.hpp"

#include "check.hpp"
#include "fixtures.hpp"

#include <array>
#include <cstring>
#include <string>
#include <vector>

using namespace oa;
using namespace oa::ui::hud;

namespace {

void layout() {
    // A 4096x2048 map in a 512x416 view: full width, 256 high, centred.
    const auto fitted = megamap_layout(128, 32, 512, 416, 4096, 2048);
    CHECK(
        fitted.left == 128 && fitted.width == 512 && fitted.height == 256 && fitted.top == 32 + 80
    );
    const auto tall = megamap_layout(0, 0, 512, 416, 1024, 2048);
    CHECK(tall.height == 416 && tall.width == 208 && tall.left == 152 && tall.top == 0);
    const auto at = megamap_point(fitted, 2048, 1024);
    CHECK(at[0] == 128 + 256 && at[1] == 112 + 128);
    const auto back = megamap_map_point(fitted, 128 + 256, 112 + 128);
    CHECK(back.has_value() && (*back)[0] == 2048 && (*back)[1] == 1024);
    CHECK(!megamap_map_point(fitted, 128, 100).has_value());
    CHECK(megamap_layout(0, 0, 0, 10, 10, 10).width == 0);
}

/// A palette of greys: entry n is (n, n, n).
std::vector<uint8_t> grey_palette() {
    std::vector<uint8_t> palette(256 * 4);
    for (int n = 0; n < 256; ++n)
        palette[n * 4] = palette[n * 4 + 1] = palette[n * 4 + 2] = static_cast<uint8_t>(n);
    return palette;
}

void terrain() {
    const auto palette = grey_palette();
    // A 4x2 map of 10 and 20 alternating columns, to 2x1: each pixel the mean, 15.
    TerrainSource source{};
    source.pixel = [](void*, int32_t x, int32_t) -> uint8_t { return x % 2 == 0 ? 10 : 20; };
    auto picture = downscale_terrain(source, 4, 2, 2, 1, palette, false, nullptr);
    CHECK(picture.size() == 2 && picture[0] == 15 && picture[1] == 15);
    // Dithered with two nearest entries 15 and 14 (or 16): odd pixels take the second.
    picture = downscale_terrain(source, 4, 2, 2, 1, palette, true, nullptr);
    CHECK(picture[0] == 15 && picture[1] != 15);
    CHECK(downscale_terrain(source, 0, 2, 2, 1, palette, false, nullptr).empty());
}

void map_colours() {
    const auto palette = grey_palette();
    // Among the map's own colours, 10 and 20, the mean 15 looks nearer 20,
    // though by red, green and blue it lies halfway and the first, 10, wins.
    const std::array<uint8_t, 4> tiles{20, 10, 10, 20};
    const auto colours = perceptual_palette(palette, tiles);
    CHECK(colours.entries.size() == 2 && colours.entries[0] == 10 && colours.entries[1] == 20);
    CHECK(nearest_perceptual(colours, 15, 15, 15) == 20);
    CHECK(nearest_palette_entry(palette, 15, 15, 15) == 15);
    TerrainSource source{};
    source.pixel = [](void*, int32_t x, int32_t) -> uint8_t { return x % 2 == 0 ? 10 : 20; };
    auto picture = downscale_terrain(source, 4, 2, 2, 1, palette, false, &colours);
    CHECK(picture.size() == 2 && picture[0] == 20 && picture[1] == 20);
    // Dithered, the odd pixels take the other of the map's colours.
    picture = downscale_terrain(source, 4, 2, 2, 1, palette, true, &colours);
    CHECK(picture[0] == 20 && picture[1] == 10);
    // No pixels: every entry of the palette.
    CHECK(perceptual_palette(palette, {}).entries.size() == 256);
}

void features() {
    // Only the indestructible features that cannot be reclaimed are drawn.
    FeatureDef def{};
    def.flags = OA_FEATURE_FLAG_INDESTRUCTIBLE;
    CHECK(megamap_draws_feature(def));
    def.flags = OA_FEATURE_FLAG_INDESTRUCTIBLE | OA_FEATURE_FLAG_RECLAIMABLE;
    CHECK(!megamap_draws_feature(def));
    def.flags = OA_FEATURE_FLAG_RECLAIMABLE;
    CHECK(!megamap_draws_feature(def));
    def.flags = 0;
    CHECK(!megamap_draws_feature(def));
    // The marks of those without a picture: metal, a "Spire" by its
    // description in any case, and the rest.
    std::snprintf(def.description, sizeof def.description, "%s", "SPIRE");
    CHECK(feature_mark_color(def) == feature_mark_spire);
    def.metal = 5.0F;
    CHECK(feature_mark_color(def) == feature_mark_metal);
    def.metal = 0.0F;
    std::snprintf(def.name, sizeof def.name, "%s", "Spire");
    std::snprintf(def.description, sizeof def.description, "%s", "Rock");
    CHECK(feature_mark_color(def) == feature_mark_other);
}

void feature_pictures() {
    const auto palette = grey_palette();
    // A 4x2 picture whose index 9 is transparent, shrunk to 2 across: the
    // left half opaque, 100 and 200, the right a quarter opaque, 50.
    const std::array<uint8_t, 8> frame{100, 100, 9, 9, 200, 200, 9, 50};
    const auto shrunk = shrink_picture(frame, 4, 2, 9, palette, 2);
    CHECK(shrunk.width == 2 && shrunk.height == 1 && shrunk.rgba.size() == 8);
    CHECK(shrunk.rgba[0] == 150 && shrunk.rgba[1] == 150 && shrunk.rgba[2] == 150);
    CHECK(shrunk.rgba[3] == 255);
    CHECK(shrunk.rgba[4] == 50 && shrunk.rgba[7] == 64);
    // Laid over black: the opaque pixel keeps its colour, the quarter one
    // takes (50 * 64 + 0 * 191) / 255 = 12; the pixel left of it is untouched.
    std::array<uint8_t, 3> canvas{0, 0, 0};
    blend_picture(canvas, 3, 1, shrunk, 1, 0, palette);
    CHECK(canvas[0] == 0 && canvas[1] == 150 && canvas[2] == 12);
    // Off the canvas's left edge only the second pixel lands, over 7:
    // (50 * 64 + 7 * 191) / 255 = 17.
    canvas = {7, 7, 7};
    blend_picture(canvas, 3, 1, shrunk, -1, 0, palette);
    CHECK(canvas[0] == 17 && canvas[1] == 7 && canvas[2] == 7);
    // A fully transparent picture leaves the canvas as it is.
    const std::array<uint8_t, 4> clear{9, 9, 9, 9};
    const auto empty = shrink_picture(clear, 2, 2, 9, palette, 2);
    blend_picture(canvas, 3, 1, empty, 0, 0, palette);
    CHECK(canvas[0] == 17 && canvas[1] == 7 && canvas[2] == 7);

    // A 4096x2048 map drawn 256x128: a 2x2 feature at cell (10, 20) on
    // ground 40 high, its frame 64 pixels on its longer side.
    MegamapLayout layout{};
    layout.width = 256;
    layout.height = 128;
    layout.map_width = 4096;
    layout.map_height = 2048;
    auto spot = megamap_feature_spot(layout, 10, 20, 2, 2, 40, 64);
    // (176, 336 - 20) scaled by 1/16, truncated; 64 / 16 + 0.5 truncated.
    CHECK(spot.x == 11 && spot.y == 19 && spot.longest == 4);
    // A frame without a size counts as 32; a small one is drawn 2 across.
    CHECK(megamap_feature_spot(layout, 10, 20, 2, 2, 40, 0).longest == 2);
    CHECK(megamap_feature_spot(layout, 10, 20, 2, 2, 40, 8).longest == 2);
    // A footprint of 0 counts as 1 cell.
    spot = megamap_feature_spot(layout, 10, 20, 0, 0, 0, 64);
    CHECK(spot.x == 10 && spot.y == 20);
    // The picture hangs from its origin row scaled: 30 of 60 rows is 2 of 4.
    CHECK(feature_picture_top(19, 30, 60, 4) == 17);
    CHECK(feature_picture_top(19, 30, 0, 4) == 15);
}

void icon_file() {
    const char* text = "; Megamap Icon Settings\n"
                       "[Option] ; General\n"
                       "FillColor=245;  ; replaced\n"
                       "SelectedColor=255;\n"
                       "UseCircleHover=FALSE;\n"
                       "UseDefaultIcon=FALSE;\n"
                       "[ICON]\n"
                       "Unknow=UNKNOWN.PCX;\n"
                       "Nothing=NONE.pcx;\n"
                       "NukeIcon=NUKEICON.pcx;\n"
                       "AIR=AIR.pcx\n"
                       "LAB=FACTORY.pcx\n"
                       "BLDG=BUILDING.pcx\n";
    auto config = parse_icon_config(text, 9);
    CHECK(config.options.fill_color == 245 && config.options.selected_color == 255);
    CHECK(config.options.transparent_color == 9 && config.options.hover_color == 0x54);
    CHECK(!config.options.default_icons && !config.options.circle_hover);
    CHECK(config.unknown_file == "UNKNOWN.PCX" && config.nothing_file == "NONE.pcx");
    CHECK(config.nuke_file == "NUKEICON.pcx");
    CHECK(
        config.lines.size() == 3 && config.lines[0].category == "air" &&
        config.lines[1].file == "FACTORY.pcx"
    );
    // The profile's count caps the category lines read.
    config = parse_icon_config(text, 2);
    CHECK(config.lines.size() == 2);

    // 130 category lines, with the special lines among them. Those three are
    // read and never count against the limit.
    std::string wide = "[Icon]\n";
    for (int index = 0; index < 130; ++index) {
        if (index == 10)
            wide += "Unknow=UNKNOWN.PCX;\n";
        if (index == 40)
            wide += "Nothing=NONE.pcx;\n";
        if (index == 90)
            wide += "NukeIcon=NUKEICON.pcx;\n";
        wide += "CAT" + std::to_string(index) + "=C.pcx\n";
    }
    config = parse_icon_config(wide, 128);
    CHECK(config.lines.size() == 128 && config.lines.back().category == "cat127");
    CHECK(config.unknown_file == "UNKNOWN.PCX" && config.nothing_file == "NONE.pcx");
    CHECK(config.nuke_file == "NUKEICON.pcx");
    config = parse_icon_config(wide, 69);
    CHECK(config.lines.size() == 69 && config.lines.back().category == "cat68");
}

void icon_masks() {
    data::defs::CategoryRegistry registry{};
    data::defs::category_registry_init(&registry);
    for (int index = 0; index < 64; ++index) {
        const std::string name = "CAT" + std::to_string(index);
        CHECK(data::defs::category_registry_ref(&registry, name.c_str()) != 0);
    }
    CHECK(registry.count == 64 && registry.count == registry.capacity);
    const IconLine known{"cAt12", "x.pcx"};
    const auto* found = data::defs::category_registry_find(&registry, "CAT12");
    CHECK(icon_category_mask(registry, known) == found && found != nullptr);
    const IconLine missing{"no-such", "x.pcx"};
    CHECK(icon_category_mask(registry, missing) == nullptr);
    for (int index = 0; index < 70; ++index) {
        const IconLine line{index % 2 == 0 ? "cat3" : "absent", "f.pcx"};
        (void)icon_category_mask(registry, line);
    }
    CHECK(registry.count == 64 && registry.capacity == 64);
    data::defs::category_registry_clear(&registry);
}

void choosing() {
    std::array<uint32_t, 2> air_words{1u << 3, 0};
    std::array<uint32_t, 2> lab_words{(1u << 3) | (1u << 5), 0};
    const data::defs::CategoryMask air{air_words.data(), 2};
    const data::defs::CategoryMask lab{lab_words.data(), 2};
    const std::array<const data::defs::CategoryMask*, 3> masks{nullptr, &air, &lab};
    std::size_t line = 99;
    CHECK(choose_icon(false, 3, masks, line) == IconChoice::nothing);
    CHECK(choose_icon(true, 3, masks, line) == IconChoice::category && line == 1);
    CHECK(choose_icon(true, 5, masks, line) == IconChoice::category && line == 2);
    CHECK(choose_icon(true, 6, masks, line) == IconChoice::unknown);
    IconOptions options{};
    options.fill_color = 245;
    options.selected_color = 255;
    CHECK(!icon_pixel(options, 9, 80, false, false).has_value());
    CHECK(*icon_pixel(options, 245, 80, false, false) == 80);
    CHECK(*icon_pixel(options, 255, 80, true, false) == 255);
    CHECK(!icon_pixel(options, 255, 80, false, false).has_value());
    CHECK(*icon_pixel(options, 255, 80, false, true) == 0x54);
    options.circle_hover = true;
    CHECK(!icon_pixel(options, 255, 80, false, true).has_value());
    CHECK(*icon_pixel(options, 17, 80, false, false) == 17);
}

void rings() {
    hud_test::TestWorld w;
    auto& game = w.game();
    for (uint8_t i = 0; i < 16; ++i)
        game.ui_colors[i] = static_cast<uint8_t>(0xb0 + i);
    UnitDef def{};
    def.radar_distance = 1200;
    def.sonar_distance = 100;
    def.radar_distance_jam = 64;
    def.sonar_distance_jam = 0;
    Unit unit{};
    unit.weapons[1].def = oa_ref_from_index(2);
    game.weapon_defs[2].flags = OA_WEAPON_FLAG_INTERCEPTOR;
    game.weapon_defs[2].coverage = 2000;
    std::array<MegamapRing, 5> out{};
    // 128, 128, 64, 64, 512: the sonar falls short of its minimum.
    auto count = megamap_rings(*w.world, unit, def, {128, 128, 64, 64, 512}, out);
    CHECK(count == 3 && out[0].radius == 1200 && out[0].color == 0xba);
    CHECK(
        out[1].radius == 64 && out[1].color == 0xbc && out[2].radius == 2000 && out[2].color == 0xbf
    );
    // 3.1c's minimums draw every ring above 0 but the anti-nuke's below 512.
    game.weapon_defs[2].coverage = 400;
    count = megamap_rings(*w.world, unit, def, {0, 0, 0, 0, 512}, out);
    CHECK(count == 3 && out[1].radius == 100);
}

} // namespace

int main() {
    layout();
    terrain();
    map_colours();
    features();
    feature_pictures();
    icon_file();
    icon_masks();
    choosing();
    rings();
    std::puts("ui-hud-megamap-test: ok");
    return 0;
}

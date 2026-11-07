// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The colour-vision distance check, and the stock palette's anchor indices.
#include "oa/present/colour_vision/colour_vision.hpp"

#include "oa/formats/gaf.hpp"
#include "oa/test/check.hpp"
#include "oa/test/game_assets.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vision = oa::present::colour_vision;

namespace {

struct RestoreMode {
    ~RestoreMode() { vision::set_active(vision::Mode::off, false); }
};

int tenth(double distance) {
    return static_cast<int>(std::round(distance * 10.0));
}

vision::Lab unchanged_lab(vision::Mode, vision::Rgb colour) noexcept {
    return vision::lab_from_srgb(colour);
}

template <typename Eye>
double least_distance(
    std::span<const vision::Rgb> colours, vision::Mode mode, Eye eye, int& first, int& second
) {
    double best = 1.0e9;
    first = -1;
    second = -1;
    for (int left = 0; left < static_cast<int>(colours.size()); ++left) {
        for (int right = left + 1; right < static_cast<int>(colours.size()); ++right) {
            const double distance = vision::ciede2000(
                eye(mode, colours[static_cast<std::size_t>(left)]),
                eye(mode, colours[static_cast<std::size_t>(right)])
            );
            if (distance < best) {
                best = distance;
                first = left;
                second = right;
            }
        }
    }
    return best;
}

// `require_goal` asks for 20 under that eye. Normal vision of the same
// anchors is recorded too, and some of those pairs sit under 20.
template <typename Eye>
void check_seen(
    vision::Mode mode,
    Eye eye,
    bool require_goal,
    int published_tenth,
    int pair_left,
    int pair_right
) {
    vision::Rgb colours[vision::kPlayerCount];
    const int count = mode == vision::Mode::achromatopsia ? 4 : vision::kPlayerCount;
    for (int player = 0; player < count; ++player)
        colours[player] = vision::anchor_srgb(mode, player);
    const std::span<const vision::Rgb> group(colours, static_cast<std::size_t>(count));
    if (require_goal) {
        for (int left = 0; left < count; ++left) {
            for (int right = left + 1; right < count; ++right) {
                const double distance =
                    vision::ciede2000(eye(mode, colours[left]), eye(mode, colours[right]));
                OA_CHECK(distance >= 20.0);
                OA_CHECK(tenth(distance) >= 200);
            }
        }
    }
    int first = -1;
    int second = -1;
    const double least = least_distance(group, mode, eye, first, second);
    OA_CHECK(tenth(least) == published_tenth);
    OA_CHECK(first == pair_left);
    OA_CHECK(second == pair_right);
}

void check_group(vision::Mode mode, std::initializer_list<vision::Rgb> group, int published_tenth) {
    const std::vector<vision::Rgb> colours(group);
    int first = -1;
    int second = -1;
    const double least = least_distance(colours, mode, vision::simulate, first, second);
    OA_CHECK(tenth(least) == published_tenth);
}

bool terrain_holds(int index) {
    for (const int listed : vision::terrain_indices()) {
        if (listed == index)
            return true;
    }
    return false;
}

void check_terrain_membership() {
    OA_CHECK(vision::terrain_indices().size() == 32U);
    OA_CHECK(terrain_holds(168));
    OA_CHECK(!terrain_holds(171));
    const vision::Mode modes[] = {
        vision::Mode::protanopia,
        vision::Mode::deuteranopia,
        vision::Mode::tritanopia,
        vision::Mode::achromatopsia
    };
    for (const vision::Mode mode : modes) {
        for (int player = 0; player < vision::kPlayerCount; ++player) {
            const int index = vision::stock_anchor_index(mode, player);
            OA_CHECK(index != 171);
            OA_CHECK(!terrain_holds(index));
        }
    }
}

vision::Rgb palette_rgb(std::span<const uint8_t> palette, int index) {
    const auto at = static_cast<std::size_t>(index) * 4U;
    return vision::Rgb{palette[at], palette[at + 1], palette[at + 2]};
}

// Least distance from an anchor to a terrain index, under one eye.
template <typename Eye>
double terrain_gap(
    vision::Mode mode, Eye eye, std::span<const uint8_t> palette, std::span<const int> terrain
) {
    double best = 1.0e9;
    const int count = mode == vision::Mode::achromatopsia ? 4 : vision::kPlayerCount;
    for (int player = 0; player < count; ++player) {
        const vision::Rgb anchor = vision::anchor_srgb(mode, player);
        for (const int index : terrain) {
            const double distance =
                vision::ciede2000(eye(mode, anchor), eye(mode, palette_rgb(palette, index)));
            if (distance < best)
                best = distance;
        }
    }
    return best;
}

void check_semantics(vision::Mode mode, int health, int war, int resource, int chat, int kit) {
    const vision::SemanticColours colours = vision::semantic_colours(mode);
    check_group(
        mode,
        {colours.health_good, colours.health_mid, colours.health_bad, colours.health_trough},
        health
    );
    check_group(
        mode, {colours.war_arm, colours.war_core, colours.war_mixed, colours.war_space}, war
    );
    check_group(
        mode,
        {colours.resource_produced, colours.resource_consumed, colours.resource_text},
        resource
    );
    check_group(
        mode,
        {colours.chat_system,
         colours.chat_newsbot,
         colours.chat_arm,
         colours.chat_core,
         colours.chat_plain},
        chat
    );
    check_group(
        mode,
        {colours.kit_ok,
         colours.kit_caution,
         colours.kit_danger,
         colours.kit_information,
         colours.kit_panel},
        kit
    );
}

void check_preferences() {
    OA_CHECK(vision::parse_mode("protanopia") == vision::Mode::protanopia);
    OA_CHECK(vision::parse_mode("deuteranopia") == vision::Mode::deuteranopia);
    OA_CHECK(vision::parse_mode("tritanopia") == vision::Mode::tritanopia);
    OA_CHECK(vision::parse_mode("achromatopsia") == vision::Mode::achromatopsia);
    OA_CHECK(vision::parse_mode("off") == vision::Mode::off);
    OA_CHECK(vision::parse_mode("Protanopia") == vision::Mode::off);
    OA_CHECK(vision::parse_mode("") == vision::Mode::off);
    OA_CHECK(vision::parse_mode("1") == vision::Mode::off);
    OA_CHECK(vision::mode_name(vision::Mode::tritanopia) == "tritanopia");
    OA_CHECK(vision::mode_name(vision::Mode::off) == "off");
    OA_CHECK(vision::shape_cues_preference("1"));
    OA_CHECK(vision::shape_cues_preference("2"));
    OA_CHECK(!vision::shape_cues_preference("0"));
    OA_CHECK(!vision::shape_cues_preference("on"));
    OA_CHECK(!vision::shape_cues_preference("true"));
    OA_CHECK(!vision::shape_cues_in_effect(vision::Mode::protanopia, false));
    OA_CHECK(vision::shape_cues_in_effect(vision::Mode::protanopia, true));
    OA_CHECK(vision::shape_cues_in_effect(vision::Mode::achromatopsia, false));
    OA_CHECK(vision::active_mode() == vision::Mode::off);
    OA_CHECK(!vision::shape_cues_active());
    vision::set_active(vision::Mode::protanopia, false);
    OA_CHECK(vision::active_mode() == vision::Mode::protanopia);
    OA_CHECK(!vision::shape_cues_active());
    OA_CHECK(
        vision::player_dot(0) ==
        static_cast<uint8_t>(vision::stock_anchor_index(vision::Mode::protanopia, 0))
    );
    OA_CHECK(vision::player_dot(99) == vision::player_dot(0));
    vision::set_active(vision::Mode::achromatopsia, false);
    OA_CHECK(vision::shape_cues_active());
    OA_CHECK(
        vision::stock_anchor_index(vision::Mode::achromatopsia, 0) ==
        vision::stock_anchor_index(vision::Mode::achromatopsia, 4)
    );
    vision::set_active(vision::Mode::off, true);
    OA_CHECK(vision::shape_cues_active());
    vision::set_active(vision::Mode::off, false);
    OA_CHECK(!vision::holds_originals());
}

void check_rows() {
    // One opaque pixel, row length 2: a literal of palette index 227.
    std::vector<uint8_t> stream{2, 0, 0, 227};
    OA_CHECK(vision::row_rle_size(stream, 1, 1) == 4U);
    vision::IndexMap map{};
    for (int colour = 0; colour < vision::kPaletteColours; ++colour)
        map[static_cast<std::size_t>(colour)] = static_cast<uint8_t>(colour);
    map[227] = 103;
    OA_CHECK(vision::remap_row_rle(stream, 1, 1, map, -1));
    OA_CHECK(stream[3] == 103);
    std::vector<uint8_t> indices;
    stream = {2, 0, 0, 227};
    OA_CHECK(vision::decode_row_rle(stream, 1, 1, 0, indices));
    OA_CHECK(indices.size() == 1U && indices[0] == 227);
    OA_CHECK(!vision::decode_row_rle(std::span<const uint8_t>(stream.data(), 1), 1, 1, 0, indices));

    std::vector<uint8_t> pixels{1, 2, 3};
    OA_CHECK(
        vision::prepare_original("trial", "one", 0, pixels, false) == vision::FramePaint::leave
    );
    OA_CHECK(pixels[0] == 1);
    OA_CHECK(
        vision::prepare_original("trial", "one", 0, pixels, true) == vision::FramePaint::ready
    );
    pixels[0] = 9;
    OA_CHECK(
        vision::prepare_original("trial", "one", 0, pixels, true) == vision::FramePaint::ready
    );
    OA_CHECK(pixels[0] == 1);
    OA_CHECK(
        vision::prepare_original("trial", "one", 0, pixels, false) == vision::FramePaint::restored
    );
    OA_CHECK(!vision::holds_originals());

    std::vector<uint8_t> palette(vision::kPaletteColours * 4U, 0);
    const vision::TeamMaps off = vision::team_maps(vision::Mode::off, palette, {});
    OA_CHECK(off[0][227] == 227);
    const vision::TeamMaps short_palette = vision::team_maps(vision::Mode::protanopia, {}, {});
    OA_CHECK(short_palette[0][227] == 227);
    const std::array<int, vision::kPlayerCount> no_bodies{};
    const vision::TeamMaps blank_blips = vision::blip_maps(vision::Mode::protanopia, {}, no_bodies);
    OA_CHECK(blank_blips[0][227] == 227);
    OA_CHECK(vision::match_index({}, vision::Rgb{1, 2, 3}) == 0);
}

std::vector<uint8_t> shade_list(std::initializer_list<int> values) {
    std::vector<uint8_t> shades;
    int previous = -1;
    bool run = false;
    for (const int value : values) {
        if (run) {
            for (int colour = previous + 1; colour <= value; ++colour)
                shades.push_back(static_cast<uint8_t>(colour));
            run = false;
        } else if (value < 0) {
            run = true;
        } else {
            shades.push_back(static_cast<uint8_t>(value));
            previous = value;
        }
    }
    return shades;
}

// Every index but the body maps to itself. The caller checks that the body
// maps to the anchor, which may already be the body.
bool blip_map_changes_body(const vision::TeamMaps& maps, int player, int body) {
    const vision::IndexMap& map = maps[static_cast<std::size_t>(player)];
    for (int index = 0; index < vision::kPaletteColours; ++index) {
        if (index != body && map[static_cast<std::size_t>(index)] != static_cast<uint8_t>(index))
            return false;
    }
    return true;
}

bool same_name(std::string_view stored, std::string_view wanted) {
    if (stored.size() != wanted.size())
        return false;
    for (std::size_t at = 0; at < stored.size(); ++at) {
        const char left = stored[at] >= 'A' && stored[at] <= 'Z'
                              ? static_cast<char>(stored[at] - 'A' + 'a')
                              : stored[at];
        const char right = wanted[at] >= 'A' && wanted[at] <= 'Z'
                               ? static_cast<char>(wanted[at] - 'A' + 'a')
                               : wanted[at];
        if (left != right)
            return false;
    }
    return true;
}

void check_installed_game() {
    const oa::AssetStore assets =
        oa::test::require_game_assets("the colour-vision anchors and team ramps");
    const auto palette_file = oa::test::read_game_file(assets, "palettes/palette.pal");
    OA_CHECK(palette_file.size() == static_cast<std::size_t>(vision::kPaletteColours) * 4U);
    if (palette_file.size() < static_cast<std::size_t>(vision::kPaletteColours) * 4U)
        return;
    const std::span<const uint8_t> palette(palette_file.data(), palette_file.size());
    // Each dichromat anchor is at L* 15 or lighter, and the least gap from the
    // terrain under both eyes is as large as the palette then allows. The floor
    // is the gap these anchors keep. Achromatopsia's four levels cannot sit
    // much further than 1.6 from a terrain grey, and the lightness floor does
    // not apply to them.
    const std::span<const int> terrain = vision::terrain_indices();
    OA_CHECK(terrain_gap(vision::Mode::protanopia, vision::simulate, palette, terrain) >= 5.58);
    OA_CHECK(
        terrain_gap(vision::Mode::protanopia, vision::simulate_vienot, palette, terrain) >= 5.58
    );
    OA_CHECK(terrain_gap(vision::Mode::deuteranopia, vision::simulate, palette, terrain) >= 4.80);
    OA_CHECK(
        terrain_gap(vision::Mode::deuteranopia, vision::simulate_vienot, palette, terrain) >= 4.80
    );
    OA_CHECK(terrain_gap(vision::Mode::tritanopia, vision::simulate, palette, terrain) >= 4.65);
    OA_CHECK(
        terrain_gap(vision::Mode::tritanopia, vision::simulate_brettel, palette, terrain) >= 4.65
    );
    OA_CHECK(terrain_gap(vision::Mode::achromatopsia, vision::simulate, palette, terrain) >= 1.5);
    const vision::Mode dichromacies[] = {
        vision::Mode::protanopia, vision::Mode::deuteranopia, vision::Mode::tritanopia
    };
    for (const vision::Mode mode : dichromacies) {
        for (int player = 0; player < vision::kPlayerCount; ++player) {
            const vision::Lab seen = vision::lab_from_srgb(vision::anchor_srgb(mode, player));
            OA_CHECK(seen.l >= 15.0);
        }
    }
    const vision::Mode modes[] = {
        vision::Mode::protanopia,
        vision::Mode::deuteranopia,
        vision::Mode::tritanopia,
        vision::Mode::achromatopsia
    };
    for (const vision::Mode mode : modes) {
        for (int player = 0; player < vision::kPlayerCount; ++player) {
            const int matched = vision::match_index(palette, vision::anchor_srgb(mode, player));
            OA_CHECK(matched == vision::stock_anchor_index(mode, player));
        }
    }

    const auto logos_file = oa::test::read_game_file(assets, "textures/logos.gaf");
    auto logos = oa::formats::gaf::parse(logos_file);
    OA_CHECK(logos.ok());
    if (!logos.ok())
        return;
    std::vector<vision::RawSequence> sequences;
    for (const oa::formats::gaf::Sequence& sequence : logos.archive->sequences) {
        if (sequence.frames.size() != static_cast<std::size_t>(vision::kPlayerCount))
            continue;
        vision::RawSequence raw;
        bool any = false;
        for (int player = 0; player < vision::kPlayerCount; ++player) {
            const oa::formats::gaf::Frame& frame =
                sequence.frames[static_cast<std::size_t>(player)];
            const auto count = static_cast<std::size_t>(frame.width) * frame.height;
            if (frame.compressed || !frame.layers.empty() || frame.width == 0 ||
                frame.height == 0 || frame.pixels.size() < count)
                continue;
            raw.frames[static_cast<std::size_t>(player)] = {
                frame.pixels.data(), count, static_cast<int>(frame.transparency_index)
            };
            any = true;
        }
        if (any)
            sequences.push_back(raw);
    }
    const auto shades = vision::team_shades(sequences);
    const std::vector<uint8_t> expected[] = {
        shade_list({98, -1, 111, 227}),
        shade_list({75, 188, 189, 198, -1, 207, 212, 249}),
        shade_list({80, -1, 89, 91, -1, 93, 255}),
        shade_list({162, -1, 173, 234, -1, 239}),
        shade_list({106, -1, 111, 230, 231, 245}),
        shade_list({141, 142, 147, -1, 157, 218, -1, 223}),
        shade_list({192, -1, 203, 208, 209}),
        shade_list({91, -1, 95, 245}),
        shade_list({128, -1, 141}),
        shade_list({64, -1, 75})
    };
    for (int player = 0; player < vision::kPlayerCount; ++player) {
        if (shades[static_cast<std::size_t>(player)] != expected[player]) {
            std::fprintf(stderr, "shades %d:", player);
            for (const uint8_t colour : shades[static_cast<std::size_t>(player)])
                std::fprintf(stderr, " %u", colour);
            std::fprintf(stderr, "\n");
        }
        OA_CHECK(shades[static_cast<std::size_t>(player)] == expected[player]);
    }

    const vision::Mode ramp_modes[] = {
        vision::Mode::protanopia,
        vision::Mode::deuteranopia,
        vision::Mode::tritanopia,
        vision::Mode::achromatopsia
    };
    // The game's own dot indices. Each map sends that index to the anchor.
    const int old_dots[vision::kPlayerCount] = {227, 212, 80, 235, 108, 219, 208, 93, 130, 67};
    for (const vision::Mode mode : ramp_modes) {
        const vision::TeamMaps maps = vision::team_maps(mode, palette, sequences);
        for (int player = 0; player < vision::kPlayerCount; ++player) {
            const auto dot = static_cast<uint8_t>(old_dots[player]);
            const auto anchor = static_cast<uint8_t>(vision::stock_anchor_index(mode, player));
            OA_CHECK(maps[static_cast<std::size_t>(player)][dot] == anchor);
        }
    }
    const vision::TeamMaps identity = vision::team_maps(vision::Mode::off, palette, sequences);
    OA_CHECK(identity[0][227] == 227);

    const auto effects_file = oa::test::read_game_file(assets, "anims/fx.gaf");
    auto effects = oa::formats::gaf::parse(effects_file);
    OA_CHECK(effects.ok());
    if (!effects.ok())
        return;
    const int expected_bodies[vision::kPlayerCount] = {
        227, 212, 255, 235, 108, 218, 208, 245, 131, 67
    };
    for (const oa::formats::gaf::Sequence& sequence : effects.archive->sequences) {
        if (sequence.frames.size() != static_cast<std::size_t>(vision::kPlayerCount))
            continue;
        if (!same_name(sequence.name, "radlogo") && !same_name(sequence.name, "nuclogo"))
            continue;
        std::array<vision::RawFrame, vision::kPlayerCount> frames{};
        for (int player = 0; player < vision::kPlayerCount; ++player) {
            const oa::formats::gaf::Frame& frame =
                sequence.frames[static_cast<std::size_t>(player)];
            const auto count = static_cast<std::size_t>(frame.width) * frame.height;
            if (frame.pixels.size() < count)
                continue;
            frames[static_cast<std::size_t>(player)] = {
                frame.pixels.data(), count, static_cast<int>(frame.transparency_index)
            };
        }
        const auto bodies = vision::blip_bodies(frames, palette);
        for (int player = 0; player < vision::kPlayerCount; ++player) {
            if (bodies[static_cast<std::size_t>(player)] != expected_bodies[player])
                std::fprintf(
                    stderr,
                    "blip %s player %d body %d\n",
                    sequence.name.c_str(),
                    player,
                    bodies[static_cast<std::size_t>(player)]
                );
            OA_CHECK(bodies[static_cast<std::size_t>(player)] == expected_bodies[player]);
        }
    }
    for (const vision::Mode mode : ramp_modes) {
        std::array<int, vision::kPlayerCount> bodies{};
        for (int player = 0; player < vision::kPlayerCount; ++player)
            bodies[static_cast<std::size_t>(player)] = expected_bodies[player];
        const vision::TeamMaps maps = vision::blip_maps(mode, palette, bodies);
        for (int player = 0; player < vision::kPlayerCount; ++player) {
            OA_CHECK(blip_map_changes_body(maps, player, expected_bodies[player]));
            const auto anchor = static_cast<uint8_t>(vision::stock_anchor_index(mode, player));
            OA_CHECK(
                maps[static_cast<std::size_t>(player)]
                    [static_cast<std::size_t>(expected_bodies[player])] == anchor
            );
            OA_CHECK(maps[static_cast<std::size_t>(player)][89] == 89);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    const RestoreMode restore;
    const vision::Lab sharma_left{50.0, 2.6772, -79.7751};
    const vision::Lab sharma_right{50.0, 0.0, -82.7485};
    OA_CHECK(std::round(vision::ciede2000(sharma_left, sharma_right) * 10000.0) == 20425.0);
    check_seen(vision::Mode::protanopia, vision::simulate, true, 201, 0, 4);
    check_seen(vision::Mode::protanopia, vision::simulate_vienot, true, 209, 3, 9);
    check_seen(vision::Mode::protanopia, unchanged_lab, false, 186, 0, 4);
    check_seen(vision::Mode::deuteranopia, vision::simulate, true, 211, 1, 3);
    check_seen(vision::Mode::deuteranopia, vision::simulate_vienot, true, 208, 1, 3);
    check_seen(vision::Mode::deuteranopia, unchanged_lab, false, 195, 5, 8);
    check_seen(vision::Mode::tritanopia, vision::simulate, true, 206, 2, 9);
    check_seen(vision::Mode::tritanopia, vision::simulate_brettel, true, 201, 0, 8);
    check_seen(vision::Mode::tritanopia, unchanged_lab, false, 209, 4, 7);
    check_seen(vision::Mode::achromatopsia, vision::simulate, true, 202, 2, 3);
    check_seen(vision::Mode::achromatopsia, unchanged_lab, false, 205, 2, 3);
    check_terrain_membership();
    check_semantics(vision::Mode::protanopia, 334, 334, 423, 230, 237);
    check_semantics(vision::Mode::deuteranopia, 372, 375, 431, 231, 218);
    check_semantics(vision::Mode::tritanopia, 421, 408, 388, 240, 221);
    check_semantics(vision::Mode::achromatopsia, 236, 240, 184, 116, 55);
    check_preferences();
    check_rows();
    if (oa::test::game_data_requested(argc, argv))
        check_installed_game();
    return oa::test::check_exit_status();
}

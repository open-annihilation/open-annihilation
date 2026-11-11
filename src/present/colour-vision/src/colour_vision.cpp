// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Anchor colours, the distance check, and the team-ramp tables.
#include "oa/present/colour_vision/colour_vision.hpp"

#include "oa/formats/gaf.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::present::colour_vision {
namespace {

constexpr std::size_t kPaletteBytes = static_cast<std::size_t>(kPaletteColours) * 4U;
constexpr double kPi = 3.14159265358979323846;
constexpr double kNearGrey = 0.05;
constexpr double kHueStep = 0.07;
constexpr int kSharedFrameCount = 3;

struct Anchor {
    int index{};
    Rgb colour{};
};

constexpr Rgb rgb(unsigned red, unsigned green, unsigned blue) noexcept {
    return Rgb{static_cast<uint8_t>(red), static_cast<uint8_t>(green), static_cast<uint8_t>(blue)};
}

constexpr Anchor anchor_of(int index, unsigned red, unsigned green, unsigned blue) noexcept {
    return Anchor{index, rgb(red, green, blue)};
}

// Stock palette indices and sRGB, one row per player. Achromatopsia repeats
// four lightness levels. Each dichromat anchor is the first entry of its
// sRGB, has a team ramp of at least three shades, and is at L* 15 or lighter.
// None is a terrain index below, and none is 171. Every pair is at least 15
// apart under normal vision and at least 20 under Machado and under Viénot
// (protanopia, deuteranopia) or Brettel (tritanopia). Within those rules the
// least gap from the terrain, under both eyes, is as large as the palette
// allows. Each set is given to the players in the order that changes their
// colours least from today's, summed under normal vision.
//
// Protanopia keeps 5.59 (Machado) and 5.75 (Viénot). Its least pairs are
// 20.07 (Machado, players 1 and 5), 20.93 (Viénot, players 4 and 10) and
// 18.59 under normal vision (players 1 and 5). Deuteranopia keeps 4.82 and
// 4.80. Its least pairs are 21.11 and 20.80 (players 2 and 4) and 19.52
// (players 6 and 9). Tritanopia keeps 5.42 (Machado) and 4.65 (Brettel). Its
// least pairs are 20.59 (players 3 and 10), 20.15 (players 1 and 9) and 20.94
// (players 5 and 8). Achromatopsia's four levels stay, including L* 9: four
// levels 20 apart under luminance cannot sit much further than 1.6 from a
// terrain grey, so the lightness floor does not apply to them.
constexpr Anchor kProtanopia[kPlayerCount] = {
    anchor_of(228, 0x0B, 0x2B, 0xBB),
    anchor_of(85, 0xAB, 0xAB, 0xAB),
    anchor_of(64, 0xFB, 0xFB, 0xD7),
    anchor_of(54, 0x5F, 0x6F, 0x53),
    anchor_of(123, 0x1B, 0x27, 0x3F),
    anchor_of(117, 0x53, 0x77, 0x8B),
    anchor_of(208, 0xFF, 0xFF, 0x00),
    anchor_of(186, 0x3B, 0x2B, 0x07),
    anchor_of(225, 0x87, 0x9F, 0xF7),
    anchor_of(179, 0xB3, 0x97, 0x3F)
};
constexpr Anchor kDeuteranopia[kPlayerCount] = {
    anchor_of(117, 0x53, 0x77, 0x8B),
    anchor_of(199, 0xE7, 0x7B, 0x0F),
    anchor_of(64, 0xFB, 0xFB, 0xD7),
    anchor_of(236, 0x1B, 0x7F, 0x0B),
    anchor_of(229, 0x07, 0x17, 0x8F),
    anchor_of(225, 0x87, 0x9F, 0xF7),
    anchor_of(208, 0xFF, 0xFF, 0x00),
    anchor_of(215, 0x57, 0x00, 0x00),
    anchor_of(113, 0xBF, 0xDB, 0xE7),
    anchor_of(52, 0x7F, 0x9F, 0x83)
};
constexpr Anchor kTritanopia[kPlayerCount] = {
    anchor_of(100, 0x7B, 0x97, 0xFF),
    anchor_of(213, 0xAB, 0x17, 0x00),
    anchor_of(80, 0xFB, 0xFB, 0xFB),
    anchor_of(167, 0x2F, 0x77, 0x1B),
    anchor_of(187, 0x37, 0x23, 0x00),
    anchor_of(219, 0x7F, 0x57, 0x9F),
    anchor_of(199, 0xE7, 0x7B, 0x0F),
    anchor_of(172, 0x17, 0x33, 0x00),
    anchor_of(32, 0x73, 0xFF, 0xDF),
    anchor_of(67, 0xAB, 0xAB, 0x83)
};
constexpr Anchor kAchromatopsiaLevel[4] = {
    anchor_of(29, 0x2F, 0x0F, 0x17),
    anchor_of(55, 0x5F, 0x63, 0x47),
    anchor_of(164, 0x4B, 0xAB, 0x2B),
    anchor_of(232, 0x7B, 0xFF, 0x77)
};

// The game's own ten dot indices. A team map sends each one to that colour's anchor.
constexpr int kStockDotIndex[kPlayerCount] = {227, 212, 80, 235, 108, 219, 208, 93, 130, 67};

// Those indices in the stock palette, as sRGB. The Settings preview draws
// them while the mode is off. `anchor_srgb` has no colour for off.
constexpr Rgb kStockPlayerRgb[kPlayerCount] = {
    {23, 71, 231},
    {211, 43, 0},
    {251, 251, 251},
    {27, 159, 19},
    {7, 31, 123},
    {127, 87, 159},
    {255, 255, 0},
    {43, 43, 43},
    {155, 203, 223},
    {171, 171, 131},
};

// The 32 palette indices the stock 3.1c maps draw most often.
//
// Each of the 275 maps the game ships (TNT version 0x2000) contributes every
// tile it places. A tile's 1024 indices are counted once for each map cell
// that uses the tile, so an index scores the pixels the terrain draws. A tie
// keeps the lower index. The count read the GOG-flattened 3.1c install, whose
// 1,118,476 tiles are the set the maps directory documents. 553,094 of them
// are distinct. Counting each distinct tile once names a different 32, and
// that list is not this one.
// Index 171, a grass green, is 124th by pixels drawn, so it is not here.
constexpr int kTerrainIndices[] = {227, 137, 104, 103, 168, 175, 128, 58,  24,  47,  139,
                                   112, 90,  138, 57,  72,  91,  59,  226, 182, 135, 224,
                                   41,  56,  94,  89,  93,  25,  43,  92,  118, 126};

const Anchor* anchors_of(Mode mode) noexcept {
    switch (mode) {
    case Mode::protanopia:
        return kProtanopia;
    case Mode::deuteranopia:
        return kDeuteranopia;
    case Mode::tritanopia:
        return kTritanopia;
    case Mode::achromatopsia:
    case Mode::off:
        break;
    }
    return nullptr;
}

SemanticColours semantics_of(
    Rgb good,
    Rgb mid,
    Rgb bad,
    Rgb arm,
    Rgb core,
    Rgb mixed,
    Rgb produced,
    Rgb consumed,
    Rgb system,
    Rgb news,
    Rgb chat_arm,
    Rgb chat_core,
    Rgb ok,
    Rgb caution,
    Rgb danger,
    Rgb information
) noexcept {
    SemanticColours colours;
    colours.health_good = good;
    colours.health_mid = mid;
    colours.health_bad = bad;
    colours.health_trough = rgb(0, 0, 0);
    colours.war_arm = arm;
    colours.war_core = core;
    colours.war_mixed = mixed;
    colours.war_space = rgb(0, 0, 0);
    colours.resource_produced = produced;
    colours.resource_consumed = consumed;
    colours.resource_text = rgb(0xFF, 0xFF, 0xFF);
    colours.chat_system = system;
    colours.chat_newsbot = news;
    colours.chat_arm = chat_arm;
    colours.chat_core = chat_core;
    colours.chat_plain = rgb(0xC3, 0xC3, 0x9B);
    colours.kit_ok = ok;
    colours.kit_caution = caution;
    colours.kit_danger = danger;
    colours.kit_information = information;
    colours.kit_panel = rgb(0x1D, 0x21, 0x19);
    return colours;
}

struct Hs {
    double hue{};
    double saturation{};
};

struct Presentation {
    Mode mode = Mode::off;
    bool shape_preference = false;
    std::array<uint8_t, kPlayerCount> dots{};
    uint8_t resource_produced = 0;
    uint8_t resource_consumed = 0;
    BlipPins pins{};
    TeamMaps maps{};
    std::map<std::string, std::vector<uint8_t>> originals{};

    Presentation() noexcept {
        for (auto& pin : pins)
            pin = {-1, -1};
        for (auto& map : maps)
            for (int colour = 0; colour < kPaletteColours; ++colour)
                map[static_cast<std::size_t>(colour)] = static_cast<uint8_t>(colour);
    }
};

Presentation& presentation() noexcept {
    static Presentation state;
    return state;
}

double linear_channel(double channel) noexcept {
    const double encoded = channel / 255.0;
    if (encoded <= 0.04045)
        return encoded / 12.92;
    return std::pow((encoded + 0.055) / 1.055, 2.4);
}

double lab_f(double channel) noexcept {
    constexpr double epsilon = 216.0 / 24389.0;
    constexpr double kappa = 24389.0 / 27.0;
    if (channel > epsilon)
        return std::cbrt(channel);
    return (kappa * channel + 16.0) / 116.0;
}

Lab lab_linear(double red, double green, double blue) noexcept {
    constexpr double white_x = 0.95047;
    constexpr double white_y = 1.0;
    constexpr double white_z = 1.08883;
    const double x = 0.4124564 * red + 0.3575761 * green + 0.1804375 * blue;
    const double y = 0.2126729 * red + 0.7151522 * green + 0.0721750 * blue;
    const double z = 0.0193339 * red + 0.1191920 * green + 0.9503041 * blue;
    const double fx = lab_f(x / white_x);
    const double fy = lab_f(y / white_y);
    const double fz = lab_f(z / white_z);
    return Lab{116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

double clamp_unit(double channel) noexcept {
    if (channel < 0.0)
        return 0.0;
    if (channel > 1.0)
        return 1.0;
    return channel;
}

double radians(double degrees) noexcept {
    return degrees * (kPi / 180.0);
}

double hue_degrees(double a, double b, double chroma) noexcept {
    if (chroma <= 0.0)
        return 0.0;
    double hue = std::atan2(b, a) * (180.0 / kPi);
    if (hue < 0.0)
        hue += 360.0;
    return hue;
}

double
hue_difference(double first, double second, double chroma_first, double chroma_second) noexcept {
    if (chroma_first * chroma_second == 0.0)
        return 0.0;
    double difference = second - first;
    if (difference > 180.0)
        difference -= 360.0;
    else if (difference < -180.0)
        difference += 360.0;
    return difference;
}

double hue_mean(double first, double second, double chroma_first, double chroma_second) noexcept {
    if (chroma_first * chroma_second == 0.0)
        return first + second;
    if (std::abs(first - second) <= 180.0)
        return (first + second) / 2.0;
    if (first + second < 360.0)
        return (first + second + 360.0) / 2.0;
    return (first + second - 360.0) / 2.0;
}

bool palette_ready(std::span<const uint8_t> palette) noexcept {
    return palette.size() >= kPaletteBytes;
}

Rgb palette_colour(std::span<const uint8_t> palette, int index) noexcept {
    const auto at = static_cast<std::size_t>(index) * 4U;
    return Rgb{palette[at], palette[at + 1], palette[at + 2]};
}

int channel_distance(Rgb left, Rgb right) noexcept {
    return std::abs(static_cast<int>(left.red) - static_cast<int>(right.red)) +
           std::abs(static_cast<int>(left.green) - static_cast<int>(right.green)) +
           std::abs(static_cast<int>(left.blue) - static_cast<int>(right.blue));
}

Hs hue_of(Rgb colour) noexcept {
    const double red = static_cast<double>(colour.red) / 255.0;
    const double green = static_cast<double>(colour.green) / 255.0;
    const double blue = static_cast<double>(colour.blue) / 255.0;
    const double upper = std::max(red, std::max(green, blue));
    const double lower = std::min(red, std::min(green, blue));
    Hs result;
    result.saturation = upper == 0.0 ? 0.0 : (upper - lower) / upper;
    const double span = upper - lower;
    if (span == 0.0)
        return result;
    double sector = 0.0;
    if (upper == red)
        sector = (green - blue) / span;
    else if (upper == green)
        sector = (blue - red) / span + 2.0;
    else
        sector = (red - green) / span + 4.0;
    if (sector < 0.0)
        sector += 6.0;
    result.hue = sector / 6.0;
    return result;
}

bool ramp_continues(Hs left, Hs right) noexcept {
    if (left.saturation < kNearGrey && right.saturation < kNearGrey)
        return true;
    double distance = std::abs(left.hue - right.hue);
    if (distance > 0.5)
        distance = 1.0 - distance;
    return distance <= kHueStep;
}

void insert_shade(std::vector<uint8_t>& shades, uint8_t index) {
    const auto at = std::lower_bound(shades.begin(), shades.end(), index);
    if (at == shades.end() || *at != index)
        shades.insert(at, index);
}

void add_sequence_shades(
    const RawSequence& sequence, std::array<std::vector<uint8_t>, kPlayerCount>& shades
) {
    std::array<int, kPaletteColours> frames_with{};
    std::array<std::array<bool, kPaletteColours>, kPlayerCount> in_frame{};
    for (int player = 0; player < kPlayerCount; ++player) {
        const RawFrame& frame = sequence.frames[static_cast<std::size_t>(player)];
        if (frame.indices == nullptr || frame.count == 0)
            continue;
        std::array<bool, kPaletteColours> seen{};
        for (std::size_t pixel = 0; pixel < frame.count; ++pixel) {
            const int index = frame.indices[pixel];
            if (index == frame.key)
                continue;
            seen[static_cast<std::size_t>(index)] = true;
        }
        for (int index = 0; index < kPaletteColours; ++index) {
            if (!seen[static_cast<std::size_t>(index)])
                continue;
            in_frame[static_cast<std::size_t>(player)][static_cast<std::size_t>(index)] = true;
            ++frames_with[static_cast<std::size_t>(index)];
        }
    }
    for (int player = 0; player < kPlayerCount; ++player) {
        for (int index = 0; index < kPaletteColours; ++index) {
            if (!in_frame[static_cast<std::size_t>(player)][static_cast<std::size_t>(index)])
                continue;
            if (frames_with[static_cast<std::size_t>(index)] >= kSharedFrameCount)
                continue;
            insert_shade(shades[static_cast<std::size_t>(player)], static_cast<uint8_t>(index));
        }
    }
}

IndexMap identity_map() noexcept {
    IndexMap map{};
    for (int index = 0; index < kPaletteColours; ++index)
        map[static_cast<std::size_t>(index)] = static_cast<uint8_t>(index);
    return map;
}

bool keeps_key(int key, uint8_t index) noexcept {
    return key >= 0 && static_cast<int>(index) == key;
}

// Walks a row-compressed frame. `visit` sees each colour byte. Returns the
// bytes consumed, or 0 when a row runs short.
template <typename Bytes, typename Visit>
std::size_t walk_row_rle(Bytes& stream, int width, int height, Visit&& visit) {
    if (width <= 0 || height <= 0)
        return 0;
    std::size_t at = 0;
    for (int row = 0; row < height; ++row) {
        if (at > stream.size() || stream.size() - at < 2)
            return 0;
        const std::size_t length =
            static_cast<std::size_t>(stream[at]) | (static_cast<std::size_t>(stream[at + 1]) << 8);
        at += 2;
        if (at > stream.size() || stream.size() - at < length)
            return 0;
        const std::size_t end = at + length;
        int column = 0;
        while (length != 0 && column < width) {
            if (at >= end)
                return 0;
            const auto command = static_cast<unsigned>(stream[at]);
            ++at;
            if ((command & 0x01U) != 0) {
                column += static_cast<int>(command >> 1);
                continue;
            }
            const int count = std::min(static_cast<int>(command >> 2) + 1, width - column);
            if ((command & 0x02U) != 0) {
                if (at >= end)
                    return 0;
                visit(stream[at]);
                ++at;
            } else {
                if (end - at < static_cast<std::size_t>(count))
                    return 0;
                for (int pixel = 0; pixel < count; ++pixel)
                    visit(stream[at + static_cast<std::size_t>(pixel)]);
                at += static_cast<std::size_t>(count);
            }
            column += count;
        }
        at = end;
    }
    return at;
}

std::string frame_key(std::string_view role, std::string_view sequence, int frame) {
    std::string key;
    key.reserve(role.size() + sequence.size() + 8);
    key.append(role);
    key.push_back('\n');
    key.append(sequence);
    key.push_back('\n');
    key.append(std::to_string(frame));
    return key;
}

bool role_holds(std::string_view role) {
    const std::string prefix = std::string(role) + "\n";
    for (const auto& [key, bytes] : presentation().originals) {
        (void)bytes;
        if (key.starts_with(prefix))
            return true;
    }
    return false;
}

bool readable_team_frame(const formats::gaf::Frame& frame) noexcept {
    return !frame.compressed && frame.layers.empty() && frame.width != 0 && frame.height != 0 &&
           frame.pixels.size() >= static_cast<std::size_t>(frame.width) * frame.height;
}

} // namespace

Mode parse_mode(std::string_view text) noexcept {
    if (text == "protanopia")
        return Mode::protanopia;
    if (text == "deuteranopia")
        return Mode::deuteranopia;
    if (text == "tritanopia")
        return Mode::tritanopia;
    if (text == "achromatopsia")
        return Mode::achromatopsia;
    return Mode::off;
}

std::string_view mode_name(Mode mode) noexcept {
    switch (mode) {
    case Mode::protanopia:
        return "protanopia";
    case Mode::deuteranopia:
        return "deuteranopia";
    case Mode::tritanopia:
        return "tritanopia";
    case Mode::achromatopsia:
        return "achromatopsia";
    case Mode::off:
        break;
    }
    return "off";
}

bool shape_cues_preference(std::string_view text) noexcept {
    long long value = 0;
    const char* const begin = text.data();
    const char* const end = begin + text.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, value);
    return parsed.ec == std::errc{} && parsed.ptr == end && value > 0;
}

bool shape_cues_in_effect(Mode mode, bool preference_on) noexcept {
    return preference_on || mode == Mode::achromatopsia;
}

Mode active_mode() noexcept {
    return presentation().mode;
}

bool shape_cues_active() noexcept {
    const Presentation& state = presentation();
    return shape_cues_in_effect(state.mode, state.shape_preference);
}

void set_active(Mode mode, bool shape_cues_on) noexcept {
    Presentation& state = presentation();
    state.mode = mode;
    state.shape_preference = shape_cues_on;
    for (int player = 0; player < kPlayerCount; ++player) {
        const int index = mode == Mode::off ? 0 : stock_anchor_index(mode, player);
        state.dots[static_cast<std::size_t>(player)] = static_cast<uint8_t>(index);
    }
}

Rgb anchor_srgb(Mode mode, int player) noexcept {
    if (player < 0 || player >= kPlayerCount || mode == Mode::off)
        return {};
    if (mode == Mode::achromatopsia)
        return kAchromatopsiaLevel[player % 4].colour;
    return anchors_of(mode)[player].colour;
}

Rgb preview_player_srgb(Mode mode, int player) noexcept {
    if (player < 0 || player >= kPlayerCount)
        return {};
    if (mode == Mode::off)
        return kStockPlayerRgb[player];
    return anchor_srgb(mode, player);
}

int stock_anchor_index(Mode mode, int player) noexcept {
    if (player < 0 || player >= kPlayerCount || mode == Mode::off)
        return 0;
    if (mode == Mode::achromatopsia)
        return kAchromatopsiaLevel[player % 4].index;
    return anchors_of(mode)[player].index;
}

SemanticColours semantic_colours(Mode mode) noexcept {
    switch (mode) {
    case Mode::protanopia:
    case Mode::deuteranopia:
        return semantics_of(
            rgb(0x7B, 0x97, 0xFF),
            rgb(0xFB, 0xFB, 0xD7),
            rgb(0xD3, 0x2B, 0x00),
            rgb(0x43, 0x6F, 0xEF),
            rgb(0xD3, 0x2B, 0x00),
            rgb(0xDB, 0xDB, 0xDB),
            rgb(0x43, 0x6F, 0xEF),
            rgb(0xD3, 0x2B, 0x00),
            rgb(0xEB, 0xF3, 0xFF),
            rgb(0xFF, 0xFF, 0x00),
            rgb(0x43, 0x6F, 0xEF),
            rgb(0xD3, 0x2B, 0x00),
            rgb(0x5A, 0xA9, 0xF0),
            rgb(0xFF, 0xE6, 0x80),
            rgb(0xF4, 0x51, 0x1E),
            rgb(0xD6, 0xD6, 0xD6)
        );
    case Mode::tritanopia:
        return semantics_of(
            rgb(0x43, 0x6F, 0xEF),
            rgb(0xFB, 0xF3, 0x83),
            rgb(0xD3, 0x2B, 0x00),
            rgb(0x53, 0x6B, 0xEF),
            rgb(0xFF, 0x47, 0x00),
            rgb(0xFB, 0xFB, 0xD7),
            rgb(0x43, 0x6F, 0xEF),
            rgb(0xFF, 0x47, 0x00),
            rgb(0xAF, 0xCF, 0xFF),
            rgb(0xE7, 0x7B, 0x0F),
            rgb(0x17, 0x47, 0xE7),
            rgb(0xAB, 0x17, 0x00),
            rgb(0x5A, 0xA9, 0xF0),
            rgb(0xE0, 0xB0, 0x4F),
            rgb(0xF4, 0x51, 0x1E),
            rgb(0xD6, 0xD6, 0xD6)
        );
    case Mode::achromatopsia:
        return semantics_of(
            rgb(0xFB, 0xFB, 0xFB),
            rgb(0x9B, 0x9B, 0x9B),
            rgb(0x5B, 0x5B, 0x5B),
            rgb(0x17, 0x47, 0xE7),
            rgb(0xE7, 0x7B, 0x0F),
            rgb(0xFB, 0xFB, 0xD7),
            rgb(0x7B, 0x97, 0xFF),
            rgb(0x6B, 0x6B, 0x6B),
            rgb(0xFB, 0xFB, 0xFB),
            rgb(0xE7, 0x7B, 0x0F),
            rgb(0x53, 0x6B, 0xEF),
            rgb(0xAB, 0x17, 0x00),
            rgb(0xF2, 0xF2, 0xF2),
            rgb(0xBD, 0xBD, 0xBD),
            rgb(0x8A, 0x8A, 0x8A),
            rgb(0xD9, 0xD9, 0xD9)
        );
    case Mode::off:
        break;
    }
    return semantics_of(
        rgb(0x53, 0xDF, 0x4F),
        rgb(0xF7, 0xE3, 0x67),
        rgb(0xFF, 0x47, 0x00),
        rgb(0x07, 0x17, 0x8F),
        rgb(0x7F, 0x07, 0x00),
        rgb(0xC3, 0xC3, 0x9B),
        rgb(0x53, 0xDF, 0x4F),
        rgb(0xFF, 0x47, 0x00),
        rgb(0x53, 0xDF, 0x4F),
        rgb(0xE7, 0x7B, 0x0F),
        rgb(0x43, 0x6F, 0xEF),
        rgb(0xD3, 0x2B, 0x00),
        rgb(0x9C, 0xCC, 0x3C),
        rgb(0xE0, 0xB0, 0x4F),
        rgb(0xE8, 0x80, 0x6F),
        rgb(0x7D, 0xB3, 0xDE)
    );
}

int match_index(std::span<const uint8_t> palette, Rgb colour) noexcept {
    if (!palette_ready(palette))
        return 0;
    int best_index = 0;
    int best = 0x7fffffff;
    for (int index = 0; index < kPaletteColours; ++index) {
        const int distance = channel_distance(palette_colour(palette, index), colour);
        if (distance < best) {
            best = distance;
            best_index = index;
        }
    }
    return best_index;
}

void paint_mark_slots(
    std::span<uint8_t> slots, std::span<const uint8_t> palette, Mode mode
) noexcept {
    constexpr std::size_t kSlotCount = 15;
    constexpr std::size_t kGood = 10;
    constexpr std::size_t kBad = 12;
    constexpr std::size_t kMid = 14;
    constexpr std::size_t kRefused = 4;
    if (mode == Mode::off || slots.size() < kSlotCount || !palette_ready(palette))
        return;
    const SemanticColours colours = semantic_colours(mode);
    const auto nearest = [palette](Rgb colour) {
        return static_cast<uint8_t>(match_index(palette, colour));
    };
    slots[kGood] = nearest(colours.health_good);
    slots[kMid] = nearest(colours.health_mid);
    const uint8_t bad = nearest(colours.health_bad);
    slots[kBad] = bad;
    slots[kRefused] = bad;
}

void match_resource_figures(std::span<const uint8_t> palette) noexcept {
    const Mode mode = active_mode();
    if (mode == Mode::off || !palette_ready(palette))
        return;
    const SemanticColours colours = semantic_colours(mode);
    Presentation& state = presentation();
    state.resource_produced = static_cast<uint8_t>(match_index(palette, colours.resource_produced));
    state.resource_consumed = static_cast<uint8_t>(match_index(palette, colours.resource_consumed));
}

uint8_t resource_produced_index() noexcept {
    return presentation().resource_produced;
}

uint8_t resource_consumed_index() noexcept {
    return presentation().resource_consumed;
}

void match_player_dots(std::span<const uint8_t> palette) noexcept {
    const Mode mode = active_mode();
    if (mode == Mode::off || !palette_ready(palette))
        return;
    Presentation& state = presentation();
    for (int player = 0; player < kPlayerCount; ++player) {
        const int index = match_index(palette, anchor_srgb(mode, player));
        state.dots[static_cast<std::size_t>(player)] = static_cast<uint8_t>(index);
    }
}

uint8_t player_dot(int colour) noexcept {
    if (colour < 0 || colour >= kPlayerCount)
        colour = 0;
    return presentation().dots[static_cast<std::size_t>(colour)];
}

Lab lab_from_srgb(Rgb colour) noexcept {
    return lab_linear(
        linear_channel(colour.red), linear_channel(colour.green), linear_channel(colour.blue)
    );
}

Lab simulate(Mode mode, Rgb colour) noexcept {
    const double red = linear_channel(colour.red);
    const double green = linear_channel(colour.green);
    const double blue = linear_channel(colour.blue);
    if (mode == Mode::off)
        return lab_linear(red, green, blue);
    if (mode == Mode::achromatopsia) {
        const double luminance = 0.2126729 * red + 0.7151522 * green + 0.0721750 * blue;
        return lab_linear(luminance, luminance, luminance);
    }
    // Machado, Oliveira and Fernandes 2009, severity 1.0. Each channel is
    // held to 0..1 before it becomes CIELAB.
    static constexpr double protanopia[3][3] = {
        {0.152286, 1.052583, -0.204868},
        {0.114503, 0.786281, 0.099216},
        {-0.003882, -0.048116, 1.051998}
    };
    static constexpr double deuteranopia[3][3] = {
        {0.367322, 0.860646, -0.227968},
        {0.280085, 0.672501, 0.047413},
        {-0.011820, 0.042940, 0.968881}
    };
    static constexpr double tritanopia[3][3] = {
        {1.255528, -0.076749, -0.178779},
        {-0.078411, 0.930809, 0.147602},
        {0.004733, 0.691367, 0.303900}
    };
    const double (&matrix)[3][3] = mode == Mode::protanopia     ? protanopia
                                   : mode == Mode::deuteranopia ? deuteranopia
                                                                : tritanopia;
    const double source[3] = {red, green, blue};
    double mapped[3];
    for (int channel = 0; channel < 3; ++channel) {
        mapped[channel] = clamp_unit(
            matrix[channel][0] * source[0] + matrix[channel][1] * source[1] +
            matrix[channel][2] * source[2]
        );
    }
    return lab_linear(mapped[0], mapped[1], mapped[2]);
}

namespace {

Lab mapped_lab(const double matrix[3][3], double red, double green, double blue) noexcept {
    const double source[3] = {red, green, blue};
    double mapped[3];
    for (int channel = 0; channel < 3; ++channel) {
        mapped[channel] = clamp_unit(
            matrix[channel][0] * source[0] + matrix[channel][1] * source[1] +
            matrix[channel][2] * source[2]
        );
    }
    return lab_linear(mapped[0], mapped[1], mapped[2]);
}

} // namespace

Lab simulate_vienot(Mode mode, Rgb colour) noexcept {
    // Viénot, Brettel and Mollon 1999. Smith and Pokorny fundamentals through
    // sRGB, with the RGB white as the neutral. Channels are held to 0..1.
    static constexpr double protanopia[3][3] = {
        {0.11238, 0.88762, 0.00000}, {0.11238, 0.88762, -0.00000}, {0.00401, -0.00401, 1.00000}
    };
    static constexpr double deuteranopia[3][3] = {
        {0.29275, 0.70725, 0.00000}, {0.29275, 0.70725, -0.00000}, {-0.02234, 0.02234, 1.00000}
    };
    if (mode != Mode::protanopia && mode != Mode::deuteranopia)
        return simulate(mode, colour);
    const double (&matrix)[3][3] = mode == Mode::protanopia ? protanopia : deuteranopia;
    const double red = linear_channel(colour.red);
    const double green = linear_channel(colour.green);
    const double blue = linear_channel(colour.blue);
    return mapped_lab(matrix, red, green, blue);
}

Lab simulate_brettel(Mode mode, Rgb colour) noexcept {
    // Brettel, Viénot and Mollon 1997, tritanopia. Two planes, and the normal
    // that chooses between them. A non-negative dot product takes the first.
    static constexpr double plane_one[3][3] = {
        {1.01277, 0.13548, -0.14826}, {-0.01243, 0.86812, 0.14431}, {0.07589, 0.80500, 0.11911}
    };
    static constexpr double plane_two[3][3] = {
        {0.93678, 0.18979, -0.12657}, {0.06154, 0.81526, 0.12320}, {-0.37562, 1.12767, 0.24796}
    };
    if (mode != Mode::tritanopia)
        return simulate(mode, colour);
    const double red = linear_channel(colour.red);
    const double green = linear_channel(colour.green);
    const double blue = linear_channel(colour.blue);
    const double side = 0.03901 * red - 0.02788 * green - 0.01113 * blue;
    return mapped_lab(side >= 0.0 ? plane_one : plane_two, red, green, blue);
}

double ciede2000(Lab left, Lab right) noexcept {
    const double delta_l = right.l - left.l;
    const double mean_l = (left.l + right.l) / 2.0;
    const double chroma_left = std::sqrt(left.a * left.a + left.b * left.b);
    const double chroma_right = std::sqrt(right.a * right.a + right.b * right.b);
    const double mean_chroma = (chroma_left + chroma_right) / 2.0;
    const double mean_chroma_7 = std::pow(mean_chroma, 7.0);
    const double boost =
        0.5 * (1.0 - std::sqrt(mean_chroma_7 / (mean_chroma_7 + std::pow(25.0, 7.0))));
    const double a_left = left.a * (1.0 + boost);
    const double a_right = right.a * (1.0 + boost);
    const double prime_left = std::sqrt(a_left * a_left + left.b * left.b);
    const double prime_right = std::sqrt(a_right * a_right + right.b * right.b);
    const double mean_prime = (prime_left + prime_right) / 2.0;
    const double hue_left = hue_degrees(a_left, left.b, prime_left);
    const double hue_right = hue_degrees(a_right, right.b, prime_right);
    const double delta_hue = hue_difference(hue_left, hue_right, prime_left, prime_right);
    const double delta_h =
        2.0 * std::sqrt(prime_left * prime_right) * std::sin(radians(delta_hue / 2.0));
    const double mean_hue = hue_mean(hue_left, hue_right, prime_left, prime_right);
    const double turning = 1.0 - 0.17 * std::cos(radians(mean_hue - 30.0)) +
                           0.24 * std::cos(radians(2.0 * mean_hue)) +
                           0.32 * std::cos(radians(3.0 * mean_hue + 6.0)) -
                           0.20 * std::cos(radians(4.0 * mean_hue - 63.0));
    const double theta = 30.0 * std::exp(-std::pow((mean_hue - 275.0) / 25.0, 2.0));
    const double mean_prime_7 = std::pow(mean_prime, 7.0);
    const double chroma_scale =
        2.0 * std::sqrt(mean_prime_7 / (mean_prime_7 + std::pow(25.0, 7.0)));
    const double l_shift = mean_l - 50.0;
    const double scale_l = 1.0 + (0.015 * l_shift * l_shift) / std::sqrt(20.0 + l_shift * l_shift);
    const double scale_c = 1.0 + 0.045 * mean_prime;
    const double scale_h = 1.0 + 0.015 * mean_prime * turning;
    const double rotation = -std::sin(radians(2.0 * theta)) * chroma_scale;
    const double delta_c = prime_right - prime_left;
    const double term_l = delta_l / scale_l;
    const double term_c = delta_c / scale_c;
    const double term_h = delta_h / scale_h;
    return std::sqrt(
        term_l * term_l + term_c * term_c + term_h * term_h + rotation * term_c * term_h
    );
}

std::array<std::vector<uint8_t>, kPlayerCount> team_shades(std::span<const RawSequence> sequences) {
    std::array<std::vector<uint8_t>, kPlayerCount> shades;
    for (const RawSequence& sequence : sequences)
        add_sequence_shades(sequence, shades);
    return shades;
}

TeamMaps
team_maps(Mode mode, std::span<const uint8_t> palette, std::span<const RawSequence> sequences) {
    TeamMaps maps;
    for (IndexMap& map : maps)
        map = identity_map();
    if (mode == Mode::off || !palette_ready(palette))
        return maps;
    std::array<Hs, kPaletteColours> hue{};
    std::array<double, kPaletteColours> lightness{};
    for (int index = 0; index < kPaletteColours; ++index) {
        const Rgb colour = palette_colour(palette, index);
        hue[static_cast<std::size_t>(index)] = hue_of(colour);
        lightness[static_cast<std::size_t>(index)] = lab_from_srgb(colour).l;
    }
    const auto shades = team_shades(sequences);
    for (int player = 0; player < kPlayerCount; ++player) {
        const int anchor = match_index(palette, anchor_srgb(mode, player));
        const int row = anchor / 16 * 16;
        int left = anchor;
        while (left > row &&
               ramp_continues(
                   hue[static_cast<std::size_t>(left)], hue[static_cast<std::size_t>(left - 1)]
               ))
            --left;
        int right = anchor;
        while (right + 1 < row + 16 &&
               ramp_continues(
                   hue[static_cast<std::size_t>(right)], hue[static_cast<std::size_t>(right + 1)]
               ))
            ++right;
        IndexMap& map = maps[static_cast<std::size_t>(player)];
        const int old_dot = kStockDotIndex[player];
        const double shift = lightness[static_cast<std::size_t>(anchor)] -
                             lightness[static_cast<std::size_t>(old_dot)];
        map[static_cast<std::size_t>(old_dot)] = static_cast<uint8_t>(anchor);
        for (const uint8_t shade : shades[static_cast<std::size_t>(player)]) {
            if (static_cast<int>(shade) == old_dot)
                continue;
            const double target = lightness[shade] + shift;
            int destination = left;
            double best = std::abs(target - lightness[static_cast<std::size_t>(left)]);
            for (int index = left + 1; index <= right; ++index) {
                const double distance =
                    std::abs(target - lightness[static_cast<std::size_t>(index)]);
                if (distance < best) {
                    best = distance;
                    destination = index;
                }
            }
            map[shade] = static_cast<uint8_t>(destination);
        }
    }
    return maps;
}

TeamMaps blip_maps(
    Mode mode, std::span<const uint8_t> palette, const std::array<int, kPlayerCount>& bodies
) {
    TeamMaps maps;
    for (IndexMap& map : maps)
        map = identity_map();
    if (mode == Mode::off || !palette_ready(palette))
        return maps;
    for (int player = 0; player < kPlayerCount; ++player) {
        const int body = bodies[static_cast<std::size_t>(player)];
        if (body < 0 || body >= kPaletteColours)
            continue;
        const int anchor = match_index(palette, anchor_srgb(mode, player));
        maps[static_cast<std::size_t>(player)][static_cast<std::size_t>(body)] =
            static_cast<uint8_t>(anchor);
    }
    return maps;
}

std::span<const int> terrain_indices() noexcept {
    return kTerrainIndices;
}

void remap_indices(std::span<uint8_t> indices, const IndexMap& map, int key) noexcept {
    for (uint8_t& index : indices) {
        if (!keeps_key(key, index))
            index = map[index];
    }
}

std::size_t row_rle_size(std::span<const uint8_t> stream, int width, int height) noexcept {
    return walk_row_rle(stream, width, height, [](const uint8_t&) {});
}

bool decode_row_rle(
    std::span<const uint8_t> stream, int width, int height, int key, std::vector<uint8_t>& indices
) {
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
        return false;
    if (row_rle_size(stream, width, height) == 0)
        return false;
    const auto count = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    const auto fill = static_cast<uint8_t>(key >= 0 && key < kPaletteColours ? key : 0);
    indices.assign(count, fill);
    std::size_t at = 0;
    for (int y = 0; y < height; ++y) {
        const std::size_t length =
            static_cast<std::size_t>(stream[at]) | (static_cast<std::size_t>(stream[at + 1]) << 8);
        at += 2;
        const std::size_t end = at + length;
        int x = 0;
        while (length != 0 && x < width) {
            const auto command = static_cast<unsigned>(stream[at]);
            ++at;
            if ((command & 0x01U) != 0) {
                x += static_cast<int>(command >> 1);
                continue;
            }
            const int run = std::min(static_cast<int>(command >> 2) + 1, width - x);
            if ((command & 0x02U) != 0) {
                const uint8_t colour = stream[at];
                ++at;
                for (int pixel = 0; pixel < run; ++pixel)
                    indices[static_cast<std::size_t>(y * width + x + pixel)] = colour;
            } else {
                for (int pixel = 0; pixel < run; ++pixel)
                    indices[static_cast<std::size_t>(y * width + x + pixel)] =
                        stream[at + static_cast<std::size_t>(pixel)];
                at += static_cast<std::size_t>(run);
            }
            x += run;
        }
        at = end;
    }
    return true;
}

bool remap_row_rle(
    std::span<uint8_t> stream, int width, int height, const IndexMap& map, int key
) noexcept {
    if (width <= 0 || height <= 0)
        return false;
    std::size_t at = 0;
    for (int row = 0; row < height; ++row) {
        if (stream.size() - at < 2)
            return false;
        const std::size_t length =
            static_cast<std::size_t>(stream[at]) | (static_cast<std::size_t>(stream[at + 1]) << 8);
        at += 2;
        if (stream.size() - at < length)
            return false;
        const std::size_t end = at + length;
        int column = 0;
        while (length != 0 && column < width) {
            if (at >= end)
                return false;
            const auto command = static_cast<unsigned>(stream[at]);
            ++at;
            if ((command & 0x01U) != 0) {
                column += static_cast<int>(command >> 1);
                continue;
            }
            const int count = std::min(static_cast<int>(command >> 2) + 1, width - column);
            if ((command & 0x02U) != 0) {
                if (at >= end)
                    return false;
                if (!keeps_key(key, stream[at]))
                    stream[at] = map[stream[at]];
                ++at;
            } else {
                if (end - at < static_cast<std::size_t>(count))
                    return false;
                for (int pixel = 0; pixel < count; ++pixel) {
                    uint8_t& index = stream[at + static_cast<std::size_t>(pixel)];
                    if (!keeps_key(key, index))
                        index = map[index];
                }
                at += static_cast<std::size_t>(count);
            }
            column += count;
        }
        at = end;
    }
    return true;
}

std::array<int, kPlayerCount>
blip_bodies(const std::array<RawFrame, kPlayerCount>& frames, std::span<const uint8_t> palette) {
    std::array<int, kPlayerCount> bodies{};
    bodies.fill(-1);
    if (!palette_ready(palette))
        return bodies;
    std::array<int, kPaletteColours> frames_with{};
    for (const RawFrame& frame : frames) {
        if (frame.indices == nullptr)
            continue;
        std::array<bool, kPaletteColours> seen{};
        for (std::size_t pixel = 0; pixel < frame.count; ++pixel) {
            const int index = frame.indices[pixel];
            if (index == frame.key)
                continue;
            seen[static_cast<std::size_t>(index)] = true;
        }
        for (int index = 0; index < kPaletteColours; ++index)
            if (seen[static_cast<std::size_t>(index)])
                ++frames_with[static_cast<std::size_t>(index)];
    }
    int outline = 0;
    int outline_frames = -1;
    for (int index = 0; index < kPaletteColours; ++index) {
        if (frames_with[static_cast<std::size_t>(index)] > outline_frames) {
            outline_frames = frames_with[static_cast<std::size_t>(index)];
            outline = index;
        }
    }
    if (outline_frames <= 0)
        return bodies;
    const Rgb outline_colour = palette_colour(palette, outline);
    for (int player = 0; player < kPlayerCount; ++player) {
        const RawFrame& frame = frames[static_cast<std::size_t>(player)];
        if (frame.indices == nullptr)
            continue;
        int body = -1;
        int best = -1;
        std::array<bool, kPaletteColours> seen{};
        for (std::size_t pixel = 0; pixel < frame.count; ++pixel) {
            const int index = frame.indices[pixel];
            if (index == frame.key || index == outline)
                continue;
            seen[static_cast<std::size_t>(index)] = true;
        }
        for (int index = 0; index < kPaletteColours; ++index) {
            if (!seen[static_cast<std::size_t>(index)])
                continue;
            const int distance = channel_distance(palette_colour(palette, index), outline_colour);
            if (body < 0 || distance > best) {
                body = index;
                best = distance;
            }
        }
        bodies[static_cast<std::size_t>(player)] = body;
    }
    return bodies;
}

const BlipPins& blip_pins() noexcept {
    return presentation().pins;
}

void remember_blip_pins(const BlipPins& pins) noexcept {
    presentation().pins = pins;
}

const TeamMaps& team_map_tables() noexcept {
    return presentation().maps;
}

void remember_team_maps(const TeamMaps& maps) noexcept {
    presentation().maps = maps;
}

bool holds_originals() noexcept {
    return !presentation().originals.empty();
}

FramePaint prepare_original(
    std::string_view role,
    std::string_view sequence,
    int frame,
    std::span<uint8_t> pixels,
    bool paint
) {
    auto& originals = presentation().originals;
    const std::string key = frame_key(role, sequence, frame);
    const auto found = originals.find(key);
    if (!paint) {
        if (found == originals.end())
            return FramePaint::leave;
        if (found->second.size() == pixels.size())
            std::copy(found->second.begin(), found->second.end(), pixels.begin());
        originals.erase(found);
        return FramePaint::restored;
    }
    if (found == originals.end() || found->second.size() != pixels.size())
        originals.insert_or_assign(key, std::vector<uint8_t>(pixels.begin(), pixels.end()));
    else
        std::copy(found->second.begin(), found->second.end(), pixels.begin());
    return FramePaint::ready;
}

void paint_team_archive(
    formats::gaf::Archive& archive, std::span<const uint8_t> palette, std::string_view role
) {
    const bool paint = active_mode() != Mode::off && palette_ready(palette);
    if (!paint && !role_holds(role))
        return;

    struct HeldFrame {
        std::span<uint8_t> indices{};
        int key{-1};
        bool ready{false};
    };

    std::vector<std::array<HeldFrame, kPlayerCount>> held;
    std::vector<RawSequence> reading;
    for (formats::gaf::Sequence& sequence : archive.sequences) {
        if (sequence.frames.size() != static_cast<std::size_t>(kPlayerCount))
            continue;
        std::array<HeldFrame, kPlayerCount> frames{};
        bool any = false;
        for (int player = 0; player < kPlayerCount; ++player) {
            formats::gaf::Frame& frame = sequence.frames[static_cast<std::size_t>(player)];
            if (!readable_team_frame(frame))
                continue;
            const auto count = static_cast<std::size_t>(frame.width) * frame.height;
            const FramePaint prepared = prepare_original(
                role, sequence.name, player, std::span<uint8_t>(frame.pixels.data(), count), paint
            );
            if (prepared != FramePaint::ready)
                continue;
            frames[static_cast<std::size_t>(player)] = {
                std::span<uint8_t>(frame.pixels.data(), count), frame.transparency_index, true
            };
            any = true;
        }
        if (any)
            held.push_back(frames);
    }
    if (!paint)
        return;
    reading.reserve(held.size());
    for (const auto& frames : held) {
        RawSequence sequence;
        for (int player = 0; player < kPlayerCount; ++player) {
            const HeldFrame& frame = frames[static_cast<std::size_t>(player)];
            if (!frame.ready)
                continue;
            sequence.frames[static_cast<std::size_t>(player)] = {
                frame.indices.data(), frame.indices.size(), frame.key
            };
        }
        reading.push_back(sequence);
    }
    const TeamMaps maps = team_maps(active_mode(), palette, reading);
    if (!reading.empty())
        remember_team_maps(maps);
    for (const auto& frames : held) {
        for (int player = 0; player < kPlayerCount; ++player) {
            const HeldFrame& frame = frames[static_cast<std::size_t>(player)];
            if (!frame.ready)
                continue;
            remap_indices(frame.indices, maps[static_cast<std::size_t>(player)], frame.key);
        }
    }
}

} // namespace oa::present::colour_vision

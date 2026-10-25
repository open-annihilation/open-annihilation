// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/hud/megamap.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>

namespace oa::ui::hud {
namespace {

constexpr std::size_t kPaletteEntryBytes = 4;
constexpr std::size_t kPaletteEntries = 256;
/// UI colours (Game.ui_colors) of the rings, as the minimap draws them.
constexpr std::size_t kSensorRingColor = 10;
constexpr std::size_t kJammerRingColor = 12;
constexpr std::size_t kInterceptorRingColor = 15;

std::string lower(std::string_view text) {
    std::string out(text);
    for (auto& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())) != 0)
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())) != 0)
        text.remove_suffix(1);
    return text;
}

/// The value of a line, up to its ';' comment, trimmed.
std::string_view value_of(std::string_view text) {
    const auto comment = text.find(';');
    if (comment != std::string_view::npos)
        text = text.substr(0, comment);
    return trim(text);
}

bool read_switch(std::string_view value) {
    return lower(value) == "true" || value == "1";
}

uint8_t read_index(std::string_view value, uint8_t fallback) {
    int32_t number = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), number);
    if (result.ec != std::errc{} || number < 0 || number > 255)
        return fallback;
    return static_cast<uint8_t>(number);
}

/// Squared distance between a palette entry and a colour.
int32_t
distance(std::span<const uint8_t> palette, std::size_t entry, int32_t r, int32_t g, int32_t b) {
    const auto at = entry * kPaletteEntryBytes;
    const int32_t dr = palette[at] - r, dg = palette[at + 1] - g, db = palette[at + 2] - b;
    return dr * dr + dg * dg + db * db;
}

/// Turns an sRGB channel, 0 to 255, into linear light, 0 to 1.
float linear_channel(int32_t value) {
    const float channel = static_cast<float>(value) / 255.0F;
    if (channel <= 0.04045F)
        return channel / 12.92F;
    const float base = (channel + 0.055F) / 1.055F;
    return static_cast<float>(std::pow(static_cast<double>(base), static_cast<double>(2.4F)));
}

/// Converts an sRGB colour to OKLab: lightness and two colour axes.
std::array<float, 3> oklab(int32_t red, int32_t green, int32_t blue) {
    const float r = linear_channel(red), g = linear_channel(green), b = linear_channel(blue);
    const float l = std::cbrt(0.4122214708F * r + 0.5363325363F * g + 0.0514459929F * b);
    const float m = std::cbrt(0.2119034982F * r + 0.6806995451F * g + 0.1073969566F * b);
    const float s = std::cbrt(0.0883024619F * r + 0.2817188376F * g + 0.6299787005F * b);
    return {
        0.2104542553F * l + 0.7936177850F * m - 0.0040720468F * s,
        1.9779984951F * l - 2.4285922050F * m + 0.4505937099F * s,
        0.0259040371F * l + 0.7827717662F * m - 0.8086757660F * s,
    };
}

/// Squared distance between two OKLab colours.
float lab_distance(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    const float dl = a[0] - b[0], da = a[1] - b[1], db = a[2] - b[2];
    return dl * dl + da * da + db * db;
}

/// The two entries of the whole palette nearest a colour by squared
/// distance in red, green and blue; the first one found wins a tie. The
/// second is none when the palette has a single entry.
std::array<std::size_t, 2>
nearest_two(std::span<const uint8_t> palette, int32_t r, int32_t g, int32_t b, bool& has_second) {
    std::size_t nearest = 0, second = 0;
    int32_t best = std::numeric_limits<int32_t>::max(), runner = best;
    for (std::size_t entry = 0; entry < kPaletteEntries; ++entry) {
        const int32_t d = distance(palette, entry, r, g, b);
        if (d < best) {
            runner = best;
            second = nearest;
            best = d;
            nearest = entry;
        } else if (d < runner) {
            runner = d;
            second = entry;
        }
    }
    has_second = runner != std::numeric_limits<int32_t>::max();
    return {nearest, second};
}

/// The two entries of a perceptual palette that look nearest a colour; the
/// first one found wins a tie.
std::array<std::size_t, 2> nearest_two_perceptual(
    const PerceptualPalette& palette, int32_t r, int32_t g, int32_t b, bool& has_second
) {
    const auto lab = oklab(r, g, b);
    std::size_t nearest = 0, second = 0;
    float best = std::numeric_limits<float>::max(), runner = best;
    for (std::size_t index = 0; index < palette.lab.size(); ++index) {
        const float d = lab_distance(palette.lab[index], lab);
        if (d < best) {
            runner = best;
            second = nearest;
            best = d;
            nearest = index;
        } else if (d < runner) {
            runner = d;
            second = index;
        }
    }
    has_second = palette.lab.size() > 1;
    return {palette.entries[nearest], palette.entries[second]};
}

} // namespace

PerceptualPalette
perceptual_palette(std::span<const uint8_t> palette, std::span<const uint8_t> pixels) {
    PerceptualPalette out;
    if (palette.size() < kPaletteEntries * kPaletteEntryBytes)
        return out;
    std::array<bool, kPaletteEntries> used{};
    for (const uint8_t pixel : pixels)
        used[pixel] = true;
    const bool every = pixels.empty();
    for (std::size_t entry = 0; entry < kPaletteEntries; ++entry) {
        if (!every && !used[entry])
            continue;
        const auto at = entry * kPaletteEntryBytes;
        out.entries.push_back(static_cast<uint8_t>(entry));
        out.lab.push_back(oklab(palette[at], palette[at + 1], palette[at + 2]));
    }
    return out;
}

uint8_t nearest_perceptual(const PerceptualPalette& palette, int32_t r, int32_t g, int32_t b) {
    if (palette.entries.empty())
        return 0;
    bool has_second = false;
    return static_cast<uint8_t>(nearest_two_perceptual(palette, r, g, b, has_second)[0]);
}

uint8_t nearest_palette_entry(std::span<const uint8_t> palette, int32_t r, int32_t g, int32_t b) {
    if (palette.size() < kPaletteEntries * kPaletteEntryBytes)
        return 0;
    bool has_second = false;
    return static_cast<uint8_t>(nearest_two(palette, r, g, b, has_second)[0]);
}

MegamapLayout megamap_layout(
    int32_t view_left,
    int32_t view_top,
    int32_t view_width,
    int32_t view_height,
    int32_t map_width,
    int32_t map_height
) noexcept {
    MegamapLayout layout{};
    if (view_width <= 0 || view_height <= 0 || map_width <= 0 || map_height <= 0)
        return layout;
    layout.map_width = map_width;
    layout.map_height = map_height;
    // The map's aspect kept: the wider side fills the view.
    if (static_cast<int64_t>(map_width) * view_height >=
        static_cast<int64_t>(map_height) * view_width) {
        layout.width = view_width;
        layout.height = std::max<int32_t>(
            1, static_cast<int32_t>(static_cast<int64_t>(map_height) * view_width / map_width)
        );
    } else {
        layout.height = view_height;
        layout.width = std::max<int32_t>(
            1, static_cast<int32_t>(static_cast<int64_t>(map_width) * view_height / map_height)
        );
    }
    layout.left = view_left + (view_width - layout.width) / 2;
    layout.top = view_top + (view_height - layout.height) / 2;
    return layout;
}

std::array<int32_t, 2>
megamap_point(const MegamapLayout& layout, int32_t map_x, int32_t map_z) noexcept {
    if (layout.map_width <= 0 || layout.map_height <= 0)
        return {layout.left, layout.top};
    return {
        layout.left +
            static_cast<int32_t>(static_cast<int64_t>(map_x) * layout.width / layout.map_width),
        layout.top +
            static_cast<int32_t>(static_cast<int64_t>(map_z) * layout.height / layout.map_height),
    };
}

std::optional<std::array<int32_t, 2>>
megamap_map_point(const MegamapLayout& layout, int32_t x, int32_t y) noexcept {
    if (layout.width <= 0 || layout.height <= 0 || x < layout.left || y < layout.top ||
        x >= layout.left + layout.width || y >= layout.top + layout.height)
        return std::nullopt;
    return std::array<int32_t, 2>{
        static_cast<int32_t>(
            static_cast<int64_t>(x - layout.left) * layout.map_width / layout.width
        ),
        static_cast<int32_t>(
            static_cast<int64_t>(y - layout.top) * layout.map_height / layout.height
        ),
    };
}

std::vector<uint8_t> downscale_terrain(
    const TerrainSource& source,
    int32_t map_width,
    int32_t map_height,
    int32_t width,
    int32_t height,
    std::span<const uint8_t> palette,
    bool dither,
    const PerceptualPalette* perceptual
) {
    std::vector<uint8_t> out;
    if (width <= 0 || height <= 0 || map_width <= 0 || map_height <= 0 || source.pixel == nullptr ||
        palette.size() < kPaletteEntries * kPaletteEntryBytes ||
        (perceptual != nullptr && perceptual->entries.empty()))
        return out;
    out.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    for (int32_t y = 0; y < height; ++y) {
        const int32_t z0 = static_cast<int32_t>(static_cast<int64_t>(y) * map_height / height);
        const int32_t z1 = std::max(
            z0 + 1, static_cast<int32_t>(static_cast<int64_t>(y + 1) * map_height / height)
        );
        for (int32_t x = 0; x < width; ++x) {
            const int32_t x0 = static_cast<int32_t>(static_cast<int64_t>(x) * map_width / width);
            const int32_t x1 = std::max(
                x0 + 1, static_cast<int32_t>(static_cast<int64_t>(x + 1) * map_width / width)
            );
            int64_t r = 0, g = 0, b = 0, count = 0;
            for (int32_t z = z0; z < z1; ++z)
                for (int32_t column = x0; column < x1; ++column) {
                    const auto entry =
                        static_cast<std::size_t>(source.pixel(source.user, column, z));
                    r += palette[entry * kPaletteEntryBytes];
                    g += palette[entry * kPaletteEntryBytes + 1];
                    b += palette[entry * kPaletteEntryBytes + 2];
                    ++count;
                }
            const auto mean_r = static_cast<int32_t>(r / count);
            const auto mean_g = static_cast<int32_t>(g / count);
            const auto mean_b = static_cast<int32_t>(b / count);
            bool has_second = false;
            const auto nearest =
                perceptual != nullptr
                    ? nearest_two_perceptual(*perceptual, mean_r, mean_g, mean_b, has_second)
                    : nearest_two(palette, mean_r, mean_g, mean_b, has_second);
            const bool alternate = dither && ((x + y) & 1) != 0 && has_second;
            out[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                static_cast<std::size_t>(x)] = static_cast<uint8_t>(nearest[alternate ? 1 : 0]);
        }
    }
    return out;
}

bool megamap_draws_feature(const FeatureDef& def) noexcept {
    return (def.flags & OA_FEATURE_FLAG_INDESTRUCTIBLE) != 0 &&
           (def.flags & OA_FEATURE_FLAG_RECLAIMABLE) == 0;
}

uint8_t feature_mark_color(const FeatureDef& def) noexcept {
    if (def.metal > 0.0F)
        return feature_mark_metal;
    constexpr std::string_view spire = "Spire";
    const std::string_view description(
        def.description, ::strnlen(def.description, sizeof def.description)
    );
    const bool is_spire =
        description.size() == spire.size() &&
        std::equal(description.begin(), description.end(), spire.begin(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    return is_spire ? feature_mark_spire : feature_mark_other;
}

ShrunkPicture shrink_picture(
    std::span<const uint8_t> pixels,
    int32_t width,
    int32_t height,
    uint8_t transparent,
    std::span<const uint8_t> palette,
    int32_t longest
) {
    ShrunkPicture out;
    if (width <= 0 || height <= 0 || longest < 1 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) ||
        palette.size() < kPaletteEntries * kPaletteEntryBytes)
        return out;
    // The longer side becomes `longest`, the other in proportion, in single
    // precision rounded to the nearest pixel.
    const auto larger = static_cast<float>(std::max(width, height));
    const auto side = [&](int32_t source) {
        return std::max<int32_t>(
            1,
            static_cast<int32_t>(
                static_cast<float>(source) * static_cast<float>(longest) / larger + 0.5F
            )
        );
    };
    out.width = side(width);
    out.height = side(height);
    out.rgba.assign(
        static_cast<std::size_t>(out.width) * static_cast<std::size_t>(out.height) * 4U, 0
    );
    for (int32_t y = 0; y < out.height; ++y) {
        const int32_t y0 = y * height / out.height;
        const int32_t y1 = std::min(height, std::max(y0 + 1, (y + 1) * height / out.height));
        for (int32_t x = 0; x < out.width; ++x) {
            const int32_t x0 = x * width / out.width;
            const int32_t x1 = std::min(width, std::max(x0 + 1, (x + 1) * width / out.width));
            int32_t r = 0, g = 0, b = 0, opaque = 0, area = 0;
            for (int32_t row = y0; row < y1; ++row)
                for (int32_t column = x0; column < x1; ++column) {
                    ++area;
                    const uint8_t index = pixels
                        [static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                         static_cast<std::size_t>(column)];
                    if (index == transparent)
                        continue;
                    const auto at = static_cast<std::size_t>(index) * kPaletteEntryBytes;
                    r += palette[at];
                    g += palette[at + 1];
                    b += palette[at + 2];
                    ++opaque;
                }
            if (opaque == 0)
                continue;
            auto* pixel = &out.rgba
                               [(static_cast<std::size_t>(y) * static_cast<std::size_t>(out.width) +
                                 static_cast<std::size_t>(x)) *
                                4U];
            pixel[0] = static_cast<uint8_t>(r / opaque);
            pixel[1] = static_cast<uint8_t>(g / opaque);
            pixel[2] = static_cast<uint8_t>(b / opaque);
            pixel[3] = static_cast<uint8_t>((opaque * 255 + area / 2) / area);
        }
    }
    return out;
}

void blend_picture(
    std::span<uint8_t> canvas,
    int32_t canvas_width,
    int32_t canvas_height,
    const ShrunkPicture& picture,
    int32_t left,
    int32_t top,
    std::span<const uint8_t> palette
) {
    if (canvas_width <= 0 || canvas_height <= 0 ||
        canvas.size() <
            static_cast<std::size_t>(canvas_width) * static_cast<std::size_t>(canvas_height) ||
        palette.size() < kPaletteEntries * kPaletteEntryBytes ||
        picture.rgba.size() <
            static_cast<std::size_t>(picture.width) * static_cast<std::size_t>(picture.height) * 4U)
        return;
    constexpr int32_t opaque = 255;
    for (int32_t y = 0; y < picture.height; ++y) {
        const int32_t row = top + y;
        if (row < 0 || row >= canvas_height)
            continue;
        for (int32_t x = 0; x < picture.width; ++x) {
            const int32_t column = left + x;
            if (column < 0 || column >= canvas_width)
                continue;
            const auto* pixel =
                &picture.rgba
                     [(static_cast<std::size_t>(y) * static_cast<std::size_t>(picture.width) +
                       static_cast<std::size_t>(x)) *
                      4U];
            const int32_t alpha = pixel[3];
            if (alpha == 0)
                continue;
            auto& under = canvas
                [static_cast<std::size_t>(row) * static_cast<std::size_t>(canvas_width) +
                 static_cast<std::size_t>(column)];
            int32_t r = pixel[0], g = pixel[1], b = pixel[2];
            if (alpha < opaque) {
                const auto at = static_cast<std::size_t>(under) * kPaletteEntryBytes;
                r = (r * alpha + palette[at] * (opaque - alpha)) / opaque;
                g = (g * alpha + palette[at + 1] * (opaque - alpha)) / opaque;
                b = (b * alpha + palette[at + 2] * (opaque - alpha)) / opaque;
            }
            under = nearest_palette_entry(palette, r, g, b);
        }
    }
}

FeatureSpot megamap_feature_spot(
    const MegamapLayout& layout,
    int32_t cell_x,
    int32_t cell_z,
    int32_t footprint_x,
    int32_t footprint_z,
    int32_t ground_height,
    int32_t frame_longest
) noexcept {
    FeatureSpot spot{};
    if (layout.map_width <= 0 || layout.map_height <= 0)
        return spot;
    constexpr int32_t cell_pixels = 16;
    constexpr int32_t frame_without_size = 32;
    constexpr int32_t least_side = 2;
    const auto width = static_cast<float>(layout.width);
    const auto map_width = static_cast<float>(layout.map_width);
    const int32_t centre_x = cell_x * cell_pixels + std::max(1, footprint_x) * cell_pixels / 2;
    const int32_t centre_z =
        cell_z * cell_pixels + std::max(1, footprint_z) * cell_pixels / 2 - ground_height / 2;
    spot.x = static_cast<int32_t>(static_cast<float>(centre_x) * width / map_width);
    spot.y = static_cast<int32_t>(
        static_cast<float>(centre_z) * static_cast<float>(layout.height) /
        static_cast<float>(layout.map_height)
    );
    const int32_t frame = frame_longest < 1 ? frame_without_size : frame_longest;
    spot.longest = std::max(
        least_side, static_cast<int32_t>(width / map_width * static_cast<float>(frame) + 0.5F)
    );
    return spot;
}

int32_t feature_picture_top(
    int32_t spot_y, int32_t origin_y, int32_t frame_height, int32_t shrunk_height
) noexcept {
    if (frame_height == 0)
        return spot_y - shrunk_height;
    return spot_y - static_cast<int32_t>(
                        static_cast<float>(origin_y) * static_cast<float>(shrunk_height) /
                            static_cast<float>(frame_height) +
                        0.5F
                    );
}

IconConfig parse_icon_config(std::string_view text, uint32_t category_limit) {
    IconConfig config{};
    std::string section;
    while (!text.empty()) {
        const auto end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        line = trim(line);
        if (line.empty() || line.front() == ';')
            continue;
        if (line.front() == '[') {
            const auto close = line.find(']');
            section = lower(
                line.substr(1, close == std::string_view::npos ? line.size() - 1 : close - 1)
            );
            continue;
        }
        const auto equals = line.find('=');
        if (equals == std::string_view::npos)
            continue;
        const std::string key = lower(trim(line.substr(0, equals)));
        const std::string_view value = value_of(line.substr(equals + 1));
        if (section == "option") {
            auto& options = config.options;
            if (key == "fillcolor")
                options.fill_color = read_index(value, options.fill_color);
            else if (key == "transparentcolor")
                options.transparent_color = read_index(value, options.transparent_color);
            else if (key == "selectedcolor")
                options.selected_color = read_index(value, options.selected_color);
            else if (key == "hovercolor")
                options.hover_color = read_index(value, options.hover_color);
            else if (key == "usecirclehover")
                options.circle_hover = read_switch(value);
            else if (key == "usedefaulticon")
                options.default_icons = read_switch(value);
        } else if (section == "icon") {
            if (key == "unknow")
                config.unknown_file = std::string(value);
            else if (key == "nothing")
                config.nothing_file = std::string(value);
            else if (key == "nukeicon")
                config.nuke_file = std::string(value);
            else if (config.lines.size() < category_limit)
                config.lines.push_back({key, std::string(value)});
        }
    }
    return config;
}

const data::defs::CategoryMask*
icon_category_mask(const data::defs::CategoryRegistry& registry, const IconLine& line) noexcept {
    return data::defs::category_registry_find(&registry, line.category.c_str());
}

IconChoice choose_icon(
    bool visible,
    uint16_t type_id,
    std::span<const data::defs::CategoryMask* const> categories,
    std::size_t& line
) noexcept {
    if (!visible)
        return IconChoice::nothing;
    for (std::size_t index = 0; index < categories.size(); ++index)
        if (categories[index] != nullptr &&
            data::defs::category_mask_contains(categories[index], type_id)) {
            line = index;
            return IconChoice::category;
        }
    return IconChoice::unknown;
}

std::optional<uint8_t> icon_pixel(
    const IconOptions& options, uint8_t pixel, uint8_t dot_color, bool selected, bool hovered
) noexcept {
    if (pixel == options.transparent_color)
        return std::nullopt;
    if (pixel == options.fill_color)
        return dot_color;
    if (pixel == options.selected_color && !selected) {
        if (hovered && !options.circle_hover)
            return options.hover_color;
        return std::nullopt;
    }
    return pixel;
}

uint32_t megamap_rings(
    const World& world,
    const Unit& unit,
    const UnitDef& def,
    const std::array<int32_t, 5>& minimums,
    std::array<MegamapRing, 5>& out
) noexcept {
    const auto& colors = world.game.ui_colors;
    uint32_t count = 0;
    const auto add = [&](int32_t radius, int32_t minimum, std::size_t color) {
        if (radius > 0 && radius >= minimum)
            out[count++] = {radius, colors[color]};
    };
    add(def.radar_distance, minimums[0], kSensorRingColor);
    add(def.sonar_distance, minimums[1], kSensorRingColor);
    add(def.radar_distance_jam, minimums[2], kJammerRingColor);
    add(def.sonar_distance_jam, minimums[3], kJammerRingColor);
    int32_t coverage = 0;
    for (const UnitWeapon& slot : unit.weapons) {
        const WeaponDef* weapon = world_weapon_def(&world, slot.def);
        if (weapon != nullptr && (weapon->flags & OA_WEAPON_FLAG_INTERCEPTOR) != 0) {
            const int32_t weapon_coverage = weapon->coverage;
            coverage = std::max(coverage, weapon_coverage);
        }
    }
    add(coverage, minimums[4], kInterceptorRingColor);
    return count;
}

} // namespace oa::ui::hud

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/frontend_renderer.hpp"

#include "oa/ui/gui_layout/gui_gadget.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::frontend_renderer {
namespace {

constexpr std::string_view main_menu_layout = "guis/mainmenu.gui";
constexpr std::string_view main_menu_background = "bitmaps/frontendx.pcx";
constexpr std::string_view main_menu_palette = "palettes/guipal.pal";
constexpr std::string_view main_menu_sprites = "anims/mainmenu.gaf";
constexpr std::string_view shared_gui_sprites = "anims/commongui.gaf";
constexpr std::string_view default_gui_font = "anims/hattfont12.gaf";
constexpr std::string_view label_gui_font = "anims/hattfont11.gaf";
constexpr std::string_view default_light_table = "palettes/palette.lht";
constexpr std::string_view default_shade_table = "palettes/palette.shd";
constexpr std::string_view game_palette_file = "palettes/palette.pal";
constexpr std::string_view global_logo_sprites = "textures/logos.gaf";

// Entries of the destination palette map the button drawing uses.
constexpr uint8_t palette_black = 0;
constexpr uint8_t palette_bevel_shadow = 17;
constexpr uint8_t palette_disabled_fill = 19;
constexpr uint8_t palette_button_fill = 20;
// A grayed-out button's art frame sits this far past its normal frame, and
// the button is then darkened through this row of the shade table.
constexpr std::size_t grayed_frame_offset = 2;
constexpr std::size_t grayed_shade_row = 0x20 - 0x14;
constexpr std::size_t shade_table_rows = 32;

struct Rectangle {
    int left = 0;
    int top = 0;
    int right = -1;
    int bottom = -1;
};

[[nodiscard]] bool ascii_equal(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        const auto l = static_cast<unsigned char>(left[i]);
        const auto r = static_cast<unsigned char>(right[i]);
        const auto lower = [](unsigned char value) {
            return static_cast<unsigned char>(
                value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value
            );
        };
        if (lower(l) != lower(r))
            return false;
    }
    return true;
}

[[nodiscard]] std::array<uint8_t, 3> palette_rgb(const PaletteBytes& palette, uint8_t index) {
    const auto offset = static_cast<std::size_t>(index) * palette_entry_bytes;
    return {palette[offset], palette[offset + 1], palette[offset + 2]};
}

void set_pixel(Surface& surface, int x, int y, const std::array<uint8_t, 3>& color) {
    if (x < 0 || y < 0 || x >= static_cast<int>(surface.width) ||
        y >= static_cast<int>(surface.height))
        return;
    const auto offset =
        (static_cast<std::size_t>(y) * surface.width + static_cast<std::size_t>(x)) * 3;
    std::copy(
        color.begin(), color.end(), surface.rgb.begin() + static_cast<std::ptrdiff_t>(offset)
    );
}

void horizontal_line(Surface& surface, int x1, int x2, int y, const std::array<uint8_t, 3>& color) {
    if (y < 0 || y >= static_cast<int>(surface.height))
        return;
    x1 = std::max(x1, 0);
    x2 = std::min(x2, static_cast<int>(surface.width) - 1);
    if (x1 > x2)
        return;
    for (int x = x1; x <= x2; ++x)
        set_pixel(surface, x, y, color);
}

void vertical_line(Surface& surface, int x, int y1, int y2, const std::array<uint8_t, 3>& color) {
    if (x < 0 || x >= static_cast<int>(surface.width))
        return;
    y1 = std::max(y1, 0);
    y2 = std::min(y2, static_cast<int>(surface.height) - 1);
    if (y1 > y2)
        return;
    for (int y = y1; y <= y2; ++y)
        set_pixel(surface, x, y, color);
}

void fill(Surface& surface, const Rectangle& rectangle, const std::array<uint8_t, 3>& color) {
    const int top = std::max(rectangle.top, 0);
    const int bottom = std::min(rectangle.bottom, static_cast<int>(surface.height) - 1);
    for (int y = top; y <= bottom; ++y) {
        horizontal_line(surface, rectangle.left, rectangle.right, y, color);
    }
}

// The game's raised and sunken bevels.
void bevel(
    Surface& surface,
    const Rectangle& rectangle,
    const std::array<uint8_t, 3>& top_left,
    const std::array<uint8_t, 3>& bottom_right
) {
    horizontal_line(surface, rectangle.left, rectangle.right, rectangle.top, top_left);
    horizontal_line(surface, rectangle.left, rectangle.right - 1, rectangle.top + 1, top_left);
    vertical_line(surface, rectangle.left, rectangle.top, rectangle.bottom, top_left);
    vertical_line(surface, rectangle.left + 1, rectangle.top, rectangle.bottom - 1, top_left);
    vertical_line(surface, rectangle.right, rectangle.top + 1, rectangle.bottom, bottom_right);
    vertical_line(surface, rectangle.right - 1, rectangle.top + 2, rectangle.bottom, bottom_right);
    horizontal_line(surface, rectangle.left + 1, rectangle.right, rectangle.bottom, bottom_right);
    horizontal_line(
        surface, rectangle.left + 2, rectangle.right, rectangle.bottom - 1, bottom_right
    );
}

[[nodiscard]] Rectangle gadget_rectangle(const ui::gui_layout::Gadget& gadget) {
    return {
        gadget.common.x,
        gadget.common.y,
        gadget.common.x + gadget.common.width - 1,
        gadget.common.y + gadget.common.height - 1
    };
}

[[nodiscard]] const ButtonPresentation*
presentation_for(std::string_view name, std::span<const ButtonPresentation> states) {
    const auto found =
        std::find_if(states.begin(), states.end(), [name](const ButtonPresentation& state) {
            return ascii_equal(name, state.name);
        });
    return found == states.end() ? nullptr : &*found;
}

[[nodiscard]] const ListPresentation*
list_for(std::string_view name, std::span<const ListPresentation> lists) {
    const auto found =
        std::find_if(lists.begin(), lists.end(), [name](const ListPresentation& list) {
            return ascii_equal(name, list.name);
        });
    return found == lists.end() ? nullptr : &*found;
}

[[nodiscard]] const formats::gaf::Sequence*
sequence_for(const formats::gaf::Archive& archive, std::string_view name) {
    const auto found = std::find_if(
        archive.sequences.begin(),
        archive.sequences.end(),
        [name](const formats::gaf::Sequence& sequence) { return ascii_equal(name, sequence.name); }
    );
    return found == archive.sequences.end() ? nullptr : &*found;
}

struct ResolvedSprite {
    const formats::gaf::Sequence* sequence = nullptr;
    std::size_t base_frame = 0;
};

[[nodiscard]] ResolvedSprite resolve_button_sprite(
    const ScreenResources& resources,
    const ui::gui_layout::Gadget& gadget,
    const ButtonPresentation* presentation = nullptr
) {
    if (presentation != nullptr && presentation->sprite.has_value()) {
        const auto& override = *presentation->sprite;
        const formats::gaf::Archive* archive = &resources.sprites;
        if (override.archive == SpriteArchive::shared)
            archive = &resources.shared_sprites;
        else if (override.archive == SpriteArchive::global)
            archive = &resources.global_sprites;
        return {sequence_for(*archive, override.sequence), 0};
    }
    if (const auto* named = sequence_for(resources.sprites, gadget.common.name)) {
        return {named, 0};
    }
    if (const auto* named = sequence_for(resources.shared_sprites, gadget.common.name)) {
        return {named, 0};
    }
    const auto* fields = std::get_if<ui::gui_layout::ButtonFields>(&gadget.fields);
    // The first panel draw skips BUTTONS0/stagebuttn for a button with the
    // checkbox or text_list attribute.
    constexpr uint32_t skip_default_button_gaf =
        ui::gui_layout::attribute::checkbox | ui::gui_layout::attribute::text_list;
    if ((static_cast<uint32_t>(gadget.common.attributes) & skip_default_button_gaf) != 0)
        return {};
    std::string fallback_name = "BUTTONS0";
    constexpr uint32_t checkbox_attribute = 0x80U;
    constexpr uint32_t stage_button_attribute = 0x4000U;
    const auto attributes = static_cast<uint32_t>(gadget.common.attributes);
    if ((attributes & checkbox_attribute) != 0) {
        fallback_name = "CHECKBOX";
    } else if (fields != nullptr && fields->stages != 0) {
        if (fields->text == "Off|On" || fields->stages == 1 ||
            (attributes & stage_button_attribute) != 0) {
            fallback_name = "stagebuttn1";
        } else {
            fallback_name = "stagebuttn" + std::to_string(fields->stages);
        }
    }
    const auto* fallback = sequence_for(resources.shared_sprites, fallback_name);
    if (fallback == nullptr || fallback->frames.empty())
        return {};
    std::size_t best = 0;
    // The best difference starts at 1000; malformed declarations farther
    // from every candidate retain group zero.
    int best_difference = 1000;
    // The frame selector advances by four, so the loop samples the first
    // frame of each four-state size group: 0, 4, 8, 12, ...
    for (std::size_t index = 0; index < fallback->frames.size(); index += 4) {
        const auto& frame = fallback->frames[index];
        const int difference = std::abs(static_cast<int>(gadget.common.width) - frame.width) +
                               std::abs(static_cast<int>(gadget.common.height) - frame.height);
        if (difference < best_difference) {
            best = index;
            best_difference = difference;
        }
    }
    return {fallback, best};
}

// The light-table row a gadget's authored colour names, or null when the
// colour is zero (the frame is drawn as authored). A type-1 button's colour
// lights its GAF frame and a label's lights its glyphs through the game's
// 32x256 PALETTE.LHT; the first draw zeroes a loaded panel's authored colours,
// so only one a merged sub-panel or an extension leaves behind is applied.
[[nodiscard]] const uint8_t*
light_row_for(std::span<const uint8_t> light_table, uint16_t foreground_color) {
    constexpr std::size_t shade_levels = 32;
    if (foreground_color == 0 || foreground_color >= shade_levels ||
        light_table.size() != shade_levels * palette_color_count)
        return nullptr;
    return light_table.data() + static_cast<std::size_t>(foreground_color) * palette_color_count;
}

// Draws a frame's covered pixels through `light_row` when it is set, so a
// button's GAF frame is lit as 3.1c draws it with a colour table.
void blit(
    Surface& surface,
    const formats::gaf::RenderedFrame& frame,
    int x,
    int y,
    const PaletteBytes& palette,
    const uint8_t* light_row = nullptr
) {
    const auto pixel_count = static_cast<std::size_t>(frame.width) * frame.height;
    if (frame.pixels.size() != pixel_count || frame.coverage.size() != pixel_count) {
        throw std::runtime_error("rendered GAF frame has inconsistent buffers");
    }
    // Clip once, then copy covered texels straight into the RGB rows.
    const auto frame_w = static_cast<int64_t>(frame.width);
    const auto frame_h = static_cast<int64_t>(frame.height);
    const auto first_column = std::max<int64_t>(0, -static_cast<int64_t>(x));
    const auto first_row = std::max<int64_t>(0, -static_cast<int64_t>(y));
    const auto end_column = std::min<int64_t>(frame_w, static_cast<int64_t>(surface.width) - x);
    const auto end_row = std::min<int64_t>(frame_h, static_cast<int64_t>(surface.height) - y);
    if (first_column >= end_column || first_row >= end_row ||
        surface.rgb.size() < static_cast<std::size_t>(surface.width) * surface.height * 3U)
        return;
    const auto* pixels = frame.pixels.data();
    const auto* coverage = frame.coverage.data();
    const auto* colors = palette.data();
    for (auto row = first_row; row < end_row; ++row) {
        const auto source = static_cast<std::size_t>(row * frame_w);
        auto* out = surface.rgb.data() + (static_cast<std::size_t>(y + row) * surface.width +
                                          static_cast<std::size_t>(x + first_column)) *
                                             3U;
        for (auto column = first_column; column < end_column; ++column, out += 3) {
            const auto offset = source + static_cast<std::size_t>(column);
            if (coverage[offset] == 0)
                continue;
            const auto source_index =
                light_row != nullptr ? light_row[pixels[offset]] : pixels[offset];
            std::memcpy(
                out, colors + static_cast<std::size_t>(source_index) * palette_entry_bytes, 3U
            );
        }
    }
}

// Type-12 image gadgets stretch the GAF frame into the
// gadget rectangle (32xlogos is 32x32; Color_N is authored 20x20).
void blit_stretched(
    Surface& surface,
    const formats::gaf::RenderedFrame& frame,
    int dest_x,
    int dest_y,
    int dest_w,
    int dest_h,
    const PaletteBytes& palette,
    const uint8_t* light_row = nullptr
) {
    if (dest_w <= 0 || dest_h <= 0) {
        blit(surface, frame, dest_x, dest_y, palette, light_row);
        return;
    }
    const auto pixel_count = static_cast<std::size_t>(frame.width) * frame.height;
    if (frame.pixels.size() != pixel_count || frame.coverage.size() != pixel_count) {
        throw std::runtime_error("rendered GAF frame has inconsistent buffers");
    }
    if (frame.width == 0 || frame.height == 0)
        return;
    for (int row = 0; row < dest_h; ++row) {
        const auto src_y =
            static_cast<uint32_t>(row) * frame.height / static_cast<uint32_t>(dest_h);
        for (int column = 0; column < dest_w; ++column) {
            const auto src_x =
                static_cast<uint32_t>(column) * frame.width / static_cast<uint32_t>(dest_w);
            const auto offset =
                static_cast<std::size_t>(src_y) * frame.width + static_cast<std::size_t>(src_x);
            if (offset >= frame.coverage.size() || frame.coverage[offset] == 0)
                continue;
            const auto source_index =
                light_row != nullptr ? light_row[frame.pixels[offset]] : frame.pixels[offset];
            set_pixel(surface, dest_x + column, dest_y + row, palette_rgb(palette, source_index));
        }
    }
}

void draw_button_text(
    Surface& surface,
    const ui::gui_layout::Gadget& gadget,
    ButtonCondition condition,
    const formats::fnt::Font& selected_font,
    const PaletteBytes& active_palette,
    std::size_t selected_stage
) {
    const auto* fields = std::get_if<ui::gui_layout::ButtonFields>(&gadget.fields);
    if (fields == nullptr || fields->text.empty())
        return;
    const auto text = staged_caption(fields->text, selected_stage);
    if (text.empty())
        return;
    const auto text_width = formats::fnt::measure_text(selected_font, text);
    const auto text_height = formats::fnt::line_height(selected_font);
    const int depressed_offset = condition == ButtonCondition::pressed ? 1 : 0;
    const int rectangle_width = gadget.common.width;
    int x = gadget.common.x + depressed_offset + 1;
    const auto attributes = static_cast<uint32_t>(gadget.common.attributes);
    constexpr uint32_t align_left = 1U;
    constexpr uint32_t align_center = 2U;
    constexpr uint32_t align_right = 4U;
    if ((attributes & align_left) != 0) {
        x = gadget.common.x + depressed_offset + 3;
    } else if ((attributes & align_right) != 0) {
        x = gadget.common.x + std::max(0, rectangle_width - static_cast<int>(text_width) - 3);
    } else if ((attributes & align_center) != 0) {
        x = gadget.common.x + (rectangle_width - static_cast<int>(text_width)) / 2 +
            depressed_offset + 1;
    }
    const int y = gadget.common.y + (gadget.common.height - static_cast<int>(text_height)) / 2 +
                  depressed_offset;

    const auto pixel_count = static_cast<std::size_t>(surface.width) * surface.height;
    std::vector<uint8_t> indices(pixel_count);
    std::vector<uint8_t> coverage(pixel_count);
    const formats::fnt::IndexedSurface target{
        surface.width, surface.height, surface.width, indices, coverage
    };
    (void)formats::fnt::raster_text(target, selected_font, text, x, y);
    for (std::size_t index = 0; index < pixel_count; ++index) {
        if (coverage[index] == 0)
            continue;
        const int pixel_x = static_cast<int>(index % surface.width);
        const int pixel_y = static_cast<int>(index / surface.width);
        set_pixel(surface, pixel_x, pixel_y, palette_rgb(active_palette, indices[index]));
    }
}

void draw_clipped_text(
    Surface& surface,
    const formats::fnt::Font& selected_font,
    std::string_view text,
    int x,
    int y,
    const Rectangle& clip,
    const PaletteBytes& active_palette,
    const uint8_t* light_row = nullptr
) {
    if (text.empty() || clip.left > clip.right || clip.top > clip.bottom)
        return;
    // Glyphs are drawn up to the first one that does not wholly fit the width
    // left; that glyph and the rest of the text are dropped, never clipped.
    const auto available = std::max(0, clip.right - x + 1);
    std::size_t fitted = 0;
    uint32_t used = 0;
    for (const unsigned char character : text) {
        const auto& glyph = selected_font.glyphs[character];
        const uint32_t width = glyph ? glyph->width : 0U;
        if (width > static_cast<uint32_t>(available) - std::min<uint32_t>(used, available))
            break;
        used += width;
        ++fitted;
    }
    text = text.substr(0, fitted);
    const auto pixel_count = static_cast<std::size_t>(surface.width) * surface.height;
    std::vector<uint8_t> indices(pixel_count);
    std::vector<uint8_t> coverage(pixel_count);
    const formats::fnt::IndexedSurface target{
        surface.width, surface.height, surface.width, indices, coverage
    };
    (void)formats::fnt::raster_text(target, selected_font, text, x, y);
    const int left = std::max(clip.left, 0);
    const int right = std::min(clip.right, static_cast<int>(surface.width) - 1);
    const int top = std::max(clip.top, 0);
    const int bottom = std::min(clip.bottom, static_cast<int>(surface.height) - 1);
    for (int row = top; row <= bottom; ++row) {
        for (int column = left; column <= right; ++column) {
            const auto offset =
                static_cast<std::size_t>(row) * surface.width + static_cast<std::size_t>(column);
            if (coverage[offset] == 0)
                continue;
            const auto source_index =
                light_row != nullptr ? light_row[indices[offset]] : indices[offset];
            set_pixel(surface, column, row, palette_rgb(active_palette, source_index));
        }
    }
}

void light_rectangle(
    Surface& surface,
    const Rectangle& rectangle,
    const PaletteBytes& palette,
    std::span<const uint8_t> light_table,
    std::size_t level
) {
    constexpr std::size_t shade_levels = 32;
    if (light_table.size() != shade_levels * palette_color_count || level >= shade_levels)
        return;
    const int left = std::max(rectangle.left, 0);
    const int right = std::min(rectangle.right, static_cast<int>(surface.width) - 1);
    const int top = std::max(rectangle.top, 0);
    const int bottom = std::min(rectangle.bottom, static_cast<int>(surface.height) - 1);
    for (int y = top; y <= bottom; ++y)
        for (int x = left; x <= right; ++x) {
            const auto offset =
                (static_cast<std::size_t>(y) * surface.width + static_cast<std::size_t>(x)) * 3U;
            uint8_t source = 0;
            for (std::size_t index = 0; index < palette_color_count; ++index) {
                const auto color = index * palette_entry_bytes;
                if (surface.rgb[offset] == palette[color] &&
                    surface.rgb[offset + 1] == palette[color + 1] &&
                    surface.rgb[offset + 2] == palette[color + 2]) {
                    source = static_cast<uint8_t>(index);
                    break;
                }
            }
            const auto transformed = light_table[level * palette_color_count + source];
            const auto rgb = palette_rgb(palette, transformed);
            std::copy(
                rgb.begin(), rgb.end(), surface.rgb.begin() + static_cast<std::ptrdiff_t>(offset)
            );
        }
}

// Darkens drawn colours through one row of the shade table, in the palette
// the table indexes. A colour is read back as the first palette entry that
// holds it, else the nearest by summed squared channel difference; entries
// 0x80..0xFF index the row as signed bytes and so read the row before. Each
// colour's darkened colour is kept in a direct-mapped table, so a colour met
// again, as most of a button's pixels are, is not searched for again while
// the table keeps it.
class GrayedShade {
  public:

    /// Sets the darkening up; it does nothing unless usable().
    ///
    /// @param palette the palette the shade table indexes
    /// @param shade_table the 32x256 shade table
    /// @param row the table row the colours are darkened through, 1 to 31
    GrayedShade(const PaletteBytes& palette, std::span<const uint8_t> shade_table, std::size_t row)
        : palette_(palette), shade_table_(shade_table), row_(row) {}

    /// Tells whether the table is whole and the row within it.
    ///
    /// @return true when darken() changes colours
    [[nodiscard]] bool usable() const {
        return shade_table_.size() == shade_table_rows * palette_color_count && row_ != 0 &&
               row_ < shade_table_rows;
    }

    /// Darkens one drawn colour in place.
    ///
    /// @param[in,out] rgb the colour's red, green and blue bytes
    void darken(uint8_t* rgb) {
        const uint32_t color = static_cast<uint32_t>(rgb[0]) << 16 |
                               static_cast<uint32_t>(rgb[1]) << 8 | static_cast<uint32_t>(rgb[2]);
        // A multiplicative hash of the 24-bit colour; its top slot_bits bits
        // pick the slot.
        Slot& slot = slots_[(color * color_hash_multiplier) >> (32U - slot_bits)];
        if (!slot.filled || slot.color != color)
            slot = {color, darkened(rgb), true};
        rgb[0] = slot.darkened[0];
        rgb[1] = slot.darkened[1];
        rgb[2] = slot.darkened[2];
    }

  private:

    static constexpr uint32_t slot_bits = 12;
    static constexpr uint32_t color_hash_multiplier = 2654435761U;

    struct Slot {
        uint32_t color{};
        std::array<uint8_t, 3> darkened{};
        bool filled{};
    };

    /// Searches the palette for a colour and darkens the entry found.
    [[nodiscard]] std::array<uint8_t, 3> darkened(const uint8_t* rgb) const {
        std::size_t source = 0;
        int32_t nearest = std::numeric_limits<int32_t>::max();
        for (std::size_t index = 0; index < palette_color_count && nearest != 0; ++index) {
            const auto entry = index * palette_entry_bytes;
            int32_t distance = 0;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const int32_t difference = static_cast<int32_t>(rgb[channel]) -
                                           static_cast<int32_t>(palette_[entry + channel]);
                distance += difference * difference;
            }
            if (distance < nearest) {
                nearest = distance;
                source = index;
            }
        }
        const auto signed_row = source >= 0x80 ? row_ - 1 : row_;
        return palette_rgb(palette_, shade_table_[signed_row * palette_color_count + source]);
    }

    const PaletteBytes& palette_;
    std::span<const uint8_t> shade_table_;
    std::size_t row_{};
    std::vector<Slot> slots_ = std::vector<Slot>(std::size_t{1} << slot_bits);
};

// Darkens a rectangle's colours through a grayed-out button's shade.
void shade_rectangle(Surface& surface, const Rectangle& rectangle, GrayedShade& shade) {
    if (!shade.usable())
        return;
    const int left = std::max(rectangle.left, 0);
    const int right = std::min(rectangle.right, static_cast<int>(surface.width) - 1);
    const int top = std::max(rectangle.top, 0);
    const int bottom = std::min(rectangle.bottom, static_cast<int>(surface.height) - 1);
    if (left > right)
        return;
    for (int y = top; y <= bottom; ++y) {
        uint8_t* pixel =
            surface.rgb.data() +
            (static_cast<std::size_t>(y) * surface.width + static_cast<std::size_t>(left)) * 3U;
        for (int x = left; x <= right; ++x, pixel += 3)
            shade.darken(pixel);
    }
}

[[nodiscard]] int aligned_text_x(
    const ui::gui_layout::Gadget& gadget,
    std::string_view text,
    const formats::fnt::Font& selected_font,
    int left_inset,
    int right_inset
) {
    const auto width = static_cast<int>(formats::fnt::measure_text(selected_font, text));
    const auto attributes = static_cast<uint32_t>(gadget.common.attributes);
    constexpr uint32_t align_left = 1U;
    constexpr uint32_t align_center = 2U;
    constexpr uint32_t align_right = 4U;
    if ((attributes & align_left) != 0)
        return gadget.common.x + left_inset;
    if ((attributes & align_right) != 0)
        return gadget.common.x + gadget.common.width - width - right_inset;
    if ((attributes & align_center) != 0)
        return gadget.common.x + (gadget.common.width - width) / 2;
    return gadget.common.x + left_inset;
}

[[nodiscard]] const formats::fnt::Font& label_font_of(const ScreenResources& resources) {
    return resources.label_font.glyphs['I'] ? resources.label_font : resources.font;
}

void draw_label(
    Surface& surface,
    const ui::gui_layout::Gadget& gadget,
    const formats::fnt::Font& selected_font,
    const PaletteBytes& active_palette,
    std::span<const uint8_t> light_table
) {
    const auto* fields = std::get_if<ui::gui_layout::LabelFields>(&gadget.fields);
    if (fields == nullptr || fields->text.empty())
        return;
    const Rectangle clip = gadget_rectangle(gadget);
    // The authored background colour fills the label's rectangle first.
    if (gadget.common.background_color != 0) {
        fill(
            surface,
            clip,
            palette_rgb(active_palette, static_cast<uint8_t>(gadget.common.background_color))
        );
    }
    const auto* light_row = light_row_for(light_table, gadget.common.foreground_color);
    const int line_height = formats::fnt::line_height(selected_font);
    // The label drawing switches to wrapped text only when two line heights
    // fit strictly inside the gadget. Wrapped text advances by line_height+2.
    if (line_height * 2 >= static_cast<int>(gadget.common.height) - 1) {
        const int x = aligned_text_x(gadget, fields->text, selected_font, 0, 0);
        // The single-line path limits the text by width only, so a label
        // authored with no height (MAINMENU's DebugString) still draws a line.
        Rectangle line_clip = clip;
        line_clip.bottom = std::max(clip.bottom, gadget.common.y + line_height - 1);
        draw_clipped_text(
            surface,
            selected_font,
            fields->text,
            x,
            gadget.common.y,
            line_clip,
            active_palette,
            light_row
        );
        return;
    }
    std::size_t start = 0;
    int y = gadget.common.y;
    const int fixed_x = aligned_text_x(gadget, fields->text, selected_font, 0, 0);
    while (start < fields->text.size() && y <= clip.bottom) {
        std::size_t scan = start;
        std::size_t last_break = std::string_view::npos;
        std::size_t end = start;
        while (scan < fields->text.size()) {
            if (fields->text[scan] == '\r') {
                end = scan;
                break;
            }
            if (fields->text[scan] == ' ')
                last_break = scan + 1;
            const auto candidate = std::string_view(fields->text).substr(start, scan - start + 1);
            if (formats::fnt::measure_text(selected_font, candidate) >
                static_cast<uint32_t>(std::max(0, static_cast<int>(gadget.common.width)))) {
                end = last_break != std::string_view::npos ? last_break : scan;
                if (end == start)
                    end = scan + 1;
                break;
            }
            ++scan;
            end = scan;
        }
        auto line = std::string_view(fields->text).substr(start, end - start);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r'))
            line.remove_suffix(1);
        draw_clipped_text(
            surface, selected_font, line, fixed_x, y, clip, active_palette, light_row
        );
        start = end;
        while (start < fields->text.size() &&
               (fields->text[start] == ' ' || fields->text[start] == '\r'))
            ++start;
        y += line_height + 2;
    }
}

void draw_list_box(
    Surface& surface,
    const ui::gui_layout::Gadget& gadget,
    const ListPresentation& list,
    const formats::fnt::Font& selected_font,
    const PaletteBytes& active_palette,
    std::span<const uint8_t> light_table
) {
    const auto* fields = std::get_if<ui::gui_layout::ListBoxFields>(&gadget.fields);
    if (fields == nullptr)
        return;
    const int font_line_height = formats::fnt::line_height(selected_font);
    // The list drawing uses line_height+1 when the authored itemheight is zero.
    const int item_height = fields->item_height == 0 ? font_line_height + 1 : fields->item_height;
    if (item_height <= 0 || list.first_visible >= list.items.size())
        return;
    const Rectangle clip{
        gadget.common.x + 2,
        gadget.common.y + 2,
        gadget.common.x + gadget.common.width - 1,
        gadget.common.y + gadget.common.height - 1
    };
    int y = gadget.common.y + 2;
    int remaining_height = gadget.common.height;
    for (std::size_t index = list.first_visible;
         index < list.items.size() && remaining_height >= font_line_height;
         ++index, y += item_height, remaining_height -= item_height) {
        const auto& text = list.items[index];
        const int x = aligned_text_x(gadget, text, selected_font, 2, 2);
        const Rectangle row_clip{clip.left, y, clip.right, std::min(clip.bottom, y + item_height)};
        draw_clipped_text(surface, selected_font, text, x, y, row_clip, active_palette);
        // A text list lights the whole selected row at light level 0x1E after
        // drawing it.
        if (list.selected && *list.selected == index)
            light_rectangle(surface, row_clip, active_palette, light_table, 0x1e);
    }
}

[[nodiscard]] Surface checked_background(const Image& image) {
    Surface result{image.width, image.height, image.rgb};
    const auto expected_rgb = static_cast<uint64_t>(result.width) * result.height * 3U;
    if (expected_rgb > std::numeric_limits<std::size_t>::max() ||
        result.rgb.size() != static_cast<std::size_t>(expected_rgb)) {
        throw std::runtime_error("GUI background has inconsistent RGB data");
    }
    return result;
}

ScreenResources load_screen_with_layout(
    AssetStore& assets, const ScreenAssetNames& names, std::span<const uint8_t> layout
) {
    ScreenResources result;
    if (!names.background.empty())
        result.background = decode_pcx(assets.read(names.background).bytes);

    const auto palette = assets.read(names.palette).bytes;
    if (palette.size() != result.gui_palette.size()) {
        throw std::runtime_error("GUI palette must contain 256 four-byte RGB entries");
    }
    std::copy(palette.begin(), palette.end(), result.gui_palette.begin());

    const auto parsed_layout = ui::gui_layout::parse(layout);
    if (!parsed_layout.ok()) {
        throw std::runtime_error(
            "cannot parse GUI layout '" + names.layout + "': " + parsed_layout.error->message
        );
    }
    result.layout = *parsed_layout.layout;

    if (!names.sprites.empty()) {
        const auto parsed_sprites = formats::gaf::parse(assets.read(names.sprites).bytes);
        if (!parsed_sprites.ok()) {
            throw std::runtime_error(
                "cannot parse GUI sprites '" + names.sprites + "': " + parsed_sprites.error->message
            );
        }
        result.sprites = *parsed_sprites.archive;
    }
    if (!names.shared_sprites.empty()) {
        const auto parsed_shared = formats::gaf::parse(assets.read(names.shared_sprites).bytes);
        if (!parsed_shared.ok()) {
            throw std::runtime_error(
                "cannot parse shared GUI sprites '" + names.shared_sprites +
                "': " + parsed_shared.error->message
            );
        }
        result.shared_sprites = *parsed_shared.archive;
    }
    result.font = formats::fnt::load_gaf(assets, default_gui_font);
    result.label_font = formats::fnt::load_gaf(assets, label_gui_font);
    const auto parsed_global = formats::gaf::parse(assets.read(global_logo_sprites).bytes);
    if (!parsed_global.ok())
        throw std::runtime_error(
            "cannot parse global LOGOS.GAF sprites: " + parsed_global.error->message
        );
    result.global_sprites = *parsed_global.archive;
    result.light_table = assets.read(default_light_table).bytes;
    if (result.light_table.size() != 32U * palette_color_count)
        throw std::runtime_error("PALETTE.LHT must contain 32x256 entries");
    // Grayed-out buttons are darkened through PALETTE.SHD; without a whole
    // table they are drawn undarkened.
    if (assets.file_size(default_shade_table) == shade_table_rows * palette_color_count)
        result.shade_table = assets.read(default_shade_table).bytes;
    if (assets.file_size(game_palette_file) == result.gui_palette.size()) {
        const auto game = assets.read(game_palette_file).bytes;
        result.game_palette.emplace();
        std::copy(game.begin(), game.end(), result.game_palette->begin());
    }
    bind_screen_buttons(result, 0);
    return result;
}

} // namespace

std::string_view staged_caption(std::string_view text, std::size_t stage) noexcept {
    if (text.find('|') == std::string_view::npos)
        return text;
    for (std::size_t skipped = 0; skipped < stage; ++skipped) {
        const auto separator = text.find('|');
        if (separator == std::string_view::npos)
            return {};
        text.remove_prefix(separator + 1);
    }
    return text.substr(0, text.find('|'));
}

void bind_screen_buttons(ScreenResources& resources, std::size_t first) {
    // The first panel draw commits the selected GAF frame dimensions back into
    // each type-1 gadget without the checkbox or text_list attribute (NEWCAMP
    // Side/Easy keep authored 159x49 / 107x41). Preserve that mutation so
    // drawing and hit testing share the same rectangles.
    constexpr uint32_t skip_default_button_gaf =
        ui::gui_layout::attribute::checkbox | ui::gui_layout::attribute::text_list;
    for (auto index = first; index < resources.layout.gadgets.size(); ++index) {
        auto& gadget = resources.layout.gadgets[index];
        // The first panel draw clears a type-1 gadget's authored colorf and
        // colorb before resolving its GAF sequence, and a label's authored
        // colorf (keeping its colorb), so later drawing takes the normal path
        // unless a color table is installed at run time.
        if (gadget.common.type == ui::gui_layout::GadgetType::label) {
            gadget.common.foreground_color = 0;
            continue;
        }
        if (gadget.common.type != ui::gui_layout::GadgetType::button)
            continue;
        gadget.common.foreground_color = 0;
        gadget.common.background_color = 0;
        if ((static_cast<uint32_t>(gadget.common.attributes) & skip_default_button_gaf) != 0)
            continue;
        const auto binding = resolve_button_sprite(resources, gadget);
        if (binding.sequence == nullptr || binding.base_frame >= binding.sequence->frames.size())
            continue;
        const auto& frame = binding.sequence->frames[binding.base_frame];
        gadget.common.width = static_cast<int16_t>(frame.width);
        gadget.common.height = static_cast<int16_t>(frame.height);
    }
}

ScreenResources load_screen(AssetStore& assets, const ScreenAssetNames& names) {
    return load_screen_with_layout(assets, names, assets.read(names.layout).bytes);
}

namespace {

/// Reads MAINMENU.GUI from one mounted archive.
///
/// @param assets the asset store
/// @param archive the archive's mount path
/// @return the bytes, or nullopt when the archive is not mounted or holds no
///         such file
std::optional<std::vector<uint8_t>>
main_menu_layout_in(const AssetStore& assets, const std::filesystem::path& archive) {
    const auto mounts = assets.mount_paths();
    for (std::size_t index = 0; index < mounts.size(); ++index) {
        if (mounts[index] != archive)
            continue;
        const auto& mounted = assets.mounted(index);
        const auto node = mounted.lookup(main_menu_layout);
        if (!node || mounted.nodes()[*node].directory())
            return std::nullopt;
        return mounted.read_node(*node);
    }
    return std::nullopt;
}

} // namespace

MainMenuResources load_main_menu(AssetStore& assets, MainMenuLayout layout) {
    auto gui = assets.read(main_menu_layout);
    // With no overlay, an archived layout goes with the archived background
    // it was drawn for, when that archive holds one.
    if (layout == MainMenuLayout::base_game && gui.archived) {
        const auto background = assets.providing_archive(main_menu_background);
        if (background && *background != gui.source)
            if (auto bytes = main_menu_layout_in(assets, *background))
                gui = {std::move(*bytes), *background, true};
    }
    auto result = load_screen_with_layout(
        assets,
        {std::string(main_menu_layout),
         std::string(main_menu_background),
         std::string(main_menu_palette),
         std::string(main_menu_sprites),
         std::string(shared_gui_sprites)},
        gui.bytes
    );
    if (result.background.width != 640 || result.background.height != 480) {
        throw std::runtime_error("FrontendX must be the game's 640x480 image");
    }
    return result;
}

Surface render_screen(
    const ScreenResources& resources,
    std::span<const ButtonPresentation> presentation,
    std::span<const ListPresentation> lists
) {
    Surface result = checked_background(resources.background);
    const auto expected_rgb = static_cast<uint64_t>(result.width) * result.height * 3U;
    if (expected_rgb > std::numeric_limits<std::size_t>::max() ||
        result.rgb.size() != static_cast<std::size_t>(expected_rgb)) {
        throw std::runtime_error("main-menu background has inconsistent RGB data");
    }

    const PaletteBytes& active_palette = resources.background.palette.has_value()
                                             ? *resources.background.palette
                                             : resources.gui_palette;
    // The screen setup remaps as remap(guipal, active FrontendX palette).
    const auto gui_to_active = remap_palette(resources.gui_palette, active_palette);
    const auto gui_color = [&](uint8_t index) {
        return palette_rgb(active_palette, gui_to_active[index]);
    };
    // Grayed-out buttons are darkened in the game palette the shade table
    // indexes: the screen bitmap's, or PALETTE.PAL's for a screen drawn in the
    // GUI palette alone. The first one darkened sets up the shade the rest of
    // the render shares.
    const PaletteBytes* shade_palette =
        resources.background.palette.has_value() ? &*resources.background.palette
        : resources.game_palette.has_value()     ? &*resources.game_palette
                                                 : nullptr;
    std::optional<GrayedShade> grayed_shade;

    for (const auto& gadget : resources.layout.gadgets) {
        if (gadget.common.active == 0)
            continue;
        const auto* state = presentation_for(gadget.common.name, presentation);
        if (gadget.common.type == ui::gui_layout::GadgetType::label) {
            draw_label(
                result, gadget, label_font_of(resources), active_palette, resources.light_table
            );
            continue;
        }
        if (gadget.common.type == ui::gui_layout::GadgetType::list_box) {
            if (const auto* list = list_for(gadget.common.name, lists)) {
                draw_list_box(
                    result, gadget, *list, resources.font, active_palette, resources.light_table
                );
            }
            continue;
        }
        // A hot surface given an image and frame at run time (the setup rows'
        // colour and allegiance) shows that frame over its rectangle.
        if (gadget.common.type == ui::gui_layout::GadgetType::hot_surface && state != nullptr &&
            state->sprite.has_value() && state->gaf_frame.has_value()) {
            const auto binding = resolve_button_sprite(resources, gadget, state);
            if (binding.sequence == nullptr)
                throw std::runtime_error("runtime image sprite sequence is missing");
            const auto frame_index = *state->gaf_frame;
            if (frame_index >= binding.sequence->frames.size())
                throw std::runtime_error("runtime image frame index is out of range");
            const auto rendered =
                formats::gaf::render_normal(binding.sequence->frames[frame_index]);
            if (!rendered.ok())
                throw std::runtime_error("cannot render runtime image: " + rendered.error->message);
            blit_stretched(
                result,
                *rendered.frame,
                gadget.common.x,
                gadget.common.y,
                gadget.common.width,
                gadget.common.height,
                active_palette
            );
            continue;
        }
        if (gadget.common.type != ui::gui_layout::GadgetType::button)
            continue;
        const auto condition = state == nullptr ? ButtonCondition::normal : state->condition;
        if (condition == ButtonCondition::hidden)
            continue;
        const auto text_stage =
            state != nullptr && state->text_stage.has_value() ? *state->text_stage : 0;
        const auto binding = resolve_button_sprite(resources, gadget, state);
        const auto* sequence = binding.sequence;
        constexpr uint32_t invisible_hit_attribute = 0x400U;
        const auto* button_fields = std::get_if<ui::gui_layout::ButtonFields>(&gadget.fields);
        const bool portrait_hit =
            (static_cast<uint32_t>(gadget.common.attributes) & invisible_hit_attribute) != 0 &&
            (button_fields == nullptr || button_fields->text.empty());
        if (sequence != nullptr && !sequence->frames.empty()) {
            // A button left with its authored colour (a merged sub-panel's) is
            // drawn lit through the light table, as 3.1c draws it.
            const auto* light_row =
                light_row_for(resources.light_table, gadget.common.foreground_color);
            std::size_t relative_frame = 0;
            if (state != nullptr && state->gaf_frame.has_value()) {
                relative_frame = *state->gaf_frame;
            } else if (condition == ButtonCondition::pressed) {
                relative_frame = 1;
            } else if (
                condition == ButtonCondition::disabled &&
                binding.base_frame < sequence->frames.size()
            ) {
                // A grayed-out button shows the frame two past its normal one,
                // or the sequence's last when it has fewer.
                relative_frame = std::min(
                    grayed_frame_offset, sequence->frames.size() - 1U - binding.base_frame
                );
            }
            const auto index = binding.base_frame + relative_frame;
            if (index >= sequence->frames.size()) {
                throw std::runtime_error("resolved GAF frame index is out of range");
            }
            const auto rendered = formats::gaf::render_normal(sequence->frames[index]);
            if (!rendered.ok()) {
                throw std::runtime_error(
                    "cannot render MAINMENU.GAF sequence '" + sequence->name +
                    "': " + rendered.error->message
                );
            }
            if (portrait_hit && gadget.common.width > 0 && gadget.common.height > 0) {
                blit_stretched(
                    result,
                    *rendered.frame,
                    gadget.common.x,
                    gadget.common.y,
                    gadget.common.width,
                    gadget.common.height,
                    active_palette,
                    light_row
                );
            } else {
                blit(
                    result,
                    *rendered.frame,
                    gadget.common.x,
                    gadget.common.y,
                    active_palette,
                    light_row
                );
            }
            auto resolved_gadget = gadget;
            resolved_gadget.common.width = static_cast<int16_t>(rendered.frame->width);
            resolved_gadget.common.height = static_cast<int16_t>(rendered.frame->height);
            draw_button_text(
                result, resolved_gadget, condition, resources.font, active_palette, text_stage
            );
            // A grayed-out button is darkened too, except one that cycles its
            // frames or plain CHECKBOX art.
            constexpr uint32_t checkbox_art_attribute = 0x80U;
            const auto attributes = static_cast<uint32_t>(gadget.common.attributes);
            const bool staged = button_fields != nullptr && button_fields->stages != 0;
            const bool shaded =
                (attributes & ui::gui_layout::attribute::cycle_frames) == 0 &&
                (staged || (attributes & ui::gui_layout::attribute::scroll_step_mask) != 0 ||
                 (attributes & checkbox_art_attribute) == 0);
            if (condition == ButtonCondition::disabled && shaded && shade_palette != nullptr) {
                if (!grayed_shade)
                    grayed_shade.emplace(*shade_palette, resources.shade_table, grayed_shade_row);
                shade_rectangle(result, gadget_rectangle(resolved_gadget), *grayed_shade);
            }
            continue;
        }
        if (portrait_hit)
            continue;

        // A button is filled with map entry 20, or 19 when grayed, then given
        // its raised or sunken bevel.
        const bool disabled = condition == ButtonCondition::disabled;
        const bool sunken = condition == ButtonCondition::pressed || disabled;
        const auto fill_index = disabled ? palette_disabled_fill : palette_button_fill;
        // A raised button has map[17] on its top and left edges and map[0] on
        // its bottom and right; a pressed or disabled one has map[0] on top and
        // left and map[17] below and right, map[19] there when disabled.
        const auto top_left = gui_color(sunken ? palette_black : palette_bevel_shadow);
        const auto bottom_right = gui_color(
            disabled ? palette_disabled_fill : (sunken ? palette_bevel_shadow : palette_black)
        );
        const auto rectangle = gadget_rectangle(gadget);
        fill(result, rectangle, gui_color(fill_index));
        bevel(result, rectangle, top_left, bottom_right);
        draw_button_text(result, gadget, condition, resources.font, active_palette, text_stage);
    }
    return result;
}

Surface render_main_menu(
    const MainMenuResources& resources, std::span<const ButtonPresentation> presentation
) {
    return render_screen(resources, presentation);
}

namespace {

constexpr int32_t spark_stride = menu_spark_width;

int lcg_rand(uint32_t& seed) {
    seed = seed * 0x343fdu + 0x269ec3u;
    return static_cast<int>((seed >> 16) & 0x7fffu);
}

bool contour_pixel(uint8_t index) {
    return (index & 0x0fu) >= 0x0du;
}

int8_t horizontal_delta(int16_t x) {
    // Odd x steps by -3, even x by 3.
    return (x & 1) != 0 ? static_cast<int8_t>(-3) : static_cast<int8_t>(3);
}

int8_t vertical_delta(int16_t y) {
    // Odd y steps by 3, even y by -3.
    return (y & 1) != 0 ? static_cast<int8_t>(3) : static_cast<int8_t>(-3);
}

// A spark born moving vertically takes the opposite sense of a turning one:
// even y => 3, odd => -3.
int8_t spawn_vertical_delta(int16_t y) {
    return (y & 1) != 0 ? static_cast<int8_t>(-3) : static_cast<int8_t>(3);
}

} // namespace

void reset_menu_sparks(MenuSparks& state, const Image& background) {
    state = {};
    state.rand_seed = 1;
    const auto count =
        static_cast<std::size_t>(menu_spark_width) * static_cast<std::size_t>(menu_spark_height);
    if (background.indices.size() >= count)
        state.dest = background.indices;
    else
        state.dest.assign(count, 0);
}

void step_menu_sparks(MenuSparks& state, Surface& surface, const Image& background) {
    const auto count =
        static_cast<std::size_t>(menu_spark_width) * static_cast<std::size_t>(menu_spark_height);
    if (state.dest.size() != count)
        reset_menu_sparks(state, background);
    if (state.dest.size() != count)
        return;
    const auto* source = background.indices.size() >= count ? background.indices.data()
                                                            : static_cast<const uint8_t*>(nullptr);
    auto* dest = state.dest.data();
    for (auto& spark : state.sparks) {
        if (spark.active == 0) {
            const auto x = static_cast<int16_t>(lcg_rand(state.rand_seed) % menu_spark_width);
            const auto y =
                static_cast<int16_t>(lcg_rand(state.rand_seed) % menu_spark_spawn_height);
            spark.x = x;
            spark.y = y;
            spark.offset = static_cast<int32_t>(y) * spark_stride + x;
            if (!contour_pixel(dest[static_cast<std::size_t>(spark.offset)]))
                continue;
            spark.active = 1;
            spark.life = static_cast<uint8_t>(static_cast<uint8_t>(lcg_rand(state.rand_seed)) + 1u);
            spark.turn = static_cast<uint8_t>((lcg_rand(state.rand_seed) & 0x1f) + 1);
            if ((spark.life & 1) != 0) {
                spark.dy = spawn_vertical_delta(spark.y);
                spark.dx = 0;
            } else {
                spark.dx = horizontal_delta(spark.x);
                spark.dy = 0;
            }
            continue;
        }
        const auto old = static_cast<std::size_t>(spark.offset);
        if (source != nullptr && old < count)
            dest[old] = source[old];
        spark.x = static_cast<int16_t>(spark.x + spark.dx);
        spark.y = static_cast<int16_t>(spark.y + spark.dy);
        if (spark.x < 0 || spark.x >= menu_spark_width || spark.y < 0 ||
            spark.y >= menu_spark_height || spark.life == 0) {
            spark.active = 0;
            continue;
        }
        --spark.life;
        spark.offset += spark.dx;
        if (spark.dy != 0)
            spark.offset += static_cast<int32_t>(spark.dy) * spark_stride;
        if (spark.offset < 0 || static_cast<std::size_t>(spark.offset) >= count ||
            !contour_pixel(dest[static_cast<std::size_t>(spark.offset)])) {
            spark.active = 0;
            continue;
        }
        dest[static_cast<std::size_t>(spark.offset)] = menu_spark_pixel;
        if (spark.turn == 0) {
            if (spark.dy != 0) {
                spark.dy = 0;
                spark.dx = horizontal_delta(spark.x);
            } else {
                spark.dy = vertical_delta(spark.y);
                spark.dx = 0;
            }
            spark.turn = static_cast<uint8_t>((lcg_rand(state.rand_seed) & 0x0f) + 1);
        } else {
            --spark.turn;
        }
    }
    if (surface.width < static_cast<uint32_t>(menu_spark_width) ||
        surface.height < static_cast<uint32_t>(menu_spark_height) ||
        surface.rgb.size() < count * 3U)
        return;
    std::array<uint8_t, 3> color{0xff, 0xff, 0xff};
    if (background.palette.has_value()) {
        const auto pal = menu_spark_pixel * 4U;
        if (pal + 2 < background.palette->size()) {
            color = {
                (*background.palette)[pal],
                (*background.palette)[pal + 1],
                (*background.palette)[pal + 2]
            };
        }
    }
    for (const auto& spark : state.sparks) {
        if (spark.active == 0 || spark.offset < 0 ||
            static_cast<std::size_t>(spark.offset) >= count ||
            dest[static_cast<std::size_t>(spark.offset)] != menu_spark_pixel)
            continue;
        if (spark.x < 0 || spark.y < 0 || spark.x >= menu_spark_width ||
            spark.y >= menu_spark_height)
            continue;
        auto* pixel = surface.rgb.data() + (static_cast<std::size_t>(spark.y) * surface.width +
                                            static_cast<std::size_t>(spark.x)) *
                                               3U;
        pixel[0] = color[0];
        pixel[1] = color[1];
        pixel[2] = color[2];
    }
}

} // namespace oa::ui::frontend_renderer

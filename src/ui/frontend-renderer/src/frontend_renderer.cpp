// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/frontend_renderer.hpp"
#include "oa/base/text/line_break.hpp"
#include "oa/ui/frontend_renderer/game_text.hpp"
#include "oa/ui/decoded.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/languages/translation.hpp"

#include "oa/ui/frontend_renderer/gadget_draw.hpp"
#include "oa/ui/frontend_renderer/scroll_bars.hpp"
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
#include <span>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oa::ui::frontend_renderer {
namespace {

constexpr std::string_view main_menu_layout_file = "mainmenu.gui";
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
constexpr uint8_t palette_underline = 2; // a caption's quick-key underline
constexpr uint8_t palette_bevel_shadow = 17;
constexpr uint8_t palette_disabled_fill = 19;
constexpr uint8_t palette_button_fill = 20;
// A grayed-out button's art frame sits this far past its normal frame, and
// the button is then darkened through this row of the shade table.
constexpr std::size_t grayed_frame_offset = 2;
constexpr std::size_t grayed_shade_row = 0x20 - 0x14;
constexpr std::size_t shade_table_rows = 32;
// A button's caption shows no wider than its record less this many pixels.
constexpr int32_t caption_margin = 6;

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
    // A checkbox is filled as a bevel, with no BUTTONS0 frame. A hold button
    // (text_list) that has no sequence of its own, such as NEWGAME's Arm and
    // Core, still takes BUTTONS0: its normal frame while it is up, the next
    // frame while it is down.
    constexpr uint32_t skip_default_button_gaf = ui::gui_layout::attribute::checkbox;
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
    // The default art is commongui.gaf's: the shared archive of a frontend
    // screen, the screen's own archive of an in-game panel, which holds
    // commongui.gaf with the side's interface art shared.
    const auto* fallback = sequence_for(resources.shared_sprites, fallback_name);
    if (fallback == nullptr || fallback->frames.empty())
        fallback = sequence_for(resources.sprites, fallback_name);
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

// A GAF frame's palette indices and coverage as formats::gaf::render_normal
// draws them.
struct FramePixels {
    uint32_t width = 0;
    uint32_t height = 0;
    std::span<const uint8_t> pixels;
    std::span<const uint8_t> coverage;
};

// Tells whether a frame is drawn as its own pixels: render_normal copies a
// frame without layers that holds all its pixels as they are.
[[nodiscard]] bool drawn_as_held(const formats::gaf::Frame& frame) noexcept {
    const auto count = static_cast<std::size_t>(frame.width) * frame.height;
    return frame.layers.empty() && frame.layer_count == 0 && count != 0 &&
           count <= formats::gaf::limit::pixels_per_frame && frame.pixels.size() == count &&
           frame.coverage.size() == count;
}

// Gives a frame's pixels as render_normal draws them: those of a frame drawn
// as its own pixels, or those of the frame drawn into `rendered`. None when
// the frame cannot be drawn, its error then in `rendered`.
[[nodiscard]] std::optional<FramePixels>
frame_pixels(const formats::gaf::Frame& frame, formats::gaf::RenderResult& rendered) {
    if (drawn_as_held(frame))
        return FramePixels{frame.width, frame.height, frame.pixels, frame.coverage};
    rendered = formats::gaf::render_normal(frame);
    if (!rendered.ok())
        return std::nullopt;
    const auto& drawn = *rendered.frame;
    return FramePixels{drawn.width, drawn.height, drawn.pixels, drawn.coverage};
}

// Draws a frame's covered pixels through `light_row` when it is set, so a
// button's GAF frame is lit as 3.1c draws it with a colour table.
void blit(
    Surface& surface,
    const FramePixels& frame,
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
    const FramePixels& frame,
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

// Cuts characters from a caption's end until it is no wider than `limit`
// pixels, its modern runs drawn at most `most_size`, or nothing is left; a
// character is a byte of the game's code page, or a whole UTF-8 sequence
// where the game text is UTF-8.
std::string_view fitted_caption(
    const formats::fnt::Font& font,
    std::string_view text,
    int32_t limit,
    bool game_text,
    int32_t most_size = present::game_font_text_size
) {
    const bool utf8 = present::game_text_settings().utf8;
    while (!text.empty() && measure_fnt_game_text(font, text, game_text, most_size) > limit)
        text = text.substr(0, utf8 ? present::last_character_start(text) : text.size() - 1);
    return text;
}

// Draws a button's caption, and the underline under its quick key when the
// caption is centred and the button is not grayed. A caption wider than the
// record less caption_margin first draws its modern text smaller, down to
// present::lowest_text_size, until it fits (fitted_fnt_text_size). The
// caption, or the first stage's of a button with stages, that the game font
// draws, or that is still too wide, then loses characters from its end until
// it fits. A side name (game text) skips that stage translation and is fitted
// to the record's inclusive width, so the table's spelling stays whole when
// it fits the button and two names that share a prefix stay distinct. It is
// placed as a caption that fits: centred, a long label shows its start inside
// the button rather than running past both its sides.
void draw_button_text(
    Surface& surface,
    const ui::gui_layout::Gadget& gadget,
    int32_t record_width,
    ButtonCondition condition,
    const formats::fnt::Font& selected_font,
    const PaletteBytes& active_palette,
    std::size_t selected_stage,
    char quick_key,
    const std::array<uint8_t, 3>& underline,
    bool game_text
) {
    const auto* fields = std::get_if<ui::gui_layout::ButtonFields>(&gadget.fields);
    if (fields == nullptr || fields->text.empty())
        return;
    auto text = staged_caption(fields->text, selected_stage);
    const bool fitted = text.data() == fields->text.data();
    const auto translated =
        game_text ? std::nullopt : translated_stage_caption(*fields, selected_stage);
    if (translated)
        text = *translated;
    // Inclusive width: the ordinary margin would cut two side names that share
    // a prefix down to that prefix on a button the size of one SIDEx frame.
    const int32_t limit = game_text ? record_width - 1 : record_width - caption_margin;
    const int32_t most_size = fitted_fnt_text_size(selected_font, text, limit, game_text);
    if (fitted)
        text = fitted_caption(selected_font, text, limit, game_text, most_size);
    if (text.empty())
        return;
    const auto text_width = measure_fnt_game_text(selected_font, text, game_text, most_size);
    const auto text_height = formats::fnt::line_height(selected_font);
    const int depressed_offset = condition == ButtonCondition::pressed ? 1 : 0;
    // A caption is placed within the record's inclusive rectangle, whose far
    // edges lie at x + width - 1 and y + height - 1: centred, it sits half the
    // space that rectangle's span leaves, rounded toward zero.
    const int rectangle_span_x = gadget.common.width - 1;
    const int rectangle_span_y = gadget.common.height - 1;
    int x = gadget.common.x + depressed_offset + 1;
    const auto attributes = static_cast<uint32_t>(gadget.common.attributes);
    constexpr uint32_t align_left = 1U;
    constexpr uint32_t align_center = 2U;
    constexpr uint32_t align_right = 4U;
    if ((attributes & align_left) != 0) {
        x = gadget.common.x + depressed_offset + 3;
    } else if ((attributes & align_right) != 0) {
        x = gadget.common.x + std::max(0, rectangle_span_x - static_cast<int>(text_width) - 3);
    } else if ((attributes & align_center) != 0) {
        x = gadget.common.x + (rectangle_span_x - static_cast<int>(text_width)) / 2 +
            depressed_offset + 1;
    }
    const int y =
        gadget.common.y + (rectangle_span_y - static_cast<int>(text_height)) / 2 + depressed_offset;
    // Game text, or text with characters past ASCII, goes through the game
    // text's runs, the modern fonts' letters in the font's own colour; its
    // caption has no quick key underlined.
    if (needs_text_runs(text, game_text)) {
        std::ignore = draw_fnt_game_text(
            surface,
            selected_font,
            text,
            x,
            y,
            palette_rgb(active_palette, fnt_font_ink(selected_font, active_palette)),
            active_palette,
            {0,
             0,
             static_cast<int32_t>(surface.width) - 1,
             static_cast<int32_t>(surface.height) - 1},
            game_text,
            most_size
        );
        return;
    }

    draw_fnt_glyphs(
        surface,
        selected_font,
        text,
        x,
        y,
        active_palette,
        {0, 0, static_cast<int32_t>(surface.width) - 1, static_cast<int32_t>(surface.height) - 1}
    );
    const bool centred =
        (attributes & (align_left | align_right)) == 0 && (attributes & align_center) != 0;
    if (!centred || condition == ButtonCondition::disabled)
        return;
    const auto key = quick_key_offset(text, quick_key);
    if (key == std::string_view::npos)
        return;
    const int left =
        x + static_cast<int>(formats::fnt::measure_text(selected_font, text.substr(0, key)));
    const int right =
        left + static_cast<int>(formats::fnt::measure_text(selected_font, text.substr(key, 1))) - 1;
    horizontal_line(surface, left, right, y + static_cast<int>(text_height) - 1, underline);
}

// Lights the focus marker's outline round a record, each ring's edges in
// turn, through the light table; pixels outside `clip` are left. The lit
// pixels are read back as palette entries (the nearest entry stands for a
// colour off the palette) and lit as entries, so a corner lit twice goes
// through the table twice.
void light_focus_outline(
    Surface& surface,
    const ui::gui_layout::GadgetRect& record,
    const Rectangle& clip,
    const PaletteBytes& palette,
    std::span<const uint8_t> light_table
) {
    constexpr int32_t light_levels = 32;
    if (light_table.size() != static_cast<std::size_t>(light_levels) * palette_color_count)
        return;
    const auto rings = focus_rings(record);
    // The outermost ring bounds the others.
    const auto& outer = rings.back().rect;
    const int32_t left = std::max(outer.x1, clip.left);
    const int32_t top = std::max(outer.y1, clip.top);
    const int32_t right = std::min(outer.x2, clip.right);
    const int32_t bottom = std::min(outer.y2, clip.bottom);
    if (left > right || top > bottom)
        return;
    auto area = read_indices(surface, palette, left, top, right - left + 1, bottom - top + 1);
    std::vector<uint8_t> lit(area.pixels.size());
    const auto light = [&](int32_t x, int32_t y, int32_t level) {
        if (x < area.left || x >= area.left + area.width || y < area.top ||
            y >= area.top + area.height || level < 0 || level >= light_levels)
            return;
        const auto at = static_cast<std::size_t>(y - area.top) * area.width +
                        static_cast<std::size_t>(x - area.left);
        area.pixels[at] =
            light_table[static_cast<std::size_t>(level) * palette_color_count + area.pixels[at]];
        lit[at] = 1;
    };
    for (const auto& ring : rings) {
        const auto& r = ring.rect;
        for (int32_t x = r.x1; x <= r.x2; ++x)
            light(x, r.y1, ring.level);
        for (int32_t y = r.y1; y <= r.y2; ++y)
            light(r.x2, y, ring.level);
        for (int32_t x = r.x1; x <= r.x2; ++x)
            light(x, r.y2, ring.level);
        for (int32_t y = r.y1; y <= r.y2; ++y)
            light(r.x1, y, ring.level);
    }
    for (int32_t row = 0; row < area.height; ++row)
        for (int32_t column = 0; column < area.width; ++column) {
            const auto at = static_cast<std::size_t>(row) * area.width + column;
            if (lit[at] != 0)
                set_pixel(
                    surface,
                    area.left + column,
                    area.top + row,
                    palette_rgb(palette, area.pixels[at])
                );
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
    const uint8_t* light_row = nullptr,
    bool game_text = false,
    int32_t most_size = present::game_font_text_size
) {
    if (text.empty() || clip.left > clip.right || clip.top > clip.bottom)
        return;
    if (needs_text_runs(text, game_text)) {
        // The modern fonts' letters take the font's own colour, lit as its
        // glyphs are.
        const uint8_t ink = fnt_font_ink(selected_font, active_palette);
        const uint8_t index = light_row != nullptr ? light_row[ink] : ink;
        std::ignore = draw_fnt_game_text(
            surface,
            selected_font,
            text,
            x,
            y,
            palette_rgb(active_palette, index),
            active_palette,
            {clip.left, clip.top, clip.right, clip.bottom},
            game_text,
            most_size
        );
        return;
    }
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
    draw_fnt_glyphs(
        surface,
        selected_font,
        text.substr(0, fitted),
        x,
        y,
        active_palette,
        {clip.left, clip.top, clip.right, clip.bottom},
        light_row
    );
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
    // Each colour is looked up in the palette once, the first time it is met.
    std::unordered_map<uint32_t, std::array<uint8_t, 3>> lit;
    for (int y = top; y <= bottom; ++y)
        for (int x = left; x <= right; ++x) {
            const auto offset =
                (static_cast<std::size_t>(y) * surface.width + static_cast<std::size_t>(x)) * 3U;
            const uint32_t drawn = static_cast<uint32_t>(surface.rgb[offset]) << 16 |
                                   static_cast<uint32_t>(surface.rgb[offset + 1]) << 8 |
                                   surface.rgb[offset + 2];
            auto [entry, added] = lit.try_emplace(drawn);
            if (added) {
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
                entry->second =
                    palette_rgb(palette, light_table[level * palette_color_count + source]);
            }
            const auto& rgb = entry->second;
            std::copy(
                rgb.begin(), rgb.end(), surface.rgb.begin() + static_cast<std::ptrdiff_t>(offset)
            );
        }
}

// Grays and darkens drawn colours: through the gray table, then one row of
// the shade table, in the palette the tables index. A colour is read back as
// the first palette entry that holds it, else the nearest by summed squared
// channel difference; its gray entry is darkened, and entries 0x80..0xFF
// index the row as signed bytes and so read the row before. Each
// colour's darkened colour is kept in a direct-mapped table, so a colour met
// again, as most of a button's pixels are, is not searched for again while
// the table keeps it.
class GrayedShade {
  public:

    /// Sets the darkening up; it does nothing unless usable().
    ///
    /// @param palette the palette the tables index
    /// @param gray_table the palette's gray table (build_gray_table); a shorter
    ///        one leaves each entry as it is
    /// @param shade_table the 32x256 shade table
    /// @param row the table row the colours are darkened through, 1 to 31
    GrayedShade(
        const PaletteBytes& palette,
        std::span<const uint8_t> gray_table,
        std::span<const uint8_t> shade_table,
        std::size_t row
    )
        : palette_(palette), gray_table_(gray_table), shade_table_(shade_table), row_(row),
          slots_(remembered_slots()) {}

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

    /// The colours darkened through the last palette, tables and row a
    /// thread darkened through: screens darken through the same ones on every
    /// frame they draw.
    struct Memory {
        bool kept{};
        PaletteBytes palette{};
        std::vector<uint8_t> gray_table;
        std::vector<uint8_t> shade_table;
        std::size_t row{};
        std::vector<Slot> slots;
    };

    /// Returns the thread's darkened colours for this shade's palette, tables
    /// and row, emptied first when they are not the ones it remembers.
    [[nodiscard]] std::vector<Slot>& remembered_slots() {
        thread_local Memory memory;
        if (!memory.kept || memory.row != row_ || memory.palette != palette_ ||
            !std::equal(
                memory.gray_table.begin(),
                memory.gray_table.end(),
                gray_table_.begin(),
                gray_table_.end()
            ) ||
            !std::equal(
                memory.shade_table.begin(),
                memory.shade_table.end(),
                shade_table_.begin(),
                shade_table_.end()
            )) {
            memory.kept = true;
            memory.palette = palette_;
            memory.gray_table.assign(gray_table_.begin(), gray_table_.end());
            memory.shade_table.assign(shade_table_.begin(), shade_table_.end());
            memory.row = row_;
            memory.slots.assign(std::size_t{1} << slot_bits, Slot{});
        }
        return memory.slots;
    }

    /// Searches the palette for a colour and darkens the entry found's gray.
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
        const std::size_t gray =
            gray_table_.size() >= palette_color_count ? gray_table_[source] : source;
        const auto signed_row = gray >= 0x80 ? row_ - 1 : row_;
        return palette_rgb(palette_, shade_table_[signed_row * palette_color_count + gray]);
    }

    const PaletteBytes& palette_;
    std::span<const uint8_t> gray_table_;
    std::span<const uint8_t> shade_table_;
    std::size_t row_{};
    std::vector<Slot>& slots_;
};

// The gray table of a palette (build_gray_table), kept for the palette a
// thread built it for last: screens gray through the same one on every frame.
[[nodiscard]] std::span<const uint8_t> remembered_gray_table(const PaletteBytes& palette) {
    struct Memory {
        bool kept{};
        PaletteBytes palette{};
        std::vector<uint8_t> table;
    };

    thread_local Memory memory;
    if (!memory.kept || memory.palette != palette) {
        memory.kept = true;
        memory.palette = palette;
        memory.table = build_gray_table(palette);
    }
    return memory.table;
}

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

// Draws a grayed-out button's frame and darkens the frame's rectangle through
// `shade`, as blit then shade_rectangle over that rectangle would: a covered
// pixel takes its palette entry's darkened colour, found once per entry, and
// an uncovered one its drawn colour's.
void blit_grayed(
    Surface& surface,
    const FramePixels& frame,
    int x,
    int y,
    const PaletteBytes& palette,
    const uint8_t* light_row,
    GrayedShade& shade
) {
    const auto pixel_count = static_cast<std::size_t>(frame.width) * frame.height;
    if (frame.pixels.size() != pixel_count || frame.coverage.size() != pixel_count) {
        throw std::runtime_error("rendered GAF frame has inconsistent buffers");
    }
    const auto frame_w = static_cast<int64_t>(frame.width);
    const auto frame_h = static_cast<int64_t>(frame.height);
    const auto first_column = std::max<int64_t>(0, -static_cast<int64_t>(x));
    const auto first_row = std::max<int64_t>(0, -static_cast<int64_t>(y));
    const auto end_column = std::min<int64_t>(frame_w, static_cast<int64_t>(surface.width) - x);
    const auto end_row = std::min<int64_t>(frame_h, static_cast<int64_t>(surface.height) - y);
    if (first_column >= end_column || first_row >= end_row ||
        surface.rgb.size() < static_cast<std::size_t>(surface.width) * surface.height * 3U)
        return;
    std::array<std::array<uint8_t, 3>, palette_color_count> darkened{};
    std::array<bool, palette_color_count> found{};
    const auto* pixels = frame.pixels.data();
    const auto* coverage = frame.coverage.data();
    for (auto row = first_row; row < end_row; ++row) {
        const auto source = static_cast<std::size_t>(row * frame_w);
        auto* out = surface.rgb.data() + (static_cast<std::size_t>(y + row) * surface.width +
                                          static_cast<std::size_t>(x + first_column)) *
                                             3U;
        for (auto column = first_column; column < end_column; ++column, out += 3) {
            const auto offset = source + static_cast<std::size_t>(column);
            if (coverage[offset] == 0) {
                shade.darken(out);
                continue;
            }
            const auto entry = light_row != nullptr ? light_row[pixels[offset]] : pixels[offset];
            if (!found[entry]) {
                darkened[entry] = palette_rgb(palette, entry);
                shade.darken(darkened[entry].data());
                found[entry] = true;
            }
            std::memcpy(out, darkened[entry].data(), 3U);
        }
    }
}

[[nodiscard]] int aligned_text_x(
    const ui::gui_layout::Gadget& gadget,
    std::string_view text,
    const formats::fnt::Font& selected_font,
    int left_inset,
    int right_inset,
    bool game_text = false,
    int32_t most_size = present::game_font_text_size
) {
    const auto width = measure_fnt_game_text(selected_font, text, game_text, most_size);
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
        // A line wider than the label draws its modern text smaller until it
        // fits (fitted_fnt_text_size), and is placed at that width.
        const int32_t most_size = fitted_fnt_text_size(
            selected_font, fields->text, std::max(0, static_cast<int>(gadget.common.width)), false
        );
        const int x = aligned_text_x(gadget, fields->text, selected_font, 0, 0, false, most_size);
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
            light_row,
            false,
            most_size
        );
        return;
    }
    // A CR ends a line. Each line is measured as it is drawn, the modern
    // fonts' letters at their own widths, and breaks where the line breaker
    // lets it (oa::base::text::first_row): at a space, or between Chinese or
    // Japanese characters; a word wider than the label breaks before the
    // character that does not fit.
    const std::string_view text = fields->text;
    const int32_t room = std::max(0, static_cast<int>(gadget.common.width));
    const auto fits = [&](std::string_view start) {
        return measure_fnt_game_text(selected_font, start, false) <= room;
    };
    std::size_t start = 0;
    int y = gadget.common.y;
    const int fixed_x = aligned_text_x(gadget, fields->text, selected_font, 0, 0);
    while (start < text.size() && y <= clip.bottom) {
        const auto paragraph =
            text.substr(start, std::min(text.find('\r', start), text.size()) - start);
        const auto row = oa::base::text::first_row(paragraph, fits);
        draw_clipped_text(
            surface,
            selected_font,
            paragraph.substr(0, row.bytes),
            fixed_x,
            y,
            clip,
            active_palette,
            light_row
        );
        start += row.next;
        while (start < text.size() && (text[start] == ' ' || text[start] == '\r'))
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
    std::span<const uint8_t> light_table,
    GrayedShade* shade
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
        const int x = aligned_text_x(gadget, text, selected_font, 2, 2, list.game_text);
        const Rectangle row_clip{clip.left, y, clip.right, std::min(clip.bottom, y + item_height)};
        draw_clipped_text(
            surface, selected_font, text, x, y, row_clip, active_palette, nullptr, list.game_text
        );
        // A greyed row is shaded after its text and before the selection
        // light, so choosing it still highlights the row.
        if (shade != nullptr && index < list.grey.size() && list.grey[index] != 0)
            shade_rectangle(surface, row_clip, *shade);
        // A text list lights the whole selected row at light level 0x1E after
        // drawing it.
        if (list.selected && *list.selected == index)
            light_rectangle(surface, row_clip, active_palette, light_table, 0x1e);
    }
}

/// Parses a GUI layout, its texts in the game's language.
///
/// @param name The layout's name, for an error
/// @param layout The layout's GUI file
/// @return The layout
/// @throws std::runtime_error when the layout is malformed
ui::gui_layout::Layout
parse_screen_layout(const std::string& name, std::span<const uint8_t> layout) {
    // The panel's texts in the game's language, as 3.1c's GUI loader
    // translates them.
    auto parsed_layout = ui::gui_layout::parse(layout, ui::gui_layout::game_translation_lookup());
    if (!parsed_layout.ok()) {
        throw std::runtime_error(
            "cannot parse GUI layout '" + name + "': " + parsed_layout.error->message
        );
    }
    return std::move(*parsed_layout.layout);
}

ScreenResources load_screen_with_layout(
    AssetStore& assets, const ScreenAssetNames& names, std::span<const uint8_t> layout
) {
    ScreenResources result = load_screen_art(assets, names);
    auto parsed = parse_screen_layout(names.layout, layout);
    return lay_out_screen(std::move(result), std::move(parsed));
}

} // namespace

ScreenResources load_screen_art(AssetStore& assets, const ScreenAssetNames& names) {
    ScreenResources result;
    if (!names.background.empty()) {
        const std::string background = screen_background_path(assets, names.background);
        result.background =
            ui::decoded::require(decode_pcx(assets.read(background).bytes), background);
    }

    const auto palette = assets.read(names.palette).bytes;
    if (palette.size() != result.gui_palette.size()) {
        throw std::runtime_error("GUI palette must contain 256 four-byte RGB entries");
    }
    std::copy(palette.begin(), palette.end(), result.gui_palette.begin());

    if (!names.sprites.empty()) {
        auto parsed_sprites = formats::gaf::parse(assets.read(names.sprites).bytes);
        if (!parsed_sprites.ok()) {
            throw std::runtime_error(
                "cannot parse GUI sprites '" + names.sprites + "': " + parsed_sprites.error->message
            );
        }
        result.sprites = std::move(*parsed_sprites.archive);
    }
    if (!names.shared_sprites.empty()) {
        auto parsed_shared = formats::gaf::parse(assets.read(names.shared_sprites).bytes);
        if (!parsed_shared.ok()) {
            throw std::runtime_error(
                "cannot parse shared GUI sprites '" + names.shared_sprites +
                "': " + parsed_shared.error->message
            );
        }
        result.shared_sprites = std::move(*parsed_shared.archive);
    }
    result.font =
        ui::decoded::require(formats::fnt::load_gaf(assets, default_gui_font), default_gui_font);
    result.label_font =
        ui::decoded::require(formats::fnt::load_gaf(assets, label_gui_font), label_gui_font);
    auto parsed_global = formats::gaf::parse(assets.read(global_logo_sprites).bytes);
    if (!parsed_global.ok())
        throw std::runtime_error(
            "cannot parse global LOGOS.GAF sprites: " + parsed_global.error->message
        );
    result.global_sprites = std::move(*parsed_global.archive);
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
    return result;
}

ScreenResources
load_screen_layout(AssetStore& assets, const ScreenResources& art, const std::string& layout) {
    auto parsed = read_screen_layout(assets, layout);
    return lay_out_screen(art, std::move(parsed));
}

ui::gui_layout::Layout read_screen_layout(AssetStore& assets, const std::string& layout) {
    return parse_screen_layout(layout, assets.read(layout).bytes);
}

ScreenResources lay_out_screen(ScreenResources art, ui::gui_layout::Layout layout) {
    art.layout = std::move(layout);
    bind_screen_buttons(art, 0);
    return art;
}

std::string screen_background_path(AssetStore& assets, const std::string& background) {
    // The background in the language's folder when the game data has it
    // there (bitmaps-German), as 3.1c looks first.
    if (auto variant = data::languages::language_folder_path(background);
        variant && assets.file_size(*variant) > 0)
        return std::move(*variant);
    return background;
}

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

std::optional<std::string>
translated_stage_caption(const ui::gui_layout::ButtonFields& button, std::size_t stage) {
    if (button.stages == 0)
        return std::nullopt;
    return data::languages::translation_of(staged_caption(button.text, stage));
}

void bind_screen_buttons(ScreenResources& resources, std::size_t first) {
    for (auto index = first; index < resources.layout.gadgets.size(); ++index) {
        auto& gadget = resources.layout.gadgets[index];
        // The first panel draw clears a type-1 gadget's authored colorf and
        // colorb before resolving its GAF sequence, and a label's authored
        // colorf (keeping its colorb), so later drawing takes the normal path
        // unless a color table is installed at run time.
        if (gadget.common.type == ui::gui_layout::GadgetType::label) {
            gadget.common.foreground_color = 0;
        } else if (gadget.common.type == ui::gui_layout::GadgetType::button) {
            gadget.common.foreground_color = 0;
            gadget.common.background_color = 0;
        }
    }
    fit_buttons_to_frames(resources, first);
}

void fit_buttons_to_frames(ScreenResources& resources, std::size_t first) {
    // The first panel draw commits the selected GAF frame dimensions back into
    // each type-1 gadget without the checkbox or text_list attribute (NEWCAMP
    // Side/Easy keep authored 159x49 / 107x41). Preserve that mutation so
    // drawing and hit testing share the same rectangles.
    constexpr uint32_t skip_default_button_gaf =
        ui::gui_layout::attribute::checkbox | ui::gui_layout::attribute::text_list;
    for (auto index = first; index < resources.layout.gadgets.size(); ++index) {
        auto& gadget = resources.layout.gadgets[index];
        if (gadget.common.type != ui::gui_layout::GadgetType::button)
            continue;
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

/// Returns whether one archive holds a file at a path.
///
/// @param archive the archive
/// @param path the file's path in the archive
/// @return true when the path names a file, not a folder
bool holds_file(const HpiArchive& archive, std::string_view path) {
    const auto node = archive.lookup(path);
    return node && !archive.nodes()[*node].directory();
}

/// Returns whether a mounted archive lies in a mod's folder: directly in a
/// folder of the store above the last one, the game folder.
///
/// @param assets the asset store
/// @param archive the archive's mount path
/// @return true when a mod's folder holds the archive
bool in_mod_folder(const AssetStore& assets, const std::filesystem::path& archive) {
    const auto folders = assets.loose_roots();
    for (std::size_t folder = 0; folder + 1 < folders.size(); ++folder) {
        std::error_code error;
        for (std::filesystem::directory_iterator entry{folders[folder], error}, last;
             !error && entry != last;
             entry.increment(error)) {
            // Mount paths are canonical, so a linked file is its target.
            std::error_code resolve_error;
            const auto resolved = std::filesystem::weakly_canonical(entry->path(), resolve_error);
            if (!resolve_error && resolved == archive)
                return true;
        }
    }
    return false;
}

/// Finds the MAINMENU.GUI the main menu takes with no overlay drawn over it.
///
/// Walks the mounted archives in lookup order and takes the first copy whose
/// archive also holds FrontendX or lies in a mod's folder. A copy in another
/// archive brings a layout without a background, as a layout made for an
/// overlay drawn over the menu does, and is passed over.
///
/// @param assets the asset store
/// @return the bytes and their archive, or nullopt when no copy qualifies
std::optional<AssetData> main_menu_layout_without_overlay(const AssetStore& assets) {
    const auto layout = oa::data::defs::gui_path(main_menu_layout_file);
    const auto mounts = assets.mount_paths();
    for (std::size_t index = 0; index < mounts.size(); ++index) {
        const auto& mounted = assets.mounted(index);
        if (!holds_file(mounted, layout))
            continue;
        if (!holds_file(mounted, main_menu_background) && !in_mod_folder(assets, mounts[index]))
            continue;
        auto bytes = ui::decoded::require(mounted.read_node(*mounted.lookup(layout)), layout);
        return AssetData{std::move(bytes), mounts[index], true};
    }
    return std::nullopt;
}

} // namespace

MainMenuResources load_main_menu(AssetStore& assets, MainMenuLayout layout) {
    const auto main_menu_layout = oa::data::defs::gui_path(main_menu_layout_file);
    auto gui = assets.read(main_menu_layout);
    // With no overlay, an archived layout made for one is passed over; a
    // loose layout comes first, as loose files do.
    if (layout == MainMenuLayout::base_game && gui.archived)
        if (auto own = main_menu_layout_without_overlay(assets))
            gui = std::move(*own);
    auto result = load_screen_with_layout(
        assets,
        {main_menu_layout,
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
    Surface result;
    render_screen_into(result, resources, presentation, lists);
    return result;
}

void render_screen_into(
    Surface& result,
    const ScreenResources& resources,
    std::span<const ButtonPresentation> presentation,
    std::span<const ListPresentation> lists
) {
    // The background is copied into the surface's own memory, which keeps
    // its size from one screen to the next.
    const Image& background = resources.background;
    const auto expected_background =
        static_cast<uint64_t>(background.width) * background.height * 3U;
    if (expected_background > std::numeric_limits<std::size_t>::max() ||
        background.rgb.size() != static_cast<std::size_t>(expected_background))
        throw std::runtime_error("GUI background has inconsistent RGB data");
    result.width = background.width;
    result.height = background.height;
    result.rgb.assign(background.rgb.begin(), background.rgb.end());
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
                const bool grey_row =
                    std::any_of(list->grey.begin(), list->grey.end(), [](uint8_t mark) {
                        return mark != 0;
                    });
                if (grey_row && shade_palette != nullptr && !grayed_shade)
                    grayed_shade.emplace(
                        *shade_palette,
                        remembered_gray_table(
                            resources.game_palette.has_value() ? *resources.game_palette
                                                               : *shade_palette
                        ),
                        resources.shade_table,
                        grayed_shade_row
                    );
                draw_list_box(
                    result,
                    gadget,
                    *list,
                    resources.font,
                    active_palette,
                    resources.light_table,
                    grey_row && grayed_shade ? &*grayed_shade : nullptr
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
            formats::gaf::RenderResult rendered;
            const auto pixels = frame_pixels(binding.sequence->frames[frame_index], rendered);
            if (!pixels)
                throw std::runtime_error("cannot render runtime image: " + rendered.error->message);
            blit_stretched(
                result,
                *pixels,
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
        const char quick_key = state != nullptr ? state->quick_key : '\0';
        // A pressed button underlines its key in the bevel's dark colour.
        const auto underline =
            gui_color(condition == ButtonCondition::pressed ? palette_black : palette_underline);
        // A side past the art shows its name on the plain face, never a frame.
        const bool caption_only = state != nullptr && state->caption_only;
        const auto binding =
            caption_only ? ResolvedSprite{} : resolve_button_sprite(resources, gadget, state);
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
            formats::gaf::RenderResult rendered;
            const auto pixels = frame_pixels(sequence->frames[index], rendered);
            if (!pixels) {
                throw std::runtime_error(
                    "cannot render MAINMENU.GAF sequence '" + sequence->name +
                    "': " + rendered.error->message
                );
            }
            // A grayed-out button is grayed and darkened too, except one that
            // cycles its frames or plain CHECKBOX art.
            constexpr uint32_t checkbox_art_attribute = 0x80U;
            const auto attributes = static_cast<uint32_t>(gadget.common.attributes);
            const bool staged = button_fields != nullptr && button_fields->stages != 0;
            const bool shaded =
                (attributes & ui::gui_layout::attribute::cycle_frames) == 0 &&
                (staged || (attributes & ui::gui_layout::attribute::scroll_step_mask) != 0 ||
                 (attributes & checkbox_art_attribute) == 0);
            const bool grayed =
                condition == ButtonCondition::disabled && shaded && shade_palette != nullptr;
            if (grayed) {
                // The gray table is PALETTE.PAL's, else the shaded palette's.
                if (!grayed_shade)
                    grayed_shade.emplace(
                        *shade_palette,
                        remembered_gray_table(
                            resources.game_palette.has_value() ? *resources.game_palette
                                                               : *shade_palette
                        ),
                        resources.shade_table,
                        grayed_shade_row
                    );
                // A button without a caption darkens its frame as it draws it.
                if (grayed_shade->usable() &&
                    (button_fields == nullptr || button_fields->text.empty())) {
                    blit_grayed(
                        result,
                        *pixels,
                        gadget.common.x,
                        gadget.common.y,
                        active_palette,
                        light_row,
                        *grayed_shade
                    );
                    continue;
                }
            }
            // The frame keeps its own size at the gadget's corner, however
            // the authored rectangle differs: NEWGAME's 160x160 side emblems
            // fill their frames from 132x106 Side0 and 129x104 Side1.
            blit(result, *pixels, gadget.common.x, gadget.common.y, active_palette, light_row);
            auto resolved_gadget = gadget;
            resolved_gadget.common.width = static_cast<int16_t>(pixels->width);
            resolved_gadget.common.height = static_cast<int16_t>(pixels->height);
            draw_button_text(
                result,
                resolved_gadget,
                gadget.common.width,
                condition,
                resources.font,
                active_palette,
                text_stage,
                quick_key,
                underline,
                state != nullptr && state->game_text
            );
            if (grayed)
                shade_rectangle(result, gadget_rectangle(resolved_gadget), *grayed_shade);
            continue;
        }
        // The plain face is the bevel even when the record is an empty hit target.
        if (portrait_hit && !caption_only)
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
        draw_button_text(
            result,
            gadget,
            gadget.common.width,
            condition,
            resources.font,
            active_palette,
            text_stage,
            quick_key,
            underline,
            state != nullptr && state->game_text
        );
    }
    // The focus marker goes on last, over the records, inside the root. A
    // label, a list or a text box shows no outline.
    if (!resources.layout.gadgets.empty()) {
        const auto root = gadget_rectangle(resources.layout.gadgets.front());
        for (const auto& gadget : resources.layout.gadgets) {
            const auto* state = presentation_for(gadget.common.name, presentation);
            const auto type = gadget.common.type;
            if (state == nullptr || !state->focused || type == ui::gui_layout::GadgetType::label ||
                type == ui::gui_layout::GadgetType::list_box ||
                type == ui::gui_layout::GadgetType::text_box)
                continue;
            const auto rect = gadget_rectangle(gadget);
            light_focus_outline(
                result,
                {rect.left, rect.top, rect.right, rect.bottom},
                root,
                active_palette,
                resources.light_table
            );
        }
    }
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

void blend_rect(
    Surface& surface,
    int x,
    int y,
    int width,
    int height,
    std::array<uint8_t, 3> color,
    uint32_t opacity
) noexcept {
    const auto columns = static_cast<int64_t>(surface.width);
    const auto rows = static_cast<int64_t>(surface.height);
    if (surface.rgb.size() < static_cast<std::size_t>(columns * rows * 3))
        return;
    const int64_t left = std::max<int64_t>(x, 0);
    const int64_t top = std::max<int64_t>(y, 0);
    const int64_t right = std::min<int64_t>(int64_t{x} + std::max(width, 0), columns);
    const int64_t bottom = std::min<int64_t>(int64_t{y} + std::max(height, 0), rows);
    const uint32_t share = std::min(opacity, blend_opaque);
    const uint32_t keep = blend_opaque - share;
    constexpr uint32_t half = blend_opaque / 2;
    std::array<uint32_t, 3> painted{};
    for (std::size_t channel = 0; channel < painted.size(); ++channel)
        painted[channel] = uint32_t{color[channel]} * share + half;
    for (int64_t row = top; row < bottom; ++row) {
        auto* pixel = surface.rgb.data() + static_cast<std::size_t>((row * columns + left) * 3);
        for (int64_t column = left; column < right; ++column, pixel += 3)
            for (std::size_t channel = 0; channel < painted.size(); ++channel)
                pixel[channel] =
                    static_cast<uint8_t>((pixel[channel] * keep + painted[channel]) / blend_opaque);
    }
}

} // namespace oa::ui::frontend_renderer

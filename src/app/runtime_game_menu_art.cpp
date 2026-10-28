// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Reads the game's own art the Settings and Library screens are drawn with
// over the game's own main menu (runtime_game_menu_art.hpp).

#include "runtime_game_menu_art.hpp"

#include "gui_record.hpp"
#include "menu_paint.hpp"

#include "oa/data/campaign/campaign_assets.hpp"
#include "oa/data/campaign/campaign_file.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/formats/fnt.hpp"
#include "oa/formats/gaf.hpp"
#include "oa/present/game_text.hpp"
#include "oa/ui/decoded.hpp"
#include "oa/ui/frontend/resource_palette.hpp"
#include "oa/ui/gui_layout.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace oa::app {

namespace {

namespace renderer = oa::ui::frontend_renderer;

/// The game's Options screen: its layout, the picture its setup asks for,
/// and the palette and art it is drawn with.
constexpr std::string_view options_layout = "startopt.gui";
constexpr const char* options_picture = "options4x";
constexpr const char* options_palette = "palettes/guipal.pal";
constexpr const char* options_sprites = "anims/commongui.gaf";

/// The bare picture the game's menus stand on, with no gadget's frame, pipe
/// or title on it.
constexpr const char* bare_picture = "frontbg";

/// The Options screen's button whose plate a big button stands on.
constexpr std::string_view plate_button = "RESTORE";

/// Where the title stands on the picture, in its pixels: the place of the
/// Options screen's own OPTIONS.
constexpr int32_t title_left = 62;
constexpr int32_t title_top = 27;
constexpr int32_t title_height = 25;

/// The picture whose frame the panels stand in: the game's score board,
/// framed with no pipe or plate on its frame.
constexpr const char* frame_picture = "outcome0";

/// A run of a picture's columns or rows: its first, and how many.
struct Span {
    int32_t first{}; ///< the first column or row
    int32_t count{}; ///< how many
};

/// The frame's columns joined, in the score board's pixels: its left
/// corner, a stretch of its top and bottom between two of the board's
/// dividers, and its right corner.
constexpr std::array<Span, 3> frame_columns{{{6, 16}, {40, 56}, {620, 15}}};

/// The frame's rows joined: its top corners, a stretch of its sides, and
/// its bottom corners, the bolts under them with them.
constexpr std::array<Span, 3> frame_rows{{{83, 17}, {120, 130}, {283, 25}}};

/// The outer edges of the frame's tube, in the score board's pixels: its
/// first column and row, and its last column and row.
constexpr int32_t tube_left = 11;
constexpr int32_t tube_top = 87;
constexpr int32_t tube_right = 629;
constexpr int32_t tube_bottom = 298;

/// The rows of the frame's top tube, its shade above it with it, in the
/// score board's pixels: a divider is a stretch of them, as wide as the
/// frame's top stretch.
constexpr Span rule_rows{86, 7};

/// The columns of the shade within the frame's left and right tubes, in the
/// score board's pixels: a divider meets the tubes there, over their shade
/// as the board's dividers meet its top tube.
constexpr int32_t tube_shade_left = 15;
constexpr int32_t tube_shade_right = 624;

/// A pixel of the score board within the frame, of its ground's colour.
constexpr int32_t ground_x = 60;
constexpr int32_t ground_y = 200;

/// The sequences of commongui.gaf the panels' content is drawn with.
constexpr std::string_view plain_button_art = "BUTTONS0";
constexpr std::string_view slider_art = "SLIDERS";
constexpr std::string_view well_art = "LIGHTBAR";
constexpr std::string_view list_box_art = "LISTBOX";
constexpr std::string_view check_box_art = "CHECKBOX";

/// How many frames a plain button has at each size: up, held, grayed, and
/// one more.
constexpr std::size_t plain_button_frames = 4;

/// The sunk bar's frame, and its columns without the stubs at its ends:
/// the first, and how many.
constexpr std::size_t well_frame = 1;
constexpr int32_t well_first_column = 6;
constexpr int32_t well_columns = 496;

/// The longest path of a picture.
constexpr std::size_t picture_path_bytes = 260;

/// How far a pixel of the Options screen's picture may lie from the bare
/// picture's under it and still be the bare picture: the sum of its three
/// colours' differences. The two pictures' palettes match their common
/// ground only that closely.
constexpr int32_t bare_likeness = 48;

/// Reads one of the game's pictures, in the language shown where the game
/// has it so.
///
/// @param assets the game's files
/// @param name the picture's name, without its directory and extension
/// @return the picture
/// @throws std::runtime_error when it is missing or cannot be read
oa::Image read_picture(oa::AssetStore& assets, const char* name) {
    const auto files = oa::data::campaign::campaign_asset_files(assets);
    std::array<char, picture_path_bytes> path{};
    oa::data::campaign::build_variant_path(
        &files,
        path.data(),
        path.size(),
        oa::ui::frontend::kResourceBitmapDirectory,
        name,
        oa::ui::frontend::kResourceBitmapExtension
    );
    return oa::ui::decoded::require(oa::decode_pcx(assets.read(path.data()).bytes), path.data());
}

/// Joins the score board's frame: its corners and a stretch of each side,
/// in frame_columns and frame_rows, cut from the board clear around the
/// frame.
///
/// @param board the score board's picture
/// @param bare the bare picture
/// @return the frame; empty when it does not lie on the board
WindowIcon join_frame(const oa::Image& board, const oa::Image& bare) {
    const Span& first_column = frame_columns.front();
    const Span& last_column = frame_columns.back();
    const Span& first_row = frame_rows.front();
    const Span& last_row = frame_rows.back();
    WindowIcon cut = cut_out(
        {board.width, board.height, board.rgb},
        first_column.first,
        first_row.first,
        last_column.first + last_column.count - first_column.first,
        last_row.first + last_row.count - first_row.first
    );
    if (cut.width == 0)
        return cut;
    clear_bare_ground(cut, bare, first_column.first, first_row.first);
    WindowIcon frame;
    for (const Span& span : frame_columns)
        frame.width += static_cast<uint32_t>(span.count);
    for (const Span& span : frame_rows)
        frame.height += static_cast<uint32_t>(span.count);
    frame.pixels.resize(static_cast<std::size_t>(frame.width) * frame.height * 4U);
    int32_t to_row = 0;
    for (const Span& rows : frame_rows)
        for (int32_t row = 0; row < rows.count; ++row, ++to_row) {
            int32_t to_column = 0;
            for (const Span& columns : frame_columns) {
                const std::size_t from =
                    (static_cast<std::size_t>(rows.first - first_row.first + row) * cut.width +
                     static_cast<std::size_t>(columns.first - first_column.first)) *
                    4U;
                const std::size_t to = (static_cast<std::size_t>(to_row) * frame.width +
                                        static_cast<std::size_t>(to_column)) *
                                       4U;
                std::copy_n(cut.pixels.begin() + static_cast<std::ptrdiff_t>(from),
                            static_cast<std::ptrdiff_t>(columns.count) * 4,
                            frame.pixels.begin() + static_cast<std::ptrdiff_t>(to));
                to_column += columns.count;
            }
        }
    return frame;
}

/// Writes a text in the bytes of one of the game's GUI fonts: each
/// character as the glyph the font holds it at, a mark the font lacks as
/// the nearest it has, and any other character as a question mark.
///
/// @param font the font
/// @param text the text, UTF-8
/// @return the font's bytes
std::string font_bytes(const oa::formats::fnt::Font& font, std::string_view text) {
    const auto characters = oa::present::FontCharacters::gui_font([&font](uint8_t byte) {
        return font.glyphs[byte].has_value();
    });
    std::string bytes;
    while (!text.empty()) {
        const oa::present::TextCharacter read = oa::present::utf8_sequence(text);
        const std::size_t taken = read.bytes == 0 ? 1 : read.bytes;
        const char32_t character =
            read.bytes == 0 ? oa::present::code_page_character(static_cast<uint8_t>(text.front()))
                            : read.code_point;
        text.remove_prefix(taken);
        if (character < 0x80U) {
            bytes += static_cast<char>(character);
            continue;
        }
        if (const auto byte = characters.byte_for(character)) {
            bytes += static_cast<char>(*byte);
            continue;
        }
        switch (character) {
        case U'…': bytes += "..."; break;
        case U'·':
        case U'•':
        case U'–':
        case U'—': bytes += '-'; break;
        case U'‘':
        case U'’': bytes += '\''; break;
        case U'“':
        case U'”': bytes += '"'; break;
        case U'→': bytes += "->"; break;
        case U'▾':
        case U'▼': break;
        default: bytes += '?'; break;
        }
    }
    return bytes;
}

/// Draws a text in one of the game's fonts as a picture: its letters in the
/// colours of a palette, clear between them.
///
/// @param font the font
/// @param text the text, UTF-8
/// @param palette the palette the font's pixels are drawn in
/// @return the picture; empty for a text that draws nothing
WindowIcon game_text_picture(
    const oa::formats::fnt::Font& font, std::string_view text, const oa::PaletteBytes& palette
) {
    WindowIcon picture;
    const std::string bytes = font_bytes(font, text);
    const uint32_t width = oa::formats::fnt::measure_text(font, bytes);
    const uint32_t height = std::max<uint32_t>(oa::formats::fnt::line_height(font), font.nominal_height);
    if (width == 0 || height == 0)
        return picture;
    std::vector<uint8_t> indices(static_cast<std::size_t>(width) * height);
    std::vector<uint8_t> coverage(indices.size());
    std::ignore = oa::formats::fnt::raster_text({width, height, width, indices, coverage}, font, bytes, 0, 0);
    picture.width = width;
    picture.height = height;
    picture.pixels.resize(indices.size() * 4U);
    for (std::size_t at = 0; at < indices.size(); ++at) {
        if (coverage[at] == 0)
            continue;
        const std::size_t entry = static_cast<std::size_t>(indices[at]) * oa::palette_entry_bytes;
        std::copy_n(palette.begin() + static_cast<std::ptrdiff_t>(entry), 3,
                    picture.pixels.begin() + static_cast<std::ptrdiff_t>(at * 4U));
        picture.pixels[at * 4U + 3U] = 255;
    }
    return picture;
}

/// Finds a sequence of a GAF archive by its name, its case aside.
///
/// @param archive the archive
/// @param name the sequence's name
/// @return the sequence; null when the archive has none of that name
const oa::formats::gaf::Sequence*
sequence_named(const oa::formats::gaf::Archive& archive, std::string_view name) {
    for (const auto& sequence : archive.sequences)
        if (sequence.name.size() == name.size() &&
            std::equal(name.begin(), name.end(), sequence.name.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            }))
            return &sequence;
    return nullptr;
}

/// Reads the first frames of a sequence of commongui.gaf into pictures.
///
/// @param[out] pictures where they go, as many as it holds
/// @param sprites commongui.gaf
/// @param name the sequence's name
/// @param palette the palette its pixels are drawn in
/// @return whether the sequence has that many frames, each drawn
template <std::size_t count>
bool read_frames(
    std::array<WindowIcon, count>& pictures,
    const oa::formats::gaf::Archive& sprites,
    std::string_view name,
    const oa::PaletteBytes& palette
) {
    const auto* sequence = sequence_named(sprites, name);
    if (sequence == nullptr || sequence->frames.size() < count)
        return false;
    for (std::size_t frame = 0; frame < count; ++frame) {
        pictures[frame] = menu_paint::picture_of_frame(sequence->frames[frame], palette);
        if (pictures[frame].width == 0)
            return false;
    }
    return true;
}

/// Reads the pieces of commongui.gaf the panels' content is drawn with into
/// the art: the plain buttons, the sliders' pieces, the sunk bar, the list
/// box, the text field and the check box.
///
/// @param[in,out] art the art
/// @param sprites commongui.gaf
/// @param palette the palette its pixels are drawn in
/// @return whether every piece is there
bool read_content_art(
    GameMenuArt& art, const oa::formats::gaf::Archive& sprites, const oa::PaletteBytes& palette
) {
    const auto* buttons = sequence_named(sprites, plain_button_art);
    const auto* sliders = sequence_named(sprites, slider_art);
    const auto* wells = sequence_named(sprites, well_art);
    if (buttons == nullptr || sliders == nullptr || wells == nullptr ||
        sliders->frames.size() < slider_piece_count || wells->frames.size() <= well_frame)
        return false;
    for (std::size_t first = 0; first + plain_button_frames <= buttons->frames.size();
         first += plain_button_frames) {
        std::array<WindowIcon, 3> button{};
        for (std::size_t frame = 0; frame < button.size(); ++frame)
            button[frame] = menu_paint::picture_of_frame(buttons->frames[first + frame], palette);
        // The first size, a small square, is a lamp rather than a button.
        if (button[0].width >= 2 * button[0].height && button[0].width == button[1].width &&
            button[0].width == button[2].width)
            art.plain_buttons.push_back(std::move(button));
    }
    for (std::size_t piece = 0; piece < slider_piece_count; ++piece)
        art.sliders.push_back(menu_paint::picture_of_frame(sliders->frames[piece], palette));
    const WindowIcon bar = menu_paint::picture_of_frame(wells->frames[well_frame], palette);
    if (art.plain_buttons.empty() ||
        std::any_of(art.sliders.begin(), art.sliders.end(), [](const WindowIcon& piece) {
            return piece.width == 0;
        }) ||
        bar.width < static_cast<uint32_t>(well_first_column + well_columns))
        return false;
    art.well.width = static_cast<uint32_t>(well_columns);
    art.well.height = bar.height;
    art.well.pixels.resize(static_cast<std::size_t>(art.well.width) * art.well.height * 4U);
    for (uint32_t row = 0; row < bar.height; ++row)
        std::copy_n(
            bar.pixels.begin() +
                static_cast<std::ptrdiff_t>((static_cast<std::size_t>(row) * bar.width + well_first_column) * 4U),
            static_cast<std::ptrdiff_t>(well_columns) * 4,
            art.well.pixels.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(row) * art.well.width * 4U)
        );
    return read_frames(art.list_box, sprites, list_box_art, palette) &&
           read_frames(art.check_box, sprites, check_box_art, palette);
}

} // namespace

renderer::ScreenResources read_options_screen(oa::AssetStore& assets) {
    renderer::ScreenResources screen = renderer::load_screen(
        assets,
        {oa::data::defs::gui_path(options_layout), "", options_palette, options_sprites, options_sprites}
    );
    screen.background = read_picture(assets, options_picture);
    return screen;
}

oa::Image read_bare_picture(oa::AssetStore& assets) { return read_picture(assets, bare_picture); }

WindowIcon cut_out(const renderer::Surface& drawn, int32_t x, int32_t y, int32_t width, int32_t height) {
    WindowIcon picture;
    if (x < 0 || y < 0 || width <= 0 || height <= 0 ||
        static_cast<uint32_t>(x + width) > drawn.width ||
        static_cast<uint32_t>(y + height) > drawn.height ||
        drawn.rgb.size() < static_cast<std::size_t>(drawn.width) * drawn.height * 3U)
        return picture;
    picture.width = static_cast<uint32_t>(width);
    picture.height = static_cast<uint32_t>(height);
    picture.pixels.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
    for (int32_t row = 0; row < height; ++row)
        for (int32_t column = 0; column < width; ++column) {
            const std::size_t from = (static_cast<std::size_t>(y + row) * drawn.width +
                                      static_cast<std::size_t>(x + column)) *
                                     3U;
            const std::size_t to = (static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                                    static_cast<std::size_t>(column)) *
                                   4U;
            std::copy_n(drawn.rgb.begin() + static_cast<std::ptrdiff_t>(from), 3,
                        picture.pixels.begin() + static_cast<std::ptrdiff_t>(to));
            picture.pixels[to + 3U] = 255;
        }
    return picture;
}

void clear_bare_ground(WindowIcon& picture, const oa::Image& bare, int32_t x, int32_t y) {
    const auto width = static_cast<int32_t>(picture.width);
    const auto height = static_cast<int32_t>(picture.height);
    const auto like_bare = [&](int32_t column, int32_t row) {
        const int32_t on_x = x + column;
        const int32_t on_y = y + row;
        if (on_x < 0 || on_y < 0 || on_x >= static_cast<int32_t>(bare.width) ||
            on_y >= static_cast<int32_t>(bare.height))
            return false;
        const std::size_t own =
            (static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
             static_cast<std::size_t>(column)) *
            4U;
        const std::size_t under =
            (static_cast<std::size_t>(on_y) * bare.width + static_cast<std::size_t>(on_x)) * 3U;
        int32_t difference = 0;
        for (std::size_t channel = 0; channel < 3U; ++channel)
            difference += std::abs(
                static_cast<int32_t>(picture.pixels[own + channel]) -
                static_cast<int32_t>(bare.rgb[under + channel])
            );
        return difference <= bare_likeness;
    };
    std::vector<bool> reached(static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
    std::vector<std::pair<int32_t, int32_t>> next;
    const auto reach = [&](int32_t column, int32_t row) {
        if (column < 0 || row < 0 || column >= width || row >= height)
            return;
        const std::size_t at =
            static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
            static_cast<std::size_t>(column);
        if (reached[at] || !like_bare(column, row))
            return;
        reached[at] = true;
        picture.pixels[at * 4U + 3U] = 0;
        next.emplace_back(column, row);
    };
    for (int32_t column = 0; column < width; ++column) {
        reach(column, 0);
        reach(column, height - 1);
    }
    for (int32_t row = 0; row < height; ++row) {
        reach(0, row);
        reach(width - 1, row);
    }
    while (!next.empty()) {
        const auto [column, row] = next.back();
        next.pop_back();
        reach(column - 1, row);
        reach(column + 1, row);
        reach(column, row - 1);
        reach(column, row + 1);
    }
}

std::optional<GameMenuArt>
read_game_menu_art(oa::AssetStore& assets, std::string_view title, std::string_view screen_name) {
    try {
        const renderer::ScreenResources screen = read_options_screen(assets);
        const oa::Image bare = read_bare_picture(assets);
        const oa::PaletteBytes palette = bare.palette ? *bare.palette : screen.gui_palette;
        GameMenuArt art;
        art.background = cut_out(
            {bare.width, bare.height, bare.rgb}, 0, 0, static_cast<int32_t>(bare.width),
            static_cast<int32_t>(bare.height)
        );
        art.caption = [font = screen.font, palette](std::string_view text) {
            return game_text_picture(font, text, palette);
        };
        art.title = art.caption(menu_paint::worded(title));
        art.title_left = title_left;
        art.title_top = title_top;
        art.title_height = title_height;
        const oa::Image board = read_picture(assets, frame_picture);
        art.frame = join_frame(board, bare);
        if (art.frame.width == 0) {
            std::cerr << screen_name << ": the frame does not lie on " << frame_picture << '\n';
            return std::nullopt;
        }
        art.rule = cut_out(
            {board.width, board.height, board.rgb},
            frame_columns[1].first,
            rule_rows.first,
            frame_columns[1].count,
            rule_rows.count
        );
        art.frame_corners = {frame_columns.front().count, frame_rows.front().count,
                             frame_columns.back().count, frame_rows.back().count};
        art.frame_reach = {tube_left - frame_columns.front().first,
                           tube_top - frame_rows.front().first,
                           frame_columns.back().first + frame_columns.back().count - 1 - tube_right,
                           frame_rows.back().first + frame_rows.back().count - 1 - tube_bottom};
        art.rule_inset = {tube_shade_left - tube_left, tube_right - tube_shade_right};
        const std::size_t ground =
            (static_cast<std::size_t>(ground_y) * board.width + static_cast<std::size_t>(ground_x)) * 3U;
        art.frame_ground = {board.rgb[ground], board.rgb[ground + 1U], board.rgb[ground + 2U]};
        if (!read_content_art(art, screen.shared_sprites, palette)) {
            std::cerr << screen_name << ": " << options_sprites << " lacks a button, slider, bar or box\n";
            return std::nullopt;
        }
        // A big button's plate: the Options screen's picture round one of
        // its buttons, which the picture holds empty.
        const auto& gadgets = screen.layout.gadgets;
        const auto named = gui_record_named(gadgets, plate_button);
        if (!named) {
            std::cerr << screen_name << ": " << options_layout << " has no button " << plate_button << '\n';
            return std::nullopt;
        }
        const auto& common = gadgets[*named].common;
        const int32_t x = common.x - options_plate_left;
        const int32_t y = common.y - options_plate_top;
        art.plate = cut_out(
            {screen.background.width, screen.background.height, screen.background.rgb},
            x,
            y,
            common.width + options_plate_left + options_plate_right,
            common.height + options_plate_top + options_plate_bottom
        );
        if (art.plate.width == 0) {
            std::cerr << screen_name << ": " << plate_button << "'s plate does not lie on " << options_picture
                      << '\n';
            return std::nullopt;
        }
        clear_bare_ground(art.plate, bare, x, y);
        art.plate_slot = {options_plate_left, options_plate_top, common.width, common.height};
        if (!GameMenuPainter(art).content()) {
            std::cerr << screen_name << ": a button, a bar or a box of " << options_sprites
                      << " cannot be cut to stretch\n";
            return std::nullopt;
        }
        return art;
    } catch (const std::exception& error) {
        std::cerr << screen_name << ": the game's menu art is unavailable: " << error.what() << '\n';
        return std::nullopt;
    }
}

} // namespace oa::app

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/formats/hpi.hpp"
#include "oa/formats/gaf.hpp"
#include "oa/formats/fnt.hpp"
#include "oa/ui/gui_layout.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::frontend_renderer {

// Runtime button conditions used by the type-1 gadget drawing path.
// `pressed` and `disabled` use the game's sunken bevel direction; the state
// machine supplies any GAF frame change explicitly. `hovered` draws as
// `normal`: a button under the pointer looks as it does without it.
enum class ButtonCondition : uint8_t {
    normal,
    hovered,
    pressed,
    disabled,
    hidden,
};

enum class SpriteArchive : uint8_t { screen, shared, global };

struct SpriteOverride {
    SpriteArchive archive = SpriteArchive::screen;
    std::string sequence;
};

struct ButtonPresentation {
    std::string name;
    ButtonCondition condition = ButtonCondition::normal;
    // Exact frame state (the button record's frame base, stage and status) is
    // maintained by the frontend state machine. The renderer accepts its
    // resolved index explicitly instead of guessing animation semantics from
    // the press.
    std::optional<std::size_t> gaf_frame;
    // Index into the authored pipe-delimited staged caption. The button
    // record's stage chooses this segment after the first panel draw replaces
    // separators.
    std::optional<std::size_t> text_stage;
    // Image pointers set at run time can replace the authored button binding
    // entirely (player colors, alliance icons, and dynamic SIDEx).
    std::optional<SpriteOverride> sprite;
    // The button record's quick key, whose first appearance in a centred
    // caption is underlined (quick_key_offset); 0 underlines nothing.
    char quick_key = 0;
    // The record holds the panel's keyboard focus: the focus marker's
    // outline (focus_rings) is lit round it, inside the panel's root.
    bool focused = false;
    // The caption is game text, such as a player's name, which the
    // Language settings may draw in the modern fonts.
    bool game_text = false;
    /// Draws the button's bevelled face and caption with no art, as a button
    /// without art is drawn, whatever art its record names.
    bool caption_only = false;
};

// Runtime records bound to a type-2 gadget. List storage lives outside the
// parsed GUI record, so callers supply it explicitly.
struct ListPresentation {
    std::string name;
    std::span<const std::string> items;
    std::size_t first_visible = 0;
    std::optional<std::size_t> selected;
    // The items are game text, such as chat lines, which the Language
    // settings may draw in the modern fonts.
    bool game_text = false;
    // One byte per item: a non-zero byte greys that row. Empty greys none.
    // A greyed row is shaded before the selection light, so a chosen grey
    // row stays highlighted.
    std::span<const uint8_t> grey{};
};

struct Surface {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgb;
};

struct ScreenResources {
    Image background;
    PaletteBytes gui_palette{};
    ui::gui_layout::Layout layout;
    formats::gaf::Archive sprites;
    formats::gaf::Archive shared_sprites;
    formats::gaf::Archive global_sprites;
    formats::fnt::Font font;
    // GUI font slot 1 (hattfont11), which the label drawing makes active
    // before it draws a label; empty when the screen was not loaded by load_screen.
    formats::fnt::Font label_font;
    // The game's 32x256 active-palette light lookup (PALETTE.LHT).
    std::vector<uint8_t> light_table;
    // The game's 32x256 shade lookup (PALETTE.SHD), which darkens grayed-out
    // buttons; empty when the screen was not loaded by load_screen.
    std::vector<uint8_t> shade_table;
    // The game palette (PALETTE.PAL) the shade table indexes, for screens
    // whose bitmap carries no palette; empty when the screen was not loaded by
    // load_screen.
    std::optional<PaletteBytes> game_palette;
};

using MainMenuResources = ScreenResources;

struct ScreenAssetNames {
    std::string layout;
    std::string background;
    std::string palette;
    std::string sprites;
    std::string shared_sprites;
};

/// Loads a screen's GUI resources by name.
///
/// Callers specify the names the game's screen setup chooses; this layer does
/// not guess naming conventions. The GUI font, label font, logo sprites and
/// light table are loaded as well.
///
/// @param assets Asset store searched loose files first, then archives.
/// @param names Layout, background, palette and sprite names; an empty
///        background, sprites or shared_sprites name loads nothing.
/// @return The loaded resources.
/// @throws std::runtime_error when a resource is missing or malformed.
[[nodiscard]] ScreenResources load_screen(AssetStore& assets, const ScreenAssetNames& names);

/// Loads a screen's GUI resources by name, all but its layout.
///
/// What load_screen() loads for the same names, with no layout: the art
/// several layouts are drawn with, such as the pages of the match's side
/// column, which load_screen_layout() lays each layout out over.
///
/// @param assets Asset store searched loose files first, then archives.
/// @param names Background, palette and sprite names, as load_screen() takes
///        them; the layout's is not read.
/// @return The loaded resources, their layout empty.
/// @throws std::runtime_error when a resource is missing or malformed.
[[nodiscard]] ScreenResources load_screen_art(AssetStore& assets, const ScreenAssetNames& names);

/// Lays a GUI layout out over a screen's art.
///
/// What load_screen() loads for the art's names and this layout, reading only
/// the layout's GUI file: the art is copied, the layout read in the game's
/// language and its buttons bound to the art as load_screen() binds them.
///
/// @param assets Asset store searched loose files first, then archives.
/// @param art Resources load_screen_art() loaded.
/// @param layout The layout's name.
/// @return The art with the layout.
/// @throws std::runtime_error when the layout is missing or malformed.
[[nodiscard]] ScreenResources
load_screen_layout(AssetStore& assets, const ScreenResources& art, const std::string& layout);

/// Binds a screen's buttons and labels as the first draw of their panel does.
///
/// Each button from `first` on loses its authored foreground and background
/// colours and each label its foreground colour; then the buttons are fitted
/// to their GAF frames (fit_buttons_to_frames()). load_screen() binds a loaded
/// panel this way.
///
/// @param[in,out] resources The screen's resources; its layout's buttons and labels change.
/// @param first First gadget bound.
void bind_screen_buttons(ScreenResources& resources, std::size_t first);

/// Gives a screen's buttons the size of their GAF frames, keeping their colours.
///
/// Each button from `first` on without the checkbox or text_list attribute
/// takes the width and height of the frame its GAF sequence shows, so that it
/// is hit where it is drawn. A sub-panel merged into a loaded one is fitted
/// this way: its buttons keep the authored foreground colour they are drawn
/// lit through.
///
/// @param[in,out] resources The screen's resources; its layout's buttons change.
/// @param first First gadget fitted.
void fit_buttons_to_frames(ScreenResources& resources, std::size_t first);

// Which MAINMENU.GUI the main menu takes. An add-on archive may carry a
// MAINMENU.GUI, and no FrontendX, whose SINGLE/MULTI/INTRO/EXIT sit under
// the buttons of a main-menu overlay drawn over the menu, outside
// FrontendX's pipe frames. With an overlay drawn, the top copy applies; with
// none, such a copy is passed over for the next one down, so that a
// FrontendX drawn for the game's own layout, a mod's among them, keeps it.
enum class MainMenuLayout : uint8_t { with_overlay, base_game };

/// Loads the four resources of the main menu screen.
///
/// Loads MAINMENU.GUI, FrontendX, guipal, and the panel's MAINMENU.GAF
/// archive; AssetStore supplies the game's loose-file-before-archive lookup
/// policy.
///
/// @param assets Asset store.
/// @param layout with_overlay takes the top copy of MAINMENU.GUI; base_game
///        takes a loose copy first, then the first archived copy whose
///        archive also holds FrontendX or lies in a mod's folder (a folder of
///        the store above the game folder), and the top copy when no archive
///        qualifies.
/// @return The loaded resources.
/// @throws std::runtime_error when a resource is missing or malformed, or
///         FrontendX is not 640x480.
[[nodiscard]] MainMenuResources load_main_menu(AssetStore& assets, MainMenuLayout layout);

/// Draws the main menu screen.
///
/// Draws the background, type-1 gadget bevels, GAF-backed gadgets, and
/// captions through the GAF font path. No host font is substituted.
///
/// @param resources Resources from load_main_menu().
/// @param presentation Runtime button states by gadget name; empty for the authored state.
/// @return The 640x480 RGB image.
[[nodiscard]] Surface render_main_menu(
    const MainMenuResources& resources, std::span<const ButtonPresentation> presentation = {}
);

/// Returns the caption a button shows at a stage.
///
/// A caption with '|' separators holds one segment per stage; one without
/// shows whole at every stage.
///
/// @param text The button's caption.
/// @param stage The button's stage.
/// @return The segment for the stage; empty past the last segment.
[[nodiscard]] std::string_view staged_caption(std::string_view text, std::size_t stage) noexcept;

/// Translates the caption a button with stages shows at a stage.
///
/// The button's caption was translated whole as its GUI file was read; each
/// stage's caption is then translated on its own, as 3.1c translates them
/// on the panel's first draw, so that "Easy|Medium|Hard" shows each word as
/// gamedata\\translate.tdf gives it.
///
/// @param button the button's fields
/// @param stage the button's stage
/// @return the stage's caption in the language shown; nothing for a button
///     without stages, or a caption without a translation
[[nodiscard]] std::optional<std::string>
translated_stage_caption(const ui::gui_layout::ButtonFields& button, std::size_t stage);

/// Draws a screen: background, gadgets, list rows and captions.
///
/// A button given a quick key in its presentation underlines it, as a
/// centred caption does in 3.1c: the key's glyph is underlined on the row
/// below the text (its top plus the font's text height, less one) in the GUI
/// palette's entry 2, or entry 0 while the button is pressed, and not at all
/// while it is grayed. Last, the focus marker's outline is lit round the
/// record whose presentation holds the focus, through the light table,
/// inside the panel's root (the layout's first record); a label, a list or a
/// text box holding the focus shows none.
///
/// @param resources Resources from load_screen().
/// @param presentation Runtime button states by gadget name; empty for the authored state.
/// @param lists Runtime rows of type-2 gadgets by name.
/// @return The RGB image at the background's size.
/// @throws std::runtime_error when the background or a runtime image is
///         inconsistent, or a resolved GAF frame cannot be drawn.
/// @quirk The focus outline is lit round the focused record even when the
///        record is not shown: a panel whose handler hides its first button
///        after the panel loaded rings the empty place, as 3.1c does.
[[nodiscard]] Surface render_screen(
    const ScreenResources& resources,
    std::span<const ButtonPresentation> presentation = {},
    std::span<const ListPresentation> lists = {}
);

/// Draws a screen as render_screen draws it, into a surface whose memory is
/// kept: a surface drawn into again takes no new memory while the
/// background's size stays the same.
///
/// @param[out] target the surface; whatever it showed is replaced
/// @param resources Resources from load_screen().
/// @param presentation Runtime button states by gadget name; empty for the authored state.
/// @param lists Runtime rows of type-2 gadgets by name.
/// @throws std::runtime_error as render_screen throws
void render_screen_into(
    Surface& target,
    const ScreenResources& resources,
    std::span<const ButtonPresentation> presentation = {},
    std::span<const ListPresentation> lists = {}
);

// 100 sparks. They walk 8-bit FrontendX pixels whose low nibble is >= 0xD
// and stamp index 0xAA.
inline constexpr std::size_t menu_spark_count = 100;
inline constexpr int menu_spark_width = 640;
inline constexpr int menu_spark_height = 480;
inline constexpr int menu_spark_spawn_height = 0xdc;
inline constexpr uint8_t menu_spark_pixel = 0xaa;

struct MenuSpark {
    int16_t x = 0;
    int16_t y = 0;
    uint8_t active = 0;
    int8_t dx = 0;
    int8_t dy = 0;
    uint8_t life = 0;
    uint8_t turn = 0;
    int32_t offset = 0;
};

struct MenuSparks {
    std::array<MenuSpark, menu_spark_count> sparks{};
    std::vector<uint8_t> dest;
    uint32_t rand_seed = 1;
};

/// Clears every spark and restarts the random seed at 1.
///
/// @param[out] state Spark state; its destination copies the background's
///        8-bit pixels (zeroes when the image is smaller than 640x480).
/// @param background FrontendX image.
void reset_menu_sparks(MenuSparks& state, const Image& background);

/// Moves every spark one step along the circuit traces, respawning idle ones in the top 0xDC rows.
///
/// Main-menu panel tick.
///
/// @param[in,out] state Spark state.
/// @param[in,out] surface RGB screen image the sparks are stamped on.
/// @param background FrontendX image whose pixels mark the traces.
void step_menu_sparks(MenuSparks& state, Surface& surface, const Image& background);

/// The opacity at which blend_rect paints its colour alone, in 256ths.
inline constexpr uint32_t blend_opaque = 256;

/// Blends a colour over a rectangle of an RGB image.
///
/// Each channel of each pixel becomes (pixel * (256 - opacity) + colour *
/// opacity + 128) / 256, rounded down: an opacity of 0 leaves the pixels as
/// they were, and blend_opaque or more paints the colour itself. The
/// rectangle is clipped to the image; an image whose pixels do not fill its
/// size is left as it is.
///
/// @param[in,out] surface the image
/// @param x the rectangle's left column
/// @param y the rectangle's top row
/// @param width the rectangle's width; 0 or less blends nothing
/// @param height the rectangle's height; 0 or less blends nothing
/// @param color the colour, red, green and blue
/// @param opacity the colour's share of each pixel, in 256ths
void blend_rect(
    Surface& surface,
    int x,
    int y,
    int width,
    int height,
    std::array<uint8_t, 3> color,
    uint32_t opacity
) noexcept;

/// Shade level the panel loader darkens the panel below a panel opened with
/// gui_input::panel_flag::shade_below with.
inline constexpr int32_t shade_below_level = -0x18;

/// Darkens a rectangle of an RGB image as the panel loader darkens the panel
/// below a panel opened with gui_input::panel_flag::shade_below.
///
/// Each pixel goes back to its palette entry (the lowest one holding its
/// colour, else the nearest by summed channel difference), the rectangle is
/// remapped as an 8-bit surface through the shade table at
/// shade_below_level (present::shade_rect_level, whose entries 0x80 to 0xFF
/// read the row before), and the entries come out through the palette
/// again. The rectangle is clipped to the image; an image whose pixels do
/// not fill its size, or a missing shade table, leaves the pixels as they
/// are.
///
/// @param[in,out] surface the image
/// @param x the rectangle's left column
/// @param y the rectangle's top row
/// @param width the rectangle's width; 0 or less darkens nothing
/// @param height the rectangle's height; 0 or less darkens nothing
/// @param palette palette the image's colours come from
/// @param shade_table the 32x256 shade table (PALETTE.SHD); null darkens nothing
/// @param keep colour, red, green and blue, whose pixels are left as they
///     are; null darkens every pixel
void shade_panel_below(
    Surface& surface,
    int x,
    int y,
    int width,
    int height,
    const PaletteBytes& palette,
    const uint8_t* shade_table,
    const uint8_t* keep = nullptr
);

} // namespace oa::ui::frontend_renderer

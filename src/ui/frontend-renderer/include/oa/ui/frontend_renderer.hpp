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
// machine supplies any GAF frame change explicitly.
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
    // hover/press.
    std::optional<std::size_t> gaf_frame;
    // Index into the authored pipe-delimited staged caption. The button
    // record's stage chooses this segment after the first panel draw replaces
    // separators.
    std::optional<std::size_t> text_stage;
    // Image pointers set at run time can replace the authored button binding
    // entirely (player colors, alliance icons, and dynamic SIDEx).
    std::optional<SpriteOverride> sprite;
};

// Runtime records bound to a type-2 gadget. List storage lives outside the
// parsed GUI record, so callers supply it explicitly.
struct ListPresentation {
    std::string name;
    std::span<const std::string> items;
    std::size_t first_visible = 0;
    std::optional<std::size_t> selected;
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

/// Binds a screen's buttons as the first draw of their panel does.
///
/// Each button from `first` on loses its authored foreground and background
/// colours, and one without the checkbox or text_list attribute takes the
/// size of its GAF frame. load_screen() binds a loaded panel's buttons; a
/// panel merged into it afterwards binds its own with this.
///
/// @param[in,out] resources The screen's resources; its layout's buttons change.
/// @param first First gadget bound.
void bind_screen_buttons(ScreenResources& resources, std::size_t first);

// Which MAINMENU.GUI the main menu takes. An add-on archive may carry a
// MAINMENU.GUI whose SINGLE/MULTI/INTRO/EXIT sit under the buttons of a
// main-menu overlay drawn over the menu, outside FrontendX's pipe frames.
// With an overlay drawn, the top copy applies; with none, the layout of the
// archive that provides FrontendX, which fits its frames.
enum class MainMenuLayout : uint8_t { with_overlay, base_game };

/// Loads the four resources of the main menu screen.
///
/// Loads MAINMENU.GUI, FrontendX, guipal, and the panel's MAINMENU.GAF
/// archive; AssetStore supplies the game's loose-file-before-archive lookup
/// policy.
///
/// @param assets Asset store.
/// @param layout with_overlay takes the top copy of MAINMENU.GUI; base_game
///        takes the copy in the archive that provides FrontendX, and the top
///        copy when FrontendX or that top copy is a loose file or that
///        archive holds no MAINMENU.GUI.
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

/// Draws a screen: background, gadgets, list rows and captions.
///
/// @param resources Resources from load_screen().
/// @param presentation Runtime button states by gadget name; empty for the authored state.
/// @param lists Runtime rows of type-2 gadgets by name.
/// @return The RGB image at the background's size.
/// @throws std::runtime_error when the background or a runtime image is
///         inconsistent, or a resolved GAF frame cannot be drawn.
[[nodiscard]] Surface render_screen(
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

} // namespace oa::ui::frontend_renderer

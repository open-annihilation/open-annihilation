// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The main menu's two buttons (Runtime::EngineSettingsMenuHost), which
// runtime_engine_settings_menu.cpp draws and drives as an overlay on the
// main menu: the OA button, the mods icon (the plates over drive bays),
// which opens the Library, and beside it the Settings button, the settings
// icon (the plates over two gears), which opens Settings. The Library is a
// screen of the OA layer (oa_layer.hpp, runtime_library.cpp), and so is
// Settings.
#pragma once

#include "oa/app/runtime.hpp"
#include "oa/app/window_icon.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/looks.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace oa::app {

struct Runtime::EngineSettingsMenuHost {
    bool button_hovered{};   ///< the pointer is over the OA button
    bool button_pressed{};   ///< a press on the OA button is held
    bool settings_hovered{}; ///< the pointer is over the Settings button
    bool settings_pressed{}; ///< a press on the Settings button is held
    /// The settings icon's visible part, which the Settings button, the
    /// in-game menu's OA button and the Settings dialog's header draw,
    /// decoded on first use.
    std::optional<WindowIcon> settings_picture{};
    bool settings_picture_missing{}; ///< decoding it failed; it is not tried again
    /// The mods icon's visible part, which the Library button and the
    /// Library's header draw, decoded on first use.
    std::optional<WindowIcon> mods_picture{};
    bool mods_picture_missing{}; ///< decoding it failed; it is not tried again

    /// The full square of an icon, corners included, decoded on first use
    /// for the window-pixel buttons. The picture above is the visible part
    /// the 640 by 480 picture draws.
    std::optional<WindowIcon> settings_square{};
    bool settings_square_missing{};
    std::optional<WindowIcon> mods_square{};
    bool mods_square_missing{};
    /// The 512-pixel squares, used when the window's button is larger than
    /// the 256-pixel one.
    std::optional<WindowIcon> settings_large{};
    bool settings_large_missing{};
    std::optional<WindowIcon> mods_large{};
    bool mods_large_missing{};

    /// What one button drew into the 640 by 480 picture this frame, and the
    /// pixels that were there before it, so a larger window can put the menu
    /// back and draw the icon in its own pixels.
    struct ButtonPixels {
        oa::ui::frontend_renderer::SourceRect at{}; ///< where it stands, in source pixels
        std::vector<uint8_t> under{};               ///< the menu before the button, red green blue
        std::vector<uint8_t> drawn{};               ///< the button, red green blue
        oa::ui::kit::OaButtonState state{oa::ui::kit::OaButtonState::idle};
        oa::ui::kit::OaButtonMark mark{};
        bool kept{}; ///< under and drawn were copied this frame
    };

    ButtonPixels library_pixels{};
    ButtonPixels settings_pixels{};

    /// The icon drawn over the scaled picture, in the window's pixels.
    struct SharpButton {
        SDL_Texture* texture{}; ///< the icon, the ring and the badge; null when none
        int width{};            ///< its width, in pixels
        int height{};           ///< its height, in pixels
        int32_t x{};            ///< where it is drawn, in the window's pixels
        int32_t y{};
        int32_t place_width{}; ///< the window's rectangle, which the texture fills
        int32_t place_height{};
        bool show{}; ///< this frame draws it
        /// What the texture was built from, so a frame that looks the same
        /// keeps it.
        int point_side{};
        oa::ui::kit::OaButtonState state{oa::ui::kit::OaButtonState::idle};
        bool focused{};
        oa::ui::kit::OaButtonMark mark{};
        const uint8_t* icon{};
        uint32_t icon_width{};
        uint32_t icon_height{};
        uint32_t darkenings{};
        uint32_t gamma_fold{};
        bool identity_gamma{true};
    };

    SharpButton library_sharp{};
    SharpButton settings_sharp{};

    /// The pointer's cursor covers a button this frame draws again in the
    /// window's pixels. The cursor is left out of the picture, which would
    /// hide that draw, and presented above the button.
    bool cursor_above{};

    EngineSettingsMenuHost() = default;
    EngineSettingsMenuHost(const EngineSettingsMenuHost&) = delete;
    EngineSettingsMenuHost& operator=(const EngineSettingsMenuHost&) = delete;

    /// Releases the window-pixel button textures.
    ~EngineSettingsMenuHost();

    /// Returns the icon a window-pixel button draws: the smallest shipped
    /// one at least as large as the button, or the 512-pixel one when
    /// neither is.
    ///
    /// @param mods the Library button's icon; false is the Settings button's
    /// @param side the button's longer side, in the window's pixels
    /// @return the icon; null when it cannot be decoded
    [[nodiscard]] const WindowIcon* icon_for(bool mods, int32_t side);

    /// Tells whether the main menu's buttons show: the menu's own panel is
    /// drawn without the LIBRARY and SETTINGS buttons add_main_menu_entries
    /// gives it, no package owns the frame, and the dialog's fonts are
    /// there.
    ///
    /// @param runtime the runtime
    /// @return true while the buttons show and answer the pointer
    [[nodiscard]] static bool button_shown(Runtime& runtime);

    /// Returns where one of the LIBRARY and SETTINGS buttons MAINMENU.GUI
    /// is given stands on the main menu's picture.
    ///
    /// @param runtime the runtime
    /// @param name the button's record name
    /// @return its rectangle, in source pixels; none while MAINMENU.GUI has
    ///     no such button
    [[nodiscard]] static std::optional<oa::ui::frontend_renderer::SourceRect>
    entry_rect(const Runtime& runtime, std::string_view name);

    /// Returns where the OA button, which opens the Library, stands on the
    /// main menu's picture: left of the Settings button, a quarter of a
    /// button's side from it; or MAINMENU.GUI's LIBRARY while it has one.
    ///
    /// @param runtime the runtime
    /// @return the button's square, in source pixels
    [[nodiscard]] static oa::ui::frontend_renderer::SourceRect button_rect(const Runtime& runtime);

    /// Returns where the Settings button stands on the main menu's picture:
    /// its bottom-right corner, or its top-right corner while an extension's
    /// overlay stands over the main menu; or MAINMENU.GUI's SETTINGS while it
    /// has one.
    ///
    /// @param runtime the runtime
    /// @return the button's square, in source pixels
    [[nodiscard]] static oa::ui::frontend_renderer::SourceRect
    settings_button_rect(const Runtime& runtime);

    /// Copies the window-size Library and Settings buttons from a presented
    /// frame into a reference, so the reference holds those squares as the
    /// window drew them. A button whose window square is the picture's own
    /// square is left as the reference has it. Pixels inside @p covered stay
    /// as the reference has them, so a dialog drawn over a button stays in
    /// the compare. A frame the size of the letterboxed picture, rather than
    /// the whole window, is addressed from the picture's origin.
    ///
    /// @param runtime the runtime
    /// @param presented the frame the window showed
    /// @param reference the frame a check compares; its button squares change
    /// @param covered a rectangle of the reference left unchanged, in the
    ///        frame's pixels; empty leaves every button pixel
    static void copy_presented_menu_buttons(
        Runtime& runtime,
        const oa::ui::frontend_renderer::Surface& presented,
        oa::ui::frontend_renderer::Surface& reference,
        oa::ui::frontend_renderer::SourceRect covered
    );

    /// Returns the settings icon the Settings button, the in-game menu's
    /// OA button and the Settings dialog's header draw: its visible part,
    /// decoded on first use.
    ///
    /// @param runtime the runtime
    /// @return the icon's pixels, which last as long as the runtime; an
    ///         empty picture, which draws the OA mark, when it cannot be decoded
    [[nodiscard]] static oa::ui::frontend_renderer::RgbaPicture settings_icon(Runtime& runtime);

    /// Returns the mods icon the Library button and the Library's header
    /// draw: its visible part, decoded on first use.
    ///
    /// @param runtime the runtime
    /// @return the icon's pixels, which last as long as the runtime; an
    ///         empty picture, which draws the OA mark, when it cannot be decoded
    [[nodiscard]] static oa::ui::frontend_renderer::RgbaPicture mods_icon(Runtime& runtime);

    /// The buttons overlay's input: hovering and pressing the OA button and
    /// the Settings button, and the shortcut.
    ///
    /// @param context the screen context, with the input
    /// @param state unused
    /// @return 1 when the input was taken
    static int button_event(oa::app::ScreenContext* context, void* state);

    /// Draws the OA button and the Settings button over the composed main menu.
    ///
    /// @param context the screen context, with the frame
    /// @param state unused
    static void button_draw(oa::app::ScreenContext* context, void* state);

    /// Returns what the main menu's OA button wears: the attention dot while
    /// updates can be installed, and in the bottom right how many packages
    /// the player has added (mods, map packs, language packs and downloaded
    /// expansions), or a spinner while that number is not final. The first
    /// call builds the Library's model when it has not been built yet, so a
    /// frame drawn before the Library's tick still shows the count the
    /// catalogues already hold. A catalogue being fetched over a list that
    /// is already in keeps the number; the spinner shows only while no list
    /// has arrived and a fetch is still running.
    ///
    /// @param runtime the runtime
    /// @return the dot and the badge
    [[nodiscard]] static oa::ui::kit::OaButtonMark library_mark(Runtime& runtime);

    /// Forgets the pixels kept for the window-pixel buttons.
    void forget_button_pixels() noexcept;

    /// Copies the menu under both buttons, before they are drawn.
    ///
    /// @param surface the main menu's picture
    /// @param library where the Library button will be drawn
    /// @param settings where the Settings button will be drawn
    void keep_button_under(
        const oa::ui::frontend_renderer::Surface& surface,
        oa::ui::frontend_renderer::SourceRect library,
        oa::ui::frontend_renderer::SourceRect settings
    );

    /// Copies both buttons after they are drawn, with the look each one has.
    ///
    /// @param surface the main menu's picture
    /// @param library_state how the Library button looks
    /// @param library_mark what the Library button wears
    /// @param settings_state how the Settings button looks
    void keep_button_drawn(
        const oa::ui::frontend_renderer::Surface& surface,
        oa::ui::kit::OaButtonState library_state,
        const oa::ui::kit::OaButtonMark& library_mark,
        oa::ui::kit::OaButtonState settings_state
    );

    /// Builds one window-pixel button and, when the frame still holds the
    /// picture's button, puts the menu back under it.
    ///
    /// @param runtime the runtime
    /// @param[in,out] frame the frame's red, green and blue
    /// @param kept what the picture's button drew
    /// @param window_x where the window shows it
    /// @param window_y where the window shows it
    /// @param window_width its width, in the window's pixels
    /// @param window_height its height, in the window's pixels
    /// @param[in,out] sharp the texture
    /// @param mods the Library button's icon; false is Settings
    /// @param passes how many times the frame was darkened
    /// @return true when the frame was put back and the texture will be drawn
    bool prepare_sharp_button(
        Runtime& runtime,
        std::vector<uint8_t>& frame,
        const ButtonPixels& kept,
        int32_t window_x,
        int32_t window_y,
        int32_t window_width,
        int32_t window_height,
        SharpButton& sharp,
        bool mods,
        uint32_t passes
    );

    /// When the window shows the buttons larger than the picture's squares,
    /// copies the frame with those squares put back to the menu under them.
    /// The icons are drawn afterwards, in the window's pixels.
    ///
    /// @param runtime the runtime
    /// @param[out] frame the copy, when this returns true
    /// @return true when frame holds that copy
    [[nodiscard]] bool replace_scaled_buttons(Runtime& runtime, std::vector<uint8_t>& frame);

    /// Draws the icons prepared by replace_scaled_buttons, in the window's
    /// own pixels.
    ///
    /// @param runtime the runtime
    void draw_sharp_buttons(Runtime& runtime);

    /// Destroys the icons' textures. The renderer that made them is still
    /// alive.
    void release_sharp_textures() noexcept;
};

} // namespace oa::app

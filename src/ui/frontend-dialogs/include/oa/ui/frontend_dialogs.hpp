// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Stacked frontend dialogs: the MSGBOX.GUI message box, the CDCHECK.GUI disc
// prompt, HELP.GUI keyboard-command pages, YESORNO.GUI watch prompt and the
// DEMOMSG.GUI notice.
// Each is one gadget panel
// loaded over the current frame. The application registers the overlay from
// screens.inc; dialogs are opened through the functions below and released by
// their own click handlers.
#pragma once

#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/screen_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace oa::ui::frontend_dialogs {

enum class DialogKind : uint8_t { none, message_box, cd_check, help, continue_watching, notice };

/// The button that closed a DEMOMSG.GUI notice.
enum class NoticeChoice : uint8_t {
    ok,      ///< OK, or Enter or Escape
    website, ///< GotoWebsite
};

// Services the dialogs reach; null members act as absent services.
struct DialogHost {
    void* context{};
    void (*play_sound)(void* context, const char* name){};
    // Text translation; a null member or result keeps the source text.
    const char* (*translate)(void* context, const char* text){};
    // CDCHECK.GUI clicks run the campaign screens' disc-check handler in the
    // host; a nonzero result releases the prompt.
    int32_t (*cd_check_click)(void* context, const char* control){};
    // Palette of the screen below, which the panels are indexed against;
    // false (or a null member) falls back to the GUI palette.
    bool (*active_palette)(void* context, PaletteBytes* out){};
    // Root rectangle of the screen's own top panel on the drawn frame, which
    // a shading dialog darkens; false (or a null member) darkens the frame.
    bool (*panel_below)(void* context, int32_t* x, int32_t* y, int32_t* width, int32_t* height){};
    // Receives the reason a dialog could not be drawn (it is then closed).
    void (*report)(void* context, const char* message){};
    // Width of the HUD strip a beside-HUD panel is centred right of on the
    // drawn frame; a null member or a non-positive width keeps 0x80.
    int32_t (*hud_strip_width)(void* context){};
    // True while the host draws the dialogs with dialog_draw_layer; the
    // overlay then leaves the frame alone.
    bool (*draws_layer)(void* context){};
};

/// Installs the services the dialogs reach.
///
/// @param host Service table; copied.
void dialogs_bind_host(const DialogHost& host) noexcept;

/// Opens the MSGBOX.GUI message box over the current frame.
///
/// The text is translated and wrapped to the width, one centred label per
/// line; the panel is sized to the lines and centred, with OK at the
/// bottom-right bound to Enter and Escape.
///
/// @param ctx Screen context supplying the assets and screen size.
/// @param text Message text before translation; cut to 254 bytes after wrapping.
/// @param width Wrap width and panel width, in pixels.
/// @param show_ok 0 deactivates the OK button.
/// @param fit_width Nonzero sizes the panel to its longest line plus 20 pixels instead of `width`.
/// @return False when the stack is full or a resource or the COMIX font is missing.
bool open_message_box(
    app::ScreenContext* ctx,
    std::string_view text,
    int32_t width,
    int32_t show_ok,
    int32_t fit_width
);

/// Opens the non-pausing YESORNO.GUI watch confirmation over the current frame.
///
/// Enter accepts (CHOICE1, "Yes") and Escape declines (CHOICE2, "No"). Each
/// button also takes its quick key, in either case: the first letter of its
/// caption in the language shown that the other's key does not hold, Y and N
/// in English, J and N in German, as in 3.1c.
///
/// @param ctx Screen context supplying the assets and screen size.
/// @param context Value passed to `chosen`.
/// @param chosen Called after the prompt closes with true to keep watching; may be null.
/// @return False when the stack is full or a resource is missing.
bool open_continue_watching(app::ScreenContext* ctx, void* context, void (*chosen)(void*, bool));

/// Opens DEMOMSG.GUI, a notice filling the screen over the demotextbg bitmap, with OK and a
/// website button.
///
/// The text is translated and wrapped to 540 pixels; its lines start 50 pixels from the left and
/// 105 pixels down, 15 pixels apart. OK, Enter and Escape close the notice with NoticeChoice::ok
/// and GotoWebsite with NoticeChoice::website, each with the BigButton sound. OK keeps the
/// caption the game data gives it; GotoWebsite takes `website_caption`, so that it names the
/// address the caller opens for it.
///
/// @param ctx Screen context supplying the assets and screen size.
/// @param text Message text before translation; a newline ends a line.
/// @param website_caption Caption of GotoWebsite; empty keeps the game data's.
/// @param context Value passed to `closed`.
/// @param closed Called after the notice closes with the button that closed it; may be null.
/// @return False when the stack is full or the layout, the bitmap or the GUI art is missing.
bool open_notice(
    app::ScreenContext* ctx,
    std::string_view text,
    std::string_view website_caption,
    void* context,
    void (*closed)(void* context, NoticeChoice choice)
);

/// Opens the CDCHECK.GUI disc prompt centred on the screen.
///
/// @param ctx Screen context supplying the assets and screen size.
/// @return False when the stack is full or a resource is missing.
bool open_cd_check(app::ScreenContext* ctx);

/// Opens HELP.GUI with the dhelp bitmap as its backdrop and fills the first page.
///
/// The panel sits right of the HUD strip and shades the panel below.
///
/// @param ctx Screen context supplying the assets and screen size.
/// @return False when the stack is full or a resource is missing.
bool open_help(app::ScreenContext* ctx);

/// Closes the top dialog.
void close_dialog();

/// Closes every dialog.
void reset_dialogs();

/// Returns the kind of the top dialog.
///
/// @return The kind, or none when no dialog is open.
[[nodiscard]] DialogKind dialog_kind() noexcept;

/// Returns the number of open dialogs.
///
/// @return Dialogs on the stack, 0..4.
[[nodiscard]] std::size_t dialog_count() noexcept;

/// Returns the records of the top dialog.
///
/// @return The resources, whose root holds the canvas position and size and
///         whose records 1.. are root-relative; null when no dialog is open.
[[nodiscard]] const ui::frontend_renderer::ScreenResources* dialog_resources() noexcept;

/// Returns the name of the top dialog's GUI file.
///
/// @return The file's name without its folder and suffix, such as "MSGBOX";
///         empty when no dialog is open.
[[nodiscard]] std::string_view dialog_layout_name() noexcept;

/// Returns the page HELP.GUI shows.
///
/// @return The page from 0, or 0 when the top dialog is not the help panel.
[[nodiscard]] int32_t help_page() noexcept;

/// Returns the stage a button of the top dialog shows, such as HELP.GUI's
/// Page on its second page.
///
/// @param index The button's record, from 1.
/// @return The stage from 0; 0 when no dialog is open or the record is not one of its.
[[nodiscard]] uint8_t dialog_button_stage(std::size_t index) noexcept;

/// Returns the record of the top dialog that holds the keyboard focus.
///
/// It is the record the dialog's loader gave it, once the dialog has been
/// drawn. Before that draw, and when the loader focused none, this returns
/// -1. The dialog's own focus is not written.
///
/// @return the record, or -1 for none
[[nodiscard]] int32_t dialog_keyboard_focus() noexcept;

/// Activates a named button of the top dialog as a click would.
///
/// @param ctx Screen context passed to the dialog's handler.
/// @param name Button name.
/// @return False when no dialog is open or the button is missing.
bool dialog_click(app::ScreenContext* ctx, const char* name);

/// Composes every open dialog over the context's surface.
///
/// Does nothing while the host draws the dialogs with dialog_draw_layer().
///
/// @param ctx Screen context whose surface is drawn on; null does nothing.
void dialog_draw(app::ScreenContext* ctx);

/// Draws the open dialogs for a presenter that keeps its frame in layers.
///
/// The panels are placed on the canvas and written where they cover it,
/// transparent elsewhere. What the bottom dialog darkens below it is left to
/// dialog_shade_below().
///
/// @param width Canvas width in pixels.
/// @param height Canvas height in pixels.
/// @param[out] rgba Receives width x height pixels of four bytes each.
void dialog_draw_layer(uint32_t width, uint32_t height, std::vector<uint8_t>& rgba);

/// Darkens a rectangle of a frame when the bottom dialog shades the panel below it.
///
/// The rectangle is darkened as ui::frontend_renderer::shade_panel_below
/// darkens it, through the shade table of the GUI art the dialogs were
/// opened with.
///
/// @param[in,out] frame RGB frame.
/// @param x Left edge of the rectangle in frame pixels.
/// @param y Top edge of the rectangle.
/// @param width Rectangle width.
/// @param height Rectangle height.
/// @param palette Palette the frame's colours are indexed against.
void dialog_shade_below(
    ui::frontend_renderer::Surface& frame,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    const PaletteBytes& palette
);

} // namespace oa::ui::frontend_dialogs

namespace oa::app {
/// Registers the dialog overlay, above every screen, with the application.
///
/// @param[in,out] registry Screen registry receiving the overlay.
void register_frontend_dialog_screens(ScreenRegistry* registry);
} // namespace oa::app

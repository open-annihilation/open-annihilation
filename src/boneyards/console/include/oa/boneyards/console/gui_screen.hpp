// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A console screen built from a GUI gadget file in coreprime.ccx.
//
// The file is the gadget text the game already loads. The new screens add
// widget types and carry their actions and pictures in fields the loader
// keeps. The 3.1c consoles stay in code; only the new screens are built here.
//
// Help text (the gadget's help field), by type:
//   Button, label and hot surface: an action, such as "goto:sample_next".
//     A "|" splits an action from a picture: "goto:name|file sequence frame".
//     A button or a label shows that picture. A hot surface keeps the action.
//   Image and icon: "file sequence frame". The frame is the one shown.
//   QR image: empty help shows BY_New.gaf sequence QR_Register frame 0;
//     otherwise the same three tokens. The registration frame is 113x113.
//   List row: "caption|action". A caption may contain the two characters
//     "\" and "n"; those become a line break, the way a message box does.
//   Text box: "mask" draws a star for every character. "pad:N" insets the
//     text by N pixels. The two may be joined with "|".
//   Green console: no help. The region is drawn with no colour, so the
//     green console layer shows through. It is not that layer.
//   Mark: "fill COLOR", "rect COLOR" or "line COLOR". A filled rectangle,
//     the outline of the rectangle, or a horizontal line across its top,
//     in that palette index. An outline or a line is clear everywhere else,
//     so a fill behind it shows through.
//   Root panel: a picture, "file sequence frame". The panel is filled with
//     palette colour 42 and that picture is drawn on it, skipping the
//     picture's key colour. Empty help leaves the panel with no fill.
//   Later panel: a child of the gadget named by assoc (0 is the root). Its
//     picture is the face, without the green fill. Empty help leaves the
//     face clear.
//   Image: "line N" is a solid rule of palette colour N, the size of the
//     gadget, drawn on the normal state. It is not a picture.
//
// A button whose picture sequence is Radio_Button is a radio. The face sits
// one pixel below the top of the control and the caption starts eleven
// pixels in. A further "|" and a number after the picture, as in
// "choice:arm|Widgets.gaf Radio_Button 0|12", is extra space before the
// caption so a side insignia can sit between them. The radio rests unpressed
// unless the gadget's active field is set, and it lets go only when a radio
// it is grouped with is chosen. Grouping is the console's, after the load.
// A button whose picture sequence is Check_Box is a check box. It rests
// unchecked unless active is set, which shows the checked frame. The caption
// is the button's text.
// A button whose picture sequence is Function_Button_Large or
// Function_Button_Small is a function button and rests unpressed.
// A label whose colorf is from 1 to 255 is drawn in that palette index.
//
// assoc, when it is not 0, is the parent gadget's index. 0 parents the
// gadget to the root. A control on the root and one inside a later panel
// are placed the same way.
//
// A picture's frame token is the frame shown for an image, an icon or a QR
// image (kept on the normal state). A button picture's frame token is the
// first of four frames, and the button rests on the unpressed state, so
// "BlankButton 0" shows the frame a message-box button shows at rest. A
// BlankButton picture uses that button, including the caption lifted one
// pixel while it is raised. A Function_Button_Large or Function_Button_Small
// picture is fitted to the gadget's width: the 12-pixel caps stay and the
// middle is repeated for a wider gadget, or cut short for a narrower one, and
// the caption is drawn as a function button.
//
// The low five bits of attribs are the text style of a caption: left, right,
// top, bottom, then wrap. With neither horizontal bit the line is centred,
// and with neither vertical bit it sits in the middle. A wrap-only caption
// is centred, in the middle, and wrapped. Font number 0 is
// anims\Hattfont12.gaf; a number N of 1 or more is the Nth font resource in
// the file. A list row joins the list box most recently built. A type the
// loader does not build is kept as a record with no window, and the load
// still succeeds.
#pragma once

#include <cstdint>

#include "oa/boneyards/toolkit.hpp"

namespace oa::boneyards::console {

// Gadget type ids. Values below 20 are the gadget file's own types.
namespace gui_kind {
inline constexpr uint8_t panel = 0;
inline constexpr uint8_t button = 1;
inline constexpr uint8_t list_box = 2;
inline constexpr uint8_t text_box = 3;
inline constexpr uint8_t scroll_bar = 4;
inline constexpr uint8_t label = 5;
inline constexpr uint8_t hot_surface = 6;
inline constexpr uint8_t font_resource = 7;
inline constexpr uint8_t file_resource = 8;
inline constexpr uint8_t image = 12;
inline constexpr uint8_t green = 20;
inline constexpr uint8_t list_row = 21;
inline constexpr uint8_t icon = 22;
inline constexpr uint8_t qr = 23;
inline constexpr uint8_t mark = 24;
} // namespace gui_kind

// One gadget of a loaded screen. The rectangle is where the window was
// placed; a font or file resource has no window and keeps the file's
// rectangle. caption is the text that is shown, with line breaks already
// converted. action is empty when the gadget has none.
struct GuiControl {
    char name[16];
    char caption[128];
    char action[128];
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint8_t kind;
    Window* window;
    // text_style bits the caption is drawn with. Zero centres it.
    int32_t caption_style;
};

struct GuiScreen;

// Filled when a load fails. message is always terminated.
struct GuiLoadError {
    char message[160];
};

/// Builds a screen from a GUI gadget file.
///
/// The file is read the way the game's files are read (a relative path, with
/// '/' and no leading "./"). The screen is a top-level window of the
/// installed desktop. A click on a control, or Enter or Escape while the
/// screen handles the key, records that control. Enter runs the panel's
/// crdefault and activates it when that control is not disabled. Escape
/// records the panel's escdefault.
///
/// @param path GUI file path; null fails the load
/// @param[out] error failure message; may be null
/// @return the screen, or null when the file cannot be read or parsed
GuiScreen* gui_screen_load(const char* path, GuiLoadError* error);

/// Remembers who owns a screen, and calls them when a control activates.
///
/// `on_fired` is called with the control's window after that control is
/// recorded. It must not destroy the screen: a screen hides and is destroyed
/// on the desktop's next pass. `on_destroy` is called after the screen and
/// its controls are gone, and must not use the screen. Either call may be
/// null. A null screen is ignored.
///
/// @param[in,out] screen loaded screen
/// @param user handed to both calls
/// @param on_fired called when a control activates; may be null
/// @param on_destroy called after the screen is destroyed; may be null
void gui_screen_set_user(
    GuiScreen* screen,
    void* user,
    void (*on_fired)(void* user, Window* source),
    void (*on_destroy)(void* user)
);

/// Destroys a screen and every control it built.
///
/// @param screen screen to destroy; null is ignored
void gui_screen_destroy(GuiScreen* screen);

/// Returns the number of gadget records, the panel included.
///
/// @param screen loaded screen; null counts as none
/// @return the record count
int32_t gui_screen_count(const GuiScreen* screen);

/// Returns one gadget record.
///
/// @param screen loaded screen; null has no records
/// @param index record index, from 0
/// @return the record, or null when the index is out of range
const GuiControl* gui_screen_at(const GuiScreen* screen, int32_t index);

/// Finds a gadget by its name.
///
/// @param screen loaded screen; null has no records
/// @param name gadget name; null matches nothing
/// @return the first record of that name, or null
const GuiControl* gui_screen_find(const GuiScreen* screen, const char* name);

/// The box a caption is drawn in, after the picture that shares the control.
///
/// A radio keeps the gap the file names and the mark beside the words. A
/// check box keeps the box picture and the gap before its words. Every other
/// control uses its own rectangle. A null control reports an empty box.
///
/// @param control loaded control; may be null
/// @param[out] width box width in pixels; may be null
/// @param[out] height box height in pixels; may be null
/// @param[out] style text_style bits; may be null
void gui_control_caption_room(
    const GuiControl* control, int32_t* width, int32_t* height, int32_t* style
);

/// Returns the name of the control that last fired.
///
/// @param screen loaded screen
/// @return the name, or "" when nothing has fired or screen is null
const char* gui_screen_fired_name(const GuiScreen* screen);

/// Returns the action of the control that last fired.
///
/// @param screen loaded screen
/// @return the action, or "" when nothing has fired, the control has no
///     action, or screen is null
const char* gui_screen_fired_action(const GuiScreen* screen);

/// Returns the screen's top-level window.
///
/// @param screen loaded screen; null has none
/// @return the window
Window* gui_screen_root(GuiScreen* screen);

} // namespace oa::boneyards::console

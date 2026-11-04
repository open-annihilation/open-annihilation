// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Boneyards widget toolkit: the window tree, message routing and the resource
// services every Boneyards screen draws through.
//
// Each window class is a plain struct whose first member is `Window` and
// whose behaviour lives in a `WindowOps` dispatch table of function pointers,
// one per window operation. The nine resource services are function-pointer
// tables too, so screens and tests can install their own.
//
// Coordinates are absolute screen pixels. Resource handles are small
// non-negative integers; `k_no_handle` (-1) means "none".
#pragma once

#include <cstddef>
#include <cstdint>

#include "oa/present/surface.h"

namespace oa::boneyards {

using oa::Palette;
using oa::PaletteEntry;
using oa::Rect32;
using oa::Sprite;
using oa::Surface;

struct Window;

using Handle = int32_t;
inline constexpr Handle k_no_handle = -1;

// ---------------------------------------------------------------------------
// Messages

// Message codes understood by window_dispatch. The mouse codes keep the
// Win32 numbering the game uses.
enum class MessageCode : int32_t {
    draw = 1,
    // Codes 2 and 3 each have a dispatch slot of their own, but nothing sends
    // them and every class leaves their slots unhandled.
    reserved_2 = 2,
    reserved_3 = 3,
    focus_gained = 4,
    focus_lost = 5,
    key_down = 6,
    key_up = 7,
    mouse_move = 0x200,
    left_down = 0x201,
    left_up = 0x202,
    left_double = 0x203,
    right_down = 0x204,
    right_up = 0x205,
    right_double = 0x206,
    middle_down = 0x207,
    middle_up = 0x208,
    middle_double = 0x209,
    wheel = 0x20A,
};

// 0x14-byte message record passed to every handler.
struct Message {
    MessageCode code;
    int32_t key; // key code for key_down/key_up (see `key`)
    int32_t x;   // pointer position
    int32_t y;
    uint32_t buttons; // held buttons (`button_*` bits)
};

static_assert(sizeof(Message) == 0x14);

// Message::buttons bits (Win32 MK_* values).
inline constexpr uint32_t button_left = 0x01;
inline constexpr uint32_t button_right = 0x02;
inline constexpr uint32_t button_middle = 0x10;
inline constexpr uint32_t button_any = button_left | button_right | button_middle;

// Key codes carried in Message::key. Printable keys use their character code;
// navigation keys use the client's own 0x18x numbering.
namespace key {
inline constexpr int32_t backspace = 8;
inline constexpr int32_t tab = 9;
inline constexpr int32_t enter = 13;
inline constexpr int32_t escape = 27;
inline constexpr int32_t del = 0x18D;
inline constexpr int32_t home = 0x18E;
inline constexpr int32_t end = 0x18F;
inline constexpr int32_t page_up = 0x190;
inline constexpr int32_t page_down = 0x191;
inline constexpr int32_t left = 0x192;
inline constexpr int32_t up = 0x193;
inline constexpr int32_t right = 0x194;
inline constexpr int32_t down = 0x195;
inline constexpr int32_t shift = 0x197;
inline constexpr int32_t control = 0x198;
inline constexpr int32_t alt = 0x199;
inline constexpr int32_t system = 0x19A; // Command or Windows
} // namespace key

// Event a control reports to its parent's on_notify.
enum class NotifyCode : int32_t {
    activated = 1,         // button released over itself, check box toggled, ...
    changed = 2,           // value edited (edit boxes, sliders)
    dragged = 3,           // scroll thumb dragged; `message` is the pointer message
    radio_selected = 7,    // sent to the siblings of a newly selected radio button
    scroll_released = 200, // scroll button let go
};

struct Notify {
    NotifyCode code;
    Window* source;
    int32_t value;          // class-specific (e.g. the new check state)
    const Message* message; // the record's third field, shared with `value`
};

// ---------------------------------------------------------------------------
// Dispatch table

// Per-class behaviour: one handler per window operation. Unused handlers
// return 0/false; "handled" results are non-zero. Operations a class adds
// beyond these are kept in its own record.
struct WindowOps {
    const char* class_name; // string compared by `is_class`

    void (*destroy)(Window* self); // frees the window and its record
    void (*set_caption)(Window* self, const char* text, int32_t style);
    const char* (*state_text)(Window* self, int32_t state); // -1 = current
    int32_t (*state_index)(Window* self, const char* state_name);
    const char* (*state_name)(Window* self, int32_t state);
    bool (*is_class)(const char* name);
    // Loads consecutive frames first.. of file/sequence as the state images;
    // returns the next frame index.
    int32_t (*bind_images)(
        Window* self, const char* file, const char* sequence, int32_t first, int32_t key
    );
    // A constant each class reports, as 3.1c's window classes do; nothing in
    // the engine calls this slot.
    int32_t (*type_code)();
    // Next window to take keyboard focus when tabbing away from `from`.
    Window* (*focus_target)(Window* self, Window* from, bool backwards);
    void (*raise)(Window* self); // move to the top of the parent's z order
    bool (*contains)(Window* self, int32_t x, int32_t y);
    Window* (*child_at)(Window* self, int32_t x, int32_t y);
    void (*on_bounds)(Window* self);  // after window_set_bounds
    void (*on_destroy)(Window* self); // before children are destroyed

    // Periodic walk by desktop_pulse (auto-repeat, timed updates).
    // `on_reset` is the earlier name of this handler, kept for existing callers.
    union {
        void (*pulse)(Window* self);
        void (*on_reset)(Window* self);
    };

    void (*on_notify)(Window* self, const Notify* notify); // event from a child
    bool (*draw)(Window* self);                            // MessageCode::draw
    bool (*refresh)(Window* self);
    bool (*tick)(Window* self);          // broadcast by desktop_tick
    bool (*on_reserved_2)(Window* self); // MessageCode::reserved_2
    bool (*on_reserved_3)(Window* self); // MessageCode::reserved_3
    bool (*focus_gained)(Window* self);
    bool (*focus_lost)(Window* self);
    void (*mouse_enter)(Window* self);
    void (*mouse_leave)(Window* self);
    bool (*key_down)(Window* self, const Message* msg);
    bool (*key_up)(Window* self, const Message* msg);
    bool (*left_down)(Window* self, const Message* msg);
    bool (*left_up)(Window* self, const Message* msg);
    bool (*left_double)(Window* self, const Message* msg);
    bool (*right_down)(Window* self, const Message* msg);
    bool (*right_up)(Window* self, const Message* msg);
    bool (*right_double)(Window* self, const Message* msg);
    bool (*middle_down)(Window* self, const Message* msg);
    bool (*middle_up)(Window* self, const Message* msg);
    bool (*middle_double)(Window* self, const Message* msg);
    bool (*mouse_move)(Window* self, const Message* msg);
    bool (*wheel)(Window* self, const Message* msg);
    // Windows are never saved or loaded, so there are no stream handlers.
};

// ---------------------------------------------------------------------------
// Window base.

// Growable array of child pointers.
struct WindowList {
    Window** items;
    int32_t count;
    int32_t capacity;
};

// Per-state resource handles; all three arrays have `state_count` entries.
struct WindowStates {
    int32_t current;
    int32_t count;
    Handle* images; // ImageService handles
    Handle* texts;  // TextService handles
    Handle* sounds; // SoundService handles
};

struct Window {
    const WindowOps* ops;
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint8_t visible; // effective only when every ancestor is visible
    uint8_t focusable;
    uint8_t focused;
    uint8_t owned;  // destroyed with its parent by window_destroy_owned
    Handle color;   // ColorService background, filled when no image
    int32_t cursor; // CursorService id shown while hovered
    Handle font;    // FontService handle, inherited from the parent
    char* name;
    Window* parent; // null for top-level windows
    WindowList children;
    WindowStates states;
    Handle text_handle; // TextService handle released on destroy
    // Set to 1 by window_init; nothing reads it.
    uint8_t reserved_after_text_handle;
    // 1 when this window is drawn with the modern chat faces. Labels, buttons
    // and the frequency and player lists stay 0 and keep the Boneyards' fonts.
    uint8_t modern_text;
    // Palette index of the modern ink. Meaningful while modern_text is 1.
    uint8_t modern_ink;
};

// Class names used by `is_class`.
namespace class_name {
inline constexpr const char* gadget = "Gadget";
inline constexpr const char* window = "Window";
inline constexpr const char* button = "Button";
inline constexpr const char* panel = "Panel";
inline constexpr const char* slider = "Slider";
inline constexpr const char* dialog = "Dialog";
inline constexpr const char* static_text = "Static";
inline constexpr const char* scroll_button = "SButton";
inline constexpr const char* list_box = "ListBox";
inline constexpr const char* check_box = "CheckBox";
inline constexpr const char* radio_button = "RadioButton";
inline constexpr const char* single_edit = "SingleEdit";
inline constexpr const char* no_focus_button = "NoFocusBtn";
inline constexpr const char* stage_button = "StageBtn";
inline constexpr const char* edit_box = "EditBox";
inline constexpr const char* progress = "Progress";
inline constexpr const char* dock_bar = "DockBar";
inline constexpr const char* group_box = "GroupBox";
inline constexpr const char* grid_box = "GridBox";
inline constexpr const char* page_control = "PageControl";
inline constexpr const char* iwindow = "IWindow";
} // namespace class_name

// Dispatch table of the plain "Window" class. Derived tables start from a
// copy of it and override handlers.
extern const WindowOps k_window_ops;

/// Initialises a record as a plain window (the base constructor).
///
/// Zeroes the record, installs `ops` and gives the window two states
/// ("Disabled", "Normal") with Normal current. The window starts hidden,
/// focusable, with a white background colour and no font, cursor or name.
///
/// @param[out] self record to initialise; its previous contents are discarded
/// @param ops dispatch table of the window's class
void window_init(Window* self, const WindowOps* ops);
/// Tears down a window's contents (the base destructor body).
///
/// Destroys every child, then frees the name and releases the colour, font,
/// state and caption handles and the state and child storage. The record
/// itself is not freed.
///
/// @param[in,out] self window to tear down
void window_release(Window* self);
/// Allocates and initialises a plain Window.
///
/// @return the new window; release it with window_destroy
Window* window_create_plain();
/// Destroys a window and its subtree.
///
/// Clears the desktop focus when the window is focused and the capture and
/// hover when they point at it, runs its on_destroy slot, destroys its
/// children, detaches it from its parent (or the desktop) and finally calls
/// its destroy slot, which frees heap records.
///
/// @param self window to destroy; null does nothing
void window_destroy(Window* self);

/// Names a window and places it (the "create" call every screen makes).
///
/// Equivalent to window_set_name followed by window_set_bounds.
///
/// @param[in,out] self window to place
/// @param name window name, copied; null keeps the current name
/// @param x left edge in screen pixels
/// @param y top edge in screen pixels
/// @param width width in pixels
/// @param height height in pixels
/// @param parent new parent, or null for a top-level window of the desktop
/// @param visible whether the window's own visible flag is set
void window_create(
    Window* self,
    const char* name,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    Window* parent,
    bool visible
);
/// Stores the bounds, re-parents the window, sets its visibility and runs its
/// on_bounds slot.
///
/// The window is appended last among the new parent's children. Children are
/// not moved.
///
/// @param[in,out] self window to place
/// @param x left edge in screen pixels
/// @param y top edge in screen pixels
/// @param width width in pixels
/// @param height height in pixels
/// @param parent new parent, or null for a top-level window of the desktop
/// @param visible whether the window's own visible flag is set
void window_set_bounds(
    Window* self, int32_t x, int32_t y, int32_t width, int32_t height, Window* parent, bool visible
);
/// Replaces the window's name with a copy of `name`.
///
/// @param[in,out] self window to rename
/// @param name new name; null keeps the current one
void window_set_name(Window* self, const char* name);
/// Sets the width, then runs the refresh slot.
///
/// The position is not clamped and children are not moved.
///
/// @param[in,out] self window to resize
/// @param width new width in pixels
void window_set_width(Window* self, int32_t width);
/// Sets the height, then runs the refresh slot.
///
/// The position is not clamped and children are not moved.
///
/// @param[in,out] self window to resize
/// @param height new height in pixels
void window_set_height(Window* self, int32_t height);
/// Sets the width and height, then runs the refresh slot.
///
/// The position is not clamped and children are not moved.
///
/// @param[in,out] self window to resize
/// @param width new width in pixels
/// @param height new height in pixels
void window_set_size(Window* self, int32_t width, int32_t height);
/// Takes the position and size of an inclusive rectangle, then runs the
/// refresh slot.
///
/// Children are not moved.
///
/// @param[in,out] self window to place
/// @param rect inclusive screen rectangle; the width is x2 - x1 + 1
void window_set_rect(Window* self, const Rect32& rect);
/// Moves the window to a new parent.
///
/// The window leaves its old parent (or the desktop) and joins `parent`
/// before its child at index `position`, or last when that index is outside
/// the children; a window already among them stays where it is. A window
/// without a font takes a reference to the new parent's font.
///
/// @param[in,out] self window to re-parent
/// @param[in,out] parent new parent, or null to make a top-level window of
///     the installed desktop
/// @param position index among the parent's children to insert before; -1
///     (or any index outside the children) appends
void window_set_parent(Window* self, Window* parent, int32_t position = -1);
/// Selects the current state.
///
/// @param[in,out] self window whose state changes
/// @param state new state index; ignored when outside the window's states
void window_set_state(Window* self, int32_t state);
/// Resizes the per-state image, text and sound arrays.
///
/// Every held handle is released first, so all states end up without
/// resources. The current state index is not changed.
///
/// @param[in,out] self window whose states are resized
/// @param count new number of states; negative counts as 0
void window_set_state_count(Window* self, int32_t count);
/// Replaces the image of one state.
///
/// The previous image is released and the new handle gains a reference.
///
/// @param[in,out] self window whose state image changes
/// @param state state index; ignored when outside the window's states
/// @param image ImageService handle, or k_no_handle for none
void window_set_state_image(Window* self, int32_t state, Handle image);
/// Replaces the caption text of one state.
///
/// The previous text is released and a new TextService text is created.
///
/// @param[in,out] self window whose state text changes
/// @param state state index; ignored when outside the window's states
/// @param text caption; null or "" leaves the state without text
/// @param style text_style bits for the caption
void window_set_state_text(Window* self, int32_t state, const char* text, int32_t style);
/// Replaces the background colour.
///
/// The previous colour is released and a new ColorService colour is created.
///
/// @param[in,out] self window whose background changes
/// @param r red component, 0-255
/// @param g green component, 0-255
/// @param b blue component, 0-255
/// @param a alpha component, 0-255
void window_set_color(Window* self, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
/// Moves the window to an absolute position and carries its children along.
///
/// The position is clamped to the parent's rectangle; a top-level window is
/// clamped so at least 5 pixels of it stay on the desktop. Each child moves
/// by the applied offset through window_move_by.
///
/// @param[in,out] self window to move
/// @param x requested left edge in screen pixels
/// @param y requested top edge in screen pixels
void window_move_to(Window* self, int32_t x, int32_t y);
/// Moves the window by an offset and every child by the same applied offset.
///
/// The offset is clamped as in window_move_to, and each child clamps its own
/// move again against the moved parent.
///
/// @param[in,out] self window to move
/// @param dx requested horizontal offset in pixels
/// @param dy requested vertical offset in pixels
void window_move_by(Window* self, int32_t dx, int32_t dy);

/// Tests whether the window and every ancestor are visible.
///
/// @param self window to test
/// @return true when the window would be drawn
bool window_is_shown(const Window* self);
/// Tests the window's name.
///
/// @param self window to test
/// @param name name to compare, case-sensitively; may be null
/// @return true when both the window's name and `name` are set and equal
bool window_is_named(const Window* self, const char* name);
/// Compares two names case-sensitively; the class-name test of every class.
///
/// @param a first name; may be null
/// @param b second name; may be null
/// @return true when both are set and equal
bool names_equal(const char* a, const char* b);
/// Tests the extension of a file name.
///
/// Only the first '.' counts, and at most three characters are compared.
///
/// @param name file name to test; null has no extension
/// @param extension extension to look for, without the dot (three
///     characters); null matches nothing
/// @return true when the characters after the first '.' of `name`, cut to
///     three, equal `extension`, ignoring case
/// @quirk A name with a line feed before its first '.' has no extension.
bool name_has_extension(const char* name, const char* extension);
/// Searches the window's descendants depth first for a name.
///
/// Each child is tested before its own subtree; the window itself is not.
///
/// @param self window whose descendants are searched
/// @param name name to find, compared case-sensitively; null finds nothing
/// @param class_filter class name the match must pass through its is_class
///     slot, or null to accept any class
/// @return the first matching descendant, or null
Window* window_find_named(Window* self, const char* name, const char* class_filter);
/// Finds the deepest shown window under a point.
///
/// Later children lie above earlier ones, so the last child hit wins.
///
/// @param self root of the subtree to search
/// @param x pointer x in screen pixels
/// @param y pointer y in screen pixels
/// @return the deepest shown window whose contains slot accepts the point,
///     `self` when no child does, or null when `self` is hidden or missed
Window* window_hit_test(Window* self, int32_t x, int32_t y);

/// Routes a message to the handler slot for its code.
///
/// @param self window receiving the message
/// @param msg message to deliver; its code selects the slot
/// @return the handler's result (true = handled); false for an unknown code
bool window_dispatch(Window* self, const Message* msg);
/// Sends a message that carries only a code.
///
/// @param self window receiving the message
/// @param code message code; the key, position and buttons are zero
/// @return the handler's result (true = handled)
bool window_send(Window* self, MessageCode code);
/// Sends draw to every child in order (the tail of the base draw).
///
/// @param self window whose children are drawn
void window_draw_children(Window* self);
/// Draws the window as the base class does.
///
/// A shown window draws the image of its current state, or else fills its
/// background colour when it has one, then draws its children. A hidden
/// window draws nothing.
///
/// @param self window to draw
/// @return true (handled)
bool window_draw_background(Window* self);

/// Chooses the window that takes the keyboard focus when tabbing (default
/// focus_target slot).
///
/// A shown, focusable window takes the focus itself; otherwise the request
/// passes to the parent, unless it came from there.
///
/// @param self window asked for a focus target
/// @param from window the focus is moving away from
/// @param backwards true for Shift+Tab
/// @return the window to focus, or null when there is none
Window* window_default_focus_target(Window* self, Window* from, bool backwards);
/// Raises the window to the top of its parent's children, then raises the
/// parent in turn (default raise slot).
///
/// A top-level window is raised on the installed desktop instead.
///
/// @param self window to raise
void window_default_raise(Window* self);
/// Tests whether a point lies inside the window (default contains slot).
///
/// @param self window to test
/// @param x pointer x in screen pixels
/// @param y pointer y in screen pixels
/// @return true when the point is inside the window's bounds
/// @quirk The right and bottom edges are inclusive, so the test also accepts
///     the column at x + width and the row at y + height.
bool window_default_contains(Window* self, int32_t x, int32_t y);
/// Finds the window under a point through the children's child_at slots
/// (default child_at slot).
///
/// @param self window to search
/// @param x pointer x in screen pixels
/// @param y pointer y in screen pixels
/// @return the last child reporting a hit, `self` when none does, or null
///     when `self` is hidden or does not contain the point
Window* window_default_child_at(Window* self, int32_t x, int32_t y);
/// Handles Tab focus movement and passes other keys to the parent (default
/// key_down slot).
///
/// Tab asks the parent (or the window itself when top-level) for the next
/// focus target, backwards while Shift is held, and focuses it.
///
/// @param self window receiving the key
/// @param msg key_down message; null is not handled
/// @return true for Tab; otherwise the parent's key_down result, or false
///     without a parent
bool window_default_key_down(Window* self, const Message* msg);
/// Passes a key release to the parent (default key_up slot).
///
/// @param self window receiving the key
/// @param msg key_up message; null is not handled
/// @return the parent's key_up result, or false without a parent
bool window_default_key_up(Window* self, const Message* msg);
/// Accepts a pointer move over the window and shows the window's cursor
/// (default mouse_move slot).
///
/// @param self window receiving the move
/// @param msg mouse_move message with the pointer position; null is not
///     handled
/// @return true when the pointer is inside the window or the window holds
///     the capture
bool window_default_mouse_move(Window* self, const Message* msg);
/// Sends the parent's on_notify slot an event from this window.
///
/// The record's `message` is null.
///
/// @param self window reporting the event
/// @param code event code
/// @param value class-specific value carried with the event
void window_notify_parent(Window* self, NotifyCode code, int32_t value);
/// Plays one of the window's state sounds.
///
/// @param self window whose sound plays
/// @param index state sound to play (0 disabled click, 1 press, 2 activate);
///     ignored when outside the window's states
void window_play_sound(Window* self, int32_t index);
/// Runs the pulse slot of the window, then of each child subtree in order.
///
/// @param self root of the subtree to pulse
void window_pulse_tree(Window* self);

// ---------------------------------------------------------------------------
// Standard controls. Classes without extra fields are plain Windows with their
// own dispatch table; the others embed Window first. Every create function
// returns a heap record released through window_destroy.

extern const WindowOps k_gadget_ops;
extern const WindowOps k_button_ops;
extern const WindowOps k_no_focus_button_ops;
extern const WindowOps k_scroll_button_ops;
extern const WindowOps k_static_ops;
extern const WindowOps k_check_box_ops;
extern const WindowOps k_radio_button_ops;
extern const WindowOps k_stage_button_ops;
extern const WindowOps k_progress_ops;

// Button-like state indexes.
namespace button_state {
inline constexpr int32_t disabled = 0;
inline constexpr int32_t pressed = 1;
inline constexpr int32_t unpressed = 2;
inline constexpr int32_t count = 3;
} // namespace button_state

namespace check_state {
inline constexpr int32_t disabled = 0;
inline constexpr int32_t unchecked_down = 1;
inline constexpr int32_t checked_down = 2;
inline constexpr int32_t unchecked_up = 3;
inline constexpr int32_t checked_up = 4;
} // namespace check_state

struct CheckBox {
    Window base;
    int32_t label_gap; // pixels between the box image and the label
};

// Cycles through its "First".."Stage9" states on each click.
struct StageButton {
    Window base;
    int32_t saved_state; // state before the press
    Handle saved_text;   // text shown while pressed
};

struct Progress {
    Window base;
    int32_t value; // nothing is drawn while zero
    // Zero from creation; nothing reads it.
    int32_t reserved_after_value;
    int32_t range;
    uint8_t reverse; // fill from the far end
};

/// Creates a Gadget: a plain window that draws its state image or colour and
/// its children, and raises through its parent.
///
/// @return the new gadget; release it with window_destroy
Window* gadget_create();
/// Creates a Button with the three button states, Unpressed current.
///
/// @return the new button; release it with window_destroy
Window* button_create();
/// Creates a NoFocusBtn: a button that never takes the keyboard focus.
///
/// @return the new button; release it with window_destroy
Window* no_focus_button_create();
/// Creates an SButton: a button that activates on press and repeats while
/// held, reporting drags and its release to the parent.
///
/// @return the new scroll button; release it with window_destroy
Window* scroll_button_create();
/// Creates a Static label, which cannot take the keyboard focus.
///
/// @return the new label; release it with window_destroy
Window* static_create();
/// Creates a CheckBox with its five states, Unchecked Up current, and a
/// 6-pixel gap before the label.
///
/// @return the new check box; release it with window_destroy
CheckBox* check_box_create();
/// Creates a RadioButton with the three button states, Unpressed current.
///
/// @return the new radio button; release it with window_destroy
Window* radio_button_create();
/// Creates a StageBtn with no saved caption.
///
/// The caller gives it its stage states with window_set_state_count.
///
/// @return the new stage button; release it with window_destroy
StageButton* stage_button_create();
/// Creates a Progress bar with a zero value and range (nothing is drawn).
///
/// @return the new progress bar; release it with window_destroy
Progress* progress_create();
/// Tests whether a check box is checked.
///
/// @param box check box to test
/// @return true in the Checked Up and Checked Down states
bool check_box_checked(const CheckBox* box);
/// Sets a progress bar's value and range; the bar fills value / range of its
/// length.
///
/// @param[in,out] progress progress bar to update
/// @param value filled amount; 0 draws nothing
/// @param range amount that fills the whole bar; 0 draws nothing
void progress_set(Progress* progress, int32_t value, int32_t range);

// ---------------------------------------------------------------------------
// Text entry

extern const WindowOps k_edit_box_ops;

// One-line entry; text never grows wider than the box. A non-zero mask is
// drawn in place of every character; the text kept is what was typed. The
// text is UTF-8. `length` and `caret` count characters.
struct EditBox {
    Window base;
    char* text;           // at most 128 characters
    int32_t length;       // characters
    int32_t caret;        // character index
    int32_t caret_height; // height of "I" plus 2, set on layout
    int32_t padding;      // horizontal inset of the text
    char mask{};          // drawn instead of every character when non-zero
    // Characters the player is still composing, drawn at the caret and not
    // part of the text.
    char* composition;
    // `text` with `composition` at the caret. Rebuilt when asked for; valid
    // until the edit next changes.
    char* shown;
};

/// Creates an EditBox with an empty 128-character buffer and the two states
/// "Disabled" and "Enabled", Enabled current.
///
/// @return the new edit box; release it with window_destroy
EditBox* edit_box_create();
/// Inserts a character at the caret and advances the caret.
///
/// The character is stored whole. Beeps instead when the code is not one the
/// player can type, when the buffer already holds 128 characters, or when the
/// whole text plus the character would be wider than the box less its padding
/// on both sides. A masked edit measures the mask character, so the box fits
/// what it shows.
///
/// @param[in,out] box edit box to edit
/// @param ch character code to insert
void edit_box_insert(EditBox* box, int32_t ch);
/// Deletes the character before the caret (Backspace) or at the caret
/// (Delete).
///
/// One character is removed, however many bytes it occupies. Beeps when
/// there is no character to delete.
///
/// @param[in,out] box edit box to edit
/// @param forward true to delete at the caret, false to delete before it
void edit_box_delete(EditBox* box, bool forward);
/// Inserts typed text at the caret, one character at a time.
///
/// Characters the player is composing are cleared first. Bytes that are not
/// characters are skipped. A character that does not fit stops the rest, and
/// a non-empty text that adds nothing beeps.
///
/// @param[in,out] box edit box to edit
/// @param text typed text; null or empty only clears the composition
void edit_box_insert_text(EditBox* box, const char* text);
/// Shows characters the player is composing, at the caret.
///
/// They are drawn in the field and are not part of its text. Null or empty
/// clears them.
///
/// @param[in,out] box edit box to edit
/// @param text characters being composed; null clears them
void edit_box_set_composition(EditBox* box, const char* text);
/// Returns the characters being composed.
///
/// @param box edit box to read
/// @return those characters, or "" when none are
const char* edit_box_composition(const EditBox* box);
/// Returns the text with the characters being composed inserted at the caret.
///
/// The pointer stays valid until the box next changes.
///
/// @param[in,out] box edit box to read
/// @return the text the field shows
const char* edit_box_shown(EditBox* box);
/// Measures from the box's left edge to the caret the player sees.
///
/// Characters being composed sit before that caret. A masked box measures
/// mask characters.
///
/// @param box edit box to measure
/// @return the distance in pixels
int32_t edit_box_caret_distance(const EditBox* box);

extern const WindowOps k_single_edit_ops;

// One-line entry with a selection, word movement, an optional length limit,
// a password mask and (with `scroll`) a horizontally scrolling view window
// [first_visible, last_visible) over text longer than the box. The text is
// UTF-8. Length, caret, selection and the view count characters.
struct SingleEdit {
    Window base;
    char* text;              // UTF-8 storage; `length` characters used
    int32_t capacity;        // bytes allocated for `text`
    int32_t length;          // characters
    int32_t selection_start; // empty when equal to selection_end
    int32_t selection_end;
    int32_t caret;
    Handle caret_color;       // white
    int32_t caret_height;     // -1 = font line height - 2
    Handle selection_color;   // blue
    int32_t selection_height; // -1 = caret height
    int32_t blink_counter;    // draws since the last blink period
    int32_t padding;          // horizontal inset of the text (6)
    int32_t max_length;       // 0 = unlimited
    int32_t blink_period;     // draws per caret blink cycle (16)
    uint8_t blink;            // caret shown only in the second half of a period
    uint8_t caret_visible;
    uint8_t focus_frame;    // draw the focus frame while focused
    uint8_t notify_changes; // call on_changed after each typed character
    uint8_t scroll;         // text may be wider than the box
    int32_t first_visible;
    int32_t last_visible; // one past the last visible character
    char mask;            // drawn instead of every character when non-zero
    // Called after a typed character when notify_changes is set; the base
    // class does nothing.
    void (*on_changed)(SingleEdit* edit);
    void* user; // owner context for on_changed
    // Characters the player is still composing, drawn at the caret and not
    // part of the text.
    char* composition;
    // `text` with `composition` at the caret. Rebuilt when asked for; valid
    // until the edit next changes.
    char* shown;
    // Where a 14-pixel line sat before a glyph taller than the font's line
    // lifted it. Zero until that line is first fitted.
    int32_t rest_y;
    int32_t rest_height;
};

/// Creates a SingleEdit with empty text.
///
/// The edit has the two states "Disabled" and "Enabled" (Enabled current), a
/// white background, a white caret, a blue selection band, 6 pixels of
/// padding and a caret blinking with a 16-draw period. It has no length
/// limit, mask, focus frame or scrolling.
///
/// @return the new edit; release it with window_destroy
SingleEdit* single_edit_create();
/// Replaces the text, puts the caret at its end, clears the selection and
/// fits the view to the box.
///
/// Text wider than the box loses characters from its end, or with `scroll`
/// set scrolls the view so its end is visible. A 14-pixel line grows to 17
/// when a glyph is taller than the font's line, and sits two pixels higher.
/// Any other height stays where it is.
///
/// @param[in,out] edit edit to fill
/// @param text new text; null counts as ""; cut to max_length when a limit
///     is set
void single_edit_set_text(SingleEdit* edit, const char* text);
/// Returns the edit's text.
///
/// @param edit edit to read
/// @return the text, or "" when the edit has no storage
const char* single_edit_text(const SingleEdit* edit);
/// Inserts a character at the caret and advances the caret.
///
/// Beeps instead when the text is at max_length, or when it would grow wider
/// than the box and the edit does not scroll. A masked edit measures the mask
/// character, so the box fits what it shows. A scrolling edit moves its view
/// to keep the caret visible. The selection is not changed.
///
/// @param[in,out] edit edit to change
/// @param ch character code to insert
void single_edit_insert(SingleEdit* edit, int32_t ch);
/// Removes the character before the caret (Backspace) or at the caret
/// (Delete).
///
/// Beeps when there is no character to remove. A scrolling edit then widens
/// its view again over the text now in reach.
///
/// @param[in,out] edit edit to change
/// @param forward true to remove at the caret, false to remove before it
void single_edit_erase(SingleEdit* edit, bool forward);
/// Removes the selected characters, leaves the caret at the selection start
/// and clears the selection.
///
/// @param[in,out] edit edit to change
void single_edit_erase_selection(SingleEdit* edit);
/// Selects the characters between two positions, in either order.
///
/// The caret does not move.
///
/// @param[in,out] edit edit whose selection changes
/// @param from one end of the selection, a character index; clamped to the
///     text
/// @param to other end of the selection; equal to `from` clears the selection
void single_edit_select(SingleEdit* edit, int32_t from, int32_t to);
/// Moves the caret and scrolls the view to keep it visible.
///
/// @param[in,out] edit edit whose caret moves
/// @param caret new caret position, a character index clamped to
///     [0, length]
void single_edit_set_caret(SingleEdit* edit, int32_t caret);
/// Sets the maximum text length; text already longer is kept.
///
/// @param[in,out] edit edit to limit
/// @param max_length maximum number of characters; 0 or negative means
///     unlimited
void single_edit_set_max_length(SingleEdit* edit, int32_t max_length);
/// Sets the height of the selection band, at most the box height.
///
/// @param[in,out] edit edit to change
/// @param height band height in pixels; -1 uses the caret height
void single_edit_set_selection_height(SingleEdit* edit, int32_t height);
/// Replaces the selection band colour, releasing the previous one if set.
///
/// @param[in,out] edit edit whose colour changes
/// @param r red component, 0-255
/// @param g green component, 0-255
/// @param b blue component, 0-255
/// @param a alpha component, 0-255
void single_edit_set_selection_color(SingleEdit* edit, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
/// Replaces the caret colour, releasing the previous one if set.
///
/// @param[in,out] edit edit whose colour changes
/// @param r red component, 0-255
/// @param g green component, 0-255
/// @param b blue component, 0-255
/// @param a alpha component, 0-255
void single_edit_set_caret_color(SingleEdit* edit, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
/// Measures the visible text before the caret the player sees.
///
/// Characters being composed are included when the caret is inside the view.
/// A mask measures that many mask characters.
///
/// @param edit edit to measure
/// @return the width in pixels from first_visible up to that caret; 0 when
///     nothing before it is in view
int32_t single_edit_caret_offset(const SingleEdit* edit);
/// Moves the caret one character left.
///
/// Without `extend` the selection is cleared. With it, a selection lying
/// before the caret shrinks from its end, otherwise the selection grows to
/// the left. The view scrolls to keep the caret visible.
///
/// @param[in,out] edit edit whose caret moves
/// @param extend true while Shift is held
void single_edit_move_left(SingleEdit* edit, bool extend);
/// Moves the caret one character right.
///
/// Without `extend` the selection is cleared. With it, a selection lying
/// after the caret shrinks from its start, otherwise the selection grows to
/// the right. The view scrolls to keep the caret visible.
///
/// @param[in,out] edit edit whose caret moves
/// @param extend true while Shift is held
void single_edit_move_right(SingleEdit* edit, bool extend);
/// Moves the caret to the start of the word before it.
///
/// One space directly before the caret is stepped over first; the caret then
/// moves left until the character before it is a space.
///
/// @param[in,out] edit edit whose caret moves
/// @param extend true while Shift is held (see single_edit_move_left)
void single_edit_move_word_left(SingleEdit* edit, bool extend);
/// Moves the caret past the rest of the current word and the spaces after
/// it.
///
/// @param[in,out] edit edit whose caret moves
/// @param extend true while Shift is held (see single_edit_move_right)
void single_edit_move_word_right(SingleEdit* edit, bool extend);
/// Moves the caret to the start of the text.
///
/// Without `extend` the selection is cleared; with it the selection runs from
/// the start to the end of the selection not at the caret (or to the caret
/// when nothing was selected).
///
/// @param[in,out] edit edit whose caret moves
/// @param extend true while Shift is held
void single_edit_move_home(SingleEdit* edit, bool extend);
/// Moves the caret to the end of the text.
///
/// Without `extend` the selection is cleared; with it the selection runs from
/// the end of the selection not at the caret (or from the caret when nothing
/// was selected) to the end.
///
/// @param[in,out] edit edit whose caret moves
/// @param extend true while Shift is held
void single_edit_move_end(SingleEdit* edit, bool extend);
/// Inserts typed text at the caret, one character at a time.
///
/// Characters being composed are cleared first. A selection is removed once,
/// before the first character that is kept. Bytes that are not characters
/// are skipped. A disabled edit changes nothing. A character that does not
/// fit stops the rest, and a non-empty text that adds nothing beeps. When
/// notify_changes is set, on_changed runs once per character added.
///
/// @param[in,out] edit edit to change
/// @param text typed text; null or empty only clears the composition
void single_edit_insert_text(SingleEdit* edit, const char* text);
/// Shows characters the player is composing, at the caret.
///
/// They are drawn in the field and are not part of its text. A masked edit
/// draws that many more mask characters and does not reveal them. Null or
/// empty clears them.
///
/// @param[in,out] edit edit to change
/// @param text characters being composed; null clears them
void single_edit_set_composition(SingleEdit* edit, const char* text);
/// Returns the characters being composed.
///
/// @param edit edit to read
/// @return those characters, or "" when none are
const char* single_edit_composition(const SingleEdit* edit);
/// Returns the text with the characters being composed inserted at the caret.
///
/// The pointer stays valid until the edit next changes.
///
/// @param[in,out] edit edit to read
/// @return the text the field shows
const char* single_edit_shown(SingleEdit* edit);
/// Measures from the edit's left edge to the caret the player sees.
///
/// The caret drawn in the field is this far from the edit's left, including
/// characters being composed when they are in view.
///
/// @param edit edit to measure
/// @return the distance in pixels
int32_t single_edit_caret_distance(const SingleEdit* edit);

// Where a text field sits, in screen pixels, and how far the caret is from
// its left. The caret's distance is in the same pixels.
struct TextFieldPlace {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    int32_t cursor;
};

/// Gives an edit box's place and the caret the player sees.
///
/// An edit box or a single-line edit fills `place`. Any other window does
/// not.
///
/// @param window window to measure; may be null
/// @param[out] place the field, in screen pixels
/// @return true when `window` is an edit box
bool window_text_field_place(const Window* window, TextFieldPlace* place);

// ---------------------------------------------------------------------------
// Scrolling: Slider and ListBox

extern const WindowOps k_slider_ops;
extern const WindowOps k_list_box_ops;

// Scroll bar. Children named "decbutton" and "incbutton" (scroll buttons)
// step it and an "sbutton" child is the thumb, all created by the owner.
struct Slider {
    Window base;
    int32_t range; // number of positions (100 when created)
    int32_t index; // current position (50 when created)
    int32_t page_step;
    int32_t press_x; // last track press
    int32_t press_y;
    int32_t repeat_count; // pulses since the arrow/track was pressed
    int32_t repeat_delay; // pulses before auto-repeat (15)
};

// Vertical list. Its first child must be a Slider named "slider"; every
// further child is an item.
struct ListBox {
    Window base;
    int32_t capacity; // items kept; adding more drops the oldest (500)
    int32_t count;
    int32_t top; // first visible item
    int32_t selected;
    int32_t last;             // last visible item
    int32_t page;             // visible item count
    Handle highlight;         // selection colour
    Handle focused_highlight; // selection colour while focused
    uint8_t bottom_align;     // stack items against the bottom edge
    uint8_t no_highlight;
    uint8_t keep_slider; // hide (not disable) the slider when all items fit
    uint8_t log_mode;    // no selection; keys scroll (chat logs)
};

/// Creates a Slider (see slider_init for its initial values).
///
/// The owner adds the "decbutton", "incbutton" and "sbutton" children.
///
/// @return the new slider; release it with window_destroy
Slider* slider_create();
/// Initialises a zeroed record as a Slider (the constructor that classes
/// derived from Slider call).
///
/// The slider gets 100 positions with the index at 50, a page step of 1, a
/// 15-pulse auto-repeat delay and the three button states with Unpressed
/// current.
///
/// @param[in,out] slider zeroed record to initialise
/// @param ops dispatch table of the slider's class
void slider_init(Slider* slider, const WindowOps* ops);
/// Initialises a zeroed record as a ListBox (the constructor that classes
/// derived from ListBox call).
///
/// The list keeps up to 500 items and gets its default selection colours
/// (RGBA 9E 6E 2D 4B, and 6F 00 CE 64 while focused).
///
/// @param[in,out] list zeroed record to initialise
/// @param ops dispatch table of the list's class
void list_box_init(ListBox* list, const WindowOps* ops);
/// Changes the number of slider positions.
///
/// With `keep_position` the index is rescaled to the same relative position
/// (rounded) and the thumb re-centred; from a single-position range the index
/// goes to the far end (the last position when reversed, else 0). Without it
/// neither the index nor the thumb changes.
///
/// @param[in,out] slider slider to change
/// @param range new number of positions; values below 1 count as 1
/// @param keep_position whether to rescale the index to the new range
void slider_set_range(Slider* slider, int32_t range, bool keep_position);
/// Returns the number of slider positions.
///
/// @param slider slider to read
/// @return the range
int32_t slider_range(const Slider* slider);
/// Returns the slider's current position.
///
/// @param slider slider to read
/// @return the index, 0 to range - 1
int32_t slider_index(const Slider* slider);
/// Sets the slider's position and centres the "sbutton" thumb on it, without
/// telling the parent.
///
/// @param[in,out] slider slider to change
/// @param index new position, clamped to 0..range - 1
void slider_set_index(Slider* slider, int32_t index);
/// Enables a slider: the slider and each of its children show Unpressed.
///
/// @param[in,out] slider slider window (its buttons change too)
void slider_enable(Window* slider);
/// Disables a slider: the slider and each of its children show Disabled.
///
/// @param[in,out] slider slider window (its buttons change too)
void slider_disable(Window* slider);
/// Sets the slider's position like slider_set_index, then reports
/// NotifyCode::activated to the parent.
///
/// @param[in,out] slider slider to change
/// @param index new position, clamped to 0..range - 1
void slider_set_value(Slider* slider, int32_t index);
/// Creates a ListBox (see list_box_init for its initial values).
///
/// The owner adds a Slider named "slider" as its first child before any item.
///
/// @return the new list; release it with window_destroy
ListBox* list_box_create();
/// Replaces the selection colour, releasing the previous one if set.
///
/// @param[in,out] list list whose colour changes
/// @param r red component, 0-255
/// @param g green component, 0-255
/// @param b blue component, 0-255
/// @param a alpha component, 0-255
void list_box_set_highlight(ListBox* list, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
/// Replaces the selection colour used while the list has the focus,
/// releasing the previous one if set.
///
/// @param[in,out] list list whose colour changes
/// @param r red component, 0-255
/// @param g green component, 0-255
/// @param b blue component, 0-255
/// @param a alpha component, 0-255
void list_box_set_focused_highlight(ListBox* list, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
/// Appends an item to the list (list_box_insert at the end).
///
/// @param[in,out] list list to add to
/// @param[in,out] item item window; the list becomes its parent
void list_box_add(ListBox* list, Window* item);
/// Adds an item to the list and lays the list out again.
///
/// The item moves to the list's top-left corner, is hidden until the layout
/// shows it and goes before the list's child at index `position`. When the
/// list already holds `capacity` items the oldest item is destroyed instead
/// of the count growing.
///
/// @param[in,out] list list to add to
/// @param[in,out] item item window; null is ignored; the list becomes its
///     parent
/// @param position child index to insert before (the slider counts as child
///     0); an index outside the children appends
void list_box_insert(ListBox* list, Window* item, int32_t position);
/// Detaches an item from the list without destroying it and lays the list
/// out again.
///
/// When the item lies at or before the selection (or the top item), the
/// selection (and the top and last visible items) move back one so they stay
/// on the same entries.
///
/// @param[in,out] list list to remove from
/// @param[in,out] item item to detach; its parent becomes null; an item not
///     in the list is ignored
void list_box_remove(ListBox* list, Window* item);
/// Destroys every item (a leading Slider child stays), resets the count,
/// selection and view to zero and lays the list out again.
///
/// @param[in,out] list list to clear
void list_box_clear(ListBox* list);
/// Selects an item, scrolls it into view and reports NotifyCode::activated to
/// the parent.
///
/// Nothing happens in log mode or when the item is already selected.
///
/// @param[in,out] list list whose selection changes
/// @param index item index, clamped to 0..count - 1
void list_box_select(ListBox* list, int32_t index);
/// Returns the selected item.
///
/// @param list list to read
/// @return the selected item index; 0 in log mode
int32_t list_box_selected(const ListBox* list);
/// Returns an item by index; item i is the list's child i + 1, after the
/// slider.
///
/// @param list list to read
/// @param index item index
/// @return the item, or null when the index is outside the children
Window* list_box_item(ListBox* list, int32_t index);
/// Finds an item's index, not counting the slider.
///
/// @param list list to search
/// @param item item to find
/// @return the item index, or -1 when the item is not in the list
int32_t list_box_index_of(ListBox* list, const Window* item);
/// Finds the first item whose current caption equals `text`
/// (case-sensitive); Slider children are skipped.
///
/// @param list list to search
/// @param text caption to find; null finds nothing
/// @return the item, or null
Window* list_box_find_text(ListBox* list, const char* text);
/// Scrolls the view by a number of items and lays the list out again.
///
/// @param[in,out] list list to scroll
/// @param delta items to scroll, negative towards the start; the top item is
///     clamped to 0..count - page; 0 does nothing
void list_box_scroll(ListBox* list, int32_t delta);
/// Scrolls so the last item is visible, unless it already is.
///
/// @param[in,out] list list to scroll
void list_box_scroll_to_end(ListBox* list);

// ---------------------------------------------------------------------------
// Containers

extern const WindowOps k_panel_ops;
extern const WindowOps k_dialog_ops;
extern const WindowOps k_group_box_ops;
extern const WindowOps k_page_control_ops;
extern const WindowOps k_iwindow_ops;
extern const WindowOps k_iwnd_ops;

// One tab of a PageControl: activating `tab` shows `page`.
struct PageControlEntry {
    Window* page;
    Window* tab;
};

struct PageControl {
    Window base;
    Window* selected; // page currently shown
    // Zero from creation; nothing reads it.
    uint8_t reserved_after_selected;
    PageControlEntry** pages; // vector of entries
    int32_t page_count;
};

// Top-level window that can be minimised to an icon rectangle.
struct IWindow {
    Window base;
    int32_t restore_x; // open rectangle, saved while minimised
    int32_t restore_y;
    int32_t restore_width;
    int32_t restore_height;
    int32_t icon_x; // minimised rectangle (20,20,20,20 initially)
    int32_t icon_y;
    int32_t icon_width;
    int32_t icon_height;
    uint8_t minimized;
};

// IWindow with a minimise button that can be dragged by its body ("IWnd").
struct IWnd {
    IWindow window;
    uint8_t dragging;
    int32_t drag_x; // last pointer position
    int32_t drag_y;
};

/// Creates a Panel: a container that draws its state image or colour and its
/// children, passes its children's events to its own parent and moves the
/// focus among its children, wrapping.
///
/// @return the new panel; release it with window_destroy
Window* panel_create();
/// Creates a Dialog: a container that keeps the keyboard focus among its
/// children and swallows pointer presses, beeping at those outside it.
///
/// @return the new dialog; release it with window_destroy
Window* dialog_create();
/// Creates a GroupBox: a panel that draws its state image and caption (no
/// background colour) and moves the focus through its children without
/// wrapping before passing it to its parent.
///
/// @return the new group box; release it with window_destroy
Window* group_box_create();
/// Creates a PageControl with no pages.
///
/// Its tabs are its children; activating a registered tab shows that tab's
/// page.
///
/// @return the new page control; release it with window_destroy
PageControl* page_control_create();
/// Registers a tab and the page it shows; visibility is not changed.
///
/// @param[in,out] control page control to extend
/// @param tab window whose NotifyCode::activated selects the page
/// @param page window shown while the tab is selected
void page_control_add(PageControl* control, Window* tab, Window* page);
/// Looks up the page registered for a tab.
///
/// @param control page control to search
/// @param tab tab window; null finds nothing
/// @return the first page registered for `tab`, or null
Window* page_control_page_for(const PageControl* control, const Window* tab);
/// Shows a page: the previously selected page is hidden and `page` shown and
/// selected.
///
/// Selecting the page already selected does nothing.
///
/// @param[in,out] control page control to change
/// @param[in,out] page page to show; null only hides the current page
void page_control_select(PageControl* control, Window* page);
/// Creates an IWindow with its initial fields (see iwindow_init_fields).
///
/// @return the new window; release it with window_destroy
IWindow* iwindow_create();
/// Sets the IWindow fields of a record: a 20,20,20,20 icon rectangle, an
/// empty open rectangle, not minimised, and the three states "Disabled",
/// "Normal" and "Icon" with Normal current.
///
/// The state count change releases every state resource already held.
///
/// @param[in,out] window record to set up, already initialised as a window
void iwindow_init_fields(IWindow* window);
/// Creates an IWnd frame with the IWindow fields; its minimise button is
/// added each time it is placed (window_create/window_set_bounds).
///
/// @return the new frame; release it with window_destroy
IWnd* iwnd_create();
/// Minimises an open IWindow to its icon rectangle.
///
/// The open rectangle is saved, the window takes the icon rectangle (running
/// its refresh slot), shows the Icon state and hides every child; children
/// are not moved. An already minimised window is left alone.
///
/// @param[in,out] window window to minimise
void iwindow_minimize(IWindow* window);
/// Restores a minimised IWindow to its saved open rectangle.
///
/// The icon rectangle is saved (so the icon remembers where it was dragged),
/// the window takes the open rectangle, shows the Normal state and shows
/// every child. A window that is not minimised is left alone.
///
/// @param[in,out] window window to restore
void iwindow_restore(IWindow* window);

// ---------------------------------------------------------------------------
// DockBar and GridBox

extern const WindowOps k_dock_bar_ops;
extern const WindowOps k_grid_box_ops;

// Screen edges a DockBar can attach to (combinations allow several).
namespace dock_edge {
inline constexpr int32_t top = 0x1;
inline constexpr int32_t bottom = 0x2;
inline constexpr int32_t left = 0x4;
inline constexpr int32_t right = 0x8;
inline constexpr int32_t floating = 0x10; // never re-docks
} // namespace dock_edge

// Notify code a DockBar sends its parent after being dropped on an edge.
inline constexpr NotifyCode k_notify_docked = static_cast<NotifyCode>(6);

// Tool bar the user drags between the desktop edges. While dragged it hands
// the outline of the edge it would dock to to the draw service's frame slot
// (which draws nothing); on release it takes that rectangle and restacks its
// children in columns.
struct DockBar {
    Window base;       // Panel
    int32_t edges;     // dock_edge mask of allowed edges
    int32_t edge;      // edge the children are laid out for
    int32_t drop_edge; // edge chosen by the drag
    uint8_t dragging;
    int32_t outline_x; // rectangle the bar takes when dropped
    int32_t outline_y;
    int32_t outline_width;
    int32_t outline_height;
    int32_t dock_width;  // width when docked left/right (50 when empty)
    int32_t dock_height; // height when docked top/bottom (50 when empty)
    // Zero from creation; nothing reads them.
    int32_t reserved_after_dock_height[2];
    int32_t drag_x; // last pointer position of the drag
    int32_t drag_y;
    int32_t spacing; // gap around children that are not GroupBoxes
};

/// Creates a DockBar with no allowed edges, no spacing and no docked size.
///
/// The owner sets `edges` (and `spacing`) before the bar is dragged.
///
/// @return the new bar; release it with window_destroy
DockBar* dock_bar_create();
/// Computes the docked size the bar needs for its children.
///
/// An empty bar gets 50 pixels for each docked size still unset. Otherwise
/// dock_width and dock_height grow to the largest child (plus the spacing on
/// both sides, except for GroupBoxes), then dock_height grows until the
/// children fit a bar along the top edge of the desktop or it reaches half
/// the desktop height. `edge` ends up holding the allowed-edge mask.
///
/// @param[in,out] bar bar to measure
/// @quirk Only dock_height is then grown until the children fit a bar along
///     the top edge; dock_width stays at the widest child and is never grown
///     to fit a bar along the left edge, as in 3.1c.
void dock_bar_measure(DockBar* bar);
/// Stacks the bar's children in columns for its current `edge`.
///
/// Children go down from (x, y), each offset by the spacing (GroupBoxes only
/// along the bar); a child that would pass the bottom starts a new column to
/// the right of the widest child so far. The bottom and right limits are
/// measured from the bar's own position plus `height` and `width`.
///
/// @param[in,out] bar bar whose children are arranged
/// @param x left edge of the first column in screen pixels
/// @param y top of the first column in screen pixels
/// @param width width available, in pixels
/// @param height height available, in pixels
/// @param place whether to move the children (false only tests the fit)
/// @return false when a new column would reach the right limit; true when
///     everything fits, the bar has no children, or `edge` is not a single
///     edge
bool dock_bar_arrange(
    DockBar* bar, int32_t x, int32_t y, int32_t width, int32_t height, bool place
);
/// Chooses the drop edge for a pointer position and sets the drop outline for
/// it.
///
/// Among the allowed edges the desktop is split along its diagonals (or into
/// halves between two opposite edges) and the pointer's region picks the
/// edge. The outline spans the desktop along that edge with the docked width
/// or height. A bar allowing a single edge, a floating bar, or an empty
/// desktop keeps its current drop.
///
/// @param[in,out] bar bar being dragged
/// @param x pointer x in screen pixels
/// @param y pointer y in screen pixels
void dock_bar_snap(DockBar* bar, int32_t x, int32_t y);

// One GridBox entry: shown in a cell as its name, state and state images.
struct GridItem {
    char* name;
    int32_t value; // state index for the cell
    Handle images[3];
};

// Grid of rows x columns equal cells paging through a list of items.
struct GridBox {
    Window base;
    int32_t cell_width;
    int32_t cell_height;
    // Zero from creation; nothing reads it.
    uint8_t reserved_after_cell_height;
    GridItem** items;
    int32_t item_count;
    int32_t item_capacity;
    int32_t rows;
    int32_t columns;
    int32_t first;         // item shown in the first cell
    Handle empty_image;    // image of cells past the last item
    Handle cell_sounds[3]; // per-state sounds given to every cell
};

// Page commands carried in Notify::value with NotifyCode 4.
namespace grid_page {
inline constexpr int32_t previous = 5;
inline constexpr int32_t next = 6;
} // namespace grid_page

/// Creates a GridBox with no items, rows, columns, empty-cell image or cell
/// sounds.
///
/// The owner sets the cell size, rows and columns before placing the grid;
/// placing it creates the cells.
///
/// @return the new grid; release it with window_destroy
GridBox* grid_box_create();
/// Appends an item to the grid's list; the cells are not refilled.
///
/// @param[in,out] grid grid to extend
/// @param name item name, copied; null counts as ""
/// @param value state index the item's cell shows
/// @param images images for cell states 0-2; each gains a reference; null
///     gives the item none
void grid_box_add_item(GridBox* grid, const char* name, int32_t value, const Handle images[3]);
/// Frees every item (releasing its images and name); the cells and `first`
/// are left as they are.
///
/// @param[in,out] grid grid to empty
void grid_box_clear(GridBox* grid);
/// Steps back one page and refills the cells.
///
/// From the first page it wraps to the page holding the last full multiple
/// of the page size.
///
/// @param[in,out] grid grid to page
void grid_box_page_previous(GridBox* grid);
/// Steps forward one page and refills the cells, wrapping to the start once
/// the new first item lies beyond the item count.
///
/// @param[in,out] grid grid to page
/// @quirk With an item count that is an exact multiple of the page size, the
///     page after the last one is shown before wrapping; it shows the first
///     items again.
void grid_box_page_next(GridBox* grid);
/// Shows the items from `first` in the cells.
///
/// Cells show consecutive items (name, state and state images). Cells past
/// the last item show "empty" with the empty-cell image up to the end of the
/// last page, after which the items repeat from the first. A grid with no
/// rows or columns changes nothing.
///
/// @param[in,out] grid grid whose cells are refilled
void grid_box_fill(GridBox* grid);

/// Replaces the sound of one state, releasing the previous one.
///
/// The window takes over the caller's reference; no reference is added.
///
/// @param[in,out] self window whose state sound changes
/// @param state state index; ignored when outside the window's states
/// @param sound SoundService handle, or k_no_handle for none
void window_set_state_sound(Window* self, int32_t state, Handle sound);

// ---------------------------------------------------------------------------
// Desktop: the root window manager (the first service slot)

struct Desktop {
    int32_t x; // screen area
    int32_t y;
    int32_t width;
    int32_t height;
    Window* focus;      // receives key messages
    Window* capture;    // receives pointer messages first
    Window* hover;      // last window that accepted mouse_move
    WindowList windows; // top-level windows, back to front
    // Called after the keyboard focus changes, including when it is cleared.
    // `focused` is the window that now has the keyboard, or null.
    void (*on_focus)(Desktop* desktop, Window* focused, void* user);
    void* focus_user;
    // 1 while on_focus is running, so the callback cannot re-enter itself.
    uint8_t focus_notify;
};

/// Resets a desktop to an empty one covering a screen area.
///
/// @param[out] desktop desktop to reset; nothing it held is released
/// @param x left edge of the area in screen pixels
/// @param y top edge of the area in screen pixels
/// @param width area width in pixels
/// @param height area height in pixels
void desktop_init(Desktop* desktop, int32_t x, int32_t y, int32_t width, int32_t height);
/// Destroys every window flagged `owned`, at any depth.
///
/// Owned top-level windows go first, then the owned descendants of the
/// remaining windows, depth first.
///
/// @param[in,out] desktop desktop to clean up
void desktop_destroy_owned(Desktop* desktop);
/// Frees the top-level list and clears the focus, capture and hover; the
/// windows themselves are not destroyed.
///
/// @param[in,out] desktop desktop to release
void desktop_release(Desktop* desktop);
/// Adds a top-level window at the top of the z order, unless it is already
/// there.
///
/// @param[in,out] desktop desktop to add to
/// @param window window to add; null is ignored
void desktop_add(Desktop* desktop, Window* window);
/// Takes a window off the top-level list; the focus, capture and hover are
/// not touched.
///
/// @param[in,out] desktop desktop to remove from
/// @param window window to remove; a window not on the list is ignored
void desktop_remove(Desktop* desktop, Window* window);
/// Moves a top-level window to the top of the z order.
///
/// @param[in,out] desktop desktop whose order changes
/// @param window window to raise; a window not on the list is ignored
void desktop_raise(Desktop* desktop, Window* window);
/// Moves the keyboard focus, sending focus_lost to the old focus and
/// focus_gained to the new one.
///
/// @param[in,out] desktop desktop whose focus changes
/// @param window window to focus, or null to clear the focus; the current
///     focus changes nothing
void desktop_set_focus(Desktop* desktop, Window* window);
/// Sets the window that receives pointer messages first.
///
/// @param[in,out] desktop desktop whose capture changes
/// @param window capturing window, or null to release the capture
void desktop_set_capture(Desktop* desktop, Window* window);
/// Routes an input message to the window that should handle it.
///
/// Key messages go to the focus first. A direction key the focus does not use
/// moves the focus to the nearest control in that direction, wrapping around
/// the screen; controls within ten pixels of a column are stepped vertically.
/// With nothing focused, a direction key or Enter or Space focuses the first
/// control in reading order and does not press it. While a dialog is showing,
/// the direction keys stay inside that dialog. Any other message goes first
/// to the capturing window, and is done if that handles it. A pointer move
/// then goes to the deepest shown window under the pointer, which becomes the
/// hover window if it accepts (the previous hover gets mouse_leave, the new
/// one mouse_enter). Button messages (0x201-0x209) go to the window under the
/// pointer, after moving the focus to that window's focus target. Other
/// messages, the wheel included, reach only the capture.
///
/// @param[in,out] desktop desktop routing the message
/// @param msg input message; null is not handled
/// @return true when a window handled the message
bool desktop_route(Desktop* desktop, const Message* msg);
/// Tests whether the desktop has any top-level window.
///
/// @param desktop desktop to test
/// @return true when the top-level list is not empty
bool desktop_has_windows(const Desktop* desktop);
/// Sends draw to every top-level window, back to front.
///
/// @param desktop desktop to draw
void desktop_draw(Desktop* desktop);
/// Runs the pulse slot of every window, each top-level tree parents before
/// children.
///
/// @param desktop desktop to pulse
void desktop_pulse(Desktop* desktop);
/// Runs the tick slot of every top-level window.
///
/// @param desktop desktop to tick
void desktop_tick(Desktop* desktop);
/// Finds the window under a point through the top-level windows' child_at
/// slots; the top-most top-level window that reports a hit wins.
///
/// @param desktop desktop to search
/// @param x pointer x in screen pixels
/// @param y pointer y in screen pixels
/// @return the reported window, or null when no top-level window is hit
Window* desktop_window_at(Desktop* desktop, int32_t x, int32_t y);
/// Tests whether a window is a top-level window of the desktop or inside
/// one; a destroyed window is neither.
///
/// @param desktop desktop to search; null holds nothing
/// @param window window to find; null is never held
/// @return true when the window is in one of the desktop's trees
bool desktop_holds(const Desktop* desktop, const Window* window);

// What a control is, as a program driving the game names it.
enum class DriverControlKind : uint8_t {
    button,     // a push button
    check_box,  // a check box, or a radio button that is selected or not
    list,       // a list of rows
    text_field, // a field that takes typed text
    slider,     // a scroll bar
    label,      // text that takes no pointer
    area,       // a surface that takes the pointer
    image,      // a picture that takes no pointer
};

// One control a program driving the game can list. Pointers are valid only
// for the visit call that carries the record.
struct DriverControlView {
    const char* name;                  // as the window spells it; empty is left out
    DriverControlKind kind;            // what it is
    int32_t x;                         // left edge in screen pixels
    int32_t y;                         // top edge in screen pixels
    int32_t width;                     // width in screen pixels
    int32_t height;                    // height in screen pixels
    bool visible;                      // it is shown
    bool enabled;                      // it takes a click
    bool focused;                      // it holds the keyboard focus
    bool checked;                      // a check box or radio button is selected
    const char* text;                  // its caption, or the text a field holds; empty when masked
    bool masked = false;               // a password field; text stays empty
    int32_t text_length = 0;           // characters a masked field holds
    const char* const* items;          // a list's rows; null when it is not a list
    int32_t item_count;                // how many rows `items` holds
    int32_t first_visible;             // a list's first row shown
    int32_t rows;                      // the rows a list shows at once
    int32_t row_height;                // a list's row pitch in screen pixels
    int32_t selected;                  // a list's selected row; -1 for none
    const char* scroll_up = nullptr;   // "decbutton" of a list or a slider; null when it has none
    const char* scroll_down = nullptr; // "incbutton" of a list or a slider; null when it has none
};

// Receives one shown control. `window` is the console or panel it sits on.
using DriverControlVisitor =
    void (*)(void* context, const char* window, const DriverControlView& control);

/// Lists the controls of the desktop's shown windows, front to back.
///
/// Each shown top-level window is one panel, and a named window inside it
/// is a panel of its own: a button, a field, a list, a check or a label on
/// it is reported under that window's name. A list's scroll bar and its
/// buttons are reported again under the list's own name, and a slider's
/// buttons under the slider's name. A list's rows stay rows of the list.
/// A hidden window is left out, and so is a control with no name. A check
/// whose window name is BYCheckBox is reported by its caption. The visit
/// must not change the windows.
///
/// @param desktop desktop to read; null lists nothing
/// @param context passed to `visit`
/// @param visit called once per control; null lists nothing
void desktop_visit_driver_controls(
    const Desktop* desktop, void* context, DriverControlVisitor visit
);

// The installed desktop (Services::desktop): its area reads 0, the queries
// return null and the requests do nothing while none is installed.

/// Returns the left edge of the installed desktop's area.
///
/// @return the left edge in screen pixels, or 0 without a desktop
int32_t services_desktop_x();
/// Returns the top edge of the installed desktop's area.
///
/// @return the top edge in screen pixels, or 0 without a desktop
int32_t services_desktop_y();
/// Returns the width of the installed desktop's area.
///
/// @return the width in pixels, or 0 without a desktop
int32_t services_desktop_width();
/// Returns the height of the installed desktop's area.
///
/// @return the height in pixels, or 0 without a desktop
int32_t services_desktop_height();
/// Adds a top-level window to the installed desktop (see desktop_add).
///
/// @param window window to add
void services_desktop_add(Window* window);
/// Takes a window off the installed desktop's top-level list (see
/// desktop_remove).
///
/// @param window window to remove
void services_desktop_remove(Window* window);
/// Raises a top-level window on the installed desktop (see desktop_raise).
///
/// @param window window to raise
void services_desktop_raise(Window* window);
/// Destroys the installed desktop's windows flagged `owned` (see
/// desktop_destroy_owned).
void services_desktop_destroy_owned();
/// Gives the keyboard focus to a window on the installed desktop (see
/// desktop_set_focus).
///
/// @param window window to focus, or null to clear the focus
void services_desktop_focus(Window* window);
/// Makes a window receive the installed desktop's pointer messages first.
///
/// @param window capturing window, or null to release the capture
void services_desktop_capture(Window* window);
/// Clears the installed desktop's pointer capture.
void services_desktop_release_capture();
/// Returns the window receiving the installed desktop's pointer messages
/// first.
///
/// @return the capturing window, or null (also without a desktop)
Window* services_desktop_captured();
/// Returns the last window that accepted a pointer move on the installed
/// desktop.
///
/// @return the hover window, or null (also without a desktop)
Window* services_desktop_hover();
/// Records the window that last accepted a pointer move on the installed
/// desktop; no mouse_enter or mouse_leave is sent.
///
/// @param window new hover window, or null
void services_desktop_set_hover(Window* window);

// ---------------------------------------------------------------------------
// Services. Each table is one interface of the GUI services; `self` is
// passed back. The resource managers share load, release, add_ref and get.

// Frame images from GAF (or PCX) files, reference counted.
struct ImageService {
    void* self;
    void (*release)(void* self, Handle image);
    void (*add_ref)(void* self, Handle image);
    // Frames of `sequence` in `file`, or of the sequence at `index` when the
    // name is empty.
    int32_t (*frame_count)(void* self, const char* file, const char* sequence, int32_t index);
    // Frame `frame` of `sequence` ("" = first sequence) in `file`. `key` is
    // stored with the entry (9 = the transparent index).
    Handle (*load)(void* self, const char* file, const char* sequence, int32_t frame, int32_t key);
    void (*size)(void* self, Handle image, int32_t* width, int32_t* height);
    // Draws at (x, y) honouring the frame origin; width/height are ignored.
    void (*draw)(void* self, int32_t x, int32_t y, int32_t width, int32_t height, Handle image);
    // The members below are calls on the image manager itself rather than
    // on its interface. A blank image is `width` x `height` pixels of
    // `fill`, drawn with `key` transparent; its pixels stay editable.
    Handle (*create_blank)(void* self, int32_t width, int32_t height, uint8_t fill, int32_t key);
    Sprite* (*sprite)(void* self, Handle image); // editable pixels, or null
    // Ticks an animation holds the image; 0 for a free handle.
    int32_t (*frame_delay)(void* self, Handle image);
};

// TextService style bits (also the `style` of set_caption/state texts).
namespace text_style {
inline constexpr int32_t left = 0x01; // else centred horizontally
inline constexpr int32_t right = 0x02;
inline constexpr int32_t top = 0x04; // else centred vertically
inline constexpr int32_t bottom = 0x08;
inline constexpr int32_t wrap = 0x10; // break lines wider than the box
} // namespace text_style

// Reference-counted strings drawn with FontService fonts.
struct TextService {
    void* self;
    void (*release)(void* self, Handle text);
    void (*add_ref)(void* self, Handle text);
    Handle (*create)(void* self, const char* text, int32_t style);
    void (*measure)(void* self, Handle font, int32_t* width, int32_t* height, Handle text);
    int32_t (*char_width)(void* self, Handle font, int32_t ch);
    // Width of characters [0, last] (inclusive) of a string.
    int32_t (*prefix_width)(void* self, Handle font, const char* text, int32_t last);
    void (*draw_string)(
        void* self,
        Handle font,
        int32_t x,
        int32_t y,
        int32_t width,
        int32_t height,
        const char* text,
        int32_t style
    );
    void (*measure_string)(
        void* self, Handle font, int32_t* width, int32_t* height, const char* text
    );
    void (*draw)(
        void* self, Handle font, int32_t x, int32_t y, int32_t width, int32_t height, Handle text
    ); // uses the style stored with the text
    const char* (*get)(void* self, Handle text);
};

// GAF glyph fonts (one frame per character code).
struct FontService {
    void* self;
    Handle (*load)(void* self, const char* name); // file under the font directory
    void (*release)(void* self, Handle font);
    void (*add_ref)(void* self, Handle font);
    int32_t (*line_height)(void* self, Handle font); // -1 = current font
};

struct InputService {
    void* self;
    bool (*is_key_down)(void* self, int32_t key);
};

struct ColorService {
    void* self;
    void (*release)(void* self, Handle color);
    Handle (*create)(void* self, uint8_t r, uint8_t g, uint8_t b, uint8_t a);
    void (*fill)(void* self, int32_t x, int32_t y, int32_t width, int32_t height, Handle color);
};

// Animated pointer images, each a GAF sequence. The shown cursor is the one
// last set, or the default while none is.
struct CursorService {
    void* self;
    int32_t (*load)(void* self, const char* file, const char* sequence);
    void (*show)(void* self);
    void (*hide)(void* self);
    void (*set)(void* self, int32_t cursor); // -1 falls back to the default
    void (*set_default)(void* self, int32_t cursor);
};

struct SoundService {
    void* self;
    Handle (*load)(void* self, const char* name);
    void (*release)(void* self, Handle sound);
    void (*play)(void* self, Handle sound);
    void (*beep)(void* self);
};

struct DrawService {
    void* self;
    void (*focus_frame)(void* self, int32_t x, int32_t y, int32_t width, int32_t height);
    void (*frame)(void* self, int32_t x, int32_t y, int32_t width, int32_t height);
    void (*caret)(void* self, int32_t x, int32_t y, int32_t visible, int32_t height);
    // Set on construction, then cleared or set by each closing message box
    // from its draw_flag_on_close; nothing reads it.
    uint8_t box_close_flag{1};
};

// The nine service singletons. Any pointer may be null; the wrapper functions
// below then behave as 3.1c does without the service: they return 0, -1 or
// null and draw nothing.
struct Services {
    Desktop* desktop;
    ImageService* images;
    TextService* texts;
    FontService* fonts;
    InputService* input;
    ColorService* colors;
    CursorService* cursors;
    SoundService* sounds;
    DrawService* draw;
};

/// Installs a new service set.
///
/// @param services service tables to use from now on; copied
/// @return the previously installed set
Services services_install(const Services& services);
/// Returns the installed service set.
///
/// @return the installed set; every member is null before the first install
const Services& services();

// Null-safe service calls used by all widget code: with no service (or no
// slot) installed they do nothing and return 0, -1 or "".

/// Drops one reference to an image.
///
/// @param image ImageService handle
void image_release(Handle image);
/// Adds one reference to an image.
///
/// @param image ImageService handle
void image_add_ref(Handle image);
/// Draws an image at (x, y), honouring the frame's origin.
///
/// @param x left edge in screen pixels
/// @param y top edge in screen pixels
/// @param width box width in pixels (the default service ignores it)
/// @param height box height in pixels (the default service ignores it)
/// @param image ImageService handle
void image_draw(int32_t x, int32_t y, int32_t width, int32_t height, Handle image);
/// Creates a reference-counted text.
///
/// @param text string to store
/// @param style text_style bits stored with the text
/// @return the TextService handle, or k_no_handle without a text service
Handle text_create(const char* text, int32_t style);
/// Drops one reference to a text.
///
/// @param text TextService handle
void text_release(Handle text);
/// Returns the string of a text.
///
/// @param text TextService handle
/// @return the string; "" for an unset handle or without a text service
const char* text_get(Handle text);
/// Draws a text inside a box with the style stored with it.
///
/// @param font FontService handle; -1 uses the current font
/// @param x left edge of the box in screen pixels
/// @param y top edge of the box in screen pixels
/// @param width box width in pixels
/// @param height box height in pixels
/// @param text TextService handle
void text_draw(Handle font, int32_t x, int32_t y, int32_t width, int32_t height, Handle text);
/// Draws a string inside a box.
///
/// @param font FontService handle; -1 uses the current font
/// @param x left edge of the box in screen pixels
/// @param y top edge of the box in screen pixels
/// @param width box width in pixels
/// @param height box height in pixels
/// @param text string to draw
/// @param align text_style bits placing the string in the box
void text_draw_string(
    Handle font,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    const char* text,
    int32_t align
);
/// Measures the first characters of a string.
///
/// @param font FontService handle; -1 uses the current font
/// @param text string to measure
/// @param last index of the last character measured (inclusive)
/// @return the width in pixels of characters 0..last; 0 without a text
///     service
int32_t text_prefix_width(Handle font, const char* text, int32_t last);
/// Measures a string.
///
/// @param font FontService handle; -1 uses the current font
/// @param text string to measure
/// @param[out] width string width in pixels; untouched without a text service
/// @param[out] height line height in pixels; untouched without a text service
void text_measure_string(Handle font, const char* text, int32_t* width, int32_t* height);
/// Measures one character.
///
/// @param font FontService handle; -1 uses the current font
/// @param ch character code
/// @return the character's advance in pixels; 0 without a text service
int32_t text_char_width(Handle font, int32_t ch);
/// Loads one frame of a GAF sequence as a reference-counted image.
///
/// @param file GAF file path
/// @param sequence sequence name; "" selects the first sequence
/// @param frame frame index within the sequence
/// @param key transparency key stored with the image (9 = the transparent
///     index)
/// @return the ImageService handle, or k_no_handle
Handle image_load(const char* file, const char* sequence, int32_t frame, int32_t key);
/// Reads an image's size.
///
/// @param image ImageService handle
/// @param[out] width image width in pixels; untouched without an image service
/// @param[out] height image height in pixels; untouched without an image
///     service
void image_size(Handle image, int32_t* width, int32_t* height);
/// Creates an image of one colour whose pixels stay editable.
///
/// @param width width in pixels
/// @param height height in pixels
/// @param fill palette index every pixel starts with
/// @param key palette index drawn transparent
/// @return the ImageService handle, or k_no_handle
Handle image_create_blank(int32_t width, int32_t height, uint8_t fill, int32_t key);
/// Makes a drawing surface over an image's pixels.
///
/// @param image ImageService handle
/// @param[out] surface surface set up over the image's pixels; untouched on
///     failure
/// @return false when the image has no editable (raw) pixels
bool image_surface(Handle image, Surface* surface);
/// Loads a glyph font.
///
/// @param name font file name, resolved by the FontService
/// @return the FontService handle, or k_no_handle
Handle font_load(const char* name);
/// Drops one reference to a font.
///
/// @param font FontService handle
void font_release(Handle font);
/// Adds one reference to a font.
///
/// @param font FontService handle
void font_add_ref(Handle font);
/// Returns a font's line height.
///
/// @param font FontService handle; -1 uses the current font
/// @return the line height in pixels; 0 without a font service
int32_t font_line_height(Handle font);
/// Loads a font by name and makes it the window's font, releasing the
/// previous one.
///
/// @param[in,out] self window whose font changes
/// @param name font file name, resolved by the FontService; a failed load
///     leaves the window without a font
void window_set_font(Window* self, const char* name);
/// Makes a loaded font the window's font: the previous font is released and
/// the new one gains a reference.
///
/// @param[in,out] self window whose font changes
/// @param font FontService handle; a negative handle leaves the window
///     without a font
void window_set_font_handle(Window* self, Handle font);
/// Tests whether a key is held.
///
/// @param key key code (see `key`)
/// @return true while held; false without an input service
bool input_key_down(int32_t key);
/// Creates a reference-counted colour.
///
/// @param r red component, 0-255
/// @param g green component, 0-255
/// @param b blue component, 0-255
/// @param a alpha component, 0-255
/// @return the ColorService handle, or k_no_handle
Handle color_create(uint8_t r, uint8_t g, uint8_t b, uint8_t a);
/// Drops one reference to a colour.
///
/// @param color ColorService handle
void color_release(Handle color);
/// Fills a rectangle with a colour.
///
/// @param x left edge in screen pixels
/// @param y top edge in screen pixels
/// @param width width in pixels
/// @param height height in pixels
/// @param color ColorService handle
void color_fill(int32_t x, int32_t y, int32_t width, int32_t height, Handle color);
/// Loads an animated cursor from a GAF sequence.
///
/// @param file GAF file path
/// @param sequence sequence name
/// @return the cursor id, or -1 without a cursor service
int32_t cursor_load(const char* file, const char* sequence);
/// Shows the pointer cursor.
void cursor_show();
/// Hides the pointer cursor.
void cursor_hide();
/// Selects the cursor shown.
///
/// @param cursor cursor id; -1 falls back to the default cursor
void cursor_set(int32_t cursor);
/// Selects the cursor shown while none is set.
///
/// @param cursor cursor id
void cursor_set_default(int32_t cursor);
/// Drops one reference to a sound.
///
/// @param sound SoundService handle
void sound_release(Handle sound);
/// Plays a sound.
///
/// @param sound SoundService handle
void sound_play(Handle sound);
/// Plays the error beep.
void sound_beep();
/// Draws the focus frame around a box.
///
/// @param x left edge in screen pixels
/// @param y top edge in screen pixels
/// @param width width in pixels
/// @param height height in pixels
void draw_focus_frame(int32_t x, int32_t y, int32_t width, int32_t height);
/// Draws a rectangle outline (the dock bar's drop outline).
///
/// @param x left edge in screen pixels
/// @param y top edge in screen pixels
/// @param width width in pixels
/// @param height height in pixels
void draw_frame(int32_t x, int32_t y, int32_t width, int32_t height);
/// Draws a text caret.
///
/// @param x left edge in screen pixels
/// @param y top edge in screen pixels
/// @param visible visibility flag (edit boxes pass 1)
/// @param height caret height in pixels
void draw_caret(int32_t x, int32_t y, int32_t visible, int32_t height);
/// Clears a draw object's box_close_flag.
///
/// @param[out] draw draw object to change
void draw_service_clear_flag(DrawService* draw);
/// Sets a draw object's box_close_flag.
///
/// @param[out] draw draw object to change
void draw_service_set_flag(DrawService* draw);
/// Clears the flag byte of the installed draw service; nothing without one.
void services_clear_draw_flag();
/// Sets the flag byte of the installed draw service; nothing without one.
void services_set_draw_flag();

// ---------------------------------------------------------------------------
// Default service implementation: GAF frame cache, image table, glyph fonts,
// text store, palette colours and UI sounds, all drawing into one 8-bit
// overlay surface ("UI MAIN") whose transparent index is 9.

// Platform boundary: file bytes (a malloc'd buffer the toolkit frees with
// std::free, or null) for paths such as "anims/Widgets.gaf".
struct ToolkitFiles {
    void* user;
    uint8_t* (*read)(void* user, const char* path, size_t* size);
};

// Platform boundary: plays a UI sound file at a volume in hundredths of a
// decibel (0 full, negative quieter).
struct ToolkitAudio {
    void* user;
    void (*play)(void* user, const char* path, int32_t volume);
};

struct ToolkitConfig {
    int32_t width; // overlay size in pixels
    int32_t height;
    const Palette* palette; // used to map RGB colours to indexes
    ToolkitFiles files;
    ToolkitAudio audio;
    // The main menu leaves the frame cache, the image table, the font slots
    // and the default font untouched. The default font stays unloaded until
    // toolkit_load_default_font. Zero prepares the tables and the font at
    // creation, which a test toolkit does.
    bool defer_tables{};
};

inline constexpr uint8_t k_overlay_transparent = 9;

struct Toolkit;

/// Builds the default services and a desktop covering the overlay.
///
/// Creates the GAF frame cache, image table, glyph fonts, text store, palette
/// colours, UI sounds, key state and cursor manager; gives the desktop the
/// area (0, 0, width, height); records the overlay size without its pixels.
/// Unless `defer_tables` is set, allocates and walks those tables, loads the
/// default font (Fonts\\BYHatFontSmall87.GAF) and makes it current. With
/// `defer_tables`, the frame, image, font and text tables are not allocated
/// until a font, an image, a frame or a text is asked for. Links the cursor
/// manager to the toolkit's image service and shows the cursor. The
/// services are not installed. The overlay's pixels are allocated the first
/// time a draw has no target of its own.
///
/// @param config overlay size, palette and platform file and audio hooks;
///     copied
/// @return the new toolkit; free it with toolkit_destroy
Toolkit* toolkit_create(const ToolkitConfig* config);
/// Frees a toolkit and everything its managers hold.
///
/// The desktop's list is freed but its windows are not destroyed; image
/// entries held once are released before the tables are freed.
///
/// @param toolkit toolkit to free; null does nothing
void toolkit_destroy(Toolkit* toolkit);
/// Returns the service set backed by the toolkit, ready for services_install.
///
/// @param toolkit toolkit providing the services
/// @return the toolkit's desktop and its eight services
Services toolkit_services(Toolkit* toolkit);
/// Returns the toolkit's desktop.
///
/// @param toolkit toolkit to read
/// @return the desktop, owned by the toolkit
Desktop* toolkit_desktop(Toolkit* toolkit);
/// Returns the toolkit's 8-bit overlay surface ("UI MAIN").
///
/// @param toolkit toolkit to read
/// @return the overlay, owned by the toolkit
Surface* toolkit_surface(Toolkit* toolkit);
/// Redirects all drawing to another surface.
///
/// @param[in,out] toolkit toolkit to change
/// @param target surface to draw into instead of the overlay, or null to draw
///     into the overlay again
void toolkit_set_target(Toolkit* toolkit, Surface* target);
/// Copies the overlay onto a surface, then clears the overlay to index 9.
///
/// Overlay pixels of index 9 are transparent. Both surfaces are aligned at
/// their top-left corners and the copy covers their common size.
///
/// @param[in,out] toolkit toolkit whose overlay is composed and cleared
/// @param[in,out] target surface to draw onto; null only clears the overlay
void toolkit_compose(Toolkit* toolkit, Surface* target);
/// Records a key's state for the input service.
///
/// @param[in,out] toolkit toolkit to change
/// @param key key code (see `key`); codes outside 0-0x1FF are ignored
/// @param down true when the key is held
void toolkit_set_key(Toolkit* toolkit, int32_t key, bool down);
/// Links the cursor manager to the image service it loads and draws cursor
/// frames through (the toolkit's own image service until replaced).
///
/// Without one, a newly loaded cursor gets a single empty frame and frames
/// advance every two ticks.
///
/// @param[in,out] toolkit toolkit to change
/// @param images image service to use, or null to unlink
void toolkit_set_cursor_images(Toolkit* toolkit, const ImageService* images);
/// Advances the shown cursor's animation by one tick.
///
/// The shown frame's countdown drops by one; once it passes zero the next
/// frame (wrapping to the first) comes up for the image's frame delay (0
/// without a delay slot), or for two ticks without an image service. Nothing
/// happens while no cursor is shown.
///
/// @param[in,out] toolkit toolkit whose cursor animates
void toolkit_step_cursor(Toolkit* toolkit);
/// Draws the shown cursor's current frame with its hotspot (the frame's
/// origin) at the pointer.
///
/// Nothing is drawn while the cursor is hidden, no cursor is shown, or no
/// image service is linked. The caller passes the pointer position.
///
/// @param toolkit toolkit whose cursor is drawn
/// @param x pointer x in screen pixels
/// @param y pointer y in screen pixels
void toolkit_draw_cursor(Toolkit* toolkit, int32_t x, int32_t y);
/// Returns the cursor last set.
///
/// @param toolkit toolkit to read
/// @return the cursor id, or -1 while the default cursor is shown
int32_t toolkit_cursor(const Toolkit* toolkit);
/// Returns the default font loaded by toolkit_create.
///
/// A toolkit created with `defer_tables` has none until
/// toolkit_load_default_font runs.
///
/// @param toolkit toolkit to read
/// @return the FontService handle, or k_no_handle when it failed to load
Handle toolkit_default_font(const Toolkit* toolkit);
/// Loads Fonts\\BYHatFontSmall87.GAF when the toolkit does not already have it.
///
/// The main menu defers this until the player enters, or until a message box
/// of the strip needs the font. A second call keeps the loaded font.
///
/// @param[in,out] toolkit toolkit to finish; null does nothing
void toolkit_load_default_font(Toolkit* toolkit);
/// Makes a loaded font the current font, the one -1 resolves to.
///
/// A handle of -1 is refused, like any handle that is not loaded.
///
/// @param[in,out] toolkit toolkit to change
/// @param font FontService handle; ignored unless it is loaded
void toolkit_select_font(Toolkit* toolkit, Handle font);
/// Releases every image entry holding exactly one reference (the image
/// table's first step when it is destroyed; toolkit_destroy runs it).
///
/// @param[in,out] toolkit toolkit whose image table is trimmed
void toolkit_release_images_held_once(Toolkit* toolkit);
/// Creates a font whose glyphs are another font's recoloured to one palette
/// index.
///
/// Every pixel of `base`'s glyphs other than index 9 becomes `color`; the
/// advances, heights, line height and kerning are copied. The new font has no
/// name and one reference.
///
/// @param[in,out] toolkit toolkit holding the fonts
/// @param base FontService handle of a loaded font
/// @param color palette index for the glyph pixels
/// @return the new font's handle, or k_no_handle when `base` is not loaded or
///     no font slot is free
Handle toolkit_font_tint(Toolkit* toolkit, Handle base, uint8_t color);
struct FontAtlas;

/// How a string's ink sits against the font's line.
struct TextFit {
    int32_t width = 0;       ///< sum of the characters' advances
    int32_t line_height = 0; ///< the font's line height
    int32_t above = 0;       ///< ink above the line, in pixels
    int32_t below = 0;       ///< ink below the line, in pixels
};

/// Tells the toolkit which analogue atlas to use for characters a face does
/// not cover.
///
/// The toolkit does not own the atlas. Null leaves those characters unmeasured
/// and undrawn. Pictures the face already has are unchanged.
///
/// @param[in,out] toolkit toolkit to change
/// @param atlas analogue glyphs; not owned
void toolkit_set_font_atlas(Toolkit* toolkit, const FontAtlas* atlas);
/// Measures how far a string's glyphs extend past the font's line.
///
/// Characters the face covers stay inside the line. Every other character is
/// measured from the analogue atlas when one is set.
///
/// @param toolkit toolkit holding the fonts
/// @param font FontService handle; -1 uses the current font
/// @param text string to measure; null measures nothing
/// @param[out] fit the advance and the ink past the line; left unset for an
///     unknown font
void toolkit_text_fit(Toolkit* toolkit, Handle font, const char* text, TextFit* fit);
/// Measures a string in a font.
///
/// Kerning is applied as if each character followed a space, so it never
/// changes the width. The height is the font's line height.
///
/// @param toolkit toolkit holding the fonts
/// @param font FontService handle; -1 uses the current font
/// @param text string to measure
/// @param[out] width sum of the characters' advances in pixels; left unset
///     for an unknown font
/// @param[out] height the font's line height in pixels; left unset for an
///     unknown font
void toolkit_measure_text(
    Toolkit* toolkit, Handle font, const char* text, int32_t* width, int32_t* height
);
/// Breaks text so no line is wider than `width`, writing the result into `dest`.
///
/// A line breaks at a space, between two CJK ideographs, or inside a word
/// when it has neither. `dest` always ends with a terminator when `dest_bytes`
/// is at least 1.
///
/// @param toolkit toolkit holding the fonts
/// @param width maximum line width in pixels
/// @param text string to break; null breaks nothing
/// @param[out] dest buffer receiving the broken text
/// @param dest_bytes size of `dest`, terminator included
/// @param font FontService handle; -1 uses the current font
/// @return the number of lines, or 0 for an unknown font
int32_t toolkit_wrap_text(
    Toolkit* toolkit, int32_t width, const char* text, char* dest, int32_t dest_bytes, Handle font
);
/// Breaks a string in place so no line of it in `font` is wider than
/// `width`.
///
/// A line that grows too wide breaks at its last space, or inside the word
/// when it has none; existing line feeds start new lines.
///
/// @param toolkit toolkit holding the fonts
/// @param width maximum line width in pixels
/// @param[in,out] text string to break; spaces become line feeds
/// @param font FontService handle; -1 uses the current font
/// @return the number of lines, or 0 for an unknown font
/// @quirk A break inside a word overwrites the character before the break
///     point with the line feed.
int32_t toolkit_wrap_lines(Toolkit* toolkit, int32_t width, char* text, Handle font);
/// Draws the first characters of a string into a surface from its left edge.
///
/// Each glyph hangs from the font's line height (its origin is the baseline);
/// spaces are skipped but advance.
///
/// @param toolkit toolkit holding the fonts
/// @param[in,out] target surface to draw into
/// @param font FontService handle; -1 uses the current font; an unknown font
///     draws nothing
/// @param text string to draw
/// @param count maximum number of bytes drawn; a character that would pass
///     the limit is left out
void toolkit_draw_text_run(
    Toolkit* toolkit, Surface* target, Handle font, const char* text, int32_t count
);

/// The modern line the chat log and the entry line share.
///
/// pixel_size is the sans faces' pixels per em. least_cjk is the least size
/// of the CJK face, or 0 when that face is left at the size derived from
/// pixel_size. ascent and descent are the rows of the line, which fit every
/// face. entry_height is the entry line's box. row_height is one log row.
struct ModernChatLine {
    int32_t pixel_size{};
    int32_t least_cjk{};
    int32_t ascent{};
    int32_t descent{};
    int32_t entry_height{};
    int32_t row_height{};
};

/// The faces the chat asks for. Null leaves modern text off.
///
/// The toolkit does not open the faces. The game, and the chat's test, supply
/// them. One caller at a time uses them, on the thread that runs the hooks.
/// `line` fills pixel_size, least_cjk, ascent and descent. The toolkit fills
/// the two heights. `advance` writes the width of `text` in pixels. `draw`
/// paints `text` so the pen sits on the baseline, clipped to the box and to
/// the surface, in the palette index `ink`.
struct ModernChatFaces {
    void* context{};
    bool (*line)(void* context, int32_t text_size, bool cjk_floor, ModernChatLine* out);
    bool (*advance)(void* context, const ModernChatLine* line, const char* text, int32_t* width);
    bool (*draw)(
        void* context,
        const ModernChatLine* line,
        const char* text,
        Surface* target,
        int32_t pen_x,
        int32_t baseline,
        uint8_t ink,
        int32_t clip_x,
        int32_t clip_y,
        int32_t clip_width,
        int32_t clip_height
    );
};

/// Installs the faces, or clears them when `faces` is null.
///
/// Clearing turns modern text off. The faces are not copied; they must outlive
/// the calls.
///
/// @param faces the faces, or null
void modern_chat_set_faces(const ModernChatFaces* faces);
/// Installs the faces that travel with the game.
///
/// The toolkit library does not call them. The game and the chat's test
/// supply this.
void modern_chat_use_engine();
/// Turns modern chat text on or off for a Text size, in percent.
///
/// On asks the faces for the in-battle message log's line at that size, with
/// the CJK face held to 12 pixels at least. The entry line is at least 14
/// pixels and tall enough for that line and for the face plus six pixels. A
/// log row is at least 17 pixels and tall enough for the line. Off, or faces
/// that cannot draw the size, leaves the Boneyards' own fonts in use.
///
/// @param modern_fonts Use modern fonts for game text is on
/// @param text_size Text size, in percent
void modern_chat_set(bool modern_fonts, int32_t text_size);
/// Reports whether the chat log and the entry line use the modern faces.
///
/// @return true while a line is installed
bool modern_chat_on();
/// Returns the line in force, or null while modern text is off.
///
/// @return the line
const ModernChatLine* modern_chat_line();
/// Measures a string in the modern line.
///
/// @param text string to measure; null measures nothing
/// @return the width in pixels, or 0 while modern text is off
int32_t modern_chat_width(const char* text);
/// Paints a modern string so its baseline sits on `baseline`.
///
/// Nothing is painted while modern text is off, the window's drawing surface
/// is missing, or the faces cannot draw the string.
///
/// @param text string to paint; null paints nothing
/// @param pen_x the pen's column
/// @param baseline the baseline's row
/// @param ink palette index
/// @param clip_x left of the clip box
/// @param clip_y top of the clip box
/// @param clip_width clip width; nothing is painted when it is not positive
/// @param clip_height clip height; nothing is painted when it is not positive
/// @return true when modern text is on and the paint was accepted
bool modern_chat_paint(
    const char* text,
    int32_t pen_x,
    int32_t baseline,
    uint8_t ink,
    int32_t clip_x,
    int32_t clip_y,
    int32_t clip_width,
    int32_t clip_height
);
/// Paints a marked window's caption with the modern faces, centred in its box.
///
/// @param window window to paint; an unmarked window is left alone
/// @param text caption; null paints nothing
/// @return true when the caption was painted this way
bool modern_chat_paint_window(const Window* window, const char* text);
/// Marks a window to be drawn with the modern faces, in `ink`.
///
/// @param[in,out] window window to mark; null is ignored
/// @param ink palette index
void modern_chat_mark(Window* window, uint8_t ink);
/// Stops a window being drawn with the modern faces.
///
/// @param[in,out] window window to unmark; null is ignored
void modern_chat_unmark(Window* window);
/// The palette index a Boneyards font is drawn in.
///
/// @param font FontService handle
/// @return the ink, or 0 when the font is not loaded
uint8_t toolkit_font_ink(Handle font);
/// The surface the toolkit is drawing into.
///
/// @return the surface, or null when no toolkit is installed
Surface* toolkit_drawing_surface();
/// Breaks a chat line into log rows in the modern faces.
///
/// The first row starts `first_indent` pixels in. A continuation starts as far
/// in as `continuation_spaces` spaces of the modern line. A row breaks at a
/// space, between CJK characters, or after at least one character when a word
/// has neither. The log follows its end only when it was already showing it,
/// and when it was empty. While modern text is off this adds nothing.
///
/// @param[in,out] list log to add to
/// @param text the line; null adds nothing
/// @param row_width width of a row's text, in pixels
/// @param first_indent inset of the first row, in pixels
/// @param continuation_spaces spaces a continuation row starts in by
/// @param ink palette index passed to `make_row`
/// @param make_row builds one row; null adds nothing
/// @param user context for `make_row`
/// @return true when modern text handled the line
bool modern_chat_write(
    ListBox* list,
    const char* text,
    int32_t row_width,
    int32_t first_indent,
    int32_t continuation_spaces,
    uint8_t ink,
    void (*make_row)(void* user, const char* line, int32_t indent, int32_t height, uint8_t ink),
    void* user
);

} // namespace oa::boneyards

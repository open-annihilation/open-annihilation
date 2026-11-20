// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// TA gadget engine: the per-screen GUI context, its stack of loaded
// panels, and the pointer/keyboard state machines that drive gadget records.
// Presentation (fonts, drawing) and devices (clock, key queue, clipboard) are
// reached through GadgetHost so this layer stays device-free and testable.

#include "oa/ui/gui_layout/gui_gadget.hpp"
#include "oa/sim/sprite_animation.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::gui_input {

// Pointer message codes the engine compares against the stored pointer event.
namespace pointer_message {
inline constexpr int32_t left_down = 0x201;
inline constexpr int32_t left_double = 0x203;
inline constexpr int32_t right_down = 0x204;
inline constexpr int32_t right_double = 0x206;
} // namespace pointer_message

// Mouse-button selector bits passed to the message predicates.
inline constexpr uint8_t kLeftButton = 1;
inline constexpr uint8_t kRightButton = 2;
inline constexpr int32_t kAnyButtonHeld = 3;

// Internal key codes produced by the frontend key queue.
namespace key_code {
inline constexpr int32_t backspace = 0x08;
inline constexpr int32_t tab = 0x09;
inline constexpr int32_t enter = 0x0D;
inline constexpr int32_t escape = 0x1B;
inline constexpr int32_t space = 0x20;
inline constexpr int32_t paste = 0xBF;
inline constexpr int32_t paste_alternate = 0xEE;
inline constexpr int32_t delete_forward = 0xEF;
inline constexpr int32_t home = 0xF0;
inline constexpr int32_t end = 0xF1;
inline constexpr int32_t page_up = 0xF2;
inline constexpr int32_t page_down = 0xF3;
inline constexpr int32_t left = 0xF4;
inline constexpr int32_t up = 0xF5;
inline constexpr int32_t right = 0xF6;
inline constexpr int32_t down = 0xF7;
inline constexpr int32_t shift = 0xF9;
inline constexpr int32_t alt = 0xFB;
} // namespace key_code

// Directions understood by focus_nearest().
enum class FocusDirection : int32_t { previous = 0, next = 1, up = 2, down = 3 };

// One pointer event as the display's pointer queue holds it; the host's
// peek_pointer, pop_pointer and current_pointer fill it.
struct PointerEvent {
    int32_t x = 0;
    int32_t y = 0;
    int32_t buttons = 0;
    int32_t tick = 0; // tick the event was received on; the gadget engine does not read it
    int32_t message = 0;
    int32_t is_button = 0; // 0 for a move, 1 for a button change; not read either
};

// Redraw requests issued by the engine; each names one gadget draw.
enum class GadgetDraw : uint8_t {
    button = 0,
    list = 1,
    scroll_bar = 2,
    text_box = 3,
    image = 4,
    progress = 5,
    focus_outline = 6, // outline part of the focus drawing
    shade_panel = 7,   // shade of level -0x18 over the root rectangle when a panel loads
};

struct GadgetPanel;

// Devices and presentation services used by the engine. Null members act as
// absent services (no keys, tick 0, zero-width text, no drawing).
struct GadgetHost {
    void* context = nullptr;
    uint32_t (*current_tick)(void* context) = nullptr;
    int32_t (*pop_key)(void* context) = nullptr;
    int32_t (*peek_key)(void* context) = nullptr;
    void (*clear_keys)(void* context) = nullptr;
    bool (*is_key_down)(void* context, int32_t code) = nullptr;
    std::size_t (*read_clipboard)(void* context, char* output, std::size_t capacity) = nullptr;
    // Text translation: returns a replacement or null for "no translation".
    const char* (*translate)(void* context, const char* text) = nullptr;
    void (*select_font)(void* context, const void* font) = nullptr; // active FNT font
    int32_t (*text_width)(void* context, const void* gaf_font, const char* text) = nullptr;
    int32_t (*line_height)(void* context, const void* gaf_font) = nullptr;
    int32_t (*ticks_per_second)(void* context) = nullptr;
    bool (*list_image_size)(
        void* context,
        const ui::gui_layout::GadgetRecord& list,
        int32_t item,
        int32_t& width,
        int32_t& height
    ) = nullptr;
    // Leading frame-count word of a button's GAF sequence (GadgetRefs::sprite).
    int32_t (*sprite_frames)(void* context, const void* sprite) = nullptr;
    void (*draw)(void* context, GadgetPanel& panel, GadgetDraw what, int32_t index) = nullptr;
    // Whole-panel draw; returns the draw's success value (1 when drawn).
    int32_t (*draw_panel)(void* context, GadgetPanel& panel, uint32_t flags) = nullptr;
    void (*draw_label)(
        void* context, GadgetPanel& panel, const char* text, int32_t x, int32_t y, int32_t color
    ) = nullptr;
    // The display's pointer queue: peek and pop copy the next queued event
    // and return true, or copy the latest move and return false when none is
    // queued; current_pointer copies the latest move.
    bool (*peek_pointer)(void* context, PointerEvent& out) = nullptr;
    bool (*pop_pointer)(void* context, PointerEvent& out) = nullptr;
    void (*current_pointer)(void* context, PointerEvent& out) = nullptr;
    // The picture the software cursor shows.
    void (*set_cursor_image)(void* context, const formats::gaf::Frame* image) = nullptr;
    const formats::gaf::Frame* (*cursor_image)(void* context) = nullptr;
};

// One loaded panel of the panel stack: its gadget records and the state kept
// with them.
struct GadgetOwner {
    std::unique_ptr<GadgetOwner> next;                // panel below this one
    ui::gui_layout::GadgetTable table;                // the panel's gadget records
    void (*on_command)(GadgetPanel& panel) = nullptr; // runs on activation and on close
    uint32_t flags = 0;                               // panel_flag bits kept from the load
    int32_t redraw = 0;                               // 1: next blit copies the face and clears it
    int32_t keys_from_queue = 0;                      // nonzero pops keys; 0 only peeks
    void (*on_tick)() = nullptr;                      // runs once per update
    int32_t focus = ui::gui_layout::kNoGadget;        // record with keyboard focus
    const void* backdrop = nullptr;                   // set by set_backdrop()
    std::array<uint8_t, 15> key_history{};            // last keys, upper-cased, oldest first
    int32_t last_message = 0;                         // as set_last_message() stores it
    void (*on_key)(GadgetPanel& panel) = nullptr;     // runs after each key is recorded

    /// Returns the panel's gadget records.
    [[nodiscard]] std::span<ui::gui_layout::GadgetRecord> records() noexcept {
        return table.records;
    }

    /// Returns the panel's gadget records, read-only.
    [[nodiscard]] std::span<const ui::gui_layout::GadgetRecord> records() const noexcept {
        return table.records;
    }
};

inline constexpr std::size_t kBlinkWordTextBytes = 0x80;

// One cycling caption entry.
struct BlinkWord {
    std::array<char, kBlinkWordTextBytes> text{}; // empty = inactive
    int32_t x = 0;
    int32_t y = 0;
    int32_t color_off = 0;
    float off_seconds = 0;
    int32_t color_on = 0;
    float on_seconds = 0;
    int32_t lit = 0;
    float next_tick = 0;
    int32_t font_record = 0; // read from entry 0 only
};

inline constexpr std::size_t kPanelPathBytes = 0x100;
inline constexpr std::size_t kColorMapBytes = 0x100;

// The GUI context passed to every gadget function.
struct GadgetPanel {
    const void* default_font = nullptr;
    const void* list_skin = nullptr;
    std::array<const void*, 3> gaf_fonts{};
    const void* active_gaf_font = nullptr;
    std::unique_ptr<GadgetOwner> owner; // top of the panel stack
    const formats::gaf::Frame* cursor_image = nullptr;
    const formats::gaf::Frame* cursor_still = nullptr;       // off the top panel, no sequence
    const formats::gaf::Frame* hover_cursor_still = nullptr; // over the top panel, no sequence
    const formats::gaf::Sequence* hover_cursor = nullptr;    // over the top panel
    const formats::gaf::Sequence* cursor = nullptr;          // off the top panel
    sim::sprite_animation::Cursor cursor_frame{};            // running cursor sequence
    PointerEvent pointer;
    int32_t buttons = 0;
    int32_t last_message = 0;
    uint32_t cursor_flags = 0; // cursor_flag bits
    int32_t activated = ui::gui_layout::kNoGadget;
    int32_t captured = ui::gui_layout::kNoGadget;
    int32_t hovered = ui::gui_layout::kNoGadget;
    int32_t help_source = 0;
    int32_t clear_quick_keys_on_draw = 0; // nonzero: first draw clears quick keys
    int32_t caret = 0;
    int32_t dragging = 0;
    PointerEvent drag_origin;
    int16_t drag_knob = 0;
    uint32_t last_tick = 0;
    uint32_t tick_delta = 0;
    int32_t word_after_tick_delta = 0; // set to 1 by init_gadget_panel(); never read
    int32_t keyboard_enabled = 0;
    std::vector<BlinkWord> blink_words;
    int32_t blink_words_active = 0;
    std::array<uint8_t, kColorMapBytes> colors{}; // palette remap
    std::array<char, kPanelPathBytes> gui_path{};
    std::array<char, kPanelPathBytes> gaf_path{};
    std::array<char, kPanelPathBytes> font_path{};
    int32_t quick_keys_enabled = 0;
    int32_t dirty = 0;
    int32_t pressed_status = 0;
    const void* backdrop = nullptr;
    uint8_t byte_after_backdrop = 0; // cleared by init_gadget_panel(); never read
    // Click and hover repeat timing, and the device and presentation host.
    int32_t click_repeat_delay = 0;
    uint32_t click_repeat_last = 0;
    uint32_t hover_repeat_timer = 0;
    GadgetHost host;
};

// GadgetPanel::cursor_flags bits.
namespace cursor_flag {
inline constexpr uint32_t animating = 0x1; // cursor_frame steps each update
} // namespace cursor_flag

/// Returns the gadget records of the top panel.
[[nodiscard]] inline std::span<ui::gui_layout::GadgetRecord> panel_records(GadgetPanel& panel) {
    return panel.owner->records();
}

// ---- Context, panel stack and small field setters ----

/// Initialises the GUI context's fields.
///
/// Drops every panel, clears the paths and fonts, enables quick keys and the
/// keyboard, and stamps the current tick.
///
/// @param[in,out] panel GUI context to reset.
void init_gadget_panel(GadgetPanel& panel);

/// Enables keyboard dispatch to the focused record.
///
/// @param[in,out] panel GUI context.
void enable_keyboard(GadgetPanel& panel);

/// Disables keyboard dispatch to the focused record.
///
/// @param[in,out] panel GUI context.
void disable_keyboard(GadgetPanel& panel);

/// Requests a full panel redraw on the next update.
///
/// @param[in,out] panel GUI context.
void mark_dirty(GadgetPanel& panel);

/// Asks the top panel to redraw.
///
/// @param[in,out] panel GUI context.
void request_owner_redraw(GadgetPanel& panel);

/// Sets whether the top panel reads keys from the key queue.
///
/// @param[in,out] panel GUI context; ignored without a panel.
/// @param value Nonzero to pop keys from the queue, 0 to only peek.
void set_keys_from_queue(GadgetPanel& panel, int32_t value);

/// Enables or disables quick keys for buttons and labels.
///
/// @param[in,out] panel GUI context.
/// @param value 1 enables quick keys; any other value disables them.
void set_quick_keys_enabled(GadgetPanel& panel, int32_t value);

/// Makes one GAF font slot the active caption font.
///
/// @param[in,out] panel GUI context.
/// @param slot Font slot, 0..2.
void select_gaf_font(GadgetPanel& panel, int32_t slot);

/// Stores the backdrop image of the top panel.
///
/// @param[in,out] panel GUI context; ignored without a panel.
/// @param backdrop Backdrop image.
/// @return Always 1.
int32_t set_backdrop(GadgetPanel& panel, const void* backdrop);

/// Stores the directory GUI files are loaded from, with a trailing backslash.
///
/// @param[in,out] panel GUI context.
/// @param path Directory; cut to 254 characters.
void set_gui_path(GadgetPanel& panel, std::string_view path);

/// Stores the directory GAF files are loaded from, with a trailing backslash.
///
/// @param[in,out] panel GUI context.
/// @param path Directory; cut to 254 characters.
void set_gaf_path(GadgetPanel& panel, std::string_view path);

/// Stores the third context path (use unresolved), with a trailing backslash.
///
/// @param[in,out] panel GUI context.
/// @param path Directory; cut to 254 characters.
void set_font_path(GadgetPanel& panel, std::string_view path);

/// Sets the FNT font a gadget without a font record of its own selects.
///
/// @param[in,out] panel GUI context.
/// @param font FNT font.
void set_default_font(GadgetPanel& panel, const void* font);

/// Compares the top panel's root name with a name, ignoring case, over 16 bytes.
///
/// @param panel GUI context.
/// @param name Panel name.
/// @return True when a top panel exists and its name matches.
[[nodiscard]] bool top_panel_named(const GadgetPanel& panel, std::string_view name);

/// Pops the top panel, running its command callback and redrawing below it.
///
/// Clears the captured, activated and hovered records first; a panel loaded
/// with panel_flag::shade_below redraws the panel beneath with its shade.
///
/// @param[in,out] panel GUI context; ignored without a panel.
void close_top_panel(GadgetPanel& panel);

/// Loads a parsed GUI as a new top panel or merges it into the top panel.
///
/// A merge centres the loaded records in the top panel's "PANEL" frame (or
/// offsets them by the loaded root's origin) and appends them. Unnamed
/// Enter/Escape defaults bind to the first OK/NEXT and PREV/Cancel buttons,
/// and focus goes to the default focus or the first focusable record. The
/// panel is drawn unless panel_flag::no_draw is set; a panel whose draw fails
/// is unlinked before it is freed.
///
/// @param[in,out] panel GUI context; the new panel becomes the top one.
/// @param layout Parsed GUI.
/// @param name Basename stored in the root record.
/// @param flags panel_flag bits.
/// @return The loaded or merged panel, or nullptr when the layout is empty,
///         a merge does not fit, or the draw fails.
GadgetOwner* load_panel(
    GadgetPanel& panel, const ui::gui_layout::Layout& layout, std::string_view name, uint32_t flags
);

// Panel load flags passed to load_panel().
namespace panel_flag {
inline constexpr uint32_t first_draw = 0x1; // the loader sets it on every panel
// The whole-panel draw keeps no copy of the screen under the panel; a panel
// merged into an existing PANEL frame carries it.
inline constexpr uint32_t no_save_under = 0x20;
// The whole-panel draw redraws the face and every record.
inline constexpr uint32_t redraw = 0x40;
inline constexpr uint32_t modal_backdrop = 0x80;
inline constexpr uint32_t centre = 0x100; // first draw centres the root on the screen
inline constexpr uint32_t merge = 0x200;
inline constexpr uint32_t no_draw = 0x400;
inline constexpr uint32_t shade_below = 0x800;
inline constexpr uint32_t beside_hud = 0x1000; // first draw centres it right of the HUD strip
} // namespace panel_flag

// Root x positions the whole-panel draw resolves before drawing.
inline constexpr int16_t root_centred = -1;
inline constexpr int16_t root_beside_hud = -2;
inline constexpr int32_t hud_strip_width = 0x80;

/// Places a panel root on the screen as the whole-panel draw does.
///
/// On a first draw, panel_flag::centre centres it and panel_flag::beside_hud
/// centres it right of a `hud_strip` wide column; a root left at either
/// marker resolves the same way, and an axis on which the root runs past the
/// screen is centred.
///
/// @param[in,out] x Root left edge, or root_centred / root_beside_hud.
/// @param[in,out] y Root top edge.
/// @param width Root width in pixels.
/// @param height Root height in pixels.
/// @param flags panel_flag bits of the draw.
/// @param screen_width Screen width in pixels.
/// @param screen_height Screen height in pixels.
/// @param hud_strip Width of the HUD column in pixels.
void place_root(
    int16_t& x,
    int16_t& y,
    int32_t width,
    int32_t height,
    uint32_t flags,
    int32_t screen_width,
    int32_t screen_height,
    int32_t hud_strip
) noexcept;

// ---- Selection, focus and messages ----

/// Reports whether the stored pointer message is a press of the given button.
///
/// @param panel GUI context.
/// @param buttons kLeftButton or kRightButton; left is tested first.
/// @return True for a down or double-click message of that button.
[[nodiscard]] bool is_button_message(const GadgetPanel& panel, uint8_t buttons);

/// Reports whether the stored pointer message is a double-click of the given button.
///
/// @param panel GUI context.
/// @param buttons kLeftButton or kRightButton; left is tested first.
/// @return True for a double-click message of that button.
[[nodiscard]] bool is_double_click(const GadgetPanel& panel, uint8_t buttons);

/// Reports whether any of the given buttons is held.
///
/// @param panel GUI context.
/// @param mask Button bits to test (kAnyButtonHeld for either).
/// @return True when a masked button is held.
[[nodiscard]] bool buttons_held(const GadgetPanel& panel, int32_t mask);

/// Records the last pointer message on the context and the top panel.
///
/// @param[in,out] panel GUI context; must have a top panel.
/// @param message 1 for the left button, 2 for the right.
void set_last_message(GadgetPanel& panel, int32_t message);

/// Returns the last pointer message recorded by set_last_message().
///
/// @param panel GUI context.
/// @return 1 for the left button, 2 for the right, 0 before any.
[[nodiscard]] int32_t last_message(const GadgetPanel& panel);

/// Captures pointer and edit input for a record unless another record holds it.
///
/// A text box never keeps the capture against another record; capturing a
/// text box puts the caret at the end of its text.
///
/// @param[in,out] panel GUI context.
/// @param index Record to capture, or -1.
/// @return False when another record keeps the capture.
bool capture_gadget(GadgetPanel& panel, int32_t index);

/// Reports whether the activated record's name equals a name (full comparison).
///
/// @param panel GUI context.
/// @param name Gadget name.
/// @return True when a record is activated and its name matches.
[[nodiscard]] bool activated_gadget_is(const GadgetPanel& panel, std::string_view name);

/// Clears the activated record.
///
/// @param[in,out] panel GUI context.
void clear_activation(GadgetPanel& panel);

/// Selects the font of the type-7 record numbered by a record's font byte.
///
/// @param[in,out] panel GUI context.
/// @param index Record whose font byte counts type-7 records from 0.
/// @return The font record index, or -1 after selecting the context default.
int32_t apply_gadget_font(GadgetPanel& panel, int32_t index);

/// Copies the hovered record's translated help into the HELPTEXT gadget.
///
/// @param[in,out] panel GUI context; the help source becomes the hovered record.
void update_help_text(GadgetPanel& panel);

/// Captures a text box for editing and gives it keyboard focus.
///
/// Selects its font, puts the caret at the end of its text, redraws it and
/// clears the key queue.
///
/// @param[in,out] panel GUI context.
/// @param index Text box record.
void focus_text_box(GadgetPanel& panel, int32_t index);

/// Gives keyboard focus to the named record, capturing it when it is a text box.
///
/// @param[in,out] panel GUI context.
/// @param name Gadget name; nothing happens when absent.
void focus_gadget_by_name(GadgetPanel& panel, std::string_view name);

/// Gives keyboard focus to a record, capturing it when it is a text box.
///
/// @param[in,out] panel GUI context.
/// @param index Record to focus.
void set_focus(GadgetPanel& panel, int32_t index);

/// Moves keyboard focus to the nearest focusable record in a direction.
///
/// @param[in,out] panel GUI context.
/// @param direction Direction to search in.
void focus_nearest(GadgetPanel& panel, FocusDirection direction);

/// Moves keyboard focus to the nearest focusable record in a direction, wrapping around.
///
/// Records are ordered by y*5000+x (previous/next) or by a 10-pixel x column
/// then y (up/down). Inactive, grayed, locked, vertical scroll bar and
/// non-selectable list records are skipped.
///
/// @param[in,out] panel GUI context; nothing happens without a focus record.
/// @param direction 0 previous, 1 next, 2 up, 3 down (FocusDirection values).
void focus_nearest(GadgetPanel& panel, int32_t direction);

/// Marks a text box focused or asks for the focus outline around another record.
///
/// Clears the foreground colour of every text box first; a focused text box
/// gets colour 0x1E, and labels and lists get no outline.
///
/// @param[in,out] panel GUI context.
/// @param index Record gaining focus.
void mark_text_box_focus(GadgetPanel& panel, int32_t index);

// ---- Record state setters and getters ----

/// Renames a record.
///
/// @param[in,out] panel GUI context; ignored without a panel.
/// @param name Current gadget name.
/// @param new_name New name, cut to 16 bytes and followed by a NUL.
void rename_gadget(GadgetPanel& panel, std::string_view name, std::string_view new_name);

/// Clears the status of other buttons in a record's group and redraws them.
///
/// @param[in,out] panel GUI context.
/// @param index Record whose nonzero group byte is matched; group 0 does nothing.
void clear_group_status(GadgetPanel& panel, int32_t index);

/// Clears the status of every button sharing a record's group byte and redraws them.
///
/// Includes the record itself and group 0.
///
/// @param[in,out] panel GUI context.
/// @param index Record whose group byte is matched.
void clear_group_status_marking(GadgetPanel& panel, int32_t index);

/// Sets a record's active byte.
///
/// A scroll bar passes the value to the buttons of its group; a deactivated
/// list deactivates its scroll bar; a deactivated focus record moves focus on.
///
/// @param[in,out] panel GUI context.
/// @param index Record to change.
/// @param active New active byte.
/// @quirk A deactivated list with no scroll bar deactivates the root record
///        (the group lookup's miss value is 0).
void set_gadget_active(GadgetPanel& panel, int32_t index, int32_t active);

/// Returns the active byte of the named record.
///
/// @param[in,out] panel GUI context.
/// @param name Gadget name.
/// @return The active byte, or -1 when absent.
[[nodiscard]] int32_t gadget_active_by_name(GadgetPanel& panel, std::string_view name);

/// Sets the active byte of the named record as set_gadget_active() does.
///
/// @param[in,out] panel GUI context; ignored without a panel.
/// @param name Gadget name; nothing happens when absent.
/// @param active New active byte.
void set_gadget_active_by_name(GadgetPanel& panel, std::string_view name, int32_t active);

/// Picks a quick key for a button or linked label from its caption.
///
/// The first caption character (spaces skipped) not already used as a quick
/// key by any button or label becomes the record's quick key, as
/// free_quick_key() chooses it: one a single key types. Buttons with
/// stages or no_quick_key, and labels without a link, get none.
///
/// @param[in,out] panel GUI context.
/// @param index Record to assign, or -1.
void assign_quick_key(GadgetPanel& panel, int32_t index);

/// What setting a button's caption does to the button's quick key.
enum class CaptionQuickKey : uint8_t {
    keep,   ///< it keeps its key: it has no_quick_key, or the caption is empty
    none,   ///< it holds none: it has stages
    assign, ///< it takes free_quick_key() of the caption, or none when that is 0
};

/// Returns what setting a button's caption does to its quick key, as the
/// gadget engine assigns one (assign_quick_key()).
///
/// Every panel model that sets button captions follows this rule; a key it
/// assigns comes from free_quick_key(), with the button's own key cleared
/// first.
///
/// @param attributes the button's attribute bits
/// @param stages the button's stage count, 0 for a plain button
/// @param caption the caption set
/// @return what happens to the key
[[nodiscard]] CaptionQuickKey
caption_quick_key(uint32_t attributes, int32_t stages, std::string_view caption) noexcept;

/// Returns the quick key a caption gives its button or linked label.
///
/// The key is the caption's first character, spaces skipped, that none of
/// `taken` matches with A-Z compared without regard to case; the character
/// keeps the case the caption has. Only a one-byte character can be a key: a
/// well-formed UTF-8 sequence of two to four bytes, such as a Hangul
/// syllable or a Chinese character, is passed over whole
/// (oa::base::text::break_character), so a caption of those alone takes
/// none. A byte of the game's one-byte code page is a character of its own.
///
/// @param caption the record's caption
/// @param taken the quick keys the panel's buttons and labels hold, 0 for none
/// @return the character, or 0 when every character of the caption is taken
[[nodiscard]] char free_quick_key(std::string_view caption, std::span<const int8_t> taken) noexcept;

/// Stores a caption in the named record (strncpy) and assigns its quick key.
///
/// @param[in,out] panel GUI context; ignored without a panel.
/// @param name Gadget name; nothing happens when absent.
/// @param text Caption, cut to 128 bytes.
void set_text_assign_quick_key(GadgetPanel& panel, std::string_view name, std::string_view text);

/// Stores text in a text box (strcpy) and moves the caret to its end when captured.
///
/// @param[in,out] panel GUI context.
/// @param index Text box record.
/// @param text New text.
void set_text_box_text(GadgetPanel& panel, int32_t index, std::string_view text);

/// Translates each NUL-separated stage caption of a button and repacks them in place.
///
/// The captions are repacked into the record's 0x80-byte caption field, whose
/// unused tail is zeroed.
///
/// @param[in,out] panel GUI context.
/// @param index Button record with stages.
void split_stage_captions(GadgetPanel& panel, int32_t index);

/// Stores translated text in a button, text box or label.
///
/// A button gets a quick key and, with stages, its '|'-separated captions are
/// split; a linked label gets a quick key.
///
/// @param[in,out] panel GUI context; ignored without a panel.
/// @param index Record to change, or -1.
/// @param text Text before translation.
/// @param max_chars New text-box maximum length; 0 keeps the current one.
void set_gadget_text(GadgetPanel& panel, int32_t index, std::string_view text, int32_t max_chars);

/// Stores translated text in the named record as set_gadget_text() does.
///
/// @param[in,out] panel GUI context; ignored without a panel.
/// @param name Gadget name; nothing happens when absent.
/// @param text Text before translation.
/// @param max_chars New text-box maximum length; 0 keeps the current one.
void set_gadget_text_by_name(
    GadgetPanel& panel, std::string_view name, std::string_view text, int32_t max_chars
);

/// Sets the foreground colour word of the named record.
///
/// @param[in,out] panel GUI context; ignored without a panel.
/// @param name Gadget name; nothing happens when absent.
/// @param color Palette index stored as the foreground colour.
void set_gadget_color_by_name(GadgetPanel& panel, std::string_view name, uint32_t color);

/// Returns the text storage of a named button, text box or label.
///
/// Without a panel nothing is reported; 3.1c reports "Internal error" then.
///
/// @param[in,out] panel GUI context.
/// @param name Gadget name.
/// @param[out] output Receives a copy of the text; may be null.
/// @return Pointer to the record's text bytes, or nullptr when there is no
///         panel, no such record, or the record has no text.
char* gadget_text_by_name(GadgetPanel& panel, std::string_view name, std::string* output);

/// Writes text to a record found by name in the panel below the top one.
///
/// Text boxes are written through the top panel's record at the same index;
/// buttons and labels are written in the lower panel. Nothing is written when
/// there is no lower panel.
///
/// @param[in,out] panel GUI context.
/// @param name Gadget name looked up in the lower panel.
/// @param text New text.
void set_parent_gadget_text(GadgetPanel& panel, std::string_view name, std::string_view text);

/// Returns a record's button status word.
///
/// @param[in,out] panel GUI context.
/// @param index Record to read.
/// @return The signed status word.
[[nodiscard]] int32_t gadget_status(GadgetPanel& panel, int32_t index);

/// Returns the current stage of the named button.
///
/// @param[in,out] panel GUI context.
/// @param name Gadget name.
/// @return The stage byte, or -1 when absent or not a button.
[[nodiscard]] int32_t button_stage_by_name(GadgetPanel& panel, std::string_view name);

/// Returns the current stage of a button.
///
/// @param[in,out] panel GUI context.
/// @param index Record to read.
/// @return The stage byte, or -1 when the record is not a button.
[[nodiscard]] int32_t button_stage(GadgetPanel& panel, int32_t index);

/// Sets the current stage of a button.
///
/// @param[in,out] panel GUI context.
/// @param index Record to change.
/// @param stage New stage.
/// @return False when the record is not a button.
bool set_button_stage(GadgetPanel& panel, int32_t index, uint8_t stage);

/// Writes the stage byte of the named record regardless of its type.
///
/// @param[in,out] panel GUI context.
/// @param name Gadget name.
/// @param stage New stage.
/// @return False when the record is absent.
bool set_button_stage_by_name(GadgetPanel& panel, std::string_view name, uint8_t stage);

/// Sets the status word of the named record and clears the rest of its group when nonzero.
///
/// @param[in,out] panel GUI context.
/// @param name Gadget name.
/// @param status New status.
/// @return False when the record is absent.
bool set_status_by_name(GadgetPanel& panel, std::string_view name, int32_t status);

/// Sets a record's status word and clears the status of the rest of its group.
///
/// @param[in,out] panel GUI context.
/// @param index Record to change.
/// @param status New status.
void set_status(GadgetPanel& panel, int32_t index, int16_t status);

/// Sets or clears a button's grayed bit and requests a redraw.
///
/// @param[in,out] panel GUI context.
/// @param index Button record.
/// @param grayed Bit 0 is stored.
void set_grayed(GadgetPanel& panel, int32_t index, uint8_t grayed);

/// Sets or clears the named button's grayed bit without requesting a redraw.
///
/// @param[in,out] panel GUI context.
/// @param name Gadget name; nothing happens when absent.
/// @param grayed Bit 0 is stored.
void set_grayed_by_name(GadgetPanel& panel, std::string_view name, uint8_t grayed);

/// Sets the per-type disabled bit of a record.
///
/// Applies to buttons, lists, scroll bars with their step buttons, labels and
/// image records; other types are left alone.
///
/// @param[in,out] panel GUI context.
/// @param index Record to change, or -1.
/// @param disabled Bit 0 is stored (a scroll bar stores the whole word).
void set_gadget_disabled(GadgetPanel& panel, int32_t index, uint32_t disabled);

/// Sets the per-type disabled bit of the named record as set_gadget_disabled() does.
///
/// @param[in,out] panel GUI context.
/// @param name Gadget name; nothing happens when absent.
/// @param disabled Bit 0 is stored.
void set_gadget_disabled_by_name(GadgetPanel& panel, std::string_view name, uint32_t disabled);

/// Writes a button quick key by name and requests a redraw.
///
/// @param[in,out] panel GUI context.
/// @param name Gadget name.
/// @param key Quick-key character; nothing is written when no record has the name.
void set_quick_key_by_name(GadgetPanel& panel, std::string_view name, uint8_t key);

/// Drops trailing caption characters until the caption fits the record width - 6.
///
/// Labels and alternate-font buttons are measured with GAF font slot 1. A
/// caption cut to nothing that is still too wide stays empty.
///
/// @param[in,out] panel GUI context.
/// @param index Button, text box or label record.
void fit_text_to_width(GadgetPanel& panel, int32_t index);

// ---- List boxes and scroll bars ----

/// Selects a list row by name, scrolling it into view and moving the linked scroll bar knob proportionally.
///
/// @param[in,out] panel GUI context.
/// @param name List gadget name; nothing happens when absent.
/// @param selected Row to select, from 0.
void set_list_selection(GadgetPanel& panel, std::string_view name, int16_t selected);

/// Resets a list to its first row, trims its height to whole rows and stamps the auto-scroll tick.
///
/// @param[in,out] panel GUI context.
/// @param index List record.
void reset_list_scroll(GadgetPanel& panel, int32_t index);

/// Binds text rows to a list and computes its last scroll origin.
///
/// The linked scroll bar is activated when the rows overflow the list and
/// deactivated otherwise.
///
/// @param[in,out] panel GUI context.
/// @param name List gadget name; nothing happens when absent.
/// @param lines NUL- or LF-separated rows; must outlive the binding.
/// @param lines_size Size of `lines` in bytes.
/// @param count Number of rows.
/// @param item_flags Per-row flag bytes; null for none.
void init_text_list(
    GadgetPanel& panel,
    std::string_view name,
    const char* lines,
    std::size_t lines_size,
    int32_t count,
    const uint8_t* item_flags
);

/// Binds an image table to a list and computes its last scroll origin.
///
/// Uses the item heights, or the fixed item height when one is set.
///
/// @param[in,out] panel GUI context.
/// @param[in,out] owner Panel whose records hold the list.
/// @param name List gadget name; nothing happens when absent.
/// @param images Table of 0x18-byte image records; must outlive the binding.
/// @param count Number of images.
void init_image_list(
    GadgetPanel& panel, GadgetOwner& owner, std::string_view name, const void* images, int32_t count
);

/// Handles the pointer for a list.
///
/// Captures on a press, selects rows (text rows or images), skips "&G"
/// headers, and auto-scrolls at the edges while held.
///
/// @param[in,out] panel GUI context.
/// @param index List record.
/// @return True when a double-click activates a row, or an activation is pending.
bool list_pointer(GadgetPanel& panel, int32_t index);

/// Sizes a scroll bar knob and travel range from its linked list's visible fraction, then redraws the bar.
///
/// An image list whose total height is zero leaves the knob unchanged.
///
/// @param[in,out] panel GUI context.
/// @param index Scroll bar record.
void update_scroll_knob_size(GadgetPanel& panel, int32_t index);

/// Handles the pointer for a scroll bar: capture, knob drag, and click-to-step.
///
/// @param[in,out] panel GUI context.
/// @param index Scroll bar record; locked or text-list bars are ignored.
void scroll_bar_pointer(GadgetPanel& panel, int32_t index);

/// Propagates a list or scroll bar change to the other members of its group.
///
/// Linked lists share the scroll origin, text boxes echo the selected row,
/// and scroll bars follow the list's scroll origin.
///
/// @param[in,out] panel GUI context.
/// @param index List or scroll bar record that changed.
void sync_group_members(GadgetPanel& panel, int32_t index);

/// Moves a scroll bar knob one step back, redrawing on change.
///
/// @param[in,out] panel GUI context.
/// @param index Scroll bar record.
void step_scroll_back(GadgetPanel& panel, int32_t index);

/// Moves a scroll bar knob one step forward, redrawing on change.
///
/// @param[in,out] panel GUI context.
/// @param index Scroll bar record.
void step_scroll_forward(GadgetPanel& panel, int32_t index);

/// Moves a scroll bar knob to a position, redrawing when it changes.
///
/// The position is kept within 0 .. range - 1. The bar's handler runs, as it
/// does for a step, whether or not the knob moved.
///
/// @param[in,out] panel GUI context.
/// @param index Scroll bar record.
/// @param knob Knob position; clamped into the bar's range.
void place_scroll_knob(GadgetPanel& panel, int32_t index, int16_t knob);

/// Moves a list selection up one row; moving onto an "&G" header is refused.
///
/// @param[in,out] panel GUI context.
/// @param index List record.
void list_select_previous(GadgetPanel& panel, int32_t index);

/// Moves a list selection down one row; moving onto an "&G" header is refused.
///
/// @param[in,out] panel GUI context.
/// @param index List record.
void list_select_next(GadgetPanel& panel, int32_t index);

/// Finds the start of a row in a buffer of NUL- or LF-terminated rows.
///
/// @param lines Row buffer.
/// @param lines_size Size of the buffer in bytes.
/// @param line Row number, from 0.
/// @return The row's first character, or "" when the walk runs off the buffer.
[[nodiscard]] const char* nth_line(const char* lines, std::size_t lines_size, int32_t line);

// ---- Pointer and key handlers per gadget type ----

/// Handles the pointer and quick key for a linked label.
///
/// @param[in,out] panel GUI context.
/// @param index Label record.
/// @param key Key read this update, 0 for none; a matching quick key is popped.
/// @return True when a click is released inside the label or its quick key is pressed.
bool label_pointer(GadgetPanel& panel, int32_t index, int32_t key);

/// Advances a running type-13 progress record by its float step each time its tick deadline passes, then redraws its value.
///
/// @param[in,out] panel GUI context.
/// @param index Progress record.
void progress_tick(GadgetPanel& panel, int32_t index);

/// Runs a hot surface's callback and, when hot, reports a click released inside it.
///
/// @param[in,out] panel GUI context.
/// @param index Hot surface record.
/// @return True when a click on a hot surface is released inside it.
/// @quirk The bounds test uses the raw pointer, not the panel-relative one.
bool hot_surface_pointer(GadgetPanel& panel, int32_t index);

/// Runs the pointer and quick-key state machine of a type-1 button.
///
/// Covers plain, hold, toggle, checkbox and frame-cycling variants,
/// auto-repeat, and linked scroll-bar stepping. Grayed buttons are ignored.
///
/// @param[in,out] panel GUI context.
/// @param index Button record.
/// @param key Key read this update, 0 for none; a matching quick key is popped.
/// @return True when the button is activated.
bool button_pointer(GadgetPanel& panel, int32_t index, int32_t key);

/// Handles pointer focus and keystroke editing for a text box.
///
/// @param[in,out] panel GUI context.
/// @param index Text box record.
/// @param key Key read this update, 0 for none.
/// @return True when Enter or Escape ends the edit; Escape also clears the text.
bool text_box_pointer(GadgetPanel& panel, int32_t index, int32_t key);

/// Applies keystrokes to a text box.
///
/// Handles caret movement, deletion, clipboard paste and filtered insertion
/// within the width and length limits, repeating while keys remain.
///
/// @param[in,out] panel GUI context.
/// @param index Text box record.
/// @param key First key; replaced by a popped key when the panel only peeks the queue.
/// @return The last key processed, 0 when none.
int32_t edit_keystroke(GadgetPanel& panel, int32_t index, int32_t key);

/// Stores the caret, or clears an over-long text, and redraws the text box.
///
/// @param[in,out] panel GUI context.
/// @param index Text box record.
/// @param[in,out] text The text box's text bytes.
/// @param max_chars Maximum text length.
/// @param clear Nonzero to clear the text.
void set_caret_and_draw(
    GadgetPanel& panel, int32_t index, char* text, int32_t max_chars, int32_t clear
);

/// Dispatches a key against the focused record.
///
/// Tab follows tab_order. Arrows still move by place, and step a horizontal
/// scroll bar or a list. Page Up, Page Down, Home and End move a horizontal
/// bar by a page or to its ends. Enter and Escape activate the panel's
/// default buttons; Space activates the focused button, list or surface.
/// A text box keeps Left, Right, Home, End and the page keys for its caret.
///
/// @param[in,out] panel GUI context.
/// @param key Key code from the frontend key queue.
/// @return 0 when the key was consumed, otherwise the key.
int32_t dispatch_key(GadgetPanel& panel, int32_t key);

/// Runs one per-frame panel update.
///
/// Steps the cursor and reads the pointer, dispatches the keyboard, redraws
/// when dirty, sets the cursor for the pointer over or off the top panel,
/// routes the pointer to each active record by type, updates the help text,
/// and handles activation: the panel's command callback runs, then the panel
/// closes unless the callback cleared the activation.
///
/// @param[in,out] panel GUI context.
/// @return 0 without a panel, otherwise 1.
int32_t update_panel(GadgetPanel& panel);

/// Word-wraps text for a message box.
///
/// Breaks after spaces or hyphens once a line reaches the width; lines end
/// with CR LF. A width too small to hold one "d" gives one character per line.
/// Text with Chinese, Japanese or Korean characters breaks before a line
/// reaches the width where the line breaker lets it
/// (oa::base::text::first_row): at a space or between two Chinese or
/// Japanese characters, never starting a line with a closing mark or ending
/// one with an opening mark, and inside a word, a Korean one among them,
/// only when the word is wider than the line.
///
/// @param[in,out] panel GUI context; its font selection is used for measuring.
/// @param text Text to wrap.
/// @param width Line width in pixels.
/// @param font_record Font record to measure with, or -1 for the GAF font.
/// @return The wrapped text.
[[nodiscard]] std::string
wrap_text(GadgetPanel& panel, std::string_view text, int32_t width, int32_t font_record);

// ---- Pointer cursor and pointer queue ----

/// Runs a cursor sequence from its first frame and shows that frame.
///
/// @param[in,out] panel GUI context; the off-panel cursor becomes `sequence`.
/// @param sequence Cursor sequence.
void set_cursor_sequence(GadgetPanel& panel, const formats::gaf::Sequence* sequence);

/// Shows the cursor for the pointer over the top panel or off it.
///
/// That place's sequence runs from its first frame unless it already runs;
/// without a sequence its still picture shows, which stops the animation.
///
/// @param[in,out] panel GUI context.
/// @param over_panel True when the pointer is over the top panel.
void follow_hover_cursor(GadgetPanel& panel, bool over_panel);

/// Makes an image every cursor still and shows it; both sequences and the held buttons are dropped.
///
/// @param[in,out] panel GUI context.
/// @param image Cursor picture.
void reset_cursor(GadgetPanel& panel, const formats::gaf::Frame* image);

/// Steps a running cursor and reads the next pointer event.
///
/// The cursor advances by the update's elapsed ticks. The next queued event
/// is taken when it lies over the top panel or holds no button; with nothing
/// queued the latest move is taken. An event with buttons off the top panel
/// stays queued for whoever reads the queue next.
///
/// @param[in,out] panel GUI context.
void tick_cursor_and_read_pointer(GadgetPanel& panel);

// ---- Cycling captions ----

/// Allocates empty cycling-caption slots and activates them.
///
/// @param[in,out] panel GUI context.
/// @param count Number of slots; negative counts allocate none.
void alloc_blink_words(GadgetPanel& panel, int32_t count);

/// Frees the cycling-caption slots and deactivates them.
///
/// @param[in,out] panel GUI context.
void free_blink_words(GadgetPanel& panel);

/// Sets the font record the cycling captions are drawn with.
///
/// @param[in,out] panel GUI context; ignored without slots.
/// @param record Font record index stored in slot 0.
void set_blink_font_record(GadgetPanel& panel, int32_t record);

/// Empties the text of every cycling-caption slot.
///
/// @param[in,out] panel GUI context.
void clear_blink_words(GadgetPanel& panel);

/// Toggles each cycling caption between its two colours on its own timers and redraws it.
///
/// @param[in,out] panel GUI context.
void update_blink_words(GadgetPanel& panel);

} // namespace oa::ui::gui_input

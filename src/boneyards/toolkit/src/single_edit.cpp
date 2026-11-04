// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// SingleEdit: one-line entry with a selection, word movement, a password
// mask and an optional scrolling view over text wider than the box.
#include <cstdlib>
#include <cstring>

#include "oa/boneyards/toolkit.hpp"
#include "oa/boneyards/utf8.hpp"
#include "ops_defaults.hpp"

namespace oa::boneyards {
namespace {

constexpr int32_t k_default_padding = 6;
constexpr int32_t k_default_blink_period = 0x10;
constexpr int32_t k_entry_line = 0xe;
constexpr int32_t k_entry_grown = 0x11;
// A grown line sits two pixels higher than the 14-pixel line, and one lower.
constexpr int32_t k_entry_lift = 2;
constexpr int32_t k_caret_width = 2;
constexpr int32_t k_caret_inset = 2;
constexpr int32_t k_state_disabled = 0;
constexpr int32_t k_state_enabled = 1;
const char* const k_state_names[] = {"Disabled", "Enabled"};

SingleEdit* as_single_edit(Window* w) {
    return reinterpret_cast<SingleEdit*>(w);
}

int32_t available_width(const SingleEdit* edit) {
    return edit->base.width + edit->padding * -2;
}

void reserve(SingleEdit* edit, int32_t length) {
    if (length + 1 <= edit->capacity) {
        return;
    }
    int32_t capacity = edit->capacity < 32 ? 32 : edit->capacity;
    while (capacity < length + 1) {
        capacity *= 2;
    }
    auto* grown = static_cast<char*>(std::realloc(edit->text, static_cast<size_t>(capacity)));
    if (grown == nullptr) {
        std::abort();
    }
    edit->text = grown;
    edit->capacity = capacity;
}

bool entry_modern(const SingleEdit* edit) {
    return edit->base.modern_text != 0 && modern_chat_on();
}

// Width of a string. A marked entry uses the modern faces while they are on.
int32_t text_width(const SingleEdit* edit, const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return 0;
    }
    if (entry_modern(edit)) {
        return modern_chat_width(text);
    }
    int32_t width = 0;
    int32_t height = 0;
    text_measure_string(edit->base.font, text, &width, &height);
    return width;
}

// Width of `count` copies of `ch`. The stored text is not used.
int32_t repeated_width(const SingleEdit* edit, char ch, int32_t count) {
    if (count <= 0) {
        return 0;
    }
    constexpr int32_t k_stack = 128;
    char stack[k_stack + 1];
    char* buffer = stack;
    char* heap = nullptr;
    if (count > k_stack) {
        heap = static_cast<char*>(std::malloc(static_cast<size_t>(count) + 1));
        if (heap == nullptr) {
            std::abort();
        }
        buffer = heap;
    }
    std::memset(buffer, static_cast<unsigned char>(ch), static_cast<size_t>(count));
    buffer[count] = '\0';
    const int32_t width = text_width(edit, buffer);
    std::free(heap);
    return width;
}

// Width of the characters the player sees in text[from, to). A masked edit
// measures that many mask characters. Empty spans measure zero. `from` and
// `to` are character indexes.
int32_t span_width(const SingleEdit* edit, int32_t from, int32_t to) {
    if (to <= from) {
        return 0;
    }
    if (edit->mask != '\0') {
        return repeated_width(edit, edit->mask, to - from);
    }
    if (edit->text == nullptr) {
        return 0;
    }
    char* text = edit->text;
    const int32_t from_byte = utf8::offset(text, from);
    const int32_t to_byte = utf8::offset(text, to);
    const char saved = text[to_byte];
    text[to_byte] = '\0';
    const int32_t width = text_width(edit, text + from_byte);
    text[to_byte] = saved;
    return width;
}

int32_t char_at(const SingleEdit* edit, int32_t index) {
    if (edit->text == nullptr || index < 0 || index >= edit->length) {
        return 0;
    }
    return static_cast<int32_t>(utf8::read(edit->text + utf8::offset(edit->text, index)).code);
}

bool composing(const SingleEdit* edit) {
    return edit->composition != nullptr && edit->composition[0] != '\0';
}

// Characters being composed are shown when the caret is inside the view,
// including at its end.
bool composition_in_view(const SingleEdit* edit) {
    return composing(edit) && edit->caret >= edit->first_visible &&
           edit->caret <= edit->last_visible;
}

int32_t composition_width(const SingleEdit* edit) {
    if (!composing(edit)) {
        return 0;
    }
    if (edit->mask != '\0') {
        return repeated_width(edit, edit->mask, utf8::characters(edit->composition));
    }
    return text_width(edit, edit->composition);
}

// The visible slice, with characters being composed at the caret. The caller
// frees it.
char* visible_text(const SingleEdit* edit) {
    const char* body = edit->text != nullptr ? edit->text : "";
    const int32_t from_byte = utf8::offset(body, edit->first_visible);
    const int32_t to_byte = utf8::offset(body, edit->last_visible);
    const bool splice = composition_in_view(edit);
    const char* extra = splice ? edit->composition : "";
    const int32_t mid = splice ? utf8::offset(body, edit->caret) : to_byte;
    const int32_t left = mid - from_byte;
    const int32_t right = to_byte - mid;
    const auto extra_bytes = static_cast<int32_t>(std::strlen(extra));
    auto* out =
        static_cast<char*>(std::malloc(static_cast<std::size_t>(left + extra_bytes + right) + 1));
    if (out == nullptr) {
        std::abort();
    }
    if (left > 0) {
        std::memcpy(out, body + from_byte, static_cast<std::size_t>(left));
    }
    if (extra_bytes > 0) {
        std::memcpy(out + left, extra, static_cast<std::size_t>(extra_bytes));
    }
    if (right > 0) {
        std::memcpy(out + left + extra_bytes, body + mid, static_cast<std::size_t>(right));
    }
    out[left + extra_bytes + right] = '\0';
    return out;
}

void clear_selection(SingleEdit* edit) {
    edit->selection_end = 0;
    edit->selection_start = 0;
}

bool has_selection(const SingleEdit* edit) {
    return edit->selection_start != edit->selection_end;
}

/// Scrolls a scrolling edit's view so the caret stays inside it.
///
/// A caret past the view's end becomes the new end and characters leave the
/// front until the view fits the box; a caret before the view's start
/// becomes the new start and characters leave the end. A caret already in
/// view, or an edit without `scroll`, changes nothing.
///
/// @param[in,out] edit edit whose first_visible/last_visible change
void adjust_view(SingleEdit* edit) {
    if (edit->scroll == 0) {
        return;
    }
    bool shrink_end = false;
    if (edit->last_visible < edit->caret) {
        edit->last_visible = edit->caret;
    } else if (edit->caret < edit->first_visible) {
        edit->first_visible = edit->caret;
        shrink_end = true;
    } else {
        return;
    }
    while (available_width(edit) < span_width(edit, edit->first_visible, edit->last_visible)) {
        if (shrink_end) {
            --edit->last_visible;
        } else {
            ++edit->first_visible;
        }
    }
}

/// Tests whether a key code types a character into the edit.
///
/// Navigation and modifier keys share their numeric range with characters, so
/// those keys are never inserted.
///
/// @param ch key code from the message
/// @return true for a character the player can type
bool is_editable_char(int32_t ch) {
    if (ch >= key::del && ch <= key::system) {
        return false;
    }
    return ch >= 0 && utf8::taken(static_cast<char32_t>(ch));
}

/// Draws the caret, 2 pixels wide in the caret colour, after the visible text
/// before it (and after characters being composed) and centred vertically.
///
/// The height is caret_height, or the font's line height less 2 when that is
/// -1. Nothing is drawn when the caret lies beyond the box's text area.
///
/// @param edit edit whose caret is drawn
void draw_caret_bar(SingleEdit* edit) {
    const int32_t offset = single_edit_caret_offset(edit);
    int32_t height = edit->caret_height;
    if (height == -1) {
        if (entry_modern(edit)) {
            const ModernChatLine* line = modern_chat_line();
            height = line != nullptr ? line->ascent + line->descent : 0;
        } else {
            height = font_line_height(edit->base.font) - 2;
        }
    }
    if (offset <= available_width(edit)) {
        const Window& w = edit->base;
        color_fill(
            w.x + edit->padding - k_caret_inset + offset,
            (w.height - height) / 2 + w.y,
            k_caret_width,
            height,
            edit->caret_color
        );
    }
}

// The visible text, left-aligned inside the padding. A marked entry uses the
// modern faces while they are on, centred on their line.
void draw_entry_text(SingleEdit* edit, const char* shown) {
    const Window& box = edit->base;
    const int32_t x = box.x + edit->padding;
    const int32_t width = box.width + edit->padding * -2;
    if (!entry_modern(edit)) {
        text_draw_string(box.font, x, box.y, width, box.height, shown, text_style::left);
        return;
    }
    const ModernChatLine* line = modern_chat_line();
    if (line == nullptr) {
        return;
    }
    const int32_t line_height = line->ascent + line->descent;
    int32_t top = box.y;
    if (line_height < box.height) {
        top = box.y + (box.height - line_height) / 2;
    }
    modern_chat_paint(shown, x, top + line->ascent, box.modern_ink, x, box.y, width, box.height);
}

/// Draws a SingleEdit and advances its caret blink.
///
/// Draws the current state's image (there is no background fill), then with
/// a font set: the selection band, the visible text left-aligned inside the
/// padding and, while focused, the focus frame (when focus_frame is set) and
/// the caret (when caret_visible is set and, with blinking on, only in the
/// second half of each blink period). A masked edit draws its mask character
/// in place of every visible character and leaves the stored text as typed.
/// The caret and the selection band use the mask character's width. Characters
/// being composed are drawn at the caret, or as that many more mask characters.
/// Children are drawn last, also while hidden.
///
/// @param[in,out] self edit (a SingleEdit record) to draw; its blink counter
///     advances
/// @return true (handled)
bool single_edit_draw(Window* self) {
    if (!window_is_shown(self)) {
        window_draw_children(self);
        return true;
    }
    SingleEdit* edit = as_single_edit(self);
    ++edit->blink_counter;
    const int32_t state = self->states.current;
    const Handle image =
        state >= 0 && state < self->states.count ? self->states.images[state] : k_no_handle;
    if (image >= 0) {
        image_draw(self->x, self->y, self->width, self->height, image);
    }
    if (self->font >= 0) {
        if (has_selection(edit)) {
            int32_t height = edit->selection_height;
            if (height == -1) {
                height = edit->caret_height;
                if (height == -1) {
                    height = font_line_height(self->font) - 2;
                }
            }
            const int32_t left = span_width(edit, edit->first_visible, edit->selection_start);
            const int32_t end =
                edit->last_visible < edit->selection_end ? edit->last_visible : edit->selection_end;
            const int32_t right = span_width(edit, edit->first_visible, end);
            if (right - left > 0) {
                color_fill(
                    edit->padding + self->x + left,
                    (self->height - height) / 2 + self->y,
                    right - left,
                    height,
                    edit->selection_color
                );
            }
        }
        const bool show_text = edit->length > 0 || composing(edit);
        if (show_text) {
            int32_t count = edit->last_visible - edit->first_visible;
            if (count < 0) {
                count = 0;
            }
            if (composition_in_view(edit)) {
                count += utf8::characters(edit->composition);
            }
            if (edit->mask != '\0') {
                if (count > 0) {
                    constexpr int32_t k_stack = 128;
                    char stack[k_stack + 1];
                    char* shown = stack;
                    char* heap = nullptr;
                    if (count > k_stack) {
                        heap = static_cast<char*>(std::malloc(static_cast<size_t>(count) + 1));
                        if (heap == nullptr) {
                            std::abort();
                        }
                        shown = heap;
                    }
                    std::memset(
                        shown, static_cast<unsigned char>(edit->mask), static_cast<size_t>(count)
                    );
                    shown[count] = '\0';
                    draw_entry_text(edit, shown);
                    std::free(heap);
                }
            } else {
                char* shown = visible_text(edit);
                draw_entry_text(edit, shown);
                std::free(shown);
            }
        }
        if (self->focused != 0) {
            if (edit->focus_frame != 0) {
                draw_focus_frame(self->x, self->y, self->width, self->height);
            }
            const bool lit = edit->blink == 0 || edit->blink_counter > edit->blink_period / 2;
            if (lit && edit->caret_visible != 0) {
                draw_caret_bar(edit);
            }
        }
    }
    if (edit->blink_period <= edit->blink_counter) {
        edit->blink_counter = 0;
    }
    window_draw_children(self);
    return true;
}

/// Handles typing, deletion and caret keys in a SingleEdit.
///
/// A character the player can type replaces the selection and is inserted
/// whole, then on_changed runs when notify_changes is set. Backspace and
/// Delete remove the selection or one character. Left and Right move by
/// character (by word with Control), and Home and End move to either end;
/// Shift extends the selection. While characters are being composed, every
/// key but Escape is kept for that composition. Other keys, and every key
/// while the edit is disabled, take the default key handling.
///
/// @param[in,out] self edit (a SingleEdit record) receiving the key
/// @param msg key_down message; null is not handled
/// @return true for the keys above, otherwise the default key_down result
bool single_edit_key_down(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    SingleEdit* edit = as_single_edit(self);
    if (self->states.current == k_state_disabled) {
        return window_default_key_down(self, msg);
    }
    const int32_t k = msg->key;
    if (composing(edit) && k != key::escape) {
        return true;
    }
    if (is_editable_char(k)) {
        if (has_selection(edit)) {
            single_edit_erase_selection(edit);
        }
        single_edit_insert(edit, k);
        if (edit->notify_changes != 0 && edit->on_changed != nullptr) {
            edit->on_changed(edit);
        }
        return true;
    }
    switch (k) {
    case key::end:
        single_edit_move_end(edit, input_key_down(key::shift));
        return true;
    case key::backspace:
        if (has_selection(edit)) {
            single_edit_erase_selection(edit);
        } else {
            single_edit_erase(edit, false);
        }
        return true;
    case key::del:
        if (has_selection(edit)) {
            single_edit_erase_selection(edit);
        } else {
            single_edit_erase(edit, true);
        }
        return true;
    case key::home:
        single_edit_move_home(edit, input_key_down(key::shift));
        return true;
    case key::left:
        if (input_key_down(key::control)) {
            single_edit_move_word_left(edit, input_key_down(key::shift));
        } else {
            single_edit_move_left(edit, input_key_down(key::shift));
        }
        return true;
    case key::right:
        if (input_key_down(key::control)) {
            single_edit_move_word_right(edit, input_key_down(key::shift));
        } else {
            single_edit_move_right(edit, input_key_down(key::shift));
        }
        return true;
    default:
        break;
    }
    return window_default_key_down(self, msg);
}

/// Looks up a SingleEdit state name ("Disabled", "Enabled").
///
/// @param self edit whose state count bounds the search
/// @param name state name, compared case-sensitively; may be null
/// @return the state index, or -1 for a null or unknown name
int32_t single_edit_state_index(Window* self, const char* name) {
    if (name == nullptr) {
        return -1;
    }
    for (int32_t i = 0; i < self->states.count && i < 2; ++i) {
        if (std::strcmp(k_state_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

/// Returns the name of a SingleEdit state.
///
/// @param self edit whose state count bounds the lookup
/// @param state state index
/// @return "Disabled" or "Enabled", or null outside the edit's states
const char* single_edit_state_name(Window* self, int32_t state) {
    if (state < 0 || state >= self->states.count || state >= 2) {
        return nullptr;
    }
    return k_state_names[state];
}

/// Tests whether a class name is "SingleEdit".
///
/// @param name class name to test; may be null
/// @return true when `name` is "SingleEdit"
bool single_edit_is_class(const char* name) {
    return names_equal(name, class_name::single_edit);
}

/// Returns the SingleEdit class's type code, which nothing in the engine
/// reads.
///
/// @return the class's constant
int32_t single_edit_type_code() {
    return 0;
}

/// Frees a SingleEdit: releases its caret and selection colours and its text
/// storage, runs the base teardown and frees the record.
///
/// The lobby text field's destructor adds nothing of its own before this.
///
/// @param self edit (a SingleEdit record) to free
void single_edit_destroy(Window* self) {
    SingleEdit* edit = as_single_edit(self);
    color_release(edit->selection_color);
    color_release(edit->caret_color);
    std::free(edit->text);
    std::free(edit->composition);
    std::free(edit->shown);
    window_release(self);
    std::free(self);
}

void shift_children(Window* self, int32_t dy) {
    for (int32_t i = 0; i < self->children.count; ++i) {
        Window* child = self->children.items[i];
        if (child == nullptr) {
            continue;
        }
        child->y += dy;
        shift_children(child, dy);
    }
}

/// Gives a marked 14-pixel line the modern face's box.
///
/// The bottom edge stays where the 14-pixel line put it. A line whose rest
/// height is anything else stays put, so a lobby field is left alone.
void fit_modern_entry(SingleEdit* edit) {
    if (edit->base.height == k_entry_line) {
        edit->rest_y = edit->base.y;
        edit->rest_height = k_entry_line;
    } else if (edit->rest_height == 0 && edit->base.height > 0) {
        edit->rest_height = k_entry_line;
        edit->rest_y = edit->base.y + edit->base.height - k_entry_line;
    }
    if (edit->rest_height != k_entry_line) {
        return;
    }
    const ModernChatLine* line = modern_chat_line();
    if (line == nullptr) {
        return;
    }
    const int32_t height = line->entry_height;
    const int32_t y = edit->rest_y + k_entry_line - height;
    const int32_t dy = y - edit->base.y;
    edit->base.y = y;
    edit->base.height = height;
    if (dy != 0) {
        shift_children(&edit->base, dy);
    }
    if (edit->selection_height == k_entry_line || edit->selection_height == k_entry_grown ||
        edit->selection_height == height) {
        edit->selection_height = height;
    }
}

/// Lifts a 14-pixel line to 17 when a glyph is taller than the font's line.
///
/// A marked line uses the modern face's box instead, for any text, and its
/// bottom edge stays put. The Boneyards line's bottom edge moves down one
/// pixel. A line of any other height, and a line whose glyphs fit the font's
/// line, stays put. Without an atlas a character the face does not cover
/// takes no extra room.
void fit_entry_line(SingleEdit* edit) {
    if (entry_modern(edit)) {
        fit_modern_entry(edit);
        return;
    }
    if (edit->base.height == k_entry_line) {
        edit->rest_y = edit->base.y;
        edit->rest_height = k_entry_line;
    } else if (edit->rest_height == 0 && edit->base.height > 0) {
        edit->rest_y = edit->base.y;
        edit->rest_height = edit->base.height;
    }
    if (edit->rest_height != k_entry_line) {
        return;
    }
    TextFit fit{};
    const FontService* fonts = services().fonts;
    if (fonts != nullptr && fonts->self != nullptr) {
        toolkit_text_fit(
            static_cast<Toolkit*>(fonts->self), edit->base.font, single_edit_text(edit), &fit
        );
    }
    const bool grow = fit.above > 0 || fit.below > 0;
    const int32_t height = grow ? k_entry_grown : k_entry_line;
    const int32_t y = grow ? edit->rest_y - k_entry_lift : edit->rest_y;
    const int32_t dy = y - edit->base.y;
    edit->base.y = y;
    edit->base.height = height;
    if (dy != 0) {
        shift_children(&edit->base, dy);
    }
    if (edit->selection_height == k_entry_line || edit->selection_height == k_entry_grown) {
        edit->selection_height = height;
    }
}

constexpr WindowOps make_single_edit_ops() {
    WindowOps ops = detail::plain_window_ops();
    ops.class_name = class_name::single_edit;
    ops.destroy = single_edit_destroy;
    ops.state_index = single_edit_state_index;
    ops.state_name = single_edit_state_name;
    ops.is_class = single_edit_is_class;
    ops.type_code = single_edit_type_code;
    ops.draw = single_edit_draw;
    ops.key_down = single_edit_key_down;
    ops.raise = detail::gadget_raise;
    return ops;
}

} // namespace

constinit const WindowOps k_single_edit_ops = make_single_edit_ops();

SingleEdit* single_edit_create() {
    auto* edit = static_cast<SingleEdit*>(std::calloc(1, sizeof(SingleEdit)));
    if (edit == nullptr) {
        std::abort();
    }
    window_init(&edit->base, &k_single_edit_ops);
    reserve(edit, 0);
    edit->text[0] = '\0';
    edit->caret_color = k_no_handle;
    edit->caret_height = -1;
    edit->selection_color = k_no_handle;
    edit->selection_height = -1;
    edit->padding = k_default_padding;
    edit->blink_period = k_default_blink_period;
    edit->blink = 1;
    edit->caret_visible = 1;
    window_set_state_count(&edit->base, 2);
    window_set_state(&edit->base, k_state_enabled);
    window_set_color(&edit->base, 0xFF, 0xFF, 0xFF, 0xFF);
    single_edit_set_selection_color(edit, 0, 0, 0xFF, 0xFF);
    single_edit_set_caret_color(edit, 0xFF, 0xFF, 0xFF, 0xFF);
    return edit;
}

void single_edit_set_selection_color(SingleEdit* edit, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (edit->selection_color > k_no_handle) {
        color_release(edit->selection_color);
    }
    edit->selection_color = color_create(r, g, b, a);
}

void single_edit_set_caret_color(SingleEdit* edit, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (edit->caret_color > k_no_handle) {
        color_release(edit->caret_color);
    }
    edit->caret_color = color_create(r, g, b, a);
}

void single_edit_set_text(SingleEdit* edit, const char* text) {
    if (text == nullptr) {
        text = "";
    }
    single_edit_set_composition(edit, nullptr);
    const utf8::Slice slice = utf8::measure_limited(text, edit->max_length, 0);
    reserve(edit, slice.bytes);
    if (slice.bytes > 0) {
        std::memcpy(edit->text, text, static_cast<std::size_t>(slice.bytes));
    }
    edit->text[slice.bytes] = '\0';
    edit->first_visible = 0;
    edit->last_visible = slice.chars;
    edit->caret = slice.chars;
    edit->length = slice.chars;
    clear_selection(edit);
    while (available_width(edit) < span_width(edit, edit->first_visible, edit->length)) {
        if (edit->scroll == 0) {
            single_edit_erase(edit, false);
        } else {
            ++edit->first_visible;
        }
    }
    fit_entry_line(edit);
}

const char* single_edit_text(const SingleEdit* edit) {
    return edit->text != nullptr ? edit->text : "";
}

// Inserts the bytes of one character. False, after a beep, when it does not
// fit. `count` is the number of bytes.
namespace {
bool insert_bytes(SingleEdit* edit, const char* bytes, int32_t count) {
    if (count <= 0) {
        return false;
    }
    if (edit->max_length > 0 && edit->length == edit->max_length) {
        sound_beep();
        return false;
    }
    int32_t added = 0;
    if (count == 1 && !entry_modern(edit)) {
        added = text_char_width(edit->base.font, static_cast<unsigned char>(bytes[0]));
    } else {
        char added_text[5];
        std::memcpy(added_text, bytes, static_cast<std::size_t>(count));
        added_text[count] = '\0';
        added = text_width(edit, added_text);
    }
    int32_t width = 0;
    if (edit->mask != '\0') {
        width = repeated_width(edit, edit->mask, edit->length + 1);
    } else {
        width = span_width(edit, 0, edit->length) + added;
    }
    const bool overflow = available_width(edit) < width;
    if (overflow && edit->scroll == 0) {
        sound_beep();
        return false;
    }
    const auto existing =
        static_cast<int32_t>(std::strlen(edit->text != nullptr ? edit->text : ""));
    reserve(edit, existing + count);
    const int32_t at = utf8::offset(edit->text, edit->caret);
    std::memmove(
        edit->text + at + count, edit->text + at, static_cast<std::size_t>(existing - at) + 1
    );
    std::memcpy(edit->text + at, bytes, static_cast<std::size_t>(count));
    ++edit->caret;
    ++edit->length;
    ++edit->last_visible;
    if (!overflow) {
        fit_entry_line(edit);
        return true;
    }
    while (available_width(edit) < span_width(edit, edit->first_visible, edit->last_visible)) {
        if (edit->caret < edit->last_visible) {
            --edit->last_visible;
        } else {
            ++edit->first_visible;
        }
    }
    fit_entry_line(edit);
    return true;
}
} // namespace

void single_edit_insert(SingleEdit* edit, int32_t ch) {
    if (!is_editable_char(ch)) {
        sound_beep();
        return;
    }
    char encoded[4];
    const int32_t count = utf8::encode(static_cast<char32_t>(ch), encoded);
    insert_bytes(edit, encoded, count);
}

void single_edit_erase(SingleEdit* edit, bool forward) {
    const int32_t at = forward ? edit->caret : edit->caret - 1;
    const bool possible = forward ? edit->caret < edit->length : edit->caret != 0;
    if (!possible) {
        sound_beep();
    } else {
        if (!forward) {
            edit->caret = at;
        }
        const int32_t byte = utf8::offset(edit->text, at);
        const int32_t count = utf8::read(edit->text + byte).bytes;
        std::memmove(
            edit->text + byte, edit->text + byte + count, std::strlen(edit->text + byte + count) + 1
        );
        --edit->length;
    }
    if (edit->length < edit->last_visible) {
        edit->last_visible = edit->length;
    }
    if (edit->scroll != 0) {
        int32_t last = edit->last_visible;
        int32_t first = edit->first_visible;
        for (;;) {
            if (last < edit->length) {
                ++last;
            } else {
                if (first < 1) {
                    break;
                }
                --first;
            }
            if (available_width(edit) < span_width(edit, first, last)) {
                break;
            }
            edit->last_visible = last;
            edit->first_visible = first;
        }
    }
    fit_entry_line(edit);
}

void single_edit_erase_selection(SingleEdit* edit) {
    int32_t count = edit->selection_end - edit->selection_start;
    edit->caret = edit->selection_start;
    for (; count > 0; --count) {
        single_edit_erase(edit, true);
    }
    clear_selection(edit);
}

void single_edit_select(SingleEdit* edit, int32_t from, int32_t to) {
    if (from == to) {
        clear_selection(edit);
        return;
    }
    if (to < from) {
        edit->selection_end = edit->length < from ? edit->length : from;
        edit->selection_start = to < 0 ? 0 : to;
        return;
    }
    edit->selection_start = from < 0 ? 0 : from;
    edit->selection_end = edit->length < to ? edit->length : to;
}

void single_edit_set_caret(SingleEdit* edit, int32_t caret) {
    if (caret < 0) {
        edit->caret = 0;
    } else if (edit->length < caret) {
        edit->caret = edit->length;
    } else {
        edit->caret = caret;
    }
    adjust_view(edit);
}

void single_edit_set_max_length(SingleEdit* edit, int32_t max_length) {
    edit->max_length = max_length < 0 ? 0 : max_length;
}

void single_edit_set_selection_height(SingleEdit* edit, int32_t height) {
    edit->selection_height = edit->base.height < height ? edit->base.height : height;
}

int32_t single_edit_caret_offset(const SingleEdit* edit) {
    int32_t offset = 0;
    if (edit->caret != 0) {
        offset = span_width(edit, edit->first_visible, edit->caret);
    }
    if (composition_in_view(edit)) {
        offset += composition_width(edit);
    }
    return offset;
}

void single_edit_move_left(SingleEdit* edit, bool extend) {
    const int32_t caret = edit->caret;
    if (caret == 0) {
        if (!extend) {
            clear_selection(edit);
        }
        return;
    }
    if (!extend) {
        clear_selection(edit);
        edit->caret = caret - 1;
    } else if (edit->selection_start != edit->selection_end && edit->selection_start < caret) {
        edit->caret = caret - 1;
        edit->selection_end = caret - 1;
    } else {
        if (edit->selection_start == edit->selection_end) {
            edit->selection_end = caret;
        }
        edit->caret = caret - 1;
        edit->selection_start = caret - 1;
    }
    adjust_view(edit);
}

void single_edit_move_right(SingleEdit* edit, bool extend) {
    const int32_t caret = edit->caret;
    if (edit->length <= caret) {
        if (!extend) {
            clear_selection(edit);
        }
        return;
    }
    if (!extend) {
        clear_selection(edit);
        edit->caret = caret + 1;
    } else if (edit->selection_start != edit->selection_end && caret < edit->selection_end) {
        edit->caret = caret + 1;
        edit->selection_start = caret + 1;
    } else {
        if (edit->selection_start == edit->selection_end) {
            edit->selection_start = caret;
        }
        edit->caret = caret + 1;
        edit->selection_end = caret + 1;
    }
    adjust_view(edit);
}

void single_edit_move_word_left(SingleEdit* edit, bool extend) {
    if (edit->caret != 0 && char_at(edit, edit->caret - 1) == ' ') {
        single_edit_move_left(edit, extend);
    }
    while (edit->caret != 0) {
        if (char_at(edit, edit->caret - 1) == ' ') {
            return;
        }
        single_edit_move_left(edit, extend);
    }
}

void single_edit_move_word_right(SingleEdit* edit, bool extend) {
    while (edit->caret < edit->length && char_at(edit, edit->caret) != ' ') {
        single_edit_move_right(edit, extend);
    }
    while (edit->caret < edit->length && char_at(edit, edit->caret) == ' ') {
        single_edit_move_right(edit, extend);
    }
}

void single_edit_move_home(SingleEdit* edit, bool extend) {
    const int32_t caret = edit->caret;
    if (caret == 0) {
        if (!extend) {
            clear_selection(edit);
        }
        return;
    }
    if (!extend) {
        edit->selection_end = 0;
    } else if (edit->selection_start == edit->selection_end) {
        edit->selection_end = caret;
    } else if (edit->selection_end == caret) {
        edit->selection_end = edit->selection_start;
    }
    edit->selection_start = 0;
    edit->caret = 0;
    adjust_view(edit);
}

void single_edit_move_end(SingleEdit* edit, bool extend) {
    const int32_t caret = edit->caret;
    const int32_t length = edit->length;
    if (caret == length) {
        if (!extend) {
            clear_selection(edit);
        }
        return;
    }
    if (!extend) {
        clear_selection(edit);
    } else if (edit->selection_start == edit->selection_end) {
        edit->selection_start = caret;
        edit->selection_end = length;
    } else {
        if (edit->selection_start == caret) {
            edit->selection_start = edit->selection_end;
        }
        edit->selection_end = length;
    }
    edit->caret = length;
    adjust_view(edit);
}

void single_edit_set_composition(SingleEdit* edit, const char* text) {
    std::free(edit->composition);
    edit->composition = nullptr;
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    const std::size_t count = std::strlen(text);
    edit->composition = static_cast<char*>(std::malloc(count + 1));
    if (edit->composition == nullptr) {
        std::abort();
    }
    std::memcpy(edit->composition, text, count + 1);
}

const char* single_edit_composition(const SingleEdit* edit) {
    return edit->composition != nullptr ? edit->composition : "";
}

const char* single_edit_shown(SingleEdit* edit) {
    const char* body = edit->text != nullptr ? edit->text : "";
    const int32_t bytes = static_cast<int32_t>(std::strlen(body)) +
                          static_cast<int32_t>(std::strlen(single_edit_composition(edit))) + 1;
    auto* shown = static_cast<char*>(std::realloc(edit->shown, static_cast<std::size_t>(bytes)));
    if (shown == nullptr) {
        std::abort();
    }
    edit->shown = shown;
    utf8::splice_into(edit->shown, body, edit->caret, edit->composition);
    return edit->shown;
}

int32_t single_edit_caret_distance(const SingleEdit* edit) {
    const int32_t cursor = edit->padding - k_caret_inset + single_edit_caret_offset(edit);
    return cursor < 0 ? 0 : cursor;
}

void single_edit_insert_text(SingleEdit* edit, const char* text) {
    if (edit->base.states.current == k_state_disabled) {
        return;
    }
    single_edit_set_composition(edit, nullptr);
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    bool inserted = false;
    bool kept = false;
    bool selection_cleared = false;
    for (int32_t byte = 0; text[byte] != '\0';) {
        const utf8::Unit unit = utf8::read(text + byte);
        if (unit.bytes <= 0) {
            break;
        }
        byte += unit.bytes;
        if (!unit.well_formed || !utf8::taken(unit.code)) {
            continue;
        }
        kept = true;
        if (!selection_cleared && has_selection(edit)) {
            single_edit_erase_selection(edit);
            selection_cleared = true;
        }
        if (!insert_bytes(edit, text + byte - unit.bytes, unit.bytes)) {
            break;
        }
        inserted = true;
        if (edit->notify_changes != 0 && edit->on_changed != nullptr) {
            edit->on_changed(edit);
        }
    }
    if (!inserted && !kept) {
        sound_beep();
    }
}

bool window_text_field_place(const Window* window, TextFieldPlace* place) {
    if (window == nullptr || place == nullptr || window->ops == nullptr ||
        window->ops->is_class == nullptr) {
        return false;
    }
    const bool edit = window->ops->is_class(class_name::edit_box);
    const bool single = window->ops->is_class(class_name::single_edit);
    if (!edit && !single) {
        return false;
    }
    place->x = window->x;
    place->y = window->y;
    place->width = window->width;
    place->height = window->height;
    if (single) {
        place->cursor = single_edit_caret_distance(reinterpret_cast<const SingleEdit*>(window));
    } else {
        place->cursor = edit_box_caret_distance(reinterpret_cast<const EditBox*>(window));
    }
    return true;
}

} // namespace oa::boneyards

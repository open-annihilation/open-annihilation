// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// EditBox: one-line text entry limited to the box width, with an optional
// password mask. A mask is drawn in place of every character; the text kept
// is what was typed. Text is kept as typed, one character at a time.
#include <cstdlib>
#include <cstring>

#include "oa/boneyards/toolkit.hpp"
#include "oa/boneyards/utf8.hpp"
#include "ops_defaults.hpp"

namespace oa::boneyards {
namespace {

constexpr int32_t k_edit_box_capacity = 0x80; // characters, excluding the terminator
constexpr int32_t k_edit_box_bytes = k_edit_box_capacity * 4 + 1;
constexpr int32_t k_caret_extra_height = 2;
const char* const k_edit_state_names[] = {"Disabled", "Enabled"};

EditBox* as_edit(Window* w) {
    return reinterpret_cast<EditBox*>(w);
}

int32_t text_width(Handle font, const char* text) {
    int32_t width = 0;
    int32_t height = 0;
    text_measure_string(font, text, &width, &height);
    return width;
}

int32_t usable_width(const EditBox* box) {
    return box->base.width + box->padding * -2;
}

// Writes `count` copies of `ch` into `buffer`, which holds `count` + 1 bytes.
void fill_repeated(char* buffer, char ch, int32_t count) {
    if (count > 0) {
        std::memset(buffer, static_cast<unsigned char>(ch), static_cast<size_t>(count));
    }
    buffer[count > 0 ? count : 0] = '\0';
}

int32_t composition_count(const EditBox* box) {
    return utf8::characters(box->composition);
}

// Width of the characters the box shows. A mask measures that many mask
// characters; otherwise the stored text. Characters being composed are not
// part of the stored text.
int32_t shown_width(const EditBox* box) {
    if (box->mask == '\0') {
        return text_width(box->base.font, box->text);
    }
    char shown[k_edit_box_capacity + 1];
    const int32_t count = box->length > k_edit_box_capacity ? k_edit_box_capacity : box->length;
    fill_repeated(shown, box->mask, count);
    return text_width(box->base.font, shown);
}

// A navigation or modifier key shares its numeric range with characters, so
// those keys are never inserted. Other codes are characters the player can type.
bool is_typed_character(int32_t key) {
    if (key >= key::del && key <= key::system) {
        return false;
    }
    return key >= 0 && utf8::taken(static_cast<char32_t>(key));
}

bool composing(const EditBox* box) {
    return box->composition != nullptr && box->composition[0] != '\0';
}

// Inserts the bytes of one character. Beeps and inserts nothing when the
// character does not fit.
bool insert_bytes(EditBox* box, const char* bytes, int32_t count) {
    if (count <= 0) {
        return false;
    }
    if (box->length >= k_edit_box_capacity) {
        sound_beep();
        return false;
    }
    const int32_t at = utf8::offset(box->text, box->caret);
    const auto existing = static_cast<int32_t>(std::strlen(box->text));
    if (existing + count >= k_edit_box_bytes) {
        sound_beep();
        return false;
    }
    int32_t width = 0;
    if (box->mask == '\0') {
        int32_t added = 0;
        if (count == 1) {
            added = text_char_width(box->base.font, static_cast<unsigned char>(bytes[0]));
        } else {
            char added_text[5];
            std::memcpy(added_text, bytes, static_cast<std::size_t>(count));
            added_text[count] = '\0';
            added = text_width(box->base.font, added_text);
        }
        width = text_width(box->base.font, box->text) + added;
    } else {
        char shown[k_edit_box_capacity + 1];
        const int32_t masks = box->length + 1;
        fill_repeated(shown, box->mask, masks > k_edit_box_capacity ? k_edit_box_capacity : masks);
        width = text_width(box->base.font, shown);
    }
    if (usable_width(box) < width) {
        sound_beep();
        return false;
    }
    std::memmove(
        box->text + at + count, box->text + at, static_cast<std::size_t>(existing - at) + 1
    );
    std::memcpy(box->text + at, bytes, static_cast<std::size_t>(count));
    ++box->caret;
    ++box->length;
    return true;
}

/// Sets the caret height to the height of "I" in the box's font plus 2
/// pixels (the on_bounds slot).
///
/// @param[in,out] self edit box (an EditBox record) that was placed
void edit_box_on_bounds(Window* self) {
    int32_t width = 0;
    int32_t height = 0;
    text_measure_string(self->font, "I", &width, &height);
    as_edit(self)->caret_height = height + k_caret_extra_height;
}

/// Replaces the text and puts the caret at its end, then deletes characters
/// from the end until the text fits the box less its padding.
///
/// At most 128 characters are kept, and the cut is never in the middle of a
/// character. The length and the caret are that stored text. Characters the
/// player was composing are cleared. A mask is measured as mask characters,
/// so the box fits what it shows.
///
/// @param[in,out] self edit box (an EditBox record) to fill
/// @param text new text; null changes nothing
/// @param style text_style bits (unused)
void edit_box_set_caption(Window* self, const char* text, int32_t /*style*/) {
    EditBox* box = as_edit(self);
    if (text == nullptr) {
        return;
    }
    edit_box_set_composition(box, nullptr);
    const utf8::Slice slice =
        utf8::measure_limited(text, k_edit_box_capacity, k_edit_box_bytes - 1);
    if (slice.bytes > 0) {
        std::memcpy(box->text, text, static_cast<std::size_t>(slice.bytes));
    }
    box->text[slice.bytes] = '\0';
    box->length = slice.chars;
    box->caret = slice.chars;
    while (usable_width(box) < shown_width(box) && box->length > 0) {
        edit_box_delete(box, false);
    }
}

int32_t edit_state_index(Window* self, const char* name) {
    if (name == nullptr) {
        return -1;
    }
    for (int32_t i = 0; i < self->states.count && i < 2; ++i) {
        if (std::strcmp(k_edit_state_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

const char* edit_state_name(Window* self, int32_t state) {
    if (state < 0 || state >= self->states.count || state >= 2) {
        return nullptr;
    }
    return k_edit_state_names[state];
}

bool edit_box_is_class(const char* name) {
    return names_equal(name, class_name::edit_box);
}

int32_t edit_box_type_code() {
    return 0;
}

/// Draws an edit box: its face, the text left-aligned inside the padding and,
/// while focused, the focus frame and a 1-pixel caret.
///
/// The face is the current state's image, or else the background colour. The
/// caret sits one pixel after the text before it, including characters being
/// composed, and is centred vertically. A masked box draws its mask character
/// for each stored character and for each character being composed, and places
/// the caret by that character's width. The stored text is not changed. A
/// hidden box draws nothing; children are not drawn.
///
/// @param self edit box (an EditBox record) to draw
/// @return true (handled)
bool edit_box_draw(Window* self) {
    if (!window_is_shown(self)) {
        return true;
    }
    EditBox* box = as_edit(self);
    const int32_t state = self->states.current;
    const Handle image =
        state >= 0 && state < self->states.count ? self->states.images[state] : k_no_handle;
    if (image < 0) {
        color_fill(self->x, self->y, self->width, self->height, self->color);
    } else {
        image_draw(self->x, self->y, self->width, self->height, image);
    }
    const int32_t masks = box->length + composition_count(box);
    char stack[k_edit_box_capacity + 1];
    char* heap = nullptr;
    char* shown = stack;
    if (box->mask != '\0' && masks > k_edit_box_capacity) {
        heap = static_cast<char*>(std::malloc(static_cast<std::size_t>(masks) + 1));
        if (heap == nullptr) {
            std::abort();
        }
        shown = heap;
    }
    const char* drawn = box->text;
    if (box->mask != '\0') {
        fill_repeated(shown, box->mask, masks);
        drawn = shown;
    } else {
        drawn = edit_box_shown(box);
    }
    if (drawn[0] != '\0') {
        text_draw_string(
            self->font,
            box->padding + self->x,
            self->y,
            usable_width(box),
            self->height,
            drawn,
            text_style::left
        );
    }
    if (self->focused != 0) {
        draw_focus_frame(self->x, self->y, self->width, self->height);
        draw_caret(
            edit_box_caret_distance(box) + self->x,
            (self->height - box->caret_height) / 2 + self->y,
            1,
            box->caret_height
        );
    }
    std::free(heap);
    return true;
}

/// Handles typing and caret keys in an edit box.
///
/// A character the player can type is inserted. Delete and Backspace delete
/// one character, and Left and Right move the caret by one character (beeping
/// at either end). Home and End move the caret and then also take the default
/// key handling; at the start or end they beep instead. While characters are
/// being composed, every key but Escape is kept for that composition. Other
/// keys take the default key handling.
///
/// @param[in,out] self edit box (an EditBox record) receiving the key
/// @param msg key_down message; null is not handled
/// @return true for inserting, deleting, Left/Right and a composition;
///     otherwise the default key_down result
bool edit_box_key_down(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    EditBox* box = as_edit(self);
    const int32_t k = msg->key;
    if (composing(box) && k != key::escape) {
        return true;
    }
    if (is_typed_character(k)) {
        char encoded[4];
        const int32_t count = utf8::encode(static_cast<char32_t>(k), encoded);
        insert_bytes(box, encoded, count);
        return true;
    }
    switch (k) {
    case key::del:
        edit_box_delete(box, true);
        return true;
    case key::backspace:
        edit_box_delete(box, false);
        return true;
    case key::left:
        if (box->caret != 0) {
            --box->caret;
        } else {
            sound_beep();
        }
        return true;
    case key::right:
        if (box->caret < box->length) {
            ++box->caret;
        } else {
            sound_beep();
        }
        return true;
    case key::home:
        if (box->caret != 0) {
            box->caret = 0;
            return window_default_key_down(self, msg);
        }
        sound_beep();
        break;
    case key::end:
        if (box->caret != box->length) {
            box->caret = box->length;
            return window_default_key_down(self, msg);
        }
        sound_beep();
        break;
    default:
        break;
    }
    return window_default_key_down(self, msg);
}

void edit_box_destroy(Window* self) {
    EditBox* box = as_edit(self);
    std::free(box->text);
    std::free(box->composition);
    std::free(box->shown);
    window_release(self);
    std::free(self);
}

constexpr WindowOps make_edit_box_ops() {
    WindowOps ops = detail::plain_window_ops();
    ops.class_name = class_name::edit_box;
    ops.destroy = edit_box_destroy;
    ops.set_caption = edit_box_set_caption;
    ops.state_index = edit_state_index;
    ops.state_name = edit_state_name;
    ops.is_class = edit_box_is_class;
    ops.type_code = edit_box_type_code;
    ops.on_bounds = edit_box_on_bounds;
    ops.draw = edit_box_draw;
    ops.key_down = edit_box_key_down;
    return ops;
}

} // namespace

constinit const WindowOps k_edit_box_ops = make_edit_box_ops();

EditBox* edit_box_create() {
    auto* box = static_cast<EditBox*>(std::calloc(1, sizeof(EditBox)));
    auto* text = static_cast<char*>(std::calloc(static_cast<std::size_t>(k_edit_box_bytes), 1));
    if (box == nullptr || text == nullptr) {
        std::abort();
    }
    window_init(&box->base, &k_edit_box_ops);
    box->text = text;
    return box;
}

void edit_box_insert(EditBox* box, int32_t ch) {
    if (!is_typed_character(ch)) {
        sound_beep();
        return;
    }
    char encoded[4];
    const int32_t count = utf8::encode(static_cast<char32_t>(ch), encoded);
    insert_bytes(box, encoded, count);
}

void edit_box_delete(EditBox* box, bool forward) {
    const bool possible = forward ? box->caret < box->length : box->caret != 0;
    if (!possible) {
        sound_beep();
        return;
    }
    const int32_t at = forward ? box->caret : box->caret - 1;
    const int32_t byte = utf8::offset(box->text, at);
    const int32_t count = utf8::read(box->text + byte).bytes;
    if (count <= 0) {
        sound_beep();
        return;
    }
    std::memmove(
        box->text + byte, box->text + byte + count, std::strlen(box->text + byte + count) + 1
    );
    if (!forward) {
        box->caret = at;
    }
    --box->length;
}

void edit_box_set_composition(EditBox* box, const char* text) {
    std::free(box->composition);
    box->composition = nullptr;
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    const std::size_t count = std::strlen(text);
    box->composition = static_cast<char*>(std::malloc(count + 1));
    if (box->composition == nullptr) {
        std::abort();
    }
    std::memcpy(box->composition, text, count + 1);
}

const char* edit_box_composition(const EditBox* box) {
    return box->composition != nullptr ? box->composition : "";
}

const char* edit_box_shown(EditBox* box) {
    const int32_t bytes = static_cast<int32_t>(std::strlen(box->text != nullptr ? box->text : "")) +
                          static_cast<int32_t>(std::strlen(edit_box_composition(box))) + 1;
    auto* shown = static_cast<char*>(std::realloc(box->shown, static_cast<std::size_t>(bytes)));
    if (shown == nullptr) {
        std::abort();
    }
    box->shown = shown;
    utf8::splice_into(box->shown, box->text, box->caret, box->composition);
    return box->shown;
}

int32_t edit_box_caret_distance(const EditBox* box) {
    const int32_t visual = box->caret + composition_count(box);
    int32_t prefix = 0;
    if (box->mask != '\0') {
        const int32_t masks = box->length + composition_count(box);
        char stack[k_edit_box_capacity + 1];
        char* heap = nullptr;
        char* shown = stack;
        if (masks > k_edit_box_capacity) {
            heap = static_cast<char*>(std::malloc(static_cast<std::size_t>(masks) + 1));
            if (heap == nullptr) {
                std::abort();
            }
            shown = heap;
        }
        fill_repeated(shown, box->mask, masks);
        prefix = text_prefix_width(box->base.font, shown, visual - 1);
        std::free(heap);
    } else if (box->text != nullptr && (box->caret > 0 || composing(box))) {
        const int32_t bytes = static_cast<int32_t>(std::strlen(box->text)) +
                              static_cast<int32_t>(std::strlen(edit_box_composition(box))) + 1;
        auto* shown = static_cast<char*>(std::malloc(static_cast<std::size_t>(bytes)));
        if (shown == nullptr) {
            std::abort();
        }
        utf8::splice_into(shown, box->text, box->caret, box->composition);
        const int32_t cut = utf8::offset(shown, visual);
        const char saved = shown[cut];
        shown[cut] = '\0';
        prefix = text_width(box->base.font, shown);
        shown[cut] = saved;
        std::free(shown);
    }
    return prefix + box->padding + 1;
}

void edit_box_insert_text(EditBox* box, const char* text) {
    edit_box_set_composition(box, nullptr);
    if (text == nullptr || text[0] == '\0') {
        return;
    }
    bool inserted = false;
    bool kept = false;
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
        if (!insert_bytes(box, text + byte - unit.bytes, unit.bytes)) {
            break;
        }
        inserted = true;
    }
    if (!inserted && !kept) {
        sound_beep();
    }
}

} // namespace oa::boneyards

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Modern faces for the chat log and the entry line. The faces themselves are
// supplied by the game; with none installed, chat keeps the Boneyards' fonts.
#include "oa/boneyards/toolkit.hpp"
#include "oa/boneyards/utf8.hpp"

#include <cstdlib>
#include <cstring>

namespace oa::boneyards {
namespace {

constexpr int32_t k_entry_line = 14;
constexpr int32_t k_list_row = 17;
constexpr int32_t k_face_pad = 6;

ModernChatFaces g_faces{};
bool g_on = false;
ModernChatLine g_line{};

/// An ideograph, kana or hangul syllable may end a line.
bool breaks_after(char32_t code) {
    if (code >= 0x1100 && code <= 0x11FF) {
        return true;
    }
    if (code >= 0x3040 && code <= 0x30FF) {
        return true;
    }
    if (code >= 0x31F0 && code <= 0x31FF) {
        return true;
    }
    if (code >= 0x3400 && code <= 0x4DBF) {
        return true;
    }
    if (code >= 0x4E00 && code <= 0x9FFF) {
        return true;
    }
    if (code >= 0xAC00 && code <= 0xD7AF) {
        return true;
    }
    return code >= 0xF900 && code <= 0xFAFF;
}

bool ready() {
    return g_on && g_faces.advance != nullptr && g_faces.draw != nullptr;
}

/// Bytes of `text` that fit in `available` pixels. At least one character when
/// `text` is not empty, so a glyph wider than the row still takes a row.
int32_t fitting_bytes(const char* text, int32_t available) {
    int32_t count = 0;
    int32_t last_break = 0;
    int32_t width = 0;
    while (text[count] != '\0' && text[count] != '\n') {
        const utf8::Unit unit = utf8::read(text + count);
        const int32_t step = unit.bytes > 0 ? unit.bytes : 1;
        char glyph[5]{};
        std::memcpy(glyph, text + count, static_cast<std::size_t>(step));
        const int32_t advance = modern_chat_width(glyph);
        if (count > 0 && width + advance > available) {
            return last_break > 0 ? last_break : count;
        }
        width += advance;
        const char32_t code =
            unit.well_formed ? unit.code : static_cast<unsigned char>(text[count]);
        if (step == 1 && (text[count] == ' ' || text[count] == '\t' || text[count] == '-' ||
                          text[count] == '/' || text[count] == '\\')) {
            last_break = count;
        } else if (breaks_after(code)) {
            last_break = count + step;
        }
        count += step;
    }
    return count;
}

int32_t spaces_width(int32_t count) {
    if (count <= 0) {
        return 0;
    }
    if (count > 31) {
        count = 31;
    }
    char spaces[32];
    std::memset(spaces, ' ', static_cast<std::size_t>(count));
    spaces[count] = '\0';
    return modern_chat_width(spaces);
}

} // namespace

void modern_chat_set_faces(const ModernChatFaces* faces) {
    if (faces == nullptr) {
        g_faces = {};
        g_on = false;
        return;
    }
    g_faces = *faces;
    g_on = false;
}

void modern_chat_set(bool modern_fonts, int32_t text_size) {
    g_on = false;
    if (!modern_fonts || g_faces.line == nullptr) {
        return;
    }
    ModernChatLine line{};
    // Players type CJK on any language, so the CJK face stays at 12 pixels
    // at least. The console is drawn in its own pixels, so the scale is one.
    if (!g_faces.line(g_faces.context, text_size, true, &line)) {
        return;
    }
    if (line.ascent < 0) {
        line.ascent = 0;
    }
    if (line.descent < 0) {
        line.descent = 0;
    }
    const int32_t rows = line.ascent + line.descent;
    const int32_t from_face = line.pixel_size + k_face_pad;
    int32_t entry = rows > from_face ? rows : from_face;
    if (entry < k_entry_line) {
        entry = k_entry_line;
    }
    line.entry_height = entry;
    line.row_height = rows < k_list_row ? k_list_row : rows;
    g_line = line;
    g_on = true;
}

bool modern_chat_on() {
    return g_on;
}

const ModernChatLine* modern_chat_line() {
    return g_on ? &g_line : nullptr;
}

int32_t modern_chat_width(const char* text) {
    if (!g_on || g_faces.advance == nullptr || text == nullptr || text[0] == '\0') {
        return 0;
    }
    int32_t width = 0;
    if (!g_faces.advance(g_faces.context, &g_line, text, &width) || width < 0) {
        return 0;
    }
    return width;
}

bool modern_chat_paint(
    const char* text,
    int32_t pen_x,
    int32_t baseline,
    uint8_t ink,
    int32_t clip_x,
    int32_t clip_y,
    int32_t clip_width,
    int32_t clip_height
) {
    if (!ready()) {
        return false;
    }
    if (text == nullptr || text[0] == '\0') {
        return true;
    }
    if (clip_width <= 0 || clip_height <= 0) {
        return true;
    }
    Surface* target = toolkit_drawing_surface();
    if (target == nullptr || target->pixels == nullptr) {
        return false;
    }
    return g_faces.draw(
        g_faces.context,
        &g_line,
        text,
        target,
        pen_x,
        baseline,
        ink,
        clip_x,
        clip_y,
        clip_width,
        clip_height
    );
}

bool modern_chat_paint_window(const Window* window, const char* text) {
    if (window == nullptr || window->modern_text == 0 || !g_on) {
        return false;
    }
    const ModernChatLine* line = &g_line;
    const int32_t line_height = line->ascent + line->descent;
    int32_t top = window->y;
    if (line_height < window->height) {
        top = window->y + (window->height - line_height) / 2;
    }
    return modern_chat_paint(
        text,
        window->x,
        top + line->ascent,
        window->modern_ink,
        window->x,
        window->y,
        window->width,
        window->height
    );
}

void modern_chat_mark(Window* window, uint8_t ink) {
    if (window == nullptr) {
        return;
    }
    window->modern_text = 1;
    window->modern_ink = ink;
}

void modern_chat_unmark(Window* window) {
    if (window == nullptr) {
        return;
    }
    window->modern_text = 0;
}

bool modern_chat_write(
    ListBox* list,
    const char* text,
    int32_t row_width,
    int32_t first_indent,
    int32_t continuation_spaces,
    uint8_t ink,
    void (*make_row)(void* user, const char* line, int32_t indent, int32_t height, uint8_t ink),
    void* user
) {
    if (!g_on || list == nullptr || make_row == nullptr) {
        return false;
    }
    if (text == nullptr) {
        text = "";
    }
    const bool follow = list->count == 0 || list->last == list->count - 1;
    int32_t continuation = spaces_width(continuation_spaces);
    if (row_width < 1) {
        row_width = 1;
    }
    if (continuation >= row_width) {
        continuation = row_width - 1;
    }
    if (continuation < 0) {
        continuation = 0;
    }
    const int32_t height = g_line.row_height;
    bool first = true;
    int32_t added = 0;
    while (text[0] != '\0') {
        while (text[0] == ' ' || text[0] == '\t' || text[0] == '\n') {
            ++text;
        }
        if (text[0] == '\0') {
            break;
        }
        const int32_t indent = first ? first_indent : continuation;
        int32_t available = row_width - indent;
        if (available < 1) {
            available = 1;
        }
        int32_t count = fitting_bytes(text, available);
        if (count <= 0) {
            const utf8::Unit unit = utf8::read(text);
            count = unit.bytes > 0 ? unit.bytes : 1;
        }
        auto* line = static_cast<char*>(std::malloc(static_cast<std::size_t>(count) + 1));
        if (line == nullptr) {
            std::abort();
        }
        std::memcpy(line, text, static_cast<std::size_t>(count));
        line[count] = '\0';
        make_row(user, line, indent, height, ink);
        std::free(line);
        text += count;
        first = false;
        ++added;
    }
    if (added == 0) {
        return true;
    }
    if (list->base.ops != nullptr && list->base.ops->refresh != nullptr) {
        list->base.ops->refresh(&list->base);
    }
    if (follow) {
        list_box_scroll_to_end(list);
    }
    return true;
}

} // namespace oa::boneyards

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The modern faces that travel with the game, for the chat log and the entry
// line. This is not part of the toolkit library: the game and the chat's test
// compile it, and the toolkit calls it through modern_chat_set_faces.
#include "oa/app/extension.hpp"
#include "oa/boneyards/toolkit.hpp"
#include "oa/present/surface.h"

namespace oa::boneyards {
namespace {

oa::app::ModernTextSize size_of(const ModernChatLine* line) {
    oa::app::ModernTextSize size;
    size.pixel_size = line->pixel_size;
    size.bold = true;
    size.least_cjk_pixel_size = line->least_cjk;
    return size;
}

bool engine_line(void*, int32_t text_size, bool cjk_floor, ModernChatLine* out) {
    if (out == nullptr) {
        return false;
    }
    const oa::app::ModernTextSize size = oa::app::message_log_text_size(text_size, 1, cjk_floor);
    const std::optional<oa::app::ModernTextChain> chain = oa::app::modern_text_chain(size);
    if (!chain) {
        return false;
    }
    out->pixel_size = size.pixel_size;
    out->least_cjk = size.least_cjk_pixel_size;
    out->ascent = chain->ascent;
    out->descent = chain->descent;
    return true;
}

bool engine_advance(void*, const ModernChatLine* line, const char* text, int32_t* width) {
    if (line == nullptr || width == nullptr) {
        return false;
    }
    if (text == nullptr || text[0] == '\0') {
        *width = 0;
        return true;
    }
    const std::optional<std::vector<oa::app::ModernTextGlyph>> placed =
        oa::app::modern_text_layout(text, size_of(line));
    if (!placed) {
        return false;
    }
    int32_t end = 0;
    for (const oa::app::ModernTextGlyph& glyph : *placed) {
        const int32_t next = glyph.pen + glyph.advance;
        if (next > end) {
            end = next;
        }
    }
    *width = end;
    return true;
}

bool inside(int32_t value, int32_t first, int32_t last) {
    return value >= first && value <= last;
}

bool engine_draw(
    void*,
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
) {
    if (line == nullptr || target == nullptr || target->pixels == nullptr) {
        return false;
    }
    if (text == nullptr || text[0] == '\0' || clip_width <= 0 || clip_height <= 0) {
        return true;
    }
    const std::optional<oa::app::ModernTextPixels> pixels =
        oa::app::modern_text_pixels(text, size_of(line));
    if (!pixels) {
        return false;
    }
    if (pixels->width <= 0 || pixels->height <= 0 || pixels->coverage.empty()) {
        return true;
    }
    const int32_t left = pen_x - pixels->origin;
    const int32_t top = baseline - pixels->baseline;
    const int32_t box_right = clip_x + clip_width - 1;
    const int32_t box_bottom = clip_y + clip_height - 1;
    for (int32_t row = 0; row < pixels->height; ++row) {
        const int32_t y = top + row;
        if (!inside(y, clip_y, box_bottom) || !inside(y, target->clip.y1, target->clip.y2) ||
            y < 0 || y >= target->height) {
            continue;
        }
        uint8_t* out = target->pixels + static_cast<std::ptrdiff_t>(y) * target->pitch;
        const uint8_t* coverage =
            pixels->coverage.data() + static_cast<std::ptrdiff_t>(row) * pixels->width;
        for (int32_t column = 0; column < pixels->width; ++column) {
            if (coverage[column] == 0) {
                continue;
            }
            const int32_t x = left + column;
            if (!inside(x, clip_x, box_right) || !inside(x, target->clip.x1, target->clip.x2) ||
                x < 0 || x >= target->width) {
                continue;
            }
            out[x] = ink;
        }
    }
    return true;
}

const ModernChatFaces k_engine_faces{nullptr, engine_line, engine_advance, engine_draw};

} // namespace

void modern_chat_use_engine() {
    modern_chat_set_faces(&k_engine_faces);
}

} // namespace oa::boneyards

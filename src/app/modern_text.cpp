// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The modern text faces an extension reads: one stack of the fonts that
// travel with the game, opened once, and the message log's size at the
// player's Text size. The game's own drawing keeps a stack of its own. The
// stack's FreeType faces and caches are not safe to share, so every call
// takes its turn under one lock, from whichever thread it comes.

#include "oa/app/extension.hpp"

#include "oa/base/threads.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/present/game_text.hpp"
#include "oa/present/text_style.hpp"

#include <algorithm>
#include <exception>
#include <memory>

namespace oa::app {
namespace {

namespace text_font = oa::platform::text_font;

static_assert(modern_text_face_count == static_cast<int>(text_font::face_count));
static_assert(
    default_game_text_size == oa::present::default_text_size,
    "extension.hpp's default Text size follows the setting's"
);
static_assert(
    static_cast<uint8_t>(ModernTextFace::sans_bold) ==
    static_cast<uint8_t>(text_font::Face::dejavu_sans_bold)
);
static_assert(
    static_cast<uint8_t>(ModernTextFace::sans) == static_cast<uint8_t>(text_font::Face::dejavu_sans)
);
static_assert(
    static_cast<uint8_t>(ModernTextFace::endonyms) ==
    static_cast<uint8_t>(text_font::Face::endonyms)
);
static_assert(
    static_cast<uint8_t>(ModernTextFace::endonyms_bengali) ==
    static_cast<uint8_t>(text_font::Face::endonyms_bengali)
);
static_assert(
    static_cast<uint8_t>(ModernTextFace::emoji) == static_cast<uint8_t>(text_font::Face::noto_emoji)
);

/// The lock every use of the faces holds, from opening them to the last read.
/// The engine's own Mutex, which Windows XP has, and ready before any static.
oa::base::threads::Mutex faces_lock;

/// The faces, opened the first time they are asked for; null when they cannot
/// be. The caller holds faces_lock.
text_font::FontStack* opened_faces() {
    static const std::unique_ptr<text_font::FontStack> stack = [] {
        try {
            return text_font::FontStack::open(text_font::bundled_font_directory());
        } catch (const std::exception&) {
            return std::unique_ptr<text_font::FontStack>{};
        }
    }();
    return stack.get();
}

/// The stack's style for a size: whole pixels, one bit a pixel, as the message log is drawn.
text_font::Style style_of(const ModernTextSize& size) noexcept {
    text_font::Style style;
    style.pixel_size = size.pixel_size;
    style.weight = size.bold ? text_font::Weight::bold : text_font::Weight::regular;
    style.rendering = text_font::Rendering::mono;
    style.least_cjk_pixel_size = size.least_cjk_pixel_size;
    return style;
}

/// Tells whether a size is one the faces draw.
bool size_in_range(const ModernTextSize& size) noexcept {
    return size.pixel_size >= 1 && size.pixel_size <= text_font::max_pixel_size &&
           size.least_cjk_pixel_size >= 0 && size.least_cjk_pixel_size <= text_font::max_pixel_size;
}

/// The extension's name for a face of the stack.
ModernTextFace modern_face(text_font::Face face) noexcept {
    switch (face) {
    case text_font::Face::dejavu_sans_bold:
        return ModernTextFace::sans_bold;
    case text_font::Face::dejavu_sans:
        return ModernTextFace::sans;
    case text_font::Face::endonyms:
        return ModernTextFace::endonyms;
    case text_font::Face::endonyms_bengali:
        return ModernTextFace::endonyms_bengali;
    case text_font::Face::noto_emoji:
        return ModernTextFace::emoji;
    }
    return ModernTextFace::sans_bold;
}

} // namespace

std::optional<ModernTextChain> modern_text_chain(const ModernTextSize& size) {
    if (!size_in_range(size))
        return std::nullopt;
    const oa::base::threads::LockGuard turn(faces_lock);
    text_font::FontStack* const faces = opened_faces();
    if (faces == nullptr)
        return std::nullopt;
    const text_font::Style style = style_of(size);
    const auto line = faces->metrics(style);
    if (!line)
        return std::nullopt;
    const std::span<const text_font::Face> chain = text_font::fallback_chain(style.weight);
    ModernTextChain result;
    result.ascent = line->ascent;
    result.descent = line->descent;
    result.count = static_cast<int32_t>(chain.size());
    for (std::size_t index = 0; index < chain.size(); ++index) {
        const auto measured = faces->face_metrics(chain[index], style);
        if (!measured)
            return std::nullopt;
        ModernFaceMetrics& entry = result.faces[index];
        entry.face = modern_face(chain[index]);
        entry.pixel_size = measured->pixel_size;
        entry.ascent = measured->ascent;
        entry.descent = measured->descent;
    }
    return result;
}

std::optional<std::vector<ModernTextGlyph>>
modern_text_layout(std::string_view text, const ModernTextSize& size) {
    if (!size_in_range(size))
        return std::nullopt;
    const oa::base::threads::LockGuard turn(faces_lock);
    text_font::FontStack* const faces = opened_faces();
    if (faces == nullptr)
        return std::nullopt;
    const auto placed = faces->layout(text, style_of(size));
    if (!placed)
        return std::nullopt;
    std::vector<ModernTextGlyph> glyphs;
    glyphs.reserve(placed->size());
    for (const text_font::Placement& placement : *placed)
        glyphs.push_back(
            {placement.character, modern_face(placement.face), placement.pen, placement.advance}
        );
    return glyphs;
}

std::optional<ModernTextPixels>
modern_text_pixels(std::string_view text, const ModernTextSize& size) {
    if (!size_in_range(size))
        return std::nullopt;
    const oa::base::threads::LockGuard turn(faces_lock);
    text_font::FontStack* const faces = opened_faces();
    if (faces == nullptr)
        return std::nullopt;
    const auto drawn = faces->draw(text, style_of(size));
    if (!drawn)
        return std::nullopt;
    ModernTextPixels pixels;
    pixels.width = drawn->width;
    pixels.height = drawn->height;
    pixels.baseline = drawn->baseline;
    pixels.origin = drawn->origin;
    pixels.advance = drawn->advance;
    pixels.coverage = drawn->alpha;
    return pixels;
}

ModernTextSize message_log_text_size(int32_t text_size, int32_t scale, bool cjk_language) noexcept {
    ModernTextSize size;
    size.pixel_size = std::clamp(
        oa::present::face_pixel_size(text_font::message_log_pixel_size, scale, text_size),
        int32_t{1},
        text_font::max_pixel_size
    );
    size.bold = true;
    size.least_cjk_pixel_size = cjk_language ? text_font::least_cjk_language_pixel_size : 0;
    return size;
}

GameTextPreferences game_text_preferences_of(bool modern_fonts, int32_t text_size) noexcept {
    return {.modern_fonts = modern_fonts, .text_size = oa::present::held_text_size(text_size)};
}

} // namespace oa::app

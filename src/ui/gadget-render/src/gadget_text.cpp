// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Gadget text: GAF-font glyph runs, FNT fallback, wrapping and font loading.
#include "render_internal.hpp"

#include "oa/data/defs/files.hpp"
#include "oa/base/text.hpp"
#include "oa/base/threads.hpp"
#include "oa/present/game_text.hpp"
#include "oa/ui/frontend_renderer/game_text.hpp"

#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::ui::gadget_render {

namespace {

/// The characters of each GAF font worked out so far, by its glyph sequence.
struct KeptCharacters {
    base::threads::Mutex mutex{};
    std::map<const void*, present::FontCharacters> fonts{};
};

KeptCharacters& kept_characters() {
    static KeptCharacters kept;
    return kept;
}

} // namespace

namespace detail {

Sprite* art_frame(const GadgetRenderer& renderer, const void* sequence, int32_t index) {
    if (sequence == nullptr || renderer.art.frame == nullptr)
        return nullptr;
    return renderer.art.frame(renderer.art.context, sequence, index);
}

int32_t art_frame_count(const GadgetRenderer& renderer, const void* sequence) {
    if (sequence == nullptr || renderer.art.frame_count == nullptr)
        return 0;
    return renderer.art.frame_count(renderer.art.context, sequence);
}

const void* art_find(const GadgetRenderer& renderer, const void* file, const char* name) {
    if (file == nullptr || renderer.art.find_sequence == nullptr)
        return nullptr;
    return renderer.art.find_sequence(renderer.art.context, file, name);
}

const void* active_glyphs(const GadgetRenderer& renderer, const GadgetPanel& panel) {
    if (panel.active_gaf_font == nullptr || renderer.art.first_sequence == nullptr)
        return nullptr;
    return renderer.art.first_sequence(renderer.art.context, panel.active_gaf_font);
}

void clear_origins(const GadgetRenderer& renderer, const void* sequence) {
    const int32_t count = art_frame_count(renderer, sequence);
    for (int32_t index = 0; index < count; ++index) {
        if (Sprite* frame = art_frame(renderer, sequence, index)) {
            frame->origin_x = 0;
            frame->origin_y = 0;
        }
    }
}

void fill_clipped(Surface* target, const Rect32& rect, uint8_t color) {
    Surface locked{};
    Surface* surface = target;
    if (surface == nullptr) {
        if (present::lock_display_surface(locked) == 0)
            return;
        surface = &locked;
    }
    Rect32 clipped = rect;
    if (present::clip_rect(*surface, clipped))
        present::fill_rect(*surface, clipped, color);
    if (surface == &locked)
        present::unlock_display_surface();
}

int32_t font_baseline(const GadgetRenderer& renderer, const void* glyphs) {
    const Sprite* glyph = art_frame(renderer, glyphs, kReferenceGlyph);
    return glyph != nullptr ? glyph->height : 0;
}

present::TextFace font_face(const GadgetRenderer& renderer, const void* glyphs) {
    return font_baseline(renderer, glyphs) >= kMessageFaceHeight ? present::TextFace::message
                                                                 : present::TextFace::status;
}

present::FontCharacters font_characters(const GadgetRenderer& renderer, const void* glyphs) {
    auto& kept = kept_characters();
    const base::threads::LockGuard lock(kept.mutex);
    if (const auto found = kept.fonts.find(glyphs); found != kept.fonts.end())
        return found->second;
    const Sprite* box = art_frame(renderer, glyphs, 0);
    auto characters = present::FontCharacters::gui_font([&](uint8_t byte) {
        const Sprite* glyph = art_frame(renderer, glyphs, byte);
        return glyph != nullptr && glyph->width != 0 && !present::same_glyph(glyph, box);
    });
    return kept.fonts.emplace(glyphs, std::move(characters)).first->second;
}

std::vector<present::TextRun>
text_runs(const GadgetRenderer& renderer, const void* glyphs, std::string_view text) {
    if (!frontend_renderer::needs_text_runs(text, false))
        return {{false, std::string(text), present::game_font_text_size}};
    return frontend_renderer::split_game_text(text, font_characters(renderer, glyphs), false);
}

int32_t measure_with_font(const GadgetRenderer& renderer, const void* gaf_font, const char* text) {
    if (text == nullptr)
        return 0;
    if (gaf_font == nullptr)
        return present::measure_text_width(
            static_cast<const uint8_t*>(present::active_font()), text
        );
    const void* glyphs = renderer.art.first_sequence != nullptr
                             ? renderer.art.first_sequence(renderer.art.context, gaf_font)
                             : nullptr;
    int32_t width = 0;
    for (const auto& run : text_runs(renderer, glyphs, text)) {
        if (run.modern)
            if (const auto layers = present::modern_text(
                    run.text,
                    font_face(renderer, glyphs),
                    1,
                    frontend_renderer::screen_text_size(run)
                )) {
                width += layers->advance;
                continue;
            }
        const std::string bytes =
            run.modern ? present::encode_game_text(run.text, false) : run.text;
        for (const unsigned char byte : bytes)
            if (const Sprite* glyph = art_frame(renderer, glyphs, byte))
                width += glyph->width;
    }
    return width;
}

int32_t line_height_with_font(const GadgetRenderer& renderer, const void* gaf_font) {
    if (gaf_font == nullptr)
        return present::active_font_height();
    const void* glyphs = renderer.art.first_sequence != nullptr
                             ? renderer.art.first_sequence(renderer.art.context, gaf_font)
                             : nullptr;
    // A missing reference glyph counts as zero high.
    const Sprite* glyph = art_frame(renderer, glyphs, kReferenceGlyph);
    return (glyph != nullptr ? glyph->height : 0) + kLineGap;
}

void fnt_text(
    GadgetRenderer& renderer,
    Surface* target,
    const char* text,
    int32_t x,
    int32_t y,
    int32_t max_width
) {
    if (renderer.fnt_text != nullptr)
        renderer.fnt_text(
            renderer.text_context, target, text, x, y, max_width, renderer.text_color
        );
}

} // namespace detail

using namespace detail;

int32_t text_width(const GadgetRenderer& renderer, const GadgetPanel& panel, const char* text) {
    return measure_with_font(renderer, panel.active_gaf_font, text);
}

int32_t text_height(const GadgetRenderer& renderer, const GadgetPanel& panel) {
    return line_height_with_font(renderer, panel.active_gaf_font);
}

void draw_text(
    GadgetRenderer& renderer,
    const GadgetPanel& panel,
    Surface* target,
    const char* text,
    int32_t x,
    int32_t y,
    int32_t max_width,
    int32_t level
) {
    if (panel.active_gaf_font == nullptr) {
        fnt_text(renderer, target, text, x, y, kNoLimit);
        return;
    }
    const void* glyphs = active_glyphs(renderer, panel);
    const int32_t baseline = y + font_baseline(renderer, glyphs);
    for (const auto& run : text_runs(renderer, glyphs, text)) {
        std::optional<present::TextLayers> layers;
        if (run.modern)
            layers = present::modern_text(
                run.text, font_face(renderer, glyphs), 1, frontend_renderer::screen_text_size(run)
            );
        if (layers) {
            if (max_width != kNoLimit && max_width < layers->advance)
                return;
            const auto& hooks = present::game_text_hooks();
            if (target != nullptr && hooks.palette != nullptr) {
                const auto palette = hooks.palette(hooks.context);
                auto canvas = present::indexed_canvas(*target, palette);
                present::lay_text(
                    canvas,
                    *layers,
                    x,
                    baseline,
                    frontend_renderer::lit_text_color(present::gui_font_color, palette, level)
                );
            }
            if (max_width != kNoLimit)
                max_width -= layers->advance;
            x += layers->advance;
            continue;
        }
        const std::string bytes =
            run.modern ? present::encode_game_text(run.text, false) : run.text;
        for (const unsigned char byte : bytes) {
            if (byte < kFirstPrintable)
                continue;
            const Sprite* glyph = art_frame(renderer, glyphs, byte);
            if (glyph == nullptr)
                continue;
            if (max_width != kNoLimit && max_width < glyph->width)
                return;
            if (byte != ' ') {
                if (level == 0)
                    present::draw_sprite(target, glyph, x, y);
                else
                    present::draw_sprite_lit(target, glyph, x, y, level);
            }
            if (max_width != kNoLimit) {
                max_width -= glyph->width;
                if (max_width < 0)
                    return;
            }
            x += glyph->width;
        }
    }
}

int32_t draw_wrapped_text(
    GadgetRenderer& renderer,
    const GadgetPanel& panel,
    Surface* target,
    char* text,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    int32_t level
) {
    const size_t length = std::strlen(text);
    size_t last_break = 0;
    size_t start = 0;
    size_t scan = 0;
    char held = text[0];
    for (;;) {
        if (held == '\0')
            return y;
        while (scan != length && text[scan] != ' ' && text[scan] != '\r')
            ++scan;
        held = text[scan];
        text[scan] = '\0';
        char* end = text + scan;
        bool emit = false;
        if (width < text_width(renderer, panel, text + start)) {
            *end = held;
            end = text + last_break;
            held = *end;
            *end = '\0';
            scan = last_break;
            emit = true;
        } else if (held == '\r' || scan == length) {
            emit = true;
        } else {
            last_break = scan;
            *end = held;
            ++scan;
        }
        if (emit) {
            draw_text(renderer, panel, target, text + start, x, y, width, level);
            *end = held;
            const int32_t step = text_height(renderer, panel) + kLineGap;
            y += step;
            height -= step;
            if (*end == '\0')
                return y;
            last_break = scan + 1;
            start = last_break;
            scan = last_break;
            if (height < 1)
                return y;
        }
        held = text[start];
    }
}

namespace {

// A GUI GAF file named `name` in the panel's GAF directory; null when absent.
const void*
load_named_gaf(const GadgetRenderer& renderer, const GadgetPanel& panel, const char* name) {
    char name_path[data::defs::path_capacity]{};
    std::size_t directory_length = 0;
    while (directory_length + 1 < sizeof(name_path) && panel.gaf_path[directory_length] != '\0')
        ++directory_length;
    std::memcpy(name_path, panel.gaf_path.data(), directory_length);
    oa::base::text::append_terminated(name_path, name);
    char path[data::defs::path_capacity]{};
    data::defs::format_with_extension(name_path, path, sizeof(path), kGafExtension);
    return renderer.art.load_gaf != nullptr ? renderer.art.load_gaf(renderer.art.context, path)
                                            : nullptr;
}

} // namespace

void load_common_gaf(GadgetRenderer& renderer, GadgetPanel& panel, const char* name) {
    if (const void* file = load_named_gaf(renderer, panel, name))
        panel.list_skin = file;
}

void load_gui_font(GadgetRenderer& renderer, GadgetPanel& panel, const char* name, int32_t slot) {
    auto& fonts = panel.gaf_fonts;
    if (slot < 0 || static_cast<size_t>(slot) >= fonts.size())
        return;
    const void* file = load_named_gaf(renderer, panel, name);
    fonts[static_cast<size_t>(slot)] = file;
    if (file == nullptr)
        return;
    const void* glyphs = renderer.art.first_sequence != nullptr
                             ? renderer.art.first_sequence(renderer.art.context, file)
                             : nullptr;
    // Without the reference glyph nothing is subtracted.
    const Sprite* reference = art_frame(renderer, glyphs, kReferenceGlyph);
    const int16_t lift =
        reference != nullptr ? static_cast<int16_t>(reference->height) : int16_t{0};
    const int32_t count = art_frame_count(renderer, glyphs);
    for (int32_t index = 0; index < count; ++index) {
        if (Sprite* glyph = art_frame(renderer, glyphs, index))
            glyph->origin_y = static_cast<int16_t>(glyph->origin_y - lift);
    }
    panel.active_gaf_font = file;
}

void forget_font_characters() {
    auto& kept = kept_characters();
    const base::threads::LockGuard lock(kept.mutex);
    kept.fonts.clear();
}

} // namespace oa::ui::gadget_render

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Service singletons and their null-safe call wrappers.
#include "oa/boneyards/toolkit.hpp"
#include "oa/present/surface.hpp"

namespace oa::boneyards {
namespace {

Services g_services{};

} // namespace

Services services_install(const Services& services) {
    const Services previous = g_services;
    g_services = services;
    return previous;
}

const Services& services() {
    return g_services;
}

void image_release(Handle image) {
    ImageService* s = g_services.images;
    if (s != nullptr && s->release != nullptr) {
        s->release(s->self, image);
    }
}

void image_add_ref(Handle image) {
    ImageService* s = g_services.images;
    if (s != nullptr && s->add_ref != nullptr) {
        s->add_ref(s->self, image);
    }
}

Handle image_load(const char* file, const char* sequence, int32_t frame, int32_t key) {
    ImageService* s = g_services.images;
    if (s == nullptr || s->load == nullptr) {
        return k_no_handle;
    }
    return s->load(s->self, file, sequence, frame, key);
}

void image_size(Handle image, int32_t* width, int32_t* height) {
    ImageService* s = g_services.images;
    if (s != nullptr && s->size != nullptr) {
        s->size(s->self, image, width, height);
    }
}

Handle image_create_blank(int32_t width, int32_t height, uint8_t fill, int32_t key) {
    ImageService* s = g_services.images;
    if (s == nullptr || s->create_blank == nullptr) {
        return k_no_handle;
    }
    return s->create_blank(s->self, width, height, fill, key);
}

bool image_surface(Handle image, Surface* surface) {
    ImageService* s = g_services.images;
    Sprite* sprite = s != nullptr && s->sprite != nullptr ? s->sprite(s->self, image) : nullptr;
    if (sprite == nullptr || sprite->data == nullptr || sprite->encoding != OA_SPRITE_RAW) {
        return false;
    }
    present::surface_from_sprite(*surface, *sprite);
    return true;
}

void image_draw(int32_t x, int32_t y, int32_t width, int32_t height, Handle image) {
    ImageService* s = g_services.images;
    if (s != nullptr && s->draw != nullptr) {
        s->draw(s->self, x, y, width, height, image);
    }
}

Handle text_create(const char* text, int32_t style) {
    TextService* s = g_services.texts;
    if (s == nullptr || s->create == nullptr) {
        return k_no_handle;
    }
    return s->create(s->self, text, style);
}

void text_release(Handle text) {
    TextService* s = g_services.texts;
    if (s != nullptr && s->release != nullptr) {
        s->release(s->self, text);
    }
}

const char* text_get(Handle text) {
    TextService* s = g_services.texts;
    if (s == nullptr || s->get == nullptr) {
        return "";
    }
    const char* value = s->get(s->self, text);
    return value != nullptr ? value : "";
}

void text_draw(Handle font, int32_t x, int32_t y, int32_t width, int32_t height, Handle text) {
    TextService* s = g_services.texts;
    if (s != nullptr && s->draw != nullptr) {
        s->draw(s->self, font, x, y, width, height, text);
    }
}

void text_draw_string(
    Handle font,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    const char* text,
    int32_t align
) {
    TextService* s = g_services.texts;
    if (s != nullptr && s->draw_string != nullptr) {
        s->draw_string(s->self, font, x, y, width, height, text, align);
    }
}

int32_t text_prefix_width(Handle font, const char* text, int32_t last) {
    TextService* s = g_services.texts;
    if (s == nullptr || s->prefix_width == nullptr) {
        return 0;
    }
    return s->prefix_width(s->self, font, text, last);
}

void text_measure_string(Handle font, const char* text, int32_t* width, int32_t* height) {
    TextService* s = g_services.texts;
    if (s != nullptr && s->measure_string != nullptr) {
        s->measure_string(s->self, font, width, height, text);
    }
}

int32_t text_char_width(Handle font, int32_t ch) {
    TextService* s = g_services.texts;
    if (s == nullptr || s->char_width == nullptr) {
        return 0;
    }
    return s->char_width(s->self, font, ch);
}

Handle font_load(const char* name) {
    FontService* s = g_services.fonts;
    if (s == nullptr || s->load == nullptr) {
        return k_no_handle;
    }
    return s->load(s->self, name);
}

int32_t font_line_height(Handle font) {
    FontService* s = g_services.fonts;
    if (s == nullptr || s->line_height == nullptr) {
        return 0;
    }
    return s->line_height(s->self, font);
}

void font_release(Handle font) {
    FontService* s = g_services.fonts;
    if (s != nullptr && s->release != nullptr) {
        s->release(s->self, font);
    }
}

void font_add_ref(Handle font) {
    FontService* s = g_services.fonts;
    if (s != nullptr && s->add_ref != nullptr) {
        s->add_ref(s->self, font);
    }
}

bool input_key_down(int32_t key) {
    InputService* s = g_services.input;
    if (s == nullptr || s->is_key_down == nullptr) {
        return false;
    }
    return s->is_key_down(s->self, key);
}

Handle color_create(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    ColorService* s = g_services.colors;
    if (s == nullptr || s->create == nullptr) {
        return k_no_handle;
    }
    return s->create(s->self, r, g, b, a);
}

void color_release(Handle color) {
    ColorService* s = g_services.colors;
    if (s != nullptr && s->release != nullptr) {
        s->release(s->self, color);
    }
}

void color_fill(int32_t x, int32_t y, int32_t width, int32_t height, Handle color) {
    ColorService* s = g_services.colors;
    if (s != nullptr && s->fill != nullptr) {
        s->fill(s->self, x, y, width, height, color);
    }
}

int32_t cursor_load(const char* file, const char* sequence) {
    CursorService* s = g_services.cursors;
    if (s == nullptr || s->load == nullptr) {
        return -1;
    }
    return s->load(s->self, file, sequence);
}

void cursor_show() {
    CursorService* s = g_services.cursors;
    if (s != nullptr && s->show != nullptr) {
        s->show(s->self);
    }
}

void cursor_hide() {
    CursorService* s = g_services.cursors;
    if (s != nullptr && s->hide != nullptr) {
        s->hide(s->self);
    }
}

void cursor_set(int32_t cursor) {
    CursorService* s = g_services.cursors;
    if (s != nullptr && s->set != nullptr) {
        s->set(s->self, cursor);
    }
}

void cursor_set_default(int32_t cursor) {
    CursorService* s = g_services.cursors;
    if (s != nullptr && s->set_default != nullptr) {
        s->set_default(s->self, cursor);
    }
}

void sound_release(Handle sound) {
    SoundService* s = g_services.sounds;
    if (s != nullptr && s->release != nullptr) {
        s->release(s->self, sound);
    }
}

void sound_play(Handle sound) {
    SoundService* s = g_services.sounds;
    if (s != nullptr && s->play != nullptr) {
        s->play(s->self, sound);
    }
}

void sound_beep() {
    SoundService* s = g_services.sounds;
    if (s != nullptr && s->beep != nullptr) {
        s->beep(s->self);
    }
}

void draw_focus_frame(int32_t x, int32_t y, int32_t width, int32_t height) {
    DrawService* s = g_services.draw;
    if (s != nullptr && s->focus_frame != nullptr) {
        s->focus_frame(s->self, x, y, width, height);
    }
}

void draw_frame(int32_t x, int32_t y, int32_t width, int32_t height) {
    DrawService* s = g_services.draw;
    if (s != nullptr && s->frame != nullptr) {
        s->frame(s->self, x, y, width, height);
    }
}

void draw_caret(int32_t x, int32_t y, int32_t visible, int32_t height) {
    DrawService* s = g_services.draw;
    if (s != nullptr && s->caret != nullptr) {
        s->caret(s->self, x, y, visible, height);
    }
}

void draw_service_clear_flag(DrawService* draw) {
    draw->box_close_flag = 0;
}

void draw_service_set_flag(DrawService* draw) {
    draw->box_close_flag = 1;
}

void services_clear_draw_flag() {
    if (g_services.draw != nullptr) {
        draw_service_clear_flag(g_services.draw);
    }
}

void services_set_draw_flag() {
    if (g_services.draw != nullptr) {
        draw_service_set_flag(g_services.draw);
    }
}

} // namespace oa::boneyards

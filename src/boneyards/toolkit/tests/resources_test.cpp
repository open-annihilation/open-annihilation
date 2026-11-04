// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Default services over the installed Boneyards data (--data): fonts, images,
// text, colours; and the file-name matching, which needs no data.
#include "test_data.hpp"

using namespace oa::boneyards;
using namespace oa::boneyards::testing;

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

int g_plays = 0;
int32_t g_last_volume = 0;
char g_last_sound[64];

void record_play(void*, const char* path, int32_t volume) {
    ++g_plays;
    g_last_volume = volume;
    std::snprintf(g_last_sound, sizeof(g_last_sound), "%s", path);
}

// Start-up gives the desktop the whole screen, makes the "UI MAIN" overlay
// that size and cleared, and the default font becomes the
// current font (line height -1 resolves to it).
void test_start_up(Toolkit* tk) {
    const Desktop* desktop = toolkit_desktop(tk);
    check(
        desktop->x == 0 && desktop->y == 0 && desktop->width == 160 && desktop->height == 120,
        "desktop covers the screen"
    );
    const Surface* overlay = toolkit_surface(tk);
    check(
        overlay->width == 160 && overlay->height == 120 && overlay->pitch == 160 &&
            overlay->clip.x2 == 159 && overlay->clip.y2 == 119,
        "overlay sized to the screen"
    );
    check(opaque_pixels(overlay) == 0, "overlay starts cleared");
    const Services s = toolkit_services(tk);
    check(
        s.fonts->line_height(s.fonts->self, -1) ==
            s.fonts->line_height(s.fonts->self, toolkit_default_font(tk)),
        "default font is current"
    );
}

/// Checks the focus frame, the dock outline and the caret, then that the manager clears and
/// sets the draw object's box_close_flag.
///
/// @param tk the toolkit whose services are checked
void test_draw_service(Toolkit* tk) {
    const Services s = toolkit_services(tk);
    check(s.draw->focus_frame == s.draw->frame, "focus frame and frame share the outline");
    check(
        s.draw->caret != nullptr && s.draw->caret != s.draw->focus_frame,
        "the caret is its own routine"
    );
    const Surface* overlay = toolkit_surface(tk);
    draw_focus_frame(10, 10, 40, 20);
    check(overlay->pixels[10 * overlay->pitch + 10] != k_overlay_transparent, "focus frame corner");
    check(
        overlay->pixels[11 * overlay->pitch + 11] == k_overlay_transparent,
        "focus frame interior stays clear"
    );
    check(
        overlay->pixels[29 * overlay->pitch + 49] != k_overlay_transparent,
        "focus frame opposite corner"
    );
    draw_caret(20, 12, 0, 11);
    check(
        overlay->pixels[12 * overlay->pitch + 20] == k_overlay_transparent,
        "a hidden caret draws nothing"
    );
    draw_caret(20, 12, 1, 11);
    check(
        overlay->pixels[12 * overlay->pitch + 20] != k_overlay_transparent &&
            overlay->pixels[12 * overlay->pitch + 21] != k_overlay_transparent &&
            overlay->pixels[12 * overlay->pitch + 22] == k_overlay_transparent,
        "caret is a two-pixel bar"
    );
    toolkit_compose(tk, nullptr);
    check(opaque_pixels(overlay) == 0, "compose clears the frame and the caret");

    // The constructor sets the draw object's flag byte; the manager clears it
    // and sets it again through the draw object's two flag routines.
    check(s.draw->box_close_flag == 1, "draw object starts flagged");
    services_clear_draw_flag();
    check(s.draw->box_close_flag == 0, "manager clears the draw flag");
    services_set_draw_flag();
    check(s.draw->box_close_flag == 1, "manager sets the draw flag");
}

void test_fonts(Toolkit* tk) {
    const Services s = toolkit_services(tk);
    const Handle font = toolkit_default_font(tk);
    check(font == 0, "default font loads into slot 0");
    check(s.fonts->line_height(s.fonts->self, font) == 9, "line height is glyph I's height");
    check(s.fonts->line_height(s.fonts->self, -1) == 9, "-1 selects the current font");
    check(s.fonts->load(s.fonts->self, "byhatfontsmall87.gaf") == font, "reload shares the slot");
    s.fonts->release(s.fonts->self, font);
    check(text_char_width(font, 'H') == 5, "H advance");
    int32_t w = 0;
    int32_t h = 0;
    text_measure_string(font, "Hello", &w, &h);
    check(w == 19 && h == 9, "Hello measures 19x9");
    check(text_prefix_width(font, "Hello", 1) == 10, "prefix width is inclusive");
    check(s.fonts->load(s.fonts->self, "Missing.gaf") == k_no_handle, "missing font fails");

    Surface* overlay = toolkit_surface(tk);
    text_draw_string(font, 10, 10, 100, 20, "Hello", text_style::left | text_style::top);
    const int32_t opaque = opaque_pixels(overlay);
    check(opaque > 20, "text draws glyph pixels");
    int32_t min_x = overlay->width;
    int32_t max_x = 0;
    int32_t min_y = overlay->height;
    for (int32_t y = 0; y < overlay->height; ++y) {
        for (int32_t x = 0; x < overlay->width; ++x) {
            if (overlay->pixels[y * overlay->pitch + x] != k_overlay_transparent) {
                min_x = x < min_x ? x : min_x;
                max_x = x > max_x ? x : max_x;
                min_y = y < min_y ? y : min_y;
            }
        }
    }
    check(min_x >= 10 && max_x < 10 + 19 + 1 && min_y == 10, "left/top text box placement");
    toolkit_compose(tk, nullptr);
    check(opaque_pixels(overlay) == 0, "compose clears the overlay");

    text_draw_string(font, 0, 0, 100, 20, "Hello", 0);
    int32_t centred_min_x = overlay->width;
    for (int32_t y = 0; y < overlay->height; ++y) {
        for (int32_t x = 0; x < overlay->width; ++x) {
            if (overlay->pixels[y * overlay->pitch + x] != k_overlay_transparent &&
                x < centred_min_x) {
                centred_min_x = x;
            }
        }
    }
    check(centred_min_x >= (100 - 19) / 2, "centred text starts near the middle");
    toolkit_compose(tk, nullptr);

    const Handle tinted = toolkit_font_tint(tk, font, 200);
    check(tinted > font, "tinted font gets its own slot");
    text_draw_string(tinted, 0, 0, 100, 20, "H", text_style::left | text_style::top);
    bool only_tint = opaque_pixels(overlay) > 0;
    for (int32_t i = 0; i < overlay->width * overlay->height; ++i) {
        if (overlay->pixels[i] != k_overlay_transparent && overlay->pixels[i] != 200) {
            only_tint = false;
        }
    }
    check(only_tint, "tinted glyphs use the tint colour only");
    int32_t tint_w = 0;
    int32_t tint_h = 0;
    toolkit_measure_text(tk, tinted, "Hello", &tint_w, &tint_h);
    check(tint_w == 19 && tint_h == 9, "tinted glyphs keep the frame sizes");

    // Selecting takes only a loaded slot below 15; releasing the current
    // font leaves none current (line height 0 for -1).
    toolkit_select_font(tk, tinted);
    s.fonts->release(s.fonts->self, tinted);
    check(s.fonts->line_height(s.fonts->self, -1) == 0, "released current font is dropped");
    toolkit_select_font(tk, tinted);
    check(s.fonts->line_height(s.fonts->self, -1) == 0, "a free slot is not selected");
    toolkit_select_font(tk, 15);
    toolkit_select_font(tk, -1);
    check(s.fonts->line_height(s.fonts->self, -1) == 0, "-1 and 15 are refused");
    toolkit_select_font(tk, font);
    check(s.fonts->line_height(s.fonts->self, -1) == 9, "the default font is current again");
    toolkit_compose(tk, nullptr);
}

void test_wrapping(Toolkit* tk) {
    const Handle font = toolkit_default_font(tk);
    Surface* overlay = toolkit_surface(tk);
    const int32_t style = text_style::left | text_style::top | text_style::wrap;
    text_draw_string(font, 0, 0, 30, 60, "Hello Hello Hello", style);
    int32_t max_y = 0;
    for (int32_t y = 0; y < overlay->height; ++y) {
        for (int32_t x = 0; x < overlay->width; ++x) {
            if (overlay->pixels[y * overlay->pitch + x] != k_overlay_transparent) {
                max_y = y;
                break;
            }
        }
    }
    check(max_y >= 18 && max_y < 27, "three wrapped lines of 9 pixels");
    toolkit_compose(tk, nullptr);
}

void test_images(Toolkit* tk) {
    const Services s = toolkit_services(tk);
    ImageService* images = s.images;
    check(
        images->frame_count(images->self, "Widgets.gaf", "", 0) == 4, "first sequence frame count"
    );
    check(
        images->frame_count(images->self, "Widgets.gaf", "Nope", 0) == 0,
        "unknown sequence has no frames"
    );
    check(
        images->frame_count(images->self, "Widgets.gaf", "", 1000) == 0, "index past the sequences"
    );
    check(
        images->frame_count(images->self, "Widgets.gaf", "", -1) == 0, "negative index selects none"
    );
    check(images->frame_count(images->self, "WIDGETS.GAF", "", 0) == 4, "extension ignores case");
    check(images->frame_count(images->self, "Widgets", "", 0) == 0, "no extension, no frames");
    const Handle box = image_load("Widgets.gaf", "Check_Box", 0, 9);
    check(box >= 0, "check box frame loads");
    int32_t w = 0;
    int32_t h = 0;
    image_size(box, &w, &h);
    check(w == 9 && h == 9, "check box is 9x9");
    check(image_load("Widgets.gaf", "check_box", 0, 9) == box, "lookup is case-insensitive");
    check(image_load("Widgets.gaf", "Check_Box", 1, 9) != box, "other frame, other handle");
    image_draw(5, 5, 0, 0, box);
    const Surface* overlay = toolkit_surface(tk);
    check(opaque_pixels(overlay) > 0, "image draws");
    check(overlay->pixels[4 * overlay->pitch + 4] == k_overlay_transparent, "drawn at origin");
    toolkit_compose(tk, nullptr);
    const Handle failed = image_load("Widgets.gaf", "Nope", 0, 9);
    check(failed >= 0, "entry kept even when the frame fails");
    w = -5;
    h = -6;
    image_size(failed, &w, &h);
    check(w == -5 && h == -6, "no frame, size left alone");

    // Entries start with two references; the destroying table first releases
    // those holding exactly one.
    const Handle once = image_load("Widgets.gaf", "Check_Box", 2, 9);
    image_size(once, &w, &h);
    check(w == 9 && h == 9, "third check box frame loads");
    image_release(once);
    toolkit_release_images_held_once(tk);
    w = -5;
    h = -6;
    image_size(once, &w, &h);
    check(w == -5 && h == -6, "entry held once is released");
    image_size(box, &w, &h);
    check(w == 9 && h == 9, "entry held twice survives");
}

uint8_t at(const Surface* surface, int32_t x, int32_t y) {
    return surface->pixels[y * surface->pitch + x];
}

// A button press plays its press sound, and Left at the start of an edit
// box beeps. A focused button wears a one-pixel white frame; a focused edit
// box wears that frame and a two-pixel caret. The palette is synthetic:
// index 1 is white and index 2 is red, and neither is the transparent index.
void test_sounds_focus_and_caret() {
    Palette palette{};
    palette.entries[1] = PaletteEntry{255, 255, 255, 0};
    palette.entries[2] = PaletteEntry{255, 0, 0, 0};
    ToolkitConfig config{};
    config.width = 160;
    config.height = 120;
    config.palette = &palette;
    config.audio = ToolkitAudio{nullptr, record_play};
    Toolkit* tk = toolkit_create(&config);
    const Services previous = services_install(toolkit_services(tk));
    const Surface* overlay = toolkit_surface(tk);

    Window* button = button_create();
    window_create(button, "ok", 10, 10, 40, 20, nullptr, true);
    window_set_color(button, 255, 0, 0, 255);
    window_send(button, MessageCode::draw);
    check(
        at(overlay, 10, 10) == 2 && at(overlay, 9, 10) == k_overlay_transparent,
        "an unfocused button has no focus frame"
    );
    desktop_set_focus(toolkit_desktop(tk), button);
    window_send(button, MessageCode::draw);
    check(
        at(overlay, 10, 10) == 1 && at(overlay, 49, 10) == 1 && at(overlay, 10, 29) == 1 &&
            at(overlay, 49, 29) == 1,
        "focus frame around the focused button"
    );
    check(
        at(overlay, 11, 11) == 2 && at(overlay, 9, 10) == k_overlay_transparent,
        "the frame is one pixel and stays on the button"
    );

    g_plays = 0;
    const Services services = toolkit_services(tk);
    const Handle press = services.sounds->load(services.sounds->self, "HOVEROK1.WAV");
    window_set_state_sound(button, button_state::pressed, press);
    Message down{};
    down.code = MessageCode::left_down;
    down.x = 15;
    down.y = 15;
    down.buttons = button_left;
    desktop_route(toolkit_desktop(tk), &down);
    check(
        g_plays == 1 && std::strcmp(g_last_sound, "sounds\\HOVEROK1.WAV") == 0 &&
            g_last_volume == -585,
        "a button press plays its sound"
    );

    EditBox* edit = edit_box_create();
    window_create(&edit->base, "name", 8, 50, 40, 16, nullptr, true);
    window_set_color(&edit->base, 255, 0, 0, 255);
    desktop_set_focus(toolkit_desktop(tk), &edit->base);
    window_send(&edit->base, MessageCode::draw);
    check(edit->caret == 0 && edit->caret_height == 2, "empty edit caret is two pixels tall");
    check(
        at(overlay, 8, 50) == 1 && at(overlay, 47, 65) == 1 && at(overlay, 12, 51) == 2,
        "focus frame around the edit box"
    );
    check(
        at(overlay, 9, 57) == 1 && at(overlay, 10, 57) == 1 && at(overlay, 9, 58) == 1 &&
            at(overlay, 10, 58) == 1 && at(overlay, 11, 57) == 2,
        "caret in the edit box"
    );
    check(at(overlay, 7, 50) == k_overlay_transparent, "edit frame stays inside the box");

    g_plays = 0;
    Message left{};
    left.code = MessageCode::key_down;
    left.key = key::left;
    window_dispatch(&edit->base, &left);
    check(
        g_plays == 1 && std::strcmp(g_last_sound, "sounds\\beep2.wav") == 0 &&
            g_last_volume == -585,
        "an invalid key beeps"
    );

    toolkit_compose(tk, nullptr);
    draw_caret(4, 4, 0, 8);
    draw_focus_frame(0, 0, 0, 10);
    check(opaque_pixels(overlay) == 0, "a hidden caret and an empty frame draw nothing");

    window_destroy(&edit->base);
    window_destroy(button);
    services_install(previous);
    toolkit_destroy(tk);
}

// name_has_extension: the three characters after the first '.', ignoring
// case; the scan gives up at the end of the name or a line feed.
void test_name_extension() {
    check(name_has_extension("anims\\cursors.gaf", "gaf"), "gaf extension");
    check(name_has_extension("CURSORS.GAF", "gaf"), "case ignored");
    check(name_has_extension("font.fntx", "fnt"), "only three characters compared");
    check(!name_has_extension("a.b.gaf", "gaf"), "first dot decides");
    check(!name_has_extension("x.ga", "gaf"), "short extension");
    check(!name_has_extension("nodot", "gaf"), "no dot");
    check(!name_has_extension("x\n.gaf", "gaf"), "line feed ends the scan");
    check(!name_has_extension(nullptr, "gaf") && !name_has_extension("x.gaf", nullptr), "null");
}

void test_colors_and_sounds(Toolkit* tk) {
    const Handle white = color_create(255, 255, 255, 255);
    check(white >= 0 && white < 256, "colour is a palette index");
    color_fill(0, 0, 4, 4, white);
    Surface* overlay = toolkit_surface(tk);
    check(overlay->pixels[3 * overlay->pitch + 3] == white, "fill writes the index");
    check(overlay->pixels[4 * overlay->pitch + 4] == k_overlay_transparent, "fill is bounded");
    uint8_t target_pixels[16 * 16];
    std::memset(target_pixels, 0, sizeof(target_pixels));
    Surface target{};
    target.width = 16;
    target.height = 16;
    target.pitch = 16;
    target.pixels = target_pixels;
    toolkit_compose(tk, &target);
    check(target_pixels[0] == white && target_pixels[5 * 16 + 5] == 0, "compose is keyed");

    const Services s = toolkit_services(tk);
    const Handle beep = s.sounds->load(s.sounds->self, "HOVEROK1.WAV");
    check(beep >= 0, "sound entry");
    sound_play(beep);
    check(
        g_plays == 1 && std::strcmp(g_last_sound, "sounds\\HOVEROK1.WAV") == 0,
        "sound plays from the sounds directory"
    );
    check(g_last_volume == -585, "UI sound volume");
    sound_release(beep);
    sound_play(beep);
    check(g_plays == 1, "released sound no longer plays");
}

// Image manager stand-in: three frames per sequence, images 100 + frame,
// image 101 held one extra tick.
struct FakeImages {
    int32_t count_calls = 0;
    char count_sequence[32]{};
    int32_t count_index = -1;
    int32_t load_keys = 0;
    int32_t draws = 0;
    Handle drawn = k_no_handle;
    int32_t draw_x = 0;
    int32_t draw_y = 0;
};

FakeImages g_fake;

int32_t fake_frame_count(void*, const char*, const char* sequence, int32_t index) {
    ++g_fake.count_calls;
    std::snprintf(g_fake.count_sequence, sizeof(g_fake.count_sequence), "%s", sequence);
    g_fake.count_index = index;
    return 3;
}

Handle fake_load(void*, const char*, const char*, int32_t frame, int32_t key) {
    g_fake.load_keys += key;
    return 100 + frame;
}

void fake_draw(void*, int32_t x, int32_t y, int32_t, int32_t, Handle image) {
    ++g_fake.draws;
    g_fake.drawn = image;
    g_fake.draw_x = x;
    g_fake.draw_y = y;
}

int32_t fake_delay(void*, Handle image) {
    return image == 101 ? 1 : 0;
}

constexpr Handle k_not_drawn = -99;

Handle drawn_cursor(Toolkit* tk) {
    g_fake.drawn = k_not_drawn;
    toolkit_draw_cursor(tk, 10, 20);
    return g_fake.drawn;
}

// The cursor manager the toolkit builds: loading takes one image per frame
// (key 9) or references a loaded cursor, set_default and set pick the default
// and shown cursors, a step counts the frame delay down and drawing happens
// only while show has shown it.
void test_cursor(Toolkit* tk) {
    check(toolkit_cursor(tk) == -1, "no cursor shown at start");
    ImageService fake{};
    fake.frame_count = fake_frame_count;
    fake.load = fake_load;
    fake.draw = fake_draw;
    fake.frame_delay = fake_delay;
    toolkit_set_cursor_images(tk, &fake);
    check(drawn_cursor(tk) == k_not_drawn, "nothing drawn before a cursor is chosen");

    const int32_t normal = cursor_load("cursors.gaf", "cursornormal");
    check(normal == 0, "first cursor takes slot 0");
    check(
        g_fake.count_calls == 1 && std::strcmp(g_fake.count_sequence, "cursornormal") == 0 &&
            g_fake.count_index == 0,
        "frames counted by sequence name"
    );
    check(g_fake.load_keys == 27, "three frames load with key 9");
    check(
        cursor_load("CURSORS.GAF", "CursorNormal") == normal && g_fake.count_calls == 1,
        "same names reference the loaded cursor"
    );
    check(cursor_load(nullptr, "cursornormal") == -1, "a missing name loads nothing");

    cursor_set(5);
    check(toolkit_cursor(tk) == -1, "an unloaded slot is not shown");
    cursor_set_default(32);
    cursor_set_default(-2);
    check(drawn_cursor(tk) == k_not_drawn, "out-of-range defaults are ignored");
    cursor_set_default(normal);
    check(
        drawn_cursor(tk) == 100 && g_fake.draw_x == 10 && g_fake.draw_y == 20,
        "the default cursor draws its first frame at the pointer"
    );
    cursor_set(normal);
    check(toolkit_cursor(tk) == normal, "a loaded slot is shown");

    cursor_hide();
    check(drawn_cursor(tk) == k_not_drawn, "a hidden cursor is not drawn");
    cursor_show();

    toolkit_step_cursor(tk);
    check(drawn_cursor(tk) == 101, "the countdown starts spent: first step advances");
    toolkit_step_cursor(tk);
    check(drawn_cursor(tk) == 101, "image 101 holds one extra step");
    toolkit_step_cursor(tk);
    check(drawn_cursor(tk) == 102, "then the next frame");
    toolkit_step_cursor(tk);
    check(drawn_cursor(tk) == 100, "the last frame wraps to the first");

    toolkit_set_cursor_images(tk, nullptr);
    const int32_t unlinked = cursor_load("other.gaf", "seq");
    check(unlinked == 1 && g_fake.count_calls == 1, "no image manager: no frames counted");
    cursor_set(unlinked);
    toolkit_set_cursor_images(tk, &fake);
    check(drawn_cursor(tk) == k_no_handle, "the unlinked cursor holds the image -1");
    cursor_set(-1);
    check(toolkit_cursor(tk) == -1 && drawn_cursor(tk) == 100, "-1 returns to the default");
    const Services s = toolkit_services(tk);
    toolkit_set_cursor_images(tk, s.images);
}

} // namespace

int main(int argc, char** argv) {
    // Only the name matching is self-contained; --data runs the services
    // over the BY1117a install's by.ccx.
    if (!oa::test::game_data_requested(argc, argv)) {
        test_name_extension();
        test_sounds_focus_and_caret();
        if (g_failures != 0) {
            std::fprintf(stderr, "%d failure(s)\n", g_failures);
            return 1;
        }
        std::puts("boneyards name matching tests passed");
        return 0;
    }
    InstalledData data;
    open_installed_data(&data);
    ToolkitConfig config = installed_config(&data, 160, 120);
    config.audio = ToolkitAudio{nullptr, record_play};
    Toolkit* tk = toolkit_create(&config);
    const Services previous = services_install(toolkit_services(tk));

    test_start_up(tk);
    test_draw_service(tk);
    test_fonts(tk);
    test_wrapping(tk);
    test_images(tk);
    test_colors_and_sounds(tk);
    test_cursor(tk);

    services_install(previous);
    toolkit_destroy(tk);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("boneyards resource tests passed");
    return 0;
}

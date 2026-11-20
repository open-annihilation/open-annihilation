// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Builds a console screen from a GUI gadget file.
#include "oa/boneyards/console/gui_screen.hpp"

#include "widgets.hpp"

#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/shown_text.hpp"
#include "oa/boneyards/screens/lobby_widgets.hpp"
#include "oa/present/raster.hpp"
#include "oa/present/surface.hpp"
#include "oa/ui/gui_layout.hpp"

#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <span>
#include <system_error>
#include <variant>
#include <vector>

namespace oa::boneyards::console {

namespace gui = oa::ui::gui_layout;
namespace screens = oa::boneyards::screens;

struct GuiScreen {
    Window base;
    GuiControl* controls;
    int32_t count;
    int32_t capacity;
    char fired_name[16];
    char fired_action[128];
    char enter_name[16];
    char escape_name[16];
    void* user;
    void (*on_fired)(void* user, Window* source);
    void (*on_destroy)(void* user);
};

namespace {

constexpr int32_t k_font_capacity = 16;
constexpr int32_t k_font_name_bytes = 32;
constexpr int32_t k_picture_states = 4;
constexpr const char* k_default_font = "anims\\Hattfont12.gaf";
constexpr const char* k_qr_file = "BY_New.gaf";
constexpr const char* k_qr_sequence = "QR_Register";
constexpr const char* k_blank_button = "BlankButton";
constexpr const char* k_radio_button = "Radio_Button";
constexpr const char* k_check_box = "Check_Box";
constexpr const char* k_function_large = "Function_Button_Large";
constexpr const char* k_function_small = "Function_Button_Small";
// The caps of a function button stay this wide when the face is fitted.
constexpr int32_t k_function_cap = 12;
// Palette colour of the console's green field.
constexpr uint8_t k_console_green = 42;
// The normal state window_init leaves current: the second of the two states.
constexpr int32_t k_state_shown = 1;

// A radio from a GUI file. The lobby radio is first so grouping and release
// treat it as one; caption_gap is extra space before the caption.
struct GuiRadio {
    screens::LobbyRadio radio;
    int32_t caption_gap;
};

const WindowOps* screen_ops();

struct Place {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
};

struct Picture {
    char file[48];
    char sequence[32];
    int32_t frame;
    bool present;
};

void (*g_list_notify)(Window*, const Notify*) = nullptr;

void set_error(GuiLoadError* error, const char* message) {
    if (error == nullptr) {
        return;
    }
    copy_text(error->message, sizeof error->message, message != nullptr ? message : "");
}

void drop_fill(Window* window) {
    if (window->color >= 0) {
        color_release(window->color);
    }
    window->color = k_no_handle;
}

int32_t text_style_of(int32_t attribs) {
    constexpr int32_t mask = text_style::left | text_style::right | text_style::top |
                             text_style::bottom | text_style::wrap;
    return attribs & mask;
}

const char* font_name(int8_t font_number, const char names[][k_font_name_bytes], int32_t count) {
    if (font_number >= 1 && font_number <= count) {
        return names[font_number - 1];
    }
    return k_default_font;
}

void use_font(
    Window* window, int8_t font_number, const char names[][k_font_name_bytes], int32_t count
) {
    window_set_font(window, font_name(font_number, names, count));
}

// A colorf of 1..255 tints the label into that palette index. Zero keeps the
// font the file named. No font service leaves the font as it is.
void tint_caption(Window* window, uint16_t colorf) {
    if (window == nullptr || colorf == 0 || colorf > 255 || window->font < 0)
        return;
    const FontService* fonts = services().fonts;
    if (fonts == nullptr || fonts->self == nullptr)
        return;
    const Handle tinted = toolkit_font_tint(
        static_cast<Toolkit*>(fonts->self), window->font, static_cast<uint8_t>(colorf)
    );
    if (tinted < 0)
        return;
    window_set_font_handle(window, tinted);
    font_release(tinted);
}

void store_caption(GuiControl* control, const char* text) {
    copy_text(control->caption, sizeof control->caption, text);
    convert_escaped_newlines(control->caption);
}

void split_help(
    const char* help, char* left, std::size_t left_n, char* right, std::size_t right_n
) {
    const char* bar = help != nullptr ? std::strchr(help, '|') : nullptr;
    if (bar == nullptr) {
        copy_text(left, left_n, help);
        if (right_n > 0) {
            right[0] = '\0';
        }
        return;
    }
    std::size_t count = static_cast<std::size_t>(bar - help);
    if (count >= left_n) {
        count = left_n - 1;
    }
    if (count > 0) {
        std::memcpy(left, help, count);
    }
    left[count] = '\0';
    copy_text(right, right_n, bar + 1);
}

/// Copies one word of at most `width` characters, skipping leading whitespace.
///
/// @param text text to read
/// @param[out] out buffer of at least `width + 1` bytes
/// @param width most characters copied
/// @return the text after the word, or null when there is no word
const char* scan_word(const char* text, char* out, std::size_t width) {
    if (text == nullptr)
        return nullptr;
    while (*text != '\0' && std::isspace(static_cast<unsigned char>(*text)))
        ++text;
    if (*text == '\0')
        return nullptr;
    std::size_t count = 0;
    while (*text != '\0' && !std::isspace(static_cast<unsigned char>(*text)) && count < width) {
        out[count++] = *text++;
    }
    out[count] = '\0';
    return text;
}

/// Reads one decimal integer, skipping leading whitespace and an optional sign.
///
/// @param text text to read
/// @param[out] out the integer
/// @return the text after the integer, or null when it is not an integer
const char* scan_int(const char* text, int* out) {
    if (text == nullptr)
        return nullptr;
    while (*text != '\0' && std::isspace(static_cast<unsigned char>(*text)))
        ++text;
    if (*text == '\0')
        return nullptr;
    const char* digits = text;
    if (*text == '+' || *text == '-')
        ++digits;
    if (*digits == '\0' || !std::isdigit(static_cast<unsigned char>(*digits)))
        return nullptr;
    const char* end = digits;
    while (std::isdigit(static_cast<unsigned char>(*end)))
        ++end;
    int value = 0;
    const char* parsed_from = *text == '+' ? digits : text;
    const auto parsed = std::from_chars(parsed_from, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end)
        return nullptr;
    *out = value;
    return end;
}

bool parse_picture(const char* text, Picture* picture) {
    picture->file[0] = '\0';
    picture->sequence[0] = '\0';
    picture->frame = 0;
    picture->present = false;
    if (text == nullptr || text[0] == '\0') {
        return false;
    }
    char file[48]{};
    char sequence[32]{};
    int frame = 0;
    const char* after_file = scan_word(text, file, sizeof file - 1);
    const char* after_sequence =
        after_file != nullptr ? scan_word(after_file, sequence, sizeof sequence - 1) : nullptr;
    if (after_sequence == nullptr || scan_int(after_sequence, &frame) == nullptr) {
        return false;
    }
    copy_text(picture->file, sizeof picture->file, file);
    copy_text(picture->sequence, sizeof picture->sequence, sequence);
    picture->frame = frame;
    picture->present = true;
    return true;
}

void show_frame(Window* window, const char* file, const char* sequence, int32_t frame) {
    const Handle image = image_load(file, sequence, frame, k_overlay_transparent);
    if (image < 0) {
        return;
    }
    window_set_state_image(window, k_state_shown, image);
    image_release(image);
}

bool function_button_picture(const Picture& picture) {
    return picture.present && (std::strcmp(picture.sequence, k_function_large) == 0 ||
                               std::strcmp(picture.sequence, k_function_small) == 0);
}

// "line N" is a solid rule. A picture is three tokens, so this does not
// match one.
bool parse_rule(const char* text, int32_t* color) {
    int value = 0;
    if (text == nullptr || std::strncmp(text, "line", 4) != 0 ||
        scan_int(text + 4, &value) == nullptr || value < 0 || value > 255) {
        return false;
    }
    *color = value;
    return true;
}

void copy_rows(
    uint8_t* target,
    int32_t pitch,
    const uint8_t* source,
    int32_t source_stride,
    int32_t height,
    int32_t dest_x,
    int32_t source_x,
    int32_t count
) {
    for (int32_t row = 0; row < height; ++row) {
        std::memcpy(
            target + static_cast<std::ptrdiff_t>(row) * pitch + dest_x,
            source + static_cast<std::ptrdiff_t>(row) * source_stride + source_x,
            static_cast<std::size_t>(count)
        );
    }
}

// Keeps the 12-pixel caps and repeats the middle until `wide`, or cuts it
// short when `wide` is narrower than the face.
void paint_wide_face(Surface* target, const Sprite& sprite, int32_t wide) {
    const auto* source = static_cast<const uint8_t*>(sprite.data);
    const int32_t width = sprite.width;
    const int32_t mid = width - 2 * k_function_cap;
    copy_rows(target->pixels, target->pitch, source, width, sprite.height, 0, 0, k_function_cap);
    int32_t x = k_function_cap;
    while (x < wide - k_function_cap) {
        const int32_t room = wide - k_function_cap - x;
        const int32_t count = mid < room ? mid : room;
        copy_rows(
            target->pixels, target->pitch, source, width, sprite.height, x, k_function_cap, count
        );
        x += mid;
    }
    copy_rows(
        target->pixels,
        target->pitch,
        source,
        width,
        sprite.height,
        wide - k_function_cap,
        width - k_function_cap,
        k_function_cap
    );
}

// Fits each state of a function button to `target`: a narrower face is
// stretched and a wider one is narrowed. The fitted face keeps the source
// frame's origin.
void fit_function_face(Window* button, int32_t target) {
    const ImageService* images = services().images;
    if (button == nullptr || target <= k_function_cap * 2 || images == nullptr ||
        images->sprite == nullptr || images->create_blank == nullptr) {
        return;
    }
    for (int32_t state = 0; state < button->states.count; ++state) {
        const Handle source_image = button->states.images[state];
        const Sprite* sprite = images->sprite(images->self, source_image);
        if (sprite == nullptr || sprite->data == nullptr || sprite->encoding != OA_SPRITE_RAW ||
            sprite->width <= k_function_cap * 2 || sprite->width == target || sprite->height <= 0) {
            continue;
        }
        const Handle blank = image_create_blank(
            target, sprite->height, static_cast<uint8_t>(sprite->key), sprite->key
        );
        if (blank < 0) {
            continue;
        }
        Surface surface{};
        if (!image_surface(blank, &surface) || surface.pixels == nullptr) {
            image_release(blank);
            continue;
        }
        Sprite* made = images->sprite(images->self, blank);
        if (made != nullptr) {
            made->origin_x = sprite->origin_x;
            made->origin_y = sprite->origin_y;
        }
        paint_wide_face(&surface, *sprite, target);
        window_set_state_image(button, state, blank);
        image_release(blank);
    }
}

void blit_keyed(Surface* target, const Sprite& sprite, int32_t x, int32_t y) {
    if (target == nullptr || target->pixels == nullptr || sprite.data == nullptr ||
        sprite.encoding != OA_SPRITE_RAW) {
        return;
    }
    const auto* source = static_cast<const uint8_t*>(sprite.data);
    const int32_t origin_x = x - sprite.origin_x;
    const int32_t origin_y = y - sprite.origin_y;
    for (int32_t row = 0; row < sprite.height; ++row) {
        const int32_t out_y = origin_y + row;
        if (out_y < 0 || out_y >= target->height) {
            continue;
        }
        for (int32_t column = 0; column < sprite.width; ++column) {
            const int32_t out_x = origin_x + column;
            if (out_x < 0 || out_x >= target->width) {
                continue;
            }
            const uint8_t value = source[static_cast<std::ptrdiff_t>(row) * sprite.width + column];
            if (value != sprite.key) {
                target->pixels[static_cast<std::ptrdiff_t>(out_y) * target->pitch + out_x] = value;
            }
        }
    }
}

// Fills the panel with console green and draws its help picture over that,
// skipping the picture's key colour. Empty help leaves the panel alone.
void compose_panel_picture(Window* panel, const char* help) {
    Picture picture{};
    if (panel == nullptr || panel->width <= 0 || panel->height <= 0 ||
        !parse_picture(help, &picture)) {
        return;
    }
    const Handle blank =
        image_create_blank(panel->width, panel->height, k_console_green, k_overlay_transparent);
    if (blank < 0) {
        return;
    }
    Surface surface{};
    if (!image_surface(blank, &surface)) {
        image_release(blank);
        return;
    }
    const Handle loaded =
        image_load(picture.file, picture.sequence, picture.frame, k_overlay_transparent);
    const ImageService* images = services().images;
    const Sprite* sprite = images != nullptr && images->sprite != nullptr
                               ? images->sprite(images->self, loaded)
                               : nullptr;
    if (sprite != nullptr) {
        blit_keyed(&surface, *sprite, 0, 0);
    }
    image_release(loaded);
    window_set_state_image(panel, 0, blank);
    window_set_state_image(panel, k_state_shown, blank);
    image_release(blank);
}

void take_rect(GuiControl* control, Window* window) {
    control->window = window;
    control->x = window->x;
    control->y = window->y;
    control->width = window->width;
    control->height = window->height;
}

void place_window(Window* window, const Place& place, Window* parent, GuiControl* control) {
    window_set_name(window, control->name);
    window_set_bounds(window, place.x, place.y, place.width, place.height, parent, true);
    take_rect(control, window);
}

// assoc is the parent gadget's index. 0, and a gadget with no window, is the
// root.
Window* placed_parent(GuiScreen* screen, const gui::Gadget& gadget) {
    const unsigned assoc = gadget.common.association;
    if (assoc == 0 || screen->controls == nullptr ||
        assoc >= static_cast<unsigned>(screen->count)) {
        return &screen->base;
    }
    Window* parent = screen->controls[assoc].window;
    return parent != nullptr ? parent : &screen->base;
}

Place place_of(const gui::Layout& layout, std::size_t index) {
    Place place{};
    const std::optional<gui::Rect> rect = gui::gadget_rectangle(layout.gadgets, index);
    if (!rect.has_value()) {
        return place;
    }
    place.x = rect->left;
    place.y = rect->top;
    place.width = rect->right >= rect->left ? rect->right - rect->left + 1 : 0;
    place.height = rect->bottom >= rect->top ? rect->bottom - rect->top + 1 : 0;
    return place;
}

void note_window(GuiScreen* screen, const Window* source) {
    if (source == nullptr) {
        return;
    }
    for (int32_t i = 0; i < screen->count; ++i) {
        const GuiControl& control = screen->controls[i];
        if (control.window == source) {
            copy_text(screen->fired_name, sizeof screen->fired_name, control.name);
            copy_text(screen->fired_action, sizeof screen->fired_action, control.action);
            return;
        }
    }
}

void report_notify(GuiScreen* screen, const Notify* notify) {
    if (screen == nullptr || notify == nullptr) {
        return;
    }
    note_window(screen, notify->source);
    if (notify->code == NotifyCode::activated && screen->on_fired != nullptr) {
        screen->on_fired(screen->user, notify->source);
    }
}

void note_named(GuiScreen* screen, const char* name) {
    const GuiControl* control = gui_screen_find(screen, name);
    if (control == nullptr) {
        return;
    }
    copy_text(screen->fired_name, sizeof screen->fired_name, control->name);
    copy_text(screen->fired_action, sizeof screen->fired_action, control->action);
}

// Enter runs the layout's default control: it is recorded, then activated
// when it is not disabled, the same as a click. The call is last, because
// the screen's owner may hide the screen from it.
void run_named(GuiScreen* screen, const char* name) {
    const GuiControl* control = gui_screen_find(screen, name);
    if (control == nullptr)
        return;
    note_named(screen, name);
    Window* window = control->window;
    if (window == nullptr || screen->on_fired == nullptr)
        return;
    if (window->states.current == button_state::disabled)
        return;
    void (*fired)(void*, Window*) = screen->on_fired;
    void* user = screen->user;
    fired(user, window);
}

void screen_on_notify(Window* self, const Notify* notify) {
    report_notify(reinterpret_cast<GuiScreen*>(self), notify);
}

bool screen_key_down(Window* self, const Message* msg) {
    if (msg == nullptr) {
        return false;
    }
    auto* screen = reinterpret_cast<GuiScreen*>(self);
    if (msg->key == key::enter) {
        run_named(screen, screen->enter_name);
        return true;
    }
    if (msg->key == key::escape) {
        // The layout's Escape control is pressed, the same way Enter presses
        // the default. A console with no such control closes as the frame's
        // release button does.
        if (screen->escape_name[0] != '\0' &&
            gui_screen_find(screen, screen->escape_name) != nullptr) {
            run_named(screen, screen->escape_name);
            return true;
        }
        if (self->parent != nullptr && self->parent->ops != nullptr)
            return self->parent->ops->key_down(self->parent, msg);
        return false;
    }
    return window_default_key_down(self, msg);
}

void screen_destroy(Window* self) {
    auto* screen = reinterpret_cast<GuiScreen*>(self);
    void* user = screen->user;
    void (*hook)(void*) = screen->on_destroy;
    screen->user = nullptr;
    screen->on_fired = nullptr;
    screen->on_destroy = nullptr;
    std::free(screen->controls);
    screen->controls = nullptr;
    window_release(self);
    std::free(screen);
    if (hook != nullptr) {
        hook(user);
    }
}

const WindowOps* screen_ops() {
    static WindowOps ops = [] {
        WindowOps made = console_panel_ops();
        made.on_notify = screen_on_notify;
        made.key_down = screen_key_down;
        made.destroy = screen_destroy;
        return made;
    }();
    return &ops;
}

WindowOps* hooked_list_ops() {
    static WindowOps ops{};
    return &ops;
}

void list_hook_notify(Window* self, const Notify* notify) {
    if (g_list_notify != nullptr) {
        g_list_notify(self, notify);
    }
    if (notify == nullptr || self->parent == nullptr || self->parent->ops != screen_ops()) {
        return;
    }
    report_notify(reinterpret_cast<GuiScreen*>(self->parent), notify);
}

void hook_list(Window* list) {
    WindowOps* hooked = hooked_list_ops();
    if (list->ops == hooked) {
        return;
    }
    *hooked = *list->ops;
    g_list_notify = list->ops->on_notify;
    hooked->on_notify = list_hook_notify;
    list->ops = hooked;
}

void add_control(GuiScreen* screen, const GuiControl& control) {
    if (screen->count == screen->capacity) {
        const int32_t next = screen->capacity == 0 ? 8 : screen->capacity * 2;
        auto* grown = static_cast<GuiControl*>(
            std::realloc(screen->controls, sizeof(GuiControl) * static_cast<std::size_t>(next))
        );
        if (grown == nullptr) {
            std::abort();
        }
        screen->controls = grown;
        screen->capacity = next;
    }
    screen->controls[screen->count] = control;
    ++screen->count;
}

void set_button_caption(Window* button, const char* caption, int32_t style) {
    for (int32_t state = 0; state < button->states.count; ++state) {
        window_set_state_text(button, state, caption, style);
    }
}

void finish_button(Window* button, const GuiControl& control, int32_t style, bool grayed) {
    set_button_caption(button, control.caption, style);
    window_set_state(button, grayed ? button_state::disabled : button_state::unpressed);
}

// Extra caption inset after the picture: "file sequence frame|12".
int32_t caption_gap(const char* picture_text) {
    const char* bar = picture_text != nullptr ? std::strchr(picture_text, '|') : nullptr;
    if (bar == nullptr)
        return 0;
    int gap = 0;
    if (scan_int(bar + 1, &gap) == nullptr || gap < 0)
        return 0;
    return gap;
}

bool gui_radio_draw(Window* self) {
    if (!window_is_shown(self))
        return true;
    const int32_t state = self->states.current;
    if (state < 0 || state >= self->states.count)
        return true;
    const auto* radio = reinterpret_cast<const GuiRadio*>(self);
    image_draw(self->x, self->y + 1, self->width, self->height, self->states.images[state]);
    text_draw(
        self->font,
        self->x + 11 + radio->caption_gap,
        self->y,
        self->width,
        self->height,
        self->states.texts[state]
    );
    return true;
}

// Lets go only for a radio this one is grouped with. Other radios on the
// same dialog keep their own choice.
void gui_radio_notify(Window* self, const Notify* notify) {
    if (!window_is_shown(self) || notify == nullptr || notify->code != NotifyCode::radio_selected)
        return;
    const auto* radio = reinterpret_cast<const screens::LobbyRadio*>(self);
    for (int32_t i = 0; i < radio->member_count; ++i) {
        if (notify->source != radio->members[i])
            continue;
        if (self->states.current == button_state::pressed)
            window_set_state(self, button_state::unpressed);
        return;
    }
}

const WindowOps* gui_radio_ops() {
    static const WindowOps ops = [] {
        WindowOps made = k_radio_button_ops;
        made.draw = gui_radio_draw;
        made.on_notify = gui_radio_notify;
        return made;
    }();
    return &ops;
}

Window* make_radio(const Picture& picture, int32_t gap, bool selected) {
    auto* radio = static_cast<GuiRadio*>(std::calloc(1, sizeof(GuiRadio)));
    if (radio == nullptr)
        std::abort();
    Window* window = &radio->radio.base;
    window_init(window, gui_radio_ops());
    radio->caption_gap = gap;
    window_set_state_count(window, button_state::count);
    if (window->ops->bind_images != nullptr)
        window->ops->bind_images(
            window, picture.file, picture.sequence, picture.frame, k_overlay_transparent
        );
    window_set_state(window, selected ? button_state::pressed : button_state::unpressed);
    return window;
}

// A check box is a button whose picture is Check_Box. active rests it checked.
Window* make_check(const Picture& picture, bool checked) {
    CheckBox* box = check_box_create();
    Window* window = &box->base;
    if (window->ops->bind_images != nullptr)
        window->ops->bind_images(
            window, picture.file, picture.sequence, picture.frame, k_overlay_transparent
        );
    if (checked)
        window_set_state(window, check_state::checked_up);
    return window;
}

void paint_mark(Window* window, const char* kind, int color) {
    const bool fill = std::strcmp(kind, "fill") == 0;
    const uint8_t ink = static_cast<uint8_t>(color);
    const Handle image = image_create_blank(
        window->width, window->height, fill ? ink : k_overlay_transparent, k_overlay_transparent
    );
    if (image < 0)
        return;
    Surface surface{};
    if (!fill && image_surface(image, &surface) && window->width > 0 && window->height > 0) {
        surface.clip = oa::Rect32{0, 0, window->width - 1, window->height - 1};
        if (std::strcmp(kind, "rect") == 0) {
            (void)oa::present::draw_rect_outline(
                &surface, oa::Rect32{0, 0, window->width - 1, window->height - 1}, ink
            );
        } else if (std::strcmp(kind, "line") == 0) {
            (void)oa::present::draw_clipped_line(&surface, 0, 0, window->width - 1, 0, ink);
        }
    }
    window_set_state_image(window, k_state_shown, image);
    image_release(image);
}

GuiControl begin_control(const gui::Gadget& gadget) {
    GuiControl control{};
    copy_text(control.name, sizeof control.name, gadget.common.name.c_str());
    control.kind = static_cast<uint8_t>(gadget.common.type);
    return control;
}

void build_mark(GuiScreen* screen, const gui::Gadget& gadget, const Place& place) {
    GuiControl control = begin_control(gadget);
    char kind[16]{};
    int color = 0;
    const char* after_kind = scan_word(gadget.common.source_help.c_str(), kind, sizeof kind - 1);
    if (after_kind != nullptr)
        (void)scan_int(after_kind, &color);
    Window* mark = static_create();
    drop_fill(mark);
    place_window(mark, place, placed_parent(screen, gadget), &control);
    paint_mark(mark, kind, color);
    add_control(screen, control);
}

void build_button(
    GuiScreen* screen,
    const gui::Gadget& gadget,
    const Place& place,
    const char names[][k_font_name_bytes],
    int32_t font_count
) {
    GuiControl control = begin_control(gadget);
    const auto* fields = std::get_if<gui::ButtonFields>(&gadget.fields);
    char action[128]{};
    char picture_text[128]{};
    split_help(
        gadget.common.source_help.c_str(), action, sizeof action, picture_text, sizeof picture_text
    );
    copy_text(control.action, sizeof control.action, action);
    store_caption(&control, fields != nullptr ? fields->text.c_str() : "");
    const int32_t style = text_style_of(gadget.common.attributes);
    control.caption_style = style;
    const bool grayed = fields != nullptr && fields->grayed_out;
    Picture picture{};
    parse_picture(picture_text, &picture);
    Window* parent = placed_parent(screen, gadget);
    Window* button = nullptr;
    if (picture.present && std::strcmp(picture.sequence, k_radio_button) == 0) {
        button = make_radio(picture, caption_gap(picture_text), gadget.common.active != 0);
        use_font(button, gadget.common.font_number, names, font_count);
        button->ops->set_caption(button, control.caption, style);
        place_window(button, place, parent, &control);
    } else if (picture.present && std::strcmp(picture.sequence, k_check_box) == 0) {
        button = make_check(picture, gadget.common.active != 0);
        use_font(button, gadget.common.font_number, names, font_count);
        button->ops->set_caption(button, control.caption, style);
        place_window(button, place, parent, &control);
    } else if (picture.present && std::strcmp(picture.sequence, k_blank_button) == 0) {
        button =
            blank_button_create(parent, place.x - parent->x, place.y - parent->y, control.caption);
        use_font(button, gadget.common.font_number, names, font_count);
        place_window(button, place, parent, &control);
        finish_button(button, control, style, grayed);
    } else if (function_button_picture(picture)) {
        button = button_create();
        const bool small = std::strcmp(picture.sequence, k_function_small) == 0;
        screens::function_button_setup(button, small, 0, 0, control.caption, parent);
        use_font(button, gadget.common.font_number, names, font_count);
        fit_function_face(button, place.width);
        finish_button(button, control, style, grayed);
        place_window(button, place, parent, &control);
    } else {
        button = button_create();
        use_font(button, gadget.common.font_number, names, font_count);
        if (picture.present) {
            window_set_state_count(button, k_picture_states);
            button->ops->bind_images(
                button, picture.file, picture.sequence, picture.frame, k_overlay_transparent
            );
        }
        finish_button(button, control, style, grayed);
        place_window(button, place, parent, &control);
    }
    add_control(screen, control);
}

void build_list(GuiScreen* screen, const gui::Gadget& gadget, const Place& place, Window** list) {
    GuiControl control = begin_control(gadget);
    screens::ByListBox* box = screens::by_list_box_create(false, true);
    *list = &box->list.base;
    hook_list(*list);
    place_window(*list, place, placed_parent(screen, gadget), &control);
    add_control(screen, control);
}

void build_row(
    GuiScreen* screen,
    const gui::Gadget& gadget,
    const Place& place,
    Window* list,
    const char names[][k_font_name_bytes],
    int32_t font_count
) {
    GuiControl control = begin_control(gadget);
    char caption[128]{};
    char action[128]{};
    split_help(gadget.common.source_help.c_str(), caption, sizeof caption, action, sizeof action);
    store_caption(&control, caption);
    control.caption_style = text_style_of(gadget.common.attributes);
    copy_text(control.action, sizeof control.action, action);
    if (list == nullptr) {
        control.x = place.x;
        control.y = place.y;
        control.width = place.width;
        control.height = place.height;
        add_control(screen, control);
        return;
    }
    screens::HatFonts fonts{};
    screens::ByTextRow* row = screens::by_text_row_create(
        reinterpret_cast<screens::ByListBox*>(list), control.caption, &fonts, true, 3
    );
    window_set_name(&row->base, control.name);
    const Handle font = font_load(font_name(gadget.common.font_number, names, font_count));
    screens::by_text_row_set_font(row, font);
    if (font >= 0) {
        font_release(font);
    }
    take_rect(&control, &row->base);
    add_control(screen, control);
}

// "mask" draws a star for every character. "pad:N" insets the text. The two
// may be joined with "|".
void apply_text_help(EditBox* box, const char* help) {
    if (box == nullptr || help == nullptr || help[0] == '\0') {
        return;
    }
    char buffer[128];
    copy_text(buffer, sizeof buffer, help);
    char* token = buffer;
    while (token != nullptr && token[0] != '\0') {
        char* bar = std::strchr(token, '|');
        if (bar != nullptr) {
            *bar = '\0';
        }
        if (std::strcmp(token, "mask") == 0) {
            box->mask = '*';
        } else if (std::strncmp(token, "pad:", 4) == 0) {
            int pad = 0;
            if (scan_int(token + 4, &pad) != nullptr && pad >= 0 && pad < 64) {
                box->padding = pad;
            }
        }
        token = bar != nullptr ? bar + 1 : nullptr;
    }
}

void build_text(
    GuiScreen* screen,
    const gui::Gadget& gadget,
    const Place& place,
    const char names[][k_font_name_bytes],
    int32_t font_count
) {
    GuiControl control = begin_control(gadget);
    const auto* fields = std::get_if<gui::TextBoxFields>(&gadget.fields);
    store_caption(&control, fields != nullptr ? fields->text.c_str() : "");
    control.caption_style = text_style_of(gadget.common.attributes);
    EditBox* box = edit_box_create();
    // The field colour is a picture behind the box, so the box itself does not fill.
    drop_fill(&box->base);
    use_font(&box->base, gadget.common.font_number, names, font_count);
    // The caption is cut to the box width, so the window is placed first
    // and the inset is set before the caption is measured.
    place_window(&box->base, place, placed_parent(screen, gadget), &control);
    apply_text_help(box, gadget.common.source_help.c_str());
    box->base.ops->set_caption(
        &box->base, control.caption, text_style_of(gadget.common.attributes)
    );
    add_control(screen, control);
}

void build_scroll(GuiScreen* screen, const gui::Gadget& gadget, const Place& place) {
    GuiControl control = begin_control(gadget);
    const auto* fields = std::get_if<gui::ScrollBarFields>(&gadget.fields);
    store_caption(&control, fields != nullptr ? fields->text.c_str() : "");
    Slider* slider = slider_create();
    place_window(&slider->base, place, placed_parent(screen, gadget), &control);
    int32_t range = fields != nullptr ? fields->range : 1;
    if (range < 1) {
        range = 1;
    }
    slider_set_range(slider, range, false);
    if (fields != nullptr) {
        slider_set_index(slider, fields->knob_position);
    }
    add_control(screen, control);
}

void build_label(
    GuiScreen* screen,
    const gui::Gadget& gadget,
    const Place& place,
    const char names[][k_font_name_bytes],
    int32_t font_count
) {
    GuiControl control = begin_control(gadget);
    const auto* fields = std::get_if<gui::LabelFields>(&gadget.fields);
    char action[128]{};
    char picture_text[128]{};
    split_help(
        gadget.common.source_help.c_str(), action, sizeof action, picture_text, sizeof picture_text
    );
    copy_text(control.action, sizeof control.action, action);
    store_caption(&control, fields != nullptr ? fields->text.c_str() : "");
    control.caption_style = text_style_of(gadget.common.attributes);
    Window* label = static_create();
    drop_fill(label);
    use_font(label, gadget.common.font_number, names, font_count);
    label->ops->set_caption(label, control.caption, text_style_of(gadget.common.attributes));
    tint_caption(label, gadget.common.foreground_color);
    Picture picture{};
    if (parse_picture(picture_text, &picture)) {
        show_frame(label, picture.file, picture.sequence, picture.frame);
    }
    place_window(label, place, placed_parent(screen, gadget), &control);
    add_control(screen, control);
}

void build_hot(GuiScreen* screen, const gui::Gadget& gadget, const Place& place) {
    GuiControl control = begin_control(gadget);
    char action[128]{};
    char picture_text[128]{};
    split_help(
        gadget.common.source_help.c_str(), action, sizeof action, picture_text, sizeof picture_text
    );
    copy_text(control.action, sizeof control.action, action);
    Window* hot = hotspot_button_create();
    place_window(hot, place, placed_parent(screen, gadget), &control);
    add_control(screen, control);
}

void build_rule(GuiScreen* screen, const gui::Gadget& gadget, const Place& place, int32_t color) {
    GuiControl control = begin_control(gadget);
    Window* rule = static_create();
    drop_fill(rule);
    if (place.width > 0 && place.height > 0) {
        const Handle blank = image_create_blank(
            place.width, place.height, static_cast<uint8_t>(color), k_overlay_transparent
        );
        if (blank >= 0) {
            window_set_state_image(rule, 0, blank);
            window_set_state_image(rule, k_state_shown, blank);
            image_release(blank);
        }
    }
    place_window(rule, place, placed_parent(screen, gadget), &control);
    add_control(screen, control);
}

void build_picture(
    GuiScreen* screen, const gui::Gadget& gadget, const Place& place, const Picture& picture
) {
    GuiControl control = begin_control(gadget);
    Window* image = static_create();
    drop_fill(image);
    if (picture.present) {
        show_frame(image, picture.file, picture.sequence, picture.frame);
    }
    place_window(image, place, placed_parent(screen, gadget), &control);
    add_control(screen, control);
}

void build_resource(GuiScreen* screen, const gui::Gadget& gadget, const Place& place) {
    GuiControl control = begin_control(gadget);
    const auto* fields = std::get_if<gui::FileResourceFields>(&gadget.fields);
    copy_text(
        control.caption, sizeof control.caption, fields != nullptr ? fields->filename.c_str() : ""
    );
    control.x = place.x;
    control.y = place.y;
    control.width = place.width;
    control.height = place.height;
    add_control(screen, control);
}

void build_bare(
    GuiScreen* screen, const gui::Gadget& gadget, const Place& place, bool transparent
) {
    GuiControl control = begin_control(gadget);
    Window* window = static_create();
    if (transparent) {
        drop_fill(window);
    }
    place_window(window, place, placed_parent(screen, gadget), &control);
    add_control(screen, control);
}

void build_child_panel(GuiScreen* screen, const gui::Gadget& gadget, const Place& place) {
    GuiControl control = begin_control(gadget);
    Window* panel = panel_create();
    drop_fill(panel);
    panel->focusable = 0;
    Picture picture{};
    if (parse_picture(gadget.common.source_help.c_str(), &picture)) {
        show_frame(panel, picture.file, picture.sequence, picture.frame);
    }
    place_window(panel, place, placed_parent(screen, gadget), &control);
    add_control(screen, control);
}

void collect_fonts(const gui::Layout& layout, char names[][k_font_name_bytes], int32_t* count) {
    for (const gui::Gadget& gadget : layout.gadgets) {
        if (static_cast<uint8_t>(gadget.common.type) != gui_kind::font_resource ||
            *count >= k_font_capacity) {
            continue;
        }
        const auto* fields = std::get_if<gui::FileResourceFields>(&gadget.fields);
        if (fields == nullptr) {
            continue;
        }
        copy_text(names[*count], k_font_name_bytes, fields->filename.c_str());
        ++*count;
    }
}

GuiScreen* build_screen(const gui::Layout& layout) {
    auto* screen = static_cast<GuiScreen*>(std::calloc(1, sizeof(GuiScreen)));
    if (screen == nullptr) {
        std::abort();
    }
    window_init(&screen->base, screen_ops());
    char fonts[k_font_capacity][k_font_name_bytes]{};
    int32_t font_count = 0;
    collect_fonts(layout, fonts, &font_count);

    const gui::Gadget& root = layout.gadgets[0];
    const Place root_place = place_of(layout, 0);
    GuiControl root_control = begin_control(root);
    drop_fill(&screen->base);
    place_window(&screen->base, root_place, nullptr, &root_control);
    if (const auto* panel = std::get_if<gui::PanelFields>(&root.fields)) {
        copy_text(
            screen->enter_name, sizeof screen->enter_name, panel->carriage_return_default.c_str()
        );
        copy_text(screen->escape_name, sizeof screen->escape_name, panel->escape_default.c_str());
    }
    add_control(screen, root_control);
    compose_panel_picture(&screen->base, root.common.source_help.c_str());

    Window* list = nullptr;
    for (std::size_t index = 1; index < layout.gadgets.size(); ++index) {
        const gui::Gadget& gadget = layout.gadgets[index];
        const Place place = place_of(layout, index);
        const uint8_t kind = static_cast<uint8_t>(gadget.common.type);
        switch (kind) {
        case gui_kind::button:
            build_button(screen, gadget, place, fonts, font_count);
            break;
        case gui_kind::list_box:
            build_list(screen, gadget, place, &list);
            break;
        case gui_kind::text_box:
            build_text(screen, gadget, place, fonts, font_count);
            break;
        case gui_kind::scroll_bar:
            build_scroll(screen, gadget, place);
            break;
        case gui_kind::label:
            build_label(screen, gadget, place, fonts, font_count);
            break;
        case gui_kind::hot_surface:
            build_hot(screen, gadget, place);
            break;
        case gui_kind::font_resource:
        case gui_kind::file_resource:
            build_resource(screen, gadget, place);
            break;
        case gui_kind::image:
        case gui_kind::icon: {
            int32_t line = 0;
            if (parse_rule(gadget.common.source_help.c_str(), &line)) {
                build_rule(screen, gadget, place, line);
                break;
            }
            Picture picture{};
            parse_picture(gadget.common.source_help.c_str(), &picture);
            build_picture(screen, gadget, place, picture);
            break;
        }
        case gui_kind::qr: {
            Picture picture{};
            if (!parse_picture(gadget.common.source_help.c_str(), &picture)) {
                copy_text(picture.file, sizeof picture.file, k_qr_file);
                copy_text(picture.sequence, sizeof picture.sequence, k_qr_sequence);
                picture.frame = 0;
                picture.present = true;
            }
            build_picture(screen, gadget, place, picture);
            break;
        }
        case gui_kind::green:
            build_bare(screen, gadget, place, true);
            break;
        case gui_kind::list_row:
            build_row(screen, gadget, place, list, fonts, font_count);
            break;
        case gui_kind::panel:
            build_child_panel(screen, gadget, place);
            break;
        case gui_kind::mark:
            build_mark(screen, gadget, place);
            break;
        default: {
            GuiControl control = begin_control(gadget);
            control.x = place.x;
            control.y = place.y;
            control.width = place.width;
            control.height = place.height;
            add_control(screen, control);
            break;
        }
        }
    }
    return screen;
}

} // namespace

GuiScreen* gui_screen_load(const char* path, GuiLoadError* error) {
    set_error(error, "");
    if (path == nullptr) {
        set_error(error, "the GUI path is missing");
        return nullptr;
    }
    std::vector<uint8_t> bytes;
    if (!rover_session_read_file(path, &bytes)) {
        set_error(error, "the GUI file could not be read");
        return nullptr;
    }
    const gui::ParseResult parsed = gui::parse(
        std::span<const uint8_t>(bytes.data(), bytes.size()),
        [](std::string_view source) { return translate_line(source); }
    );
    if (!parsed.ok() || !parsed.layout.has_value()) {
        const char* message = parsed.error.has_value() && !parsed.error->message.empty()
                                  ? parsed.error->message.c_str()
                                  : "the GUI file could not be parsed";
        set_error(error, message);
        return nullptr;
    }
    if (parsed.layout->gadgets.empty() ||
        static_cast<uint8_t>(parsed.layout->gadgets[0].common.type) != gui_kind::panel) {
        set_error(error, "first GUI record is not the required panel gadget");
        return nullptr;
    }
    return build_screen(*parsed.layout);
}

void gui_screen_set_user(
    GuiScreen* screen,
    void* user,
    void (*on_fired)(void* user, Window* source),
    void (*on_destroy)(void* user)
) {
    if (screen == nullptr) {
        return;
    }
    screen->user = user;
    screen->on_fired = on_fired;
    screen->on_destroy = on_destroy;
}

void gui_screen_set_escape(GuiScreen* screen, const char* name) {
    if (screen == nullptr || name == nullptr || name[0] == '\0') {
        return;
    }
    if (screen->escape_name[0] != '\0' || gui_screen_find(screen, name) == nullptr) {
        return;
    }
    copy_text(screen->escape_name, sizeof screen->escape_name, name);
}

void gui_screen_destroy(GuiScreen* screen) {
    if (screen == nullptr) {
        return;
    }
    window_destroy(&screen->base);
}

int32_t gui_screen_count(const GuiScreen* screen) {
    return screen != nullptr ? screen->count : 0;
}

const GuiControl* gui_screen_at(const GuiScreen* screen, int32_t index) {
    if (screen == nullptr || index < 0 || index >= screen->count) {
        return nullptr;
    }
    return &screen->controls[index];
}

void gui_control_caption_room(
    const GuiControl* control, int32_t* width, int32_t* height, int32_t* style
) {
    int32_t room_w = 0;
    int32_t room_h = 0;
    int32_t room_style = 0;
    if (control != nullptr) {
        room_w = control->width;
        room_h = control->height;
        room_style = control->caption_style;
        Window* window = control->window;
        if (window != nullptr && window->ops != nullptr && window->ops->is_class != nullptr) {
            if (window->ops->is_class(class_name::radio_button)) {
                const auto* radio = reinterpret_cast<const GuiRadio*>(window);
                room_w -= 11 + radio->caption_gap;
            } else if (window->ops->is_class(class_name::check_box)) {
                const auto* box = reinterpret_cast<const CheckBox*>(window);
                int32_t image_w = 0;
                int32_t image_h = 0;
                const int32_t state = window->states.current;
                if (state >= 0 && state < window->states.count && window->states.images != nullptr)
                    image_size(window->states.images[state], &image_w, &image_h);
                room_w -= box->label_gap + image_w;
            }
        }
        if (room_w < 0)
            room_w = 0;
    }
    if (width != nullptr)
        *width = room_w;
    if (height != nullptr)
        *height = room_h;
    if (style != nullptr)
        *style = room_style;
}

const GuiControl* gui_screen_find(const GuiScreen* screen, const char* name) {
    if (screen == nullptr || name == nullptr) {
        return nullptr;
    }
    for (int32_t i = 0; i < screen->count; ++i) {
        if (std::strcmp(screen->controls[i].name, name) == 0) {
            return &screen->controls[i];
        }
    }
    return nullptr;
}

const char* gui_screen_fired_name(const GuiScreen* screen) {
    return screen != nullptr ? screen->fired_name : "";
}

const char* gui_screen_fired_action(const GuiScreen* screen) {
    return screen != nullptr ? screen->fired_action : "";
}

Window* gui_screen_root(GuiScreen* screen) {
    return screen != nullptr ? &screen->base : nullptr;
}

} // namespace oa::boneyards::console

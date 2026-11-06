// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Boneyards message box and its blank caption buttons.
#include "oa/boneyards/message_box.hpp"

#include "oa/boneyards/shown_text.hpp"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace oa::boneyards {
namespace {

constexpr int32_t k_blank_button_states = 4;
constexpr int32_t k_blank_button_raised_state = 2;

enum class Choice : int32_t { ok = 1, cancel = 2, yes = 4, no = 8, none = 0x10 };

MessageBox* as_box(Window* w) {
    return reinterpret_cast<MessageBox*>(w);
}

Handle state_handle(const Handle* handles, const Window* w) {
    const int32_t state = w->states.current;
    return state >= 0 && state < w->states.count ? handles[state] : k_no_handle;
}

void set_flag(bool* flag) {
    if (flag != nullptr) {
        *flag = true;
    }
}

// Marks the box for destruction on the next desktop cycle.
void close_box(MessageBox* box) {
    box->base.owned = 1;
    if (box->on_closed != nullptr) {
        MessageBoxClosed closed = box->on_closed;
        box->on_closed = nullptr;
        closed(box->user, box);
    }
}

/// Draws the face of the current state and its caption, lifted one pixel while raised, plus the
/// focus frame.
///
/// @param self the button
/// @return true always
bool blank_button_draw(Window* self) {
    if (window_is_shown(self)) {
        const Handle text = state_handle(self->states.texts, self);
        image_draw(
            self->x, self->y, self->width, self->height, state_handle(self->states.images, self)
        );
        const int32_t state = self->states.current;
        if (state >= 0 && state <= 3) {
            const int32_t lift = state == k_blank_button_raised_state ? 1 : 0;
            text_draw(self->font, self->x, self->y - lift, self->width, self->height, text);
        }
        if (self->focused != 0) {
            draw_focus_frame(self->x, self->y, self->width, self->height);
        }
    }
    window_draw_children(self);
    return true;
}

WindowOps make_blank_button_ops() {
    WindowOps ops = k_button_ops;
    ops.draw = blank_button_draw;
    return ops;
}

/// Handles a button press in the box.
///
/// Ok and Yes set `accepted`; Cancel and No set `declined` (only when an
/// `accepted` flag was also supplied). Any button hands the focus back and
/// closes the box. The focus stays put when the window it would go back to
/// has closed under the box, as the Main Frame does when EXIT is pressed
/// with a box up.
///
/// @param self the box
/// @param notify control notice; notices from other windows are ignored
void message_box_on_notify(Window* self, const Notify* notify) {
    MessageBox* box = as_box(self);
    Window* source = notify->source;
    if (window_is_named(source, message_box_name::ok) ||
        window_is_named(source, message_box_name::yes)) {
        set_flag(box->accepted);
    } else if (
        window_is_named(source, message_box_name::cancel) ||
        window_is_named(source, message_box_name::no)
    ) {
        if (box->declined != nullptr && box->accepted != nullptr) {
            *box->declined = true;
        }
    } else {
        return;
    }
    if (desktop_holds(services().desktop, box->focus_return)) {
        services_desktop_focus(box->focus_return);
    }
    close_box(box);
}

/// Presses the button whose caption starts with the typed letter.
///
/// Enter presses Ok, or Yes when there is no Ok; Escape presses Cancel, or No
/// when there is no Cancel. Space presses Ok when it is the only button. A key
/// that answers the box hands the focus back, as a button does.
///
/// @param self the box
/// @param msg key message
/// @return true always: every key is consumed
bool message_box_key_down(Window* self, const Message* msg) {
    MessageBox* box = as_box(self);
    const Window* ok = window_find_named(self, message_box_name::ok, class_name::button);
    const Window* cancel = window_find_named(self, message_box_name::cancel, class_name::button);
    const Window* yes = window_find_named(self, message_box_name::yes, class_name::button);
    const Window* no = window_find_named(self, message_box_name::no, class_name::button);
    const char typed = static_cast<char>(std::toupper(msg->key));
    Choice choice = Choice::none;
    if (typed == message_box_name::yes[0] && yes != nullptr) {
        choice = Choice::yes;
    }
    if (typed == message_box_name::cancel[0] && cancel != nullptr) {
        choice = Choice::cancel;
    }
    if (typed == message_box_name::ok[0] && ok != nullptr) {
        choice = Choice::ok;
    }
    if (typed == message_box_name::no[0] && no != nullptr) {
        choice = Choice::no;
    }
    if (msg->key == key::enter) {
        if (ok != nullptr)
            choice = Choice::ok;
        else if (yes != nullptr)
            choice = Choice::yes;
    }
    if (msg->key == key::escape) {
        if (cancel != nullptr)
            choice = Choice::cancel;
        else if (no != nullptr)
            choice = Choice::no;
    }
    if (ok != nullptr && cancel == nullptr && yes == nullptr && no == nullptr && typed == ' ') {
        choice = Choice::ok;
    }
    switch (choice) {
    case Choice::ok:
    case Choice::yes:
        set_flag(box->accepted);
        break;
    case Choice::cancel:
    case Choice::no:
        set_flag(box->declined);
        break;
    case Choice::none:
        break;
    }
    if (choice != Choice::none) {
        if (desktop_holds(services().desktop, box->focus_return)) {
            services_desktop_focus(box->focus_return);
        }
        close_box(box);
    }
    return true;
}

/// Sets the draw object's flag from draw_flag_on_close, then tears the box down and frees it.
///
/// @param self the box
void message_box_destroy(Window* self) {
    if (as_box(self)->draw_flag_on_close == 0) {
        services_clear_draw_flag();
    } else {
        services_set_draw_flag();
    }
    window_release(self);
    std::free(self);
}

WindowOps make_message_box_ops() {
    WindowOps ops = k_dialog_ops;
    ops.destroy = message_box_destroy;
    ops.on_notify = message_box_on_notify;
    ops.key_down = message_box_key_down;
    return ops;
}

void bind_images(Window* w, const char* file, const char* sequence) {
    w->ops->bind_images(w, file, sequence, 0, message_box_resource::key);
}

/// Sum of the wrapped lines. A line is the font's line plus any ink past it.
/// `tall` is set when a glyph passes the font's line.
int32_t wrapped_block(Toolkit* toolkit, Handle font, const char* text, int32_t width, bool* tall) {
    *tall = false;
    const std::size_t owned = std::strlen(text);
    auto* wrapped = static_cast<char*>(std::malloc(owned * 2 + 2));
    if (wrapped == nullptr) {
        return 0;
    }
    toolkit_wrap_text(toolkit, width, text, wrapped, static_cast<int32_t>(owned * 2 + 2), font);
    int32_t block = 0;
    char* cursor = wrapped;
    while (*cursor != '\0') {
        char* end = cursor;
        while (*end != '\0' && *end != '\n') {
            ++end;
        }
        const char saved = *end;
        *end = '\0';
        TextFit fit{};
        toolkit_text_fit(toolkit, font, cursor, &fit);
        *end = saved;
        int32_t pitch = fit.line_height + fit.above + fit.below;
        if (pitch < fit.line_height) {
            pitch = fit.line_height;
        }
        if (fit.above > 0 || fit.below > 0) {
            *tall = true;
        }
        block += pitch;
        if (saved == '\0') {
            break;
        }
        cursor = end + 1;
        if (*cursor == '\0') {
            break;
        }
    }
    std::free(wrapped);
    return block;
}

void copy_face_rows(
    uint8_t* dest,
    int32_t dest_pitch,
    int32_t dest_row,
    const uint8_t* source,
    int32_t source_stride,
    int32_t source_row,
    int32_t width,
    int32_t rows
) {
    for (int32_t row = 0; row < rows; ++row) {
        std::memcpy(
            dest + static_cast<std::ptrdiff_t>(dest_row + row) * dest_pitch,
            source + static_cast<std::ptrdiff_t>(source_row + row) * source_stride,
            static_cast<std::size_t>(width)
        );
    }
}

/// Lengthens the box picture by `extra` rows.
///
/// The top edge stays. The panel above the buttons repeats to fill the gap.
/// The bar the buttons sit on, and the bottom edge under that bar, move down
/// with the buttons, so the buttons stay on the bar and inside the picture.
void lengthen_face(Window* box, int32_t extra) {
    const ImageService* images = services().images;
    if (extra <= 0 || box == nullptr || images == nullptr || images->sprite == nullptr ||
        images->create_blank == nullptr) {
        return;
    }
    const int32_t top = message_box_layout::text_dy;
    const int32_t bar = message_box_layout::button_dy;
    const int32_t edge = bar + message_box_layout::button_height;
    for (int32_t state = 0; state < box->states.count; ++state) {
        const Handle source_image = box->states.images[state];
        const Sprite* sprite = images->sprite(images->self, source_image);
        if (sprite == nullptr || sprite->data == nullptr || sprite->encoding != OA_SPRITE_RAW ||
            sprite->width <= 0 || sprite->height <= edge || top >= bar) {
            continue;
        }
        const int32_t tall = sprite->height + extra;
        const Handle blank =
            image_create_blank(sprite->width, tall, static_cast<uint8_t>(sprite->key), sprite->key);
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
        const auto* source = static_cast<const uint8_t*>(sprite->data);
        const int32_t panel = bar - top;
        const int32_t band = panel / 2 > 0 ? panel / 2 : panel;
        const int32_t band_at = top + (panel - band) / 2;
        copy_face_rows(
            surface.pixels, surface.pitch, 0, source, sprite->width, 0, sprite->width, bar
        );
        int32_t filled = 0;
        while (filled < extra) {
            const int32_t count = band < extra - filled ? band : extra - filled;
            copy_face_rows(
                surface.pixels,
                surface.pitch,
                bar + filled,
                source,
                sprite->width,
                band_at,
                sprite->width,
                count
            );
            filled += count;
        }
        copy_face_rows(
            surface.pixels,
            surface.pitch,
            bar + extra,
            source,
            sprite->width,
            bar,
            sprite->width,
            sprite->height - bar
        );
        window_set_state_image(box, state, blank);
        image_release(blank);
    }
}

/// Lengthens the text, the box and its picture, and moves the buttons down,
/// when the wrapped lines do not fit the text area. A message whose glyphs
/// fit the font's line keeps the box.
void grow_for_text(MessageBox* box, Window* label) {
    const FontService* fonts = services().fonts;
    if (fonts == nullptr || fonts->self == nullptr || label->font < 0) {
        return;
    }
    auto* toolkit = static_cast<Toolkit*>(fonts->self);
    const char* text = "";
    if (label->states.count > 1 && label->states.texts != nullptr && label->states.texts[1] >= 0) {
        text = text_get(label->states.texts[1]);
    }
    bool tall = false;
    const int32_t block = wrapped_block(toolkit, label->font, text, label->width, &tall);
    if (!tall || block <= message_box_layout::text_height) {
        return;
    }
    const int32_t extra = block - message_box_layout::text_height;
    label->height += extra;
    box->base.height += extra;
    for (int32_t i = 0; i < box->base.children.count; ++i) {
        Window* child = box->base.children.items[i];
        if (child == nullptr || child->ops == nullptr || child->ops->is_class == nullptr) {
            continue;
        }
        if (child->ops->is_class(class_name::button)) {
            child->y += extra;
        }
    }
    lengthen_face(&box->base, extra);
}

void add_button(MessageBox* box, int32_t dx, const char* caption) {
    Window* button = blank_button_create(&box->base, dx, message_box_layout::button_dy, caption);
    window_set_state(button, k_blank_button_raised_state);
    window_set_name(button, caption);
}

/// Sizes and places the box, adds the buttons, fills the wrapped text and shows it on top.
///
/// @param[in,out] box box to build
/// @param text message; literal "\n" sequences become line breaks; null shows nothing
/// @param buttons message_box_buttons set
/// @param[out] accepted flag cleared now and set by Ok or Yes; may be null
/// @param[out] declined flag cleared now and set by Cancel or No; may be null
void message_box_build(
    MessageBox* box, const char* text, uint32_t buttons, bool* accepted, bool* declined
) {
    namespace layout = message_box_layout;
    box->accepted = accepted;
    box->declined = declined;
    if (accepted != nullptr) {
        *accepted = false;
    }
    if (declined != nullptr) {
        *declined = false;
    }
    Window* self = &box->base;
    window_set_bounds(self, 0, 0, layout::width, layout::height, nullptr, true);
    window_move_to(self, layout::x, layout::y);
    bind_images(self, message_box_resource::gaf, message_box_resource::face);
    if ((buttons & message_box_buttons::ok) != 0) {
        add_button(box, layout::single_button_dx, message_box_name::ok);
    }
    if ((buttons & message_box_buttons::ok_cancel) != 0) {
        add_button(box, layout::left_button_dx, message_box_name::ok);
        add_button(box, layout::right_button_dx, message_box_name::cancel);
    }
    if ((buttons & message_box_buttons::yes_no) != 0) {
        add_button(box, layout::left_button_dx, message_box_name::yes);
        add_button(box, layout::right_button_dx, message_box_name::no);
    }
    Window* label = static_create();
    window_set_bounds(
        label,
        self->x + layout::text_dx,
        self->y + layout::text_dy,
        layout::text_width,
        layout::text_height,
        self,
        true
    );
    window_set_name(label, message_box_name::text);
    window_set_font(label, message_box_resource::font);
    const char* source = shown_text(text != nullptr ? text : "");
    const size_t length = std::strlen(source) + 1;
    auto* copy = static_cast<char*>(std::malloc(length));
    if (copy == nullptr) {
        std::abort();
    }
    std::memcpy(copy, source, length);
    convert_escaped_newlines(copy);
    label->ops->set_caption(label, copy, text_style::wrap);
    std::free(copy);
    grow_for_text(box, label);
    window_set_name(self, message_box_name::box);
    self->visible = 1;
    self->ops->raise(self);
}

} // namespace

// Derived from tables that are constant-initialised, so copying them during
// dynamic initialisation is order-safe.
const WindowOps k_blank_button_ops = make_blank_button_ops();
const WindowOps k_message_box_ops = make_message_box_ops();

namespace {
MessageBoxOpened g_box_opened = nullptr;
}

void message_box_set_opened(MessageBoxOpened opened) {
    g_box_opened = opened;
}

MessageBox* message_box_create(
    const char* text, uint32_t buttons, bool* accepted, bool* declined, Window* focus_return
) {
    auto* box = static_cast<MessageBox*>(std::calloc(1, sizeof(MessageBox)));
    if (box == nullptr) {
        std::abort();
    }
    window_init(&box->base, &k_message_box_ops);
    // The box takes the keyboard, so its keys answer it whatever had the focus
    // (a text field, say); that window gets it back when the box closes,
    // unless the caller names another.
    Desktop* desktop = services().desktop;
    Window* had_focus = desktop != nullptr ? desktop->focus : nullptr;
    box->focus_return = focus_return != nullptr ? focus_return : had_focus;
    message_box_build(box, text, buttons, accepted, declined);
    if (desktop != nullptr) {
        services_desktop_focus(&box->base);
    }
    if (g_box_opened != nullptr)
        g_box_opened(text != nullptr ? text : "");
    return box;
}

Window* blank_button_create(Window* parent, int32_t dx, int32_t dy, const char* caption) {
    namespace layout = message_box_layout;
    Window* button = button_create();
    button->ops = &k_blank_button_ops;
    window_set_bounds(
        button,
        parent->x + dx,
        parent->y + dy,
        layout::button_width,
        layout::button_height,
        parent,
        true
    );
    window_set_state_count(button, k_blank_button_states);
    window_set_state(button, 0);
    bind_images(button, message_box_resource::gaf, message_box_resource::button_face);
    window_set_font(button, message_box_resource::font);
    const char* face = shown_text(caption != nullptr ? caption : "");
    for (int32_t state = 0; state < k_blank_button_states; ++state) {
        window_set_state_text(button, state, face, 0);
    }
    return button;
}

void convert_escaped_newlines(char* text) {
    for (; *text != '\0'; ++text) {
        if (text[0] == '\\' && text[1] == 'n') {
            text[0] = '\r';
            text[1] = '\n';
        }
    }
}

MessageBox* message_box_find_open(Desktop* desktop) {
    if (desktop == nullptr) {
        return nullptr;
    }
    for (int32_t i = desktop->windows.count - 1; i >= 0; --i) {
        Window* w = desktop->windows.items[i];
        if (w != nullptr && w->ops == &k_message_box_ops && w->owned == 0) {
            return as_box(w);
        }
    }
    return nullptr;
}

void message_box_keep_in_front(Desktop* desktop) {
    if (desktop == nullptr)
        return;
    // Raising moves a window inside the live list, so the open boxes are
    // copied first. Oldest first leaves the previous top box on top.
    std::vector<Window*> boxes;
    const int32_t window_count = desktop->windows.count;
    if (window_count > 0)
        boxes.reserve(static_cast<std::size_t>(window_count));
    for (int32_t i = 0; i < window_count; ++i) {
        Window* box_window = desktop->windows.items[i];
        if (box_window != nullptr && box_window->ops == &k_message_box_ops &&
            box_window->owned == 0)
            boxes.push_back(box_window);
    }
    for (Window* box_window : boxes)
        desktop_raise(desktop, box_window);
}

} // namespace oa::boneyards

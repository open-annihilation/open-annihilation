// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// HTMLView and its HTMLSlider.
//
// The view owns a markup document laid out to its width less the slider,
// paints it once into the document's canvas and draws the slice the slider
// selects. The slider's index runs from the bottom (0) to the top of the
// document (range - 1), because its increment button sits above the
// decrement button.

#include "html_view.hpp"

#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "console_state.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/message_box.hpp"
#include "oa/present/blit.hpp"
#include "oa/present/raster.hpp"
#include "oa/present/surface.hpp"
#include "widgets.hpp"

namespace oa::boneyards::console {

namespace {

constexpr const char* k_view_name = "HTMLView";
constexpr const char* k_slider_name = "HTMLSlider";
// The slider is 0x11 wide, 0x12 in from the view's right edge; the document
// is laid out 0x17 narrower than the view.
constexpr int32_t k_slider_inset = 0x12;
constexpr int32_t k_slider_width = 0x11;
constexpr int32_t k_document_inset = 0x17;
// A page step is the view height less this.
constexpr int32_t k_page_overlap = 10;
// Slider buttons: arrows 9 high, a thumb 0x1e high on sliders taller than
// 0x59 (0x11 otherwise), all from Widgets.gaf "scrollbar".
constexpr int32_t k_arrow_height = 9;
constexpr int32_t k_tall_slider = 0x59;
constexpr int32_t k_tall_thumb = 0x1e;
constexpr int32_t k_short_thumb = 0x11;
constexpr const char* k_widgets_gaf = "Widgets.gaf";
constexpr const char* k_scrollbar_sequence = "scrollbar";
constexpr int32_t k_inc_frame = 0;
constexpr int32_t k_dec_frame = 4;
constexpr int32_t k_tall_thumb_frame = 8;
constexpr int32_t k_short_thumb_frame = 12;
constexpr uint8_t k_scrollbar_key = 9;
// The track is a vertical line in the bar's own image. Enabled states share
// one line; the disabled state uses a dimmer one.
constexpr int32_t k_track_x = 8;
constexpr int32_t k_track_top = 0xb;
constexpr int32_t k_track_bottom_inset = 0xc;
constexpr uint8_t k_track_color = 0x5a;
constexpr uint8_t k_track_disabled_color = 0x5d;
// Without a host text service markup text is set in the game's FNT fonts,
// the larger one from 16 pixels.
constexpr const char* k_regular_font = "fonts/hatt12.fnt";
constexpr const char* k_large_font = "fonts/hatt14.fnt";
constexpr int32_t k_large_font_from = 16;

// Last slider index any view acted on (initially -1); one value shared by
// every view, as in 3.1c.
int32_t g_shown_index = -1;

HtmlView* as_view(Window* w) {
    return reinterpret_cast<HtmlView*>(w);
}

bool load_font(StatusWidget* owner, const char* path, oa::formats::fnt::Font* out) {
    std::vector<uint8_t> bytes;
    const FileReader& files = owner->config.files;
    if (files.read == nullptr || !files.read(files.user, path, &bytes))
        return false;
    auto parsed = oa::formats::fnt::parse_fnt(bytes);
    if (!parsed.ok())
        return false;
    *out = std::move(*parsed.value);
    return true;
}

bool read_file(void* user, const char* path, std::vector<uint8_t>* out) {
    const FileReader& files = static_cast<StatusWidget*>(user)->config.files;
    if (files.read != nullptr && files.read(files.user, path, out))
        return true;
    // A Help page names a picture as Boneyards\Images\I-Battle.png. The disk
    // spells that images/I-battle.png. The session reader turns the
    // backslashes into separators and matches each component without regard
    // to case, which a reader that opens the path as written does not.
    return rover_session_read_file(path, out);
}

// 3.1c shows decoder notices in a system message box titled "PNG
// DECOMPRESS"; a Boneyards message box takes its place.
void show_notice(void*, const char*, const char* text) {
    message_box_create(text, message_box_buttons::ok, nullptr, nullptr, nullptr);
}

html::Environment environment(StatusWidget* owner, HtmlViewState* state) {
    html::Environment env{};
    state->text_fonts.service = owner->config.text_fonts;
    state->text_fonts.fallback = state->has_fonts ? &state->fonts : nullptr;
    env.text = html::service_text_backend(&state->text_fonts);
    env.files = html::FileReader{owner, read_file};
    if (owner->config.palette != nullptr)
        env.palette = html::markup_palette(*owner->config.palette);
    env.notices = html::Notifier{nullptr, show_notice};
    return env;
}

/// Points the view's region at the rows of the canvas the slider selects.
///
/// @param[in,out] view HTML view
void html_view_update_region(HtmlView* view) {
    html::Document& doc = view->state->doc;
    const Surface* canvas = html::document_surface(doc);
    if (canvas == nullptr) {
        view->region = Surface{};
        return;
    }
    const Slider* slider = view->slider;
    int32_t height = html::document_height(doc);
    if (view->base.height <= height)
        height = view->base.height;
    const int32_t top = (slider_range(slider) - slider_index(slider)) - 1;
    oa::present::init_surface(
        view->region,
        canvas->width,
        height,
        canvas->pitch,
        canvas->pixels + static_cast<std::ptrdiff_t>(canvas->pitch) * top
    );
}

/// Reports a click on a link to the parent.
///
/// The document point is taken one row lower than the slice the view draws.
///
/// @param self the HTML view
/// @param msg mouse message
/// @return true always
bool html_view_left_down(Window* self, const Message* msg) {
    HtmlView* view = as_view(self);
    const int32_t x = msg->x - self->x;
    const int32_t y =
        ((msg->y - self->y) - slider_index(view->slider)) + slider_range(view->slider);
    const char* link = html::document_link_at(view->state->doc, x, y);
    if (link != nullptr && self->parent != nullptr) {
        view->link = link;
        Notify notify{};
        notify.code = k_html_link_notify;
        notify.source = self;
        self->parent->ops->on_notify(self->parent, &notify);
    }
    return true;
}

/// Sends Page Up, Page Down, Up and Down to the slider; other keys take the default path.
///
/// @param self the HTML view
/// @param msg key message
/// @return whether the key was consumed
bool html_view_key_down(Window* self, const Message* msg) {
    const int32_t k = msg->key;
    if (k < key::page_up || (k > key::page_down && k != key::up && k != key::down))
        return window_default_key_down(self, msg);
    Window* slider = &as_view(self)->slider->base;
    return slider->ops->key_down(slider, msg);
}

// One run of a link on a single baseline. Neighbouring runs of the same
// link, with a gap of at most this many pixels, are one stop.
constexpr int32_t k_link_gap = 8;
// The words a link stop is announced as. Longer text is cut.
constexpr int32_t k_link_words = 160;

struct LinkSpan {
    const char* href{};
    int32_t x{};
    int32_t top{};
    int32_t width{};
    int32_t height{};
    int32_t bottom{};
    char words[k_link_words]{};
};

/// Appends `text` to `dest`, which already holds a C string, without passing
/// `cap` bytes including the terminating NUL.
void append_capped(char* dest, int32_t cap, const char* text) {
    if (dest == nullptr || cap <= 1 || text == nullptr || text[0] == '\0')
        return;
    int32_t used = 0;
    while (used < cap - 1 && dest[used] != '\0')
        ++used;
    int32_t i = 0;
    while (used < cap - 1 && text[i] != '\0')
        dest[used++] = text[i++];
    dest[used] = '\0';
}

/// Joins `more` onto `dest` with one space. An empty piece adds nothing, so
/// an image merged into a text link does not leave a stray space.
void join_words(char* dest, int32_t cap, const char* more) {
    if (dest == nullptr || more == nullptr || more[0] == '\0')
        return;
    if (dest[0] != '\0')
        append_capped(dest, cap, " ");
    append_capped(dest, cap, more);
}

/// The words of one flow item.
///
/// A text run and the pieces `head.next` joins are one word split by inline
/// markup, so they are concatenated with no space. An image has none.
void words_of(const html::Document& doc, html::NodeRef node, char* out, int32_t cap) {
    if (out == nullptr || cap <= 0)
        return;
    out[0] = '\0';
    int32_t guard = 0;
    html::NodeRef cursor = node;
    while (cursor.kind == html::NodeKind::text && guard < 32) {
        if (cursor.index < 0 || static_cast<std::size_t>(cursor.index) >= doc.texts.size())
            break;
        const html::TextRun& run = doc.texts[static_cast<std::size_t>(cursor.index)];
        append_capped(out, cap, run.text);
        cursor = run.head.next;
        ++guard;
    }
}

const html::NodeHeader* link_header(const html::Document& doc, html::NodeRef node) {
    if (node.kind == html::NodeKind::text) {
        if (node.index < 0 || static_cast<std::size_t>(node.index) >= doc.texts.size())
            return nullptr;
        return &doc.texts[static_cast<std::size_t>(node.index)].head;
    }
    if (node.kind == html::NodeKind::image) {
        if (node.index < 0 || static_cast<std::size_t>(node.index) >= doc.images.size())
            return nullptr;
        return &doc.images[static_cast<std::size_t>(node.index)].head;
    }
    return nullptr;
}

bool link_box(
    const html::Document& doc,
    html::NodeRef node,
    int32_t* x,
    int32_t* bottom,
    int32_t* width,
    int32_t* height,
    const char** href
) {
    const html::NodeHeader* head = link_header(doc, node);
    if (head == nullptr || head->href == nullptr || head->href[0] == '\0')
        return false;
    int32_t box_width = 0;
    int32_t box_height = 0;
    if (node.kind == html::NodeKind::text) {
        const html::TextRun& run = doc.texts[static_cast<std::size_t>(node.index)];
        box_width = run.cx;
        box_height = run.cy;
    } else {
        const html::Image& image = doc.images[static_cast<std::size_t>(node.index)];
        box_width = image.width;
        box_height = image.height;
    }
    if (box_width <= 0 || box_height <= 0 || head->x < 0 || head->y < 0)
        return false;
    *x = head->x;
    *bottom = head->y;
    *width = box_width;
    *height = box_height;
    *href = head->href;
    return true;
}

void append_span(
    LinkSpan* spans,
    int32_t* count,
    const char* href,
    int32_t x,
    int32_t bottom,
    int32_t width,
    int32_t height,
    const char* words
) {
    const int32_t top = bottom - height;
    if (*count > 0) {
        LinkSpan& prev = spans[*count - 1];
        const int32_t gap = x - (prev.x + prev.width);
        if (std::strcmp(prev.href, href) == 0 && prev.bottom == bottom && gap <= k_link_gap &&
            gap >= -1) {
            const int32_t right = x + width;
            if (right > prev.x + prev.width)
                prev.width = right - prev.x;
            if (top < prev.top) {
                prev.height += prev.top - top;
                prev.top = top;
            }
            join_words(prev.words, k_link_words, words);
            return;
        }
    }
    if (*count >= k_html_link_stop_cap)
        return;
    spans[*count] = LinkSpan{href, x, top, width, height, bottom};
    append_capped(spans[*count].words, k_link_words, words);
    *count += 1;
}

void collect_link_spans(const html::Document& doc, LinkSpan* spans, int32_t* count) {
    *count = 0;
    for (const html::Flow& flow : doc.flows) {
        for (const html::NodeRef& item : flow.items) {
            if (item.kind != html::NodeKind::text && item.kind != html::NodeKind::image)
                continue;
            int32_t x = 0;
            int32_t bottom = 0;
            int32_t width = 0;
            int32_t height = 0;
            const char* href = nullptr;
            if (!link_box(doc, item, &x, &bottom, &width, &height, &href))
                continue;
            char words[k_link_words];
            words_of(doc, item, words, k_link_words);
            append_span(spans, count, href, x, bottom, width, height, words);
        }
    }
}

/// A link stop is for the keyboard. The pointer reaches the page under it,
/// so a click on a link is the page's, as before.
bool link_stop_contains(Window* /*self*/, int32_t /*x*/, int32_t /*y*/) {
    return false;
}

const WindowOps* link_stop_ops() {
    static const WindowOps ops = [] {
        WindowOps made = k_hotspot_button_ops;
        made.contains = link_stop_contains;
        return made;
    }();
    return &ops;
}

void destroy_link_stops(HtmlView* view) {
    // A followed link rebuilds the page while that stop still has the
    // keyboard. After a click the page keeps it, as before. After Return the
    // console's list takes it, so the keyboard stays in the console.
    Desktop* desktop = services().desktop;
    if (desktop != nullptr) {
        for (int32_t i = 0; i < view->link_stop_count; ++i) {
            if (desktop->focus == view->link_stops[i]) {
                Window* home = view->keyboard_home;
                const bool keyed =
                    desktop->pointer_last == 0 && home != nullptr && window_is_shown(home);
                desktop_set_focus(desktop, keyed ? home : &view->base);
                break;
            }
        }
    }
    for (int32_t i = 0; i < view->link_stop_count; ++i) {
        if (view->link_stops[i] != nullptr)
            window_destroy(view->link_stops[i]);
        view->link_stops[i] = nullptr;
        view->link_hrefs[i] = nullptr;
    }
    view->link_stop_count = 0;
}

void place_link_stop(HtmlView* view, int32_t index) {
    Window* stop = view->link_stops[index];
    if (stop == nullptr)
        return;
    const int32_t screen_x = view->link_doc_x[index] + view->base.x;
    const int32_t screen_y = view->link_doc_top[index] + view->base.y + slider_index(view->slider) -
                             slider_range(view->slider);
    const int32_t right = screen_x + view->link_doc_width[index];
    const int32_t bottom = screen_y + view->link_doc_height[index];
    const int32_t view_right = view->base.x + view->base.width - k_document_inset;
    const int32_t view_bottom = view->base.y + view->base.height;
    const bool outside = view->link_doc_x[index] < 0 || view->link_doc_top[index] < 0 ||
                         right < view->base.x || screen_x > view_right || bottom < view->base.y ||
                         screen_y > view_bottom;
    stop->x = screen_x;
    stop->y = screen_y;
    stop->width = view->link_doc_width[index];
    stop->height = view->link_doc_height[index];
    stop->visible = outside ? 0 : 1;
}

void place_link_stops(HtmlView* view) {
    for (int32_t i = 0; i < view->link_stop_count; ++i)
        place_link_stop(view, i);
}

void rebuild_link_stops(HtmlView* view) {
    destroy_link_stops(view);
    if (view->state == nullptr)
        return;
    LinkSpan spans[k_html_link_stop_cap];
    int32_t count = 0;
    collect_link_spans(view->state->doc, spans, &count);
    for (int32_t i = 0; i < count; ++i) {
        Window* stop = hotspot_button_create();
        stop->ops = link_stop_ops();
        char name[16];
        std::snprintf(name, sizeof name, "Link%d", static_cast<int>(i + 1));
        window_create(
            stop,
            name,
            view->base.x,
            view->base.y,
            spans[i].width,
            spans[i].height,
            &view->base,
            false
        );
        window_set_state(stop, button_state::unpressed);
        if (spans[i].words[0] != '\0' && stop->ops != nullptr && stop->ops->set_caption != nullptr)
            stop->ops->set_caption(stop, spans[i].words, 0);
        stop->stop_when_ring = 1;
        stop->tab_place = 0;
        view->link_stops[i] = stop;
        view->link_hrefs[i] = spans[i].href;
        view->link_doc_x[i] = spans[i].x;
        view->link_doc_top[i] = spans[i].top;
        view->link_doc_width[i] = spans[i].width;
        view->link_doc_height[i] = spans[i].height;
        view->link_stop_count = i + 1;
        place_link_stop(view, i);
    }
}

void follow_link_stop(HtmlView* view, Window* source) {
    for (int32_t i = 0; i < view->link_stop_count; ++i) {
        if (view->link_stops[i] != source)
            continue;
        view->link = view->link_hrefs[i];
        if (view->base.parent == nullptr || view->base.parent->ops == nullptr ||
            view->base.parent->ops->on_notify == nullptr)
            return;
        Notify notify{};
        notify.code = k_html_link_notify;
        notify.source = &view->base;
        view->base.parent->ops->on_notify(view->base.parent, &notify);
        return;
    }
}

/// Follows the slider when its index changes, and a link hotspot when it is pressed.
///
/// @param self the HTML view
/// @param notify control notice; may be null
void html_view_on_notify(Window* self, const Notify* notify) {
    HtmlView* view = as_view(self);
    if (notify == nullptr || notify->code != NotifyCode::activated)
        return;
    if (notify->source == &view->slider->base) {
        const int32_t index = slider_index(view->slider);
        if (g_shown_index != index) {
            g_shown_index = index;
            html_view_update_region(view);
        }
        place_link_stops(view);
        return;
    }
    follow_link_stop(view, notify->source);
}

/// Draws the selected slice keyed on index 9, then the slider.
///
/// @param self the HTML view
/// @return true always
bool html_view_draw(Window* self) {
    HtmlView* view = as_view(self);
    if (self->parent == nullptr || !window_is_shown(self->parent) || !window_is_shown(self) ||
        view->region.pixels == nullptr)
        return true;
    if (Surface* target = console_images_target(view->owner->images); target != nullptr)
        oa::present::blit_surface_keyed(target, view->region, self->x, self->y, k_html_view_key);
    Window* slider = &view->slider->base;
    if (window_is_shown(slider))
        slider->ops->draw(slider);
    return true;
}

/// Releases the document and the window.
///
/// Link stops are children, so the window teardown frees them before this
/// runs. Freeing them again would use the dead windows.
///
/// @param self the HTML view
void html_view_destroy(Window* self) {
    HtmlView* view = as_view(self);
    view->link_stop_count = 0;
    html::service_fonts_clear(view->state->text_fonts);
    delete view->state;
    console_window_free(self);
}

/// Drops a window's background colour so the green console layer shows through it.
///
/// @param[in,out] w window whose colour is released
void drop_window_colour(Window* w) {
    color_release(w->color);
    w->color = k_no_handle;
}

/// A blank image of the bar's size, transparent throughout. The caller drops the creation reference
/// once the bar's states hold the image.
///
/// @param w the slider
/// @return the image, or k_no_handle when it cannot be made
Handle blank_slider_image(const Window* w) {
    return image_create_blank(w->width, w->height, k_scrollbar_key, k_scrollbar_key);
}

/// Paints the track line into an image the bar already shows.
///
/// @param image image to draw into
/// @param height bar height in pixels
/// @param color palette index of the line
void draw_track(Handle image, int32_t height, uint8_t color) {
    Surface surface{};
    if (image_surface(image, &surface))
        (void)present::draw_clipped_line(
            &surface, k_track_x, k_track_top, k_track_x, height - k_track_bottom_inset, color
        );
}

/// Redraws the track images (dimmer while disabled), keeps the arrows at the ends of the bar and
/// re-centres the thumb.
///
/// @param self the HTMLSlider
/// @return true always
bool html_slider_refresh(Window* self) {
    const Handle enabled = blank_slider_image(self);
    window_set_state_image(self, button_state::pressed, enabled);
    window_set_state_image(self, button_state::unpressed, enabled);
    draw_track(enabled, self->height, k_track_color);
    image_release(enabled);
    const Handle disabled = blank_slider_image(self);
    window_set_state_image(self, button_state::disabled, disabled);
    draw_track(disabled, self->height, k_track_disabled_color);
    image_release(disabled);
    if (Window* inc = window_find_named(self, "incbutton", nullptr))
        inc->y = self->y;
    if (Window* dec = window_find_named(self, "decbutton", nullptr))
        dec->y = self->height - k_arrow_height + self->y;
    auto* slider = reinterpret_cast<Slider*>(self);
    slider_set_index(slider, slider_index(slider));
    return true;
}

/// Creates the HTMLSlider's buttons: the thumb, the increment arrow at the top and the decrement
/// arrow at the bottom.
///
/// The slider starts empty, disabled and hidden, and without a background
/// colour. Its track image is keyed, so the green console layer shows through
/// every pixel except the track line.
///
/// @param self the HTMLSlider
void html_slider_create_buttons(Window* self) {
    drop_window_colour(self);
    const bool tall = self->height > k_tall_slider;
    Window* thumb = scroll_button_create();
    window_create(
        thumb,
        "sbutton",
        self->x,
        self->y + k_arrow_height,
        k_slider_width,
        tall ? k_tall_thumb : k_short_thumb,
        self,
        true
    );
    console_bind_images(
        thumb,
        k_widgets_gaf,
        k_scrollbar_sequence,
        tall ? k_tall_thumb_frame : k_short_thumb_frame,
        k_scrollbar_key
    );
    Window* inc = scroll_button_create();
    window_create(inc, "incbutton", self->x, self->y, k_slider_width, k_arrow_height, self, true);
    console_bind_images(inc, k_widgets_gaf, k_scrollbar_sequence, k_inc_frame, k_scrollbar_key);
    Window* dec = scroll_button_create();
    window_create(
        dec,
        "decbutton",
        self->x,
        self->y + self->height - k_arrow_height,
        k_slider_width,
        k_arrow_height,
        self,
        true
    );
    console_bind_images(dec, k_widgets_gaf, k_scrollbar_sequence, k_dec_frame, k_scrollbar_key);
    auto* slider = reinterpret_cast<Slider*>(self);
    slider_set_index(slider, 0);
    slider_set_range(slider, 1, false);
    self->ops->refresh(self);
    slider_disable(self);
    self->visible = 0;
}

WindowOps make_view_ops() {
    WindowOps ops = console_panel_ops();
    ops.destroy = html_view_destroy;
    ops.on_notify = html_view_on_notify;
    ops.draw = html_view_draw;
    ops.key_down = html_view_key_down;
    ops.left_down = html_view_left_down;
    return ops;
}

WindowOps make_slider_ops() {
    WindowOps ops = k_slider_ops;
    ops.on_bounds = html_slider_create_buttons;
    ops.refresh = html_slider_refresh;
    return ops;
}

const WindowOps k_view_ops = make_view_ops();
const WindowOps k_html_slider_ops = make_slider_ops();

} // namespace

HtmlView* html_view_create(
    StatusWidget* owner, Window* parent, int32_t dx, int32_t dy, int32_t width, int32_t height
) {
    auto* view = reinterpret_cast<HtmlView*>(console_window_alloc(sizeof(HtmlView), &k_view_ops));
    view->owner = owner;
    view->state = new HtmlViewState{};
    HtmlViewState* state = view->state;
    state->has_fonts = load_font(owner, k_regular_font, &state->regular);
    state->fonts.regular = &state->regular;
    if (load_font(owner, k_large_font, &state->large)) {
        state->fonts.large = &state->large;
        state->fonts.large_from = k_large_font_from;
    }
    // The bundled faces, when they cannot be opened, leave the FNT fonts.
    // Characters those fonts lack are drawn from the same chain the faces use.
    state->scripts.user = html::page_script_fonts();
    if (state->scripts.user != nullptr) {
        state->scripts.lookup = html::fallback_lookup;
        state->fonts.scripts = &state->scripts;
    }
    html::document_init(state->doc, environment(owner, state));
    window_create(
        &view->base, k_view_name, parent->x + dx, parent->y + dy, width, height, parent, false
    );
    Slider* slider = html_slider_create();
    view->slider = slider;
    window_create(
        &slider->base,
        k_slider_name,
        view->base.x - k_slider_inset + view->base.width,
        view->base.y,
        k_slider_width,
        view->base.height,
        &view->base,
        false
    );
    slider_set_range(slider, 1, false);
    slider_set_index(slider, 0);
    slider->page_step = view->base.height - k_page_overlap;
    slider_disable(&slider->base);
    html::document_set_width(state->doc, view->base.width - k_document_inset);
    return view;
}

void html_view_set_text(HtmlView* view, const char* text) {
    HtmlViewState* state = view->state;
    // Without fonts nothing can be measured, so nothing is shown.
    if (!html::service_fonts_usable(state->text_fonts))
        return;
    html::Document& doc = state->doc;
    html::document_load(doc, text);
    html::document_paint(doc);
    html::document_pending_images(doc, &state->pending);
    const int32_t height = html::document_height(doc);
    Slider* slider = view->slider;
    if (view->base.height < height) {
        slider_set_range(slider, (height - view->base.height) + 1, false);
        slider_set_index(slider, slider_range(slider));
        slider->base.visible = 1;
        slider_enable(&slider->base);
    } else {
        slider_set_range(slider, 1, false);
        slider_set_index(slider, 0);
        slider->base.visible = 0;
    }
    html_view_update_region(view);
    rebuild_link_stops(view);
}

void html_view_set_pair(HtmlView* view, const char* name, const char* path) {
    if (!html::service_fonts_usable(view->state->text_fonts))
        return;
    html::document_reload_image(view->state->doc, name, path);
    html::document_paint(view->state->doc);
    html_view_update_region(view);
    rebuild_link_stops(view);
}

Slider* html_slider_create() {
    Slider* slider = slider_create();
    slider->base.ops = &k_html_slider_ops;
    return slider;
}

const char* html_view_pop_pending(HtmlView* view) {
    std::vector<const char*>& pending = view->state->pending;
    if (pending.empty())
        return nullptr;
    const char* name = pending.back();
    pending.pop_back();
    return name;
}

} // namespace oa::boneyards::console

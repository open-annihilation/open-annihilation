// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Main Frame -> Help console with the BY1117a data: the contents file fills
// the list and opens the first page, a category row shows its headlines, a
// headline opens its page with its IMG decoded through the PNG codec, and a
// link inside a page opens its target. Then the News console against a
// service with content servers: its remote contents file and pages are
// downloaded to temporary files that arrive through the service's events
// and are deleted once read; and the Ladders console, whose first page
// comes from the ladder server. Last, the shipped Help contents opened,
// closed and scrolled: one title per row, the arrows centred on their
// titles, the scroll bar along the list's right edge with its thumb sized
// and placed by the rows in view, and the shown page's headline lit. With
// --frames DIR each of those states is written there as a picture.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "console_layer.hpp"
#include "console_state.hpp"
#include "oa/boneyards/message_box.hpp"
#include "html_view.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/boneyards_test/installs.hpp"
#include "oa/boneyards/console/status_widget.hpp"

using namespace oa::boneyards;
using namespace oa::boneyards::console;

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

struct Data {
    oa::AssetStore store{oa::boneyards_test::by_install()};
    oa::Palette palette{};
};

bool read_asset(void* user, const char* path, std::vector<uint8_t>* out) {
    try {
        *out = static_cast<Data*>(user)->store.read(path).bytes;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool connected(void*) {
    return true;
}

ActivationResult activate(void*) {
    return ActivationResult::ok;
}

std::vector<std::string> g_sounds;

void record_sound(void*, const char* name) {
    g_sounds.emplace_back(name);
}

Message pointer(MessageCode code, int32_t x, int32_t y, uint32_t buttons) {
    Message msg{};
    msg.code = code;
    msg.x = x;
    msg.y = y;
    msg.buttons = buttons;
    return msg;
}

void click_at(StatusWidget* widget, int32_t x, int32_t y) {
    boneyards_status_widget_event(widget, pointer(MessageCode::left_down, x, y, button_left));
    boneyards_status_widget_event(widget, pointer(MessageCode::left_up, x, y, 0));
}

void click_window(StatusWidget* widget, const Window* w) {
    click_at(widget, w->x + w->width / 2, w->y + w->height / 2);
}

void press(StatusWidget* widget, int32_t k) {
    Message msg{};
    msg.code = MessageCode::key_down;
    msg.key = k;
    boneyards_status_widget_event(widget, msg);
    msg.code = MessageCode::key_up;
    boneyards_status_widget_event(widget, msg);
}

StatusWidget* g_widget = nullptr;

// Captions live in the widget's text service.
std::string caption(Window* w) {
    ScopedServices scope(g_widget);
    const char* text = w->ops->state_text(w, -1);
    return text != nullptr ? text : "";
}

// The row of the contents list whose title (its last child; a category
// row's arrow comes first) reads `title`, or null.
Window* row_titled(ListBox* list, const char* title) {
    for (int32_t i = 0; i < list->count; ++i) {
        Window* row = list_box_item(list, i);
        if (row != nullptr && row->children.count > 0 &&
            caption(row->children.items[row->children.count - 1]) == title)
            return row;
    }
    return nullptr;
}

// Pixels of the image a window shows in its current state.
std::vector<uint8_t> shown_pixels(Window* w) {
    ScopedServices scope(g_widget);
    Surface surface{};
    std::vector<uint8_t> pixels;
    if (w->states.current >= 0 && w->states.current < w->states.count &&
        image_surface(w->states.images[w->states.current], &surface))
        for (int32_t y = 0; y < surface.height; ++y)
            for (int32_t x = 0; x < surface.width; ++x)
                pixels.push_back(surface.pixels[y * surface.pitch + x]);
    return pixels;
}

std::string_view markup(const HtmlView* view) {
    const std::vector<char>& buffer = view->state->doc.buffer;
    return std::string_view(buffer.data(), buffer.size());
}

// Shown point of the first link in the view, found through the same
// document mapping the view's click handler uses.
bool find_link(HtmlView* view, int32_t* x, int32_t* y) {
    const Window& w = view->base;
    for (int32_t py = w.y; py < w.y + w.height; ++py)
        for (int32_t px = w.x; px < w.x + w.width - 0x17; ++px) {
            const int32_t doc_y = ((py - w.y) - view->slider->index) + view->slider->range;
            if (html::document_link_at(view->state->doc, px - w.x, doc_y) != nullptr) {
                *x = px;
                *y = py;
                return true;
            }
        }
    return false;
}

int32_t draw_frame(StatusWidget* widget, std::vector<uint8_t>* pixels) {
    pixels->assign(640 * 480, 0);
    oa::Surface surface{};
    surface.width = 640;
    surface.height = 480;
    surface.pitch = 640;
    surface.pixels = pixels->data();
    surface.clip = oa::Rect32{0, 0, 639, 479};
    boneyards_status_widget_draw(widget, &surface);
    int32_t painted = 0;
    for (uint8_t index : *pixels)
        painted += index != 0 ? 1 : 0;
    return painted;
}

void test_help_console(Data* data) {
    StatusWidgetConfig config{};
    config.files = FileReader{data, read_asset};
    config.palette = &data->palette;
    config.host.play_sound = record_sound;
    config.service.network_available = connected;
    config.service.activate = activate;
    config.service.connected = connected;
    StatusWidget* widget = boneyards_status_widget_create(config);
    g_widget = widget;
    check(widget != nullptr, "status widget");
    if (widget == nullptr)
        return;
    click_window(widget, boneyards_status_widget_part(widget, OverlayPart::enter));
    MainFrame* frame = widget->system.main_frame;
    check(frame != nullptr, "Main Frame shown");
    if (frame == nullptr)
        return;
    for (int32_t i = 0; frame->console_screen != console_screen::login && i < 160; ++i)
        boneyards_status_widget_tick(widget);
    check(frame->console_screen == console_screen::login, "login console shown");

    // With the console buttons on, as a login leaves them, Help replaces the
    // login console under the covering green layer.
    {
        ScopedServices scope(widget);
        main_frame_enable_buttons(frame);
    }
    click_window(widget, frame->console_buttons[static_cast<int32_t>(ConsoleButton::help)]);
    check(frame->console_screen == console_screen::help, "help console opened");
    Window* console = frame->active_screen;
    check(console != nullptr && window_is_named(console, "HelpConsole"), "HelpConsole built");
    if (console == nullptr)
        return;
    auto* help_button = reinterpret_cast<ConsoleNavButton*>(
        frame->console_buttons[static_cast<int32_t>(ConsoleButton::help)]
    );
    check(
        help_button->lit >= 0 && help_button->normal >= 0 &&
            help_button->lit != help_button->normal,
        "console_button_create keeps the lit frame 0 and the normal frame 2"
    );
    check(
        help_button->base.states.images[button_state::unpressed] == help_button->lit,
        "contents_console_open lights the open console's Main Frame button"
    );
    auto* view = reinterpret_cast<HtmlView*>(window_find_named(console, "HTMLView", nullptr));
    auto* list = reinterpret_cast<ListBox*>(window_find_named(console, "ContentsList", nullptr));
    check(view != nullptr && list != nullptr, "view and contents list");
    if (view == nullptr || list == nullptr)
        return;
    check(view->base.x == console->x + 0x81 && view->base.y == console->y + 5, "view placement");
    check(view->base.width == console->width - 0x83, "view width");
    check(
        list->base.x == 0x17 && list->base.y == 0x93 && list->base.width == 0x7c, "list placement"
    );
    check(
        window_find_named(&view->base, "HTMLSlider", nullptr) == &view->slider->base, "view slider"
    );

    // Help.def: six categories, the first opened with its headlines, and
    // the first headline's page in the view.
    check(list->count == 12, "six categories and the first one's six headlines");
    check(row_titled(list, "Getting Started") != nullptr, "first category row");
    check(row_titled(list, "Where Do I Begin?") != nullptr, "first headline row");
    check(row_titled(list, "Login Screen") == nullptr, "closed category hides its headlines");
    Window* open_category = row_titled(list, "Getting Started");
    Window* closed_category = row_titled(list, "Screen Help");
    check(open_category != nullptr && closed_category != nullptr, "category rows");
    if (open_category != nullptr && closed_category != nullptr) {
        Window* open_icon = open_category->children.items[0];
        Window* closed_icon = closed_category->children.items[0];
        check(
            window_is_named(open_icon, "CategoryIcon") && open_icon->width == 0xb &&
                open_icon->height == 0xb,
            "category_row_create CategoryIcon"
        );
        check(
            open_icon->x == open_category->x && open_icon->y == open_category->y + 3,
            "category_row_create the arrow at the row's left edge, 3 pixels below its top"
        );
        boneyards_status_widget_tick(widget);
        const std::vector<uint8_t> down = shown_pixels(open_icon);
        const std::vector<uint8_t> up = shown_pixels(closed_icon);
        check(
            !down.empty() && down.size() == up.size() && down != up,
            "category_row_pulse Icon_Arrow_Down on an open category, Icon_Arrow_Up on a closed one"
        );
    }
    check(markup(view).find("Welcome") != std::string_view::npos, "first page loaded");
    check(view->region.pixels != nullptr, "page painted");
    check(
        frame->activity_light == nullptr || frame->activity_light->running == 0,
        "activity light stops once the page is in"
    );

    // Until Tab or F6, Down from Help does not enter the contents list, so
    // Return can still open News. After F6 the list chooses, opens and
    // closes, and Return follows a link on the page.
    check(widget->desktop.focus == &help_button->base, "Help keeps the keyboard");
    check(widget->desktop.keyboard_ring == 0, "no keyboard place before Tab or F6");
    const int32_t rows_before = list->count;
    const int32_t selected_before = list->selected;
    press(widget, key::down);
    check(
        widget->desktop.focus != &list->base && frame->console_screen == console_screen::help &&
            list->count == rows_before && list->selected == selected_before &&
            widget->desktop.keyboard_ring == 0,
        "Down from Help leaves the contents list alone"
    );
    {
        ScopedServices scope(widget);
        desktop_set_focus(&widget->desktop, &help_button->base);
    }
    press(widget, key::f6);
    check(
        widget->desktop.focus == &list->base && widget->desktop.keyboard_ring == 1,
        "F6 lands on the contents list"
    );
    const int32_t chosen = list->selected;
    press(widget, key::down);
    check(
        list->selected != chosen && frame->console_screen == console_screen::help &&
            markup(view).find("Welcome") != std::string_view::npos,
        "Down chooses the next row and does not open it"
    );
    bool found_screen = false;
    for (int32_t step = 0; step < list->count + 2; ++step) {
        Window* row = list_box_item(list, list->selected);
        if (row != nullptr && caption(row) == "+ Screen Help") {
            found_screen = true;
            break;
        }
        press(widget, key::down);
    }
    check(found_screen, "Down reaches the closed Screen Help category");
    const int32_t closed_rows = list->count;
    press(widget, key::right);
    check(
        row_titled(list, "Login Screen") != nullptr && list->count > closed_rows,
        "Right opens Screen Help"
    );
    Window* screen_row = list_box_item(list, list->selected);
    check(
        screen_row != nullptr && caption(screen_row) == "- Screen Help",
        "the open category stays selected"
    );
    press(widget, key::left);
    check(
        row_titled(list, "Login Screen") == nullptr && list->count == closed_rows &&
            row_titled(list, "Getting Started") != nullptr,
        "Left closes Screen Help and leaves Getting Started open"
    );
    // Screens Help, under Galactic War Help, is the page whose link the click
    // below follows. Home walks from the top, because that category sits
    // above Screen Help and Down does not wrap.
    press(widget, key::home);
    bool found_war = false;
    for (int32_t step = 0; step < list->count + 2 && !found_war; ++step) {
        Window* row = list_box_item(list, list->selected);
        if (row != nullptr && caption(row) == "+ Galactic War Help")
            found_war = true;
        else
            press(widget, key::down);
    }
    check(found_war, "Down reaches the closed Galactic War Help category");
    {
        std::vector<uint8_t> pixels;
        draw_frame(widget, &pixels);
        Window* war = row_titled(list, "Galactic War Help");
        Window* title = war != nullptr && war->children.count > 0
                            ? war->children.items[war->children.count - 1]
                            : nullptr;
        check(
            title != nullptr && title->font == widget->system.hat_fonts.small_light,
            "the chosen category takes the light font while the ring shows"
        );
        {
            ScopedServices scope(widget);
            widget->desktop.keyboard_ring = 0;
        }
        draw_frame(widget, &pixels);
        check(
            title != nullptr && title->font == k_no_handle,
            "hiding the ring restores the category's font"
        );
        {
            ScopedServices scope(widget);
            widget->desktop.keyboard_ring = 1;
        }
    }
    press(widget, key::right);
    bool found_screens = false;
    for (int32_t step = 0; step < list->count + 2 && !found_screens; ++step) {
        Window* row = list_box_item(list, list->selected);
        if (row != nullptr && caption(row) == "Screens Help")
            found_screens = true;
        else
            press(widget, key::down);
    }
    check(found_screens, "Down reaches Screens Help");
    press(widget, key::enter);
    check(markup(view).find("Help112.htm") != std::string_view::npos, "Return opens Screens Help");
    bool followed = false;
    for (int32_t step = 0; step < 40 && !followed; ++step) {
        press(widget, key::tab);
        Window* focus = widget->desktop.focus;
        if (focus == nullptr || focus->name == nullptr ||
            std::strncmp(focus->name, "Link", 4) != 0 || focus->visible == 0)
            continue;
        const std::string words = caption(focus);
        check(
            !words.empty() && (words.size() < 4 || words.compare(0, 4, "Link") != 0),
            "a link stop is announced as its words"
        );
        const std::string before_link(markup(view));
        press(widget, key::enter);
        followed = std::string(markup(view)) != before_link;
    }
    check(followed, "Return follows a link on the page");
    check(widget->desktop.focus == &list->base, "after Return on a link the list has the keyboard");
    // Close the category again so the click below still opens it.
    {
        ScopedServices scope(widget);
        desktop_set_focus(&widget->desktop, &list->base);
        widget->desktop.keyboard_ring = 1;
    }
    press(widget, key::home);
    bool closed_war = false;
    for (int32_t step = 0; step < list->count + 2 && !closed_war; ++step) {
        Window* row = list_box_item(list, list->selected);
        if (row != nullptr && caption(row) == "- Galactic War Help") {
            press(widget, key::left);
            closed_war = row_titled(list, "Screens Help") == nullptr;
        } else {
            press(widget, key::down);
        }
    }
    check(closed_war, "Left closes Galactic War Help");
    {
        ScopedServices scope(widget);
        widget->desktop.keyboard_ring = 0;
        desktop_set_focus(&widget->desktop, &help_button->base);
    }
    check(row_titled(list, "Login Screen") == nullptr, "Screen Help stays closed for the click");

    // Opening "Screen Help" lists its headlines; "Login Screen" shows a page
    // whose IMG is decoded.
    Window* screen_help = row_titled(list, "Screen Help");
    check(screen_help != nullptr, "Screen Help category");
    if (screen_help != nullptr)
        click_window(widget, screen_help);
    check(!g_sounds.empty() && g_sounds.back() == "GREEN_BUTTON", "row click sound");
    {
        std::vector<uint8_t> pixels;
        draw_frame(widget, &pixels);
        Window* clicked_row = row_titled(list, "Screen Help");
        Window* title = clicked_row != nullptr && clicked_row->children.count > 0
                            ? clicked_row->children.items[clicked_row->children.count - 1]
                            : nullptr;
        check(
            title != nullptr && title->font == k_no_handle,
            "a click leaves the chosen category's font as it was"
        );
    }
    Window* login = row_titled(list, "Login Screen");
    check(login != nullptr, "category opened");
    if (login != nullptr)
        click_window(widget, login);
    const html::Document& doc = view->state->doc;
    check(markup(view).find("login.png") != std::string_view::npos, "Login Screen page loaded");
    check(doc.images.size() == 1, "one IMG");
    if (doc.images.size() == 1) {
        const html::Image& image = doc.images[0];
        check(
            image.loaded && image.source_width == 255 && image.source_height == 191,
            "login.png decoded"
        );
        check(image.row_bytes == 255 && image.pixels.size() == 255u * 191u, "palette rows");
    }
    std::vector<uint8_t> pixels;
    check(draw_frame(widget, &pixels) > 100000, "frame drawn");

    // The clicked headline is the list's selected row: drawing gives its
    // title the light small font (headline_row_select) and every other headline's the
    // small font (headline_row_unselect), and the rows draw their titles (list_row_draw).
    const screens::HatFonts& fonts = widget->system.hat_fonts;
    Window* begin = row_titled(list, "Where Do I Begin?");
    check(login != nullptr && begin != nullptr, "headline rows after the rebuild");
    if (login != nullptr && begin != nullptr) {
        Window* login_title = login->children.items[login->children.count - 1];
        Window* begin_title = begin->children.items[begin->children.count - 1];
        check(fonts.small_light >= 0 && fonts.small_light != fonts.small, "hat fonts loaded");
        check(
            login_title->font == fonts.small_light,
            "headline_row_select selected headline title light"
        );
        check(begin_title->font == fonts.small, "headline_row_unselect other headline title small");
        std::vector<uint8_t> untitled;
        login_title->visible = 0;
        draw_frame(widget, &untitled);
        login_title->visible = 1;
        int32_t title_pixels = 0;
        for (int32_t y = login_title->y; y < login_title->y + login_title->height; ++y)
            for (int32_t x = login_title->x; x < login_title->x + login_title->width; ++x)
                title_pixels += pixels[y * 640 + x] != untitled[y * 640 + x] ? 1 : 0;
        check(title_pixels > 0, "list_row_draw a row draws its title");
    }
    check(view->slider->base.visible == 1, "long page scrolls");
    check(view->slider->index == view->slider->range - 1, "page starts at its top");
    const uint8_t* top = view->region.pixels;
    {
        ScopedServices scope(widget);
        slider_set_value(view->slider, view->slider->index - 3);
    }
    const Surface* canvas = html::document_surface(view->state->doc);
    check(
        canvas != nullptr && view->region.pixels == top + canvas->pitch * 3,
        "the slider moves the shown rows"
    );

    // A link on "Screens Help" opens its target.
    Window* war = row_titled(list, "Galactic War Help");
    if (war != nullptr)
        click_window(widget, war);
    Window* screens_help = row_titled(list, "Screens Help");
    check(screens_help != nullptr, "Screens Help headline");
    if (screens_help != nullptr)
        click_window(widget, screens_help);
    check(markup(view).find("Help112.htm") != std::string_view::npos, "Screens Help page");
    int32_t lx = 0;
    int32_t ly = 0;
    check(find_link(view, &lx, &ly), "a link is shown");
    const std::string before(markup(view));
    click_at(widget, lx, ly);
    check(std::string(markup(view)) != before, "link opens another page");
    check(widget->desktop.focus == &view->base, "a click on a link leaves the page the keyboard");

    click_window(widget, &help_button->base);
    check(frame->console_screen == console_screen::none, "help button closes its console");
    check(
        help_button->base.states.images[button_state::unpressed] == help_button->normal,
        "console_screen_close puts the console's button back on its normal face"
    );
    auto* release = reinterpret_cast<ConsoleNavButton*>(
        frame->console_buttons[static_cast<int32_t>(ConsoleButton::release)]
    );
    check(
        release->base.states.images[button_state::unpressed] == release->normal,
        "main_frame_close_screen Release button on its normal face"
    );
    boneyards_status_widget_destroy(widget);
}

// A host text service with only Arial: an em of |height| pixels, glyphs
// em/2 wide, a cell em + em/4 tall, every pixel above the baseline inked.
struct HostFonts {
    std::vector<std::string> faces;
    std::vector<html::FontRequest> requests;
    int32_t ems[64]{};
    int32_t opened = 0;
    int32_t closed = 0;
};

void* host_open(void* user, const html::FontRequest& request) {
    auto* fonts = static_cast<HostFonts*>(user);
    fonts->faces.emplace_back(request.face);
    fonts->requests.push_back(request);
    if (std::strcmp(request.face, "Arial") != 0 || fonts->opened == 64)
        return nullptr;
    int32_t* em = &fonts->ems[fonts->opened++];
    *em = request.height < 0 ? -request.height : request.height;
    return em;
}

void host_close(void* user, void*) {
    ++static_cast<HostFonts*>(user)->closed;
}

html::FontMetrics host_metrics(void*, void* font) {
    const int32_t em = *static_cast<int32_t*>(font);
    return html::FontMetrics{em + em / 4, em, em + 1, 1};
}

int32_t host_advance(void*, void* font, const char*, int32_t length) {
    return length * (*static_cast<int32_t*>(font) / 2);
}

void host_render(
    void*, void* font, const char*, int32_t length, uint8_t* coverage, int32_t width, int32_t height
) {
    const int32_t em = *static_cast<int32_t*>(font);
    for (int32_t row = 0; row < em && row < height; ++row)
        for (int32_t col = 0; col < length * (em / 2) && col < width; ++col)
            coverage[row * width + col] = 0xff;
}

// Canvas entry nearest the default text colour, the first of equals.
uint8_t default_text_index(const oa::Palette& game) {
    const oa::Palette palette = html::markup_palette(game);
    const uint32_t rgb = html::k_default_text_colorref;
    int32_t best = -1;
    uint8_t index = 0;
    for (int32_t i = 0; i < OA_PALETTE_COLORS; ++i) {
        const int32_t dr = palette.entries[i].r - static_cast<int32_t>(rgb & 0xffu);
        const int32_t dg = palette.entries[i].g - static_cast<int32_t>((rgb >> 8) & 0xffu);
        const int32_t db = palette.entries[i].b - static_cast<int32_t>((rgb >> 16) & 0xffu);
        const int32_t distance = dr * dr + dg * dg + db * db;
        if (best < 0 || distance < best) {
            best = distance;
            index = static_cast<uint8_t>(i);
        }
    }
    return index;
}

bool requested(const HostFonts& fonts, const char* face, int32_t height, int32_t weight) {
    for (std::size_t i = 0; i < fonts.requests.size(); ++i)
        if (fonts.faces[i] == face && fonts.requests[i].height == height &&
            fonts.requests[i].weight == weight)
            return true;
    return false;
}

// With a host text service the Help pages are set in its faces: the waiting
// page's Haettenschweiler +2 walks to Arial at -16, and the first page asks
// for its SIZE="+4" bold title at -20 and its body at the default -12.
void test_help_host_fonts(Data* data) {
    HostFonts host;
    StatusWidgetConfig config{};
    config.files = FileReader{data, read_asset};
    config.palette = &data->palette;
    config.text_fonts =
        html::FontService{&host, host_open, host_close, host_metrics, host_advance, host_render};
    config.service.network_available = connected;
    config.service.activate = activate;
    config.service.connected = connected;
    StatusWidget* widget = boneyards_status_widget_create(config);
    g_widget = widget;
    check(widget != nullptr, "status widget with host fonts");
    if (widget == nullptr)
        return;
    click_window(widget, boneyards_status_widget_part(widget, OverlayPart::enter));
    MainFrame* frame = widget->system.main_frame;
    if (frame == nullptr) {
        check(false, "Main Frame shown with host fonts");
        return;
    }
    for (int32_t i = 0; frame->console_screen != console_screen::login && i < 160; ++i)
        boneyards_status_widget_tick(widget);
    {
        ScopedServices scope(widget);
        main_frame_enable_buttons(frame);
    }
    click_window(widget, frame->console_buttons[static_cast<int32_t>(ConsoleButton::help)]);
    Window* console = frame->active_screen;
    auto* view = console != nullptr
                     ? reinterpret_cast<HtmlView*>(window_find_named(console, "HTMLView", nullptr))
                     : nullptr;
    check(view != nullptr, "help view with host fonts");
    if (view == nullptr)
        return;
    check(requested(host, "Haettenschweiler", -16, html::k_weight_normal), "waiting page face");
    check(
        requested(host, "Saira ExtraCondensed ExtraBold", -16, html::k_weight_normal),
        "Haettenschweiler stand-in"
    );
    check(requested(host, "Arial", -16, html::k_weight_normal), "waiting page falls to Arial");
    check(markup(view).find("Welcome") != std::string_view::npos, "first page loaded");
    check(requested(host, "Arial", -20, html::k_weight_bold), "SIZE=+4 bold title");
    check(requested(host, "Arial", -12, html::k_weight_normal), "default body font");

    const html::Document& doc = view->state->doc;
    const html::TextRun* title = nullptr;
    for (const html::TextRun& run : doc.texts)
        if (title == nullptr && std::strcmp(run.text, "Getting") == 0)
            title = &run;
    check(title != nullptr, "title's first word");
    if (title != nullptr)
        check(
            title->cx == 7 * 10 && title->cy == 25 && title->ascent == 20, "title in the -20 face"
        );
    const Surface* canvas = html::document_surface(doc);
    const uint8_t ink = default_text_index(data->palette);
    int32_t inked = 0;
    if (canvas != nullptr)
        for (int32_t y = 0; y < canvas->height; ++y)
            for (int32_t x = 0; x < canvas->width; ++x)
                inked += canvas->pixels[y * canvas->pitch + x] == ink ? 1 : 0;
    check(inked > 1000, "page text drawn in the host faces");
    const int32_t opened = host.opened;
    boneyards_status_widget_destroy(widget);
    check(opened > 0 && host.closed == opened, "every host font closed with its view");
}

// The service's side of the downloads: slot 40's answer for the news
// server, each fetch's URL and temporary file, and the events the status
// widget installed.
struct Downloads {
    std::vector<int32_t> server_types;
    std::vector<std::string> urls;
    std::vector<std::string> files;
    ServiceEvents events{};
    int32_t next = 0;
};

Downloads g_downloads;

std::filesystem::path download_directory() {
    return std::filesystem::temp_directory_path() / "oa-contents-console-test";
}

bool content_servers(void*, int32_t type, char* host, std::size_t capacity, uint16_t* port) {
    g_downloads.server_types.push_back(type);
    if (type != kContentServerNews && type != kContentServerLadders)
        return false;
    std::snprintf(host, capacity, type == kContentServerNews ? "news.example" : "ladders.example");
    *port = type == kContentServerNews ? 80 : 8080;
    return true;
}

ServiceResult fetch_to_temp(void*, const char* url, char* dest, std::size_t capacity) {
    const std::filesystem::path file =
        download_directory() / ("0BY" + std::to_string(++g_downloads.next) + ".TMP");
    std::ofstream(file, std::ios::binary).flush();
    std::snprintf(dest, capacity, "%s", file.string().c_str());
    g_downloads.urls.emplace_back(url);
    g_downloads.files.push_back(file.string());
    return ServiceResult::pending;
}

void keep_events(void*, const ServiceEvents* events) {
    g_downloads.events = events != nullptr ? *events : ServiceEvents{};
}

// Saves `bytes` as download `index` and reports its arrival as Rover does
// from the widget's pump.
void arrive(std::size_t index, const std::string& bytes) {
    if (index >= g_downloads.files.size() || g_downloads.events.file_arrived == nullptr)
        return;
    std::ofstream(g_downloads.files[index], std::ios::binary) << bytes;
    ScopedServices scope(g_widget);
    g_downloads.events.file_arrived(
        g_downloads.events.user,
        g_downloads.urls[index].c_str(),
        g_downloads.files[index].c_str(),
        0
    );
}

std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A status widget on a service with both content servers, its Main Frame
// showing the login console with the console buttons on; null on failure.
StatusWidget* downloading_widget(Data* data) {
    g_downloads = Downloads{};
    std::filesystem::create_directories(download_directory());
    StatusWidgetConfig config{};
    config.files = FileReader{data, read_asset};
    config.palette = &data->palette;
    config.service.network_available = connected;
    config.service.activate = activate;
    config.service.connected = connected;
    config.service.content_server = content_servers;
    config.service.fetch_file = fetch_to_temp;
    config.service.set_events = keep_events;
    StatusWidget* widget = boneyards_status_widget_create(config);
    g_widget = widget;
    if (widget == nullptr)
        return nullptr;
    click_window(widget, boneyards_status_widget_part(widget, OverlayPart::enter));
    MainFrame* frame = widget->system.main_frame;
    if (frame == nullptr) {
        boneyards_status_widget_destroy(widget);
        return nullptr;
    }
    for (int32_t i = 0; frame->console_screen != console_screen::login && i < 160; ++i)
        boneyards_status_widget_tick(widget);
    ScopedServices scope(widget);
    main_frame_enable_buttons(frame);
    return widget;
}

void test_news_console(Data* data) {
    const std::filesystem::path samples(OA_BY_CLIENT_TEST_DATA);
    StatusWidget* widget = downloading_widget(data);
    check(widget != nullptr, "news: status widget and Main Frame");
    if (widget == nullptr)
        return;
    MainFrame* frame = widget->system.main_frame;
    click_window(widget, frame->console_buttons[static_cast<int32_t>(ConsoleButton::news)]);
    Window* console = frame->active_screen;
    check(console != nullptr && window_is_named(console, "NewsConsole"), "NewsConsole built");
    if (console == nullptr)
        return;
    check(
        frame->contents_console == console,
        "contents_console_create makes the new console the one documents reach"
    );
    auto* view = reinterpret_cast<HtmlView*>(window_find_named(console, "HTMLView", nullptr));
    auto* list = reinterpret_cast<ListBox*>(window_find_named(console, "ContentsList", nullptr));
    check(view != nullptr && list != nullptr, "news: view and contents list");
    if (view == nullptr || list == nullptr)
        return;

    // contents_resolve: #Remote#/boneyards/ta/news/news.def on the news server (kind
    // 0) becomes http://host:port/path and is fetched into a temporary file.
    check(
        g_downloads.server_types.size() == 1 && g_downloads.server_types[0] == kContentServerNews,
        "the News console asks for the news server"
    );
    check(
        g_downloads.urls.size() == 1 &&
            g_downloads.urls[0] == "http://news.example:80/boneyards/ta/news/news.def",
        "the contents file is fetched from http://news.example:80/boneyards/ta/news/news.def"
    );
    check(markup(view).find("Waiting") != std::string_view::npos, "waiting page");
    check(list->count == 0, "no contents before the file arrives");
    check(frame->activity_light->running != 0, "the light runs while the contents file is out");

    // contents_file_arrived: the contents file fills the list and opens the first
    // headline, whose page is fetched; the light stops and the file is
    // deleted.
    arrive(0, read_bytes(samples / "news" / "news.def"));
    check(list->count == 4, "two categories and the first one's two headlines");
    check(
        row_titled(list, "Front Line") != nullptr && row_titled(list, "Galactic War") != nullptr,
        "category rows"
    );
    check(row_titled(list, "Servers Restored") != nullptr, "headlines in sort order");
    check(row_titled(list, "Front Report") == nullptr, "a closed category hides its headlines");
    check(!std::filesystem::exists(g_downloads.files[0]), "the arrived contents file is deleted");
    check(frame->activity_light->running == 0, "the light stops once the contents are in");
    check(
        g_downloads.urls.size() == 2 &&
            g_downloads.urls[1] == "http://news.example:80/boneyards/ta/news/news1.htm",
        "the first headline's page is fetched from the news server"
    );
    check(
        markup(view).find("Waiting") != std::string_view::npos,
        "the waiting page stays until the page arrives"
    );

    // The page replaces the waiting page; its image, named in the install,
    // needs no download.
    arrive(1, read_bytes(samples / "news" / "news1.htm"));
    check(markup(view).find("Restored") != std::string_view::npos, "first page shown");
    check(!std::filesystem::exists(g_downloads.files[1]), "the arrived page is deleted");
    const html::Document& doc = view->state->doc;
    check(
        doc.images.size() == 1 && doc.images[0].loaded,
        "the page's install image is loaded where it is named"
    );
    check(g_downloads.urls.size() == 2, "nothing else is fetched");
    check(frame->activity_light->running == 0, "the light stops with nothing outstanding");

    // A clicked headline's page is fetched and shown the same way.
    Window* second = row_titled(list, "Ladder Season Opens");
    if (second != nullptr)
        click_window(widget, second);
    check(
        g_downloads.urls.size() == 3 &&
            g_downloads.urls[2] == "http://news.example:80/boneyards/ta/news/news2.htm",
        "a headline click fetches its page"
    );
    arrive(2, "<HTML>late</HTML>");
    check(markup(view).find("late") != std::string_view::npos, "the clicked page is shown");
    check(!std::filesystem::exists(g_downloads.files[2]), "its file is deleted");

    // Closing the console hides it and keeps it. A document for another page
    // is deleted and leaves the page on screen; opening News shows that page.
    click_window(widget, frame->console_buttons[static_cast<int32_t>(ConsoleButton::news)]);
    check(frame->console_screen == console_screen::none, "news button closes its console");
    boneyards_status_widget_tick(widget);
    check(
        frame->contents_console == console && console->visible == 0,
        "the closed news console stays and still takes documents"
    );
    check(markup(view).find("late") != std::string_view::npos, "the page it was showing stays");
    char stray[0x104] = {};
    fetch_to_temp(nullptr, "http://news.example:80/stray.htm", stray, sizeof stray);
    arrive(g_downloads.files.size() - 1, "<HTML>kept</HTML>");
    check(
        markup(view).find("late") != std::string_view::npos &&
            markup(view).find("kept") == std::string_view::npos,
        "a document for another page leaves the one on screen"
    );
    check(!std::filesystem::exists(stray), "the closed console deletes the arrived file");
    const ConsoleLayer* layer = frame->console;
    for (int32_t i = 0; layer->sliding != 0 && i < 100; ++i)
        boneyards_status_widget_tick(widget);
    click_window(widget, frame->console_buttons[static_cast<int32_t>(ConsoleButton::news)]);
    for (int32_t i = 0; layer->sliding != 0 && i < 100; ++i)
        boneyards_status_widget_tick(widget);
    check(
        frame->active_screen == console && console->visible == 1 &&
            markup(view).find("late") != std::string_view::npos,
        "opening News shows the same console and the page it held"
    );
    boneyards_status_widget_destroy(widget);
    std::filesystem::remove_all(download_directory());
}

// The Ladders console reads its local contents file at once; its first
// headline's #Remote# page is on the ladder server (kind 1).
void test_ladders_console(Data* data) {
    const std::filesystem::path samples(OA_BY_CLIENT_TEST_DATA);
    StatusWidget* widget = downloading_widget(data);
    check(widget != nullptr, "ladders: status widget and Main Frame");
    if (widget == nullptr)
        return;
    MainFrame* frame = widget->system.main_frame;
    click_window(widget, frame->console_buttons[static_cast<int32_t>(ConsoleButton::ladders)]);
    Window* console = frame->active_screen;
    check(console != nullptr && window_is_named(console, "LadderConsole"), "LadderConsole built");
    if (console == nullptr)
        return;
    auto* view = reinterpret_cast<HtmlView*>(window_find_named(console, "HTMLView", nullptr));
    auto* list = reinterpret_cast<ListBox*>(window_find_named(console, "ContentsList", nullptr));
    check(view != nullptr && list != nullptr, "ladders: view and contents list");
    if (view == nullptr || list == nullptr)
        return;
    check(
        list->count == 4 && row_titled(list, "Duel") != nullptr,
        "three categories and the first one's headline"
    );
    check(
        g_downloads.server_types.size() == 1 &&
            g_downloads.server_types[0] == kContentServerLadders,
        "the Ladders console asks for the ladder server"
    );
    check(
        g_downloads.urls.size() == 1 &&
            g_downloads.urls[0] == "http://ladders.example:8080/Boneyards/TA/Ladders/DUEL1.htm",
        "Duel's page is fetched from the ladder server"
    );
    check(
        frame->activity_light->running == 0,
        "the local contents file stops the light while the page is out"
    );
    arrive(0, read_bytes(samples / "ladders" / "DUEL1.htm"));
    check(markup(view).find("Ghost_Commander") != std::string_view::npos, "Duel's page shown");
    check(!std::filesystem::exists(g_downloads.files[0]), "the arrived page is deleted");
    check(frame->activity_light->running == 0, "the light is dark once the page is in");

    // The name search under the list: the "Name" caption, a framed entry of
    // 15 characters at (6, 0x104) and SEARCH at (0xc, 0x11d), which follows
    // whether the entry holds text.
    SingleEdit* entry = nullptr;
    Window* search = nullptr;
    Window* name_caption = nullptr;
    for (int32_t i = 0; i < console->children.count; ++i) {
        Window* child = console->children.items[i];
        if (child->ops->is_class(class_name::single_edit))
            entry = reinterpret_cast<SingleEdit*>(child);
        else if (caption(child) == "SEARCH")
            search = child;
        else if (caption(child) == "Name")
            name_caption = child;
    }
    check(
        entry != nullptr && search != nullptr && name_caption != nullptr,
        "the Ladders console builds its name search"
    );
    if (entry == nullptr || search == nullptr || name_caption == nullptr)
        return;
    check(
        entry->base.x == console->x + 6 && entry->base.y == console->y + 0x104 &&
            entry->base.width == 0x70 && entry->max_length == 0xf,
        "the name entry's place and length"
    );
    check(
        search->x == console->x + 0xc && search->y == console->y + 0x11d, "SEARCH under the entry"
    );
    check(
        name_caption->y + name_caption->height - 1 == console->y + 0x100,
        "the caption ends above the entry"
    );
    check(search->states.current == button_state::disabled, "SEARCH starts disabled");
    single_edit_set_text(entry, "ghost_commander");
    boneyards_status_widget_tick(widget);
    check(search->states.current == button_state::unpressed, "text in the entry enables SEARCH");

    // Links on the page come before the name field. Tab still reaches it,
    // and the next stop is SEARCH.
    press(widget, key::f6);
    bool search_field = false;
    for (int32_t step = 0; step < 80; ++step) {
        Window* focus = widget->desktop.focus;
        if (focus != nullptr && window_is_named(focus, "Ladder search")) {
            search_field = true;
            break;
        }
        press(widget, key::tab);
    }
    check(search_field, "Tab reaches the Ladders search field");
    press(widget, key::tab);
    check(widget->desktop.focus == search, "the next Tab reaches SEARCH");
    {
        ScopedServices scope(widget);
        widget->desktop.keyboard_ring = 0;
    }

    // SEARCH fetches the index of the name's first letter in place of the
    // page's number; it lists the name's line, whose page (a hundred lines
    // to a page) is fetched next.
    click_window(widget, search);
    check(
        g_downloads.urls.size() == 2 &&
            g_downloads.urls[1] == "http://ladders.example:8080/Boneyards/TA/Ladders/DUELg.byi",
        "SEARCH fetches the ladder's index for the first letter"
    );
    arrive(1, "Somebody 12\nGHOST_COMMANDER 150\n");
    check(
        g_downloads.urls.size() == 3 &&
            g_downloads.urls[2] == "http://ladders.example:8080/Boneyards/TA/Ladders/DUEL2.htm",
        "the index names the page listing the name"
    );
    check(frame->activity_light->running != 0, "the light runs while that page is out");
    arrive(2, "<HTML>Secondpage</HTML>");
    check(markup(view).find("Secondpage") != std::string_view::npos, "the found page shown");

    // A name the index does not list says so in a message box.
    single_edit_set_text(entry, "Nobody");
    click_window(widget, search);
    check(
        g_downloads.urls.size() == 4 &&
            g_downloads.urls[3] == "http://ladders.example:8080/Boneyards/TA/Ladders/DUELN.byi",
        "the second search's index"
    );
    arrive(3, "Somebody 12\n");
    MessageBox* box = message_box_find_open(&widget->desktop);
    Window* text =
        box != nullptr ? window_find_named(&box->base, message_box_name::text, nullptr) : nullptr;
    check(
        text != nullptr && caption(text) == "User \x93Nobody\x94 is not listed in this ladder.",
        "an unlisted name's message"
    );
    check(g_downloads.urls.size() == 4, "nothing more is fetched");
    single_edit_set_text(entry, "");
    boneyards_status_widget_tick(widget);
    check(search->states.current == button_state::disabled, "an empty entry disables SEARCH");
    boneyards_status_widget_destroy(widget);
    std::filesystem::remove_all(download_directory());
}

// Screen pixel, or 0 outside the frame.
uint8_t frame_pixel(const std::vector<uint8_t>& frame, int32_t x, int32_t y) {
    if (x < 0 || y < 0 || x >= 640 || y >= 480)
        return 0;
    return frame[static_cast<std::size_t>(y) * 640 + static_cast<std::size_t>(x)];
}

// Copies the green Console layer's picture (the remapped map under the Green_Screen border).
bool copy_green_layer(
    StatusWidget* widget,
    ConsoleLayer* layer,
    std::vector<uint8_t>* pixels,
    int32_t* width,
    int32_t* pitch
) {
    ScopedServices scope(widget);
    Surface surface{};
    if (layer->base.states.current < 0 || layer->base.states.current >= layer->base.states.count ||
        !image_surface(layer->base.states.images[layer->base.states.current], &surface) ||
        surface.pixels == nullptr)
        return false;
    *width = surface.width;
    *pitch = surface.pitch;
    pixels->assign(
        surface.pixels, surface.pixels + static_cast<std::ptrdiff_t>(surface.pitch) * surface.height
    );
    return true;
}

uint8_t layer_pixel(
    const std::vector<uint8_t>& layer,
    int32_t pitch,
    const ConsoleLayer* console,
    int32_t x,
    int32_t y
) {
    const int32_t lx = x - console->base.x;
    const int32_t ly = y - console->base.y;
    return layer
        [static_cast<std::size_t>(ly) * static_cast<std::size_t>(pitch) +
         static_cast<std::size_t>(lx)];
}

// The shared contents image's column rule, and the scroll-bar track line.
constexpr int32_t k_rule_x = 0x7b;
constexpr int32_t k_rule_top = 3;
constexpr int32_t k_rule_bottom = 0x135;
constexpr uint8_t k_rule_color = 0x5d;
constexpr int32_t k_track_x = 8;
constexpr int32_t k_track_top = 0xb;
constexpr uint8_t k_track_color = 0x5a;
constexpr uint8_t k_track_disabled_color = 0x5d;
constexpr uint8_t k_track_key = 9;

// A horizontal run of the drawn frame equals the green layer there.
bool run_matches_layer(
    const std::vector<uint8_t>& frame,
    const std::vector<uint8_t>& layer,
    int32_t width,
    int32_t pitch,
    const ConsoleLayer* console,
    int32_t x,
    int32_t y,
    int32_t count
) {
    const int32_t height =
        pitch == 0 ? 0 : static_cast<int32_t>(layer.size() / static_cast<std::size_t>(pitch));
    for (int32_t i = 0; i < count; ++i) {
        const int32_t lx = x + i - console->base.x;
        const int32_t ly = y - console->base.y;
        if (lx < 0 || ly < 0 || lx >= width || ly >= height)
            return false;
        if (frame_pixel(frame, x + i, y) != layer_pixel(layer, pitch, console, x + i, y))
            return false;
    }
    return true;
}

// A clear run of the page, in screen pixels. Clear page pixels are left unpainted, so the green
// layer behind the console shows through them.
bool find_clear_page_run(const HtmlView* view, int32_t* x, int32_t* y) {
    const Surface* canvas = html::document_surface(view->state->doc);
    if (canvas == nullptr || canvas->pixels == nullptr)
        return false;
    const int32_t top = (view->slider->range - view->slider->index) - 1;
    const int32_t height =
        view->base.height < canvas->height - top ? view->base.height : canvas->height - top;
    constexpr int32_t k_run = 16;
    for (int32_t row = 0; row < height; ++row) {
        int32_t run = 0;
        const int32_t src = row + top;
        for (int32_t col = 0; col < canvas->width; ++col) {
            const uint8_t pixel = canvas->pixels[src * canvas->pitch + col];
            if (pixel == html::k_canvas_clear_index) {
                ++run;
                if (run >= k_run) {
                    *x = view->base.x + col - k_run + 1;
                    *y = view->base.y + row;
                    return true;
                }
            } else {
                run = 0;
            }
        }
    }
    return false;
}

// One pixel of an image, or 0 when the image cannot be read.
uint8_t image_pixel(Handle image, int32_t x, int32_t y) {
    Surface surface{};
    if (!image_surface(image, &surface) || surface.pixels == nullptr || x < 0 || y < 0 ||
        x >= surface.width || y >= surface.height)
        return 0;
    return surface.pixels[y * surface.pitch + x];
}

// The bar's enabled track is the brighter line; the disabled track is the dimmer one. Keyed pixels
// stay transparent.
bool slider_track_art(Window* bar) {
    if (bar == nullptr || bar->states.count < 3)
        return false;
    const Handle enabled = bar->states.images[button_state::unpressed];
    const Handle disabled = bar->states.images[button_state::disabled];
    if (image_pixel(enabled, k_track_x, k_track_top) != k_track_color)
        return false;
    if (image_pixel(enabled, k_track_x - 1, k_track_top) != k_track_key)
        return false;
    if (image_pixel(enabled, k_track_x, k_track_top - 1) != k_track_key)
        return false;
    if (image_pixel(enabled, k_track_x, bar->height - 0xc) != k_track_color)
        return false;
    if (image_pixel(enabled, k_track_x, bar->height - 0xb) != k_track_key)
        return false;
    return image_pixel(disabled, k_track_x, k_track_top) == k_track_disabled_color &&
           image_pixel(bar->states.images[button_state::pressed], k_track_x, k_track_top) ==
               k_track_color;
}

// A horizontal run of the drawn frame equals the green layer, except the track line.
bool run_matches_track(
    const std::vector<uint8_t>& frame,
    const std::vector<uint8_t>& layer,
    int32_t width,
    int32_t pitch,
    const ConsoleLayer* console,
    Window* bar,
    int32_t x,
    int32_t y,
    int32_t count
) {
    for (int32_t i = 0; i < count; ++i) {
        const int32_t sx = x + i;
        const uint8_t pixel = frame_pixel(frame, sx, y);
        if (sx == bar->x + k_track_x) {
            if (pixel != k_track_color)
                return false;
        } else if (!run_matches_layer(frame, layer, width, pitch, console, sx, y, 1)) {
            return false;
        }
    }
    return count > k_track_x;
}

// One row of the page scroll bar's track, between the thumb and the lower arrow.
bool scroll_track_run(Window* bar, int32_t* x, int32_t* y, int32_t* count) {
    const Window* thumb = window_find_named(bar, "sbutton", nullptr);
    const Window* dec = window_find_named(bar, "decbutton", nullptr);
    if (thumb == nullptr || dec == nullptr || bar->visible == 0)
        return false;
    const int32_t gap_top = thumb->y + thumb->height;
    const int32_t gap_bottom = dec->y;
    if (gap_bottom - gap_top < 4)
        return false;
    *x = bar->x + 1;
    *y = gap_top + (gap_bottom - gap_top) / 2;
    *count = bar->width - 2;
    return *count > 0;
}

// Help, Ladders and News share this build. The green layer (the war map
// through the console's green remap, under the Green_Screen border) shows
// through the console's own background, the ContentsList and the page's
// scroll bar. Chat, Battle, Recon and Profile already draw this way.
void test_contents_green_background(Data* data) {
    StatusWidgetConfig config{};
    config.files = FileReader{data, read_asset};
    config.palette = &data->palette;
    config.service.network_available = connected;
    config.service.activate = activate;
    config.service.connected = connected;
    StatusWidget* widget = boneyards_status_widget_create(config);
    g_widget = widget;
    check(widget != nullptr, "green background: status widget");
    if (widget == nullptr)
        return;
    click_window(widget, boneyards_status_widget_part(widget, OverlayPart::enter));
    MainFrame* frame = widget->system.main_frame;
    check(frame != nullptr && frame->console != nullptr, "green background: Main Frame");
    if (frame == nullptr || frame->console == nullptr)
        return;
    for (int32_t i = 0; frame->console_screen != console_screen::login && i < 160; ++i)
        boneyards_status_widget_tick(widget);
    {
        ScopedServices scope(widget);
        main_frame_enable_buttons(frame);
    }
    click_window(widget, frame->console_buttons[static_cast<int32_t>(ConsoleButton::help)]);
    Window* console = frame->active_screen;
    check(
        console != nullptr && window_is_named(console, "HelpConsole") && window_is_shown(console),
        "green background: Help shown over the covering layer"
    );
    if (console == nullptr || !window_is_shown(console))
        return;
    auto* view = reinterpret_cast<HtmlView*>(window_find_named(console, "HTMLView", nullptr));
    auto* list = reinterpret_cast<ListBox*>(window_find_named(console, "ContentsList", nullptr));
    check(view != nullptr && list != nullptr && list->count > 0, "green background: page and list");
    if (view == nullptr || list == nullptr || list->count == 0)
        return;

    boneyards_status_widget_tick(widget);
    std::vector<uint8_t> shown;
    draw_frame(widget, &shown);
    std::vector<uint8_t> layer;
    int32_t layer_width = 0;
    int32_t layer_pitch = 0;
    ConsoleLayer* green = frame->console;
    check(
        copy_green_layer(widget, green, &layer, &layer_width, &layer_pitch),
        "green background: layer picture"
    );
    check(green->covering != 0 && green->sliding == 0, "green background: layer covering");
    if (layer.empty())
        return;

    // The console's own background, above the title and the page.
    const int32_t margin_x = console->x + 0x90;
    const int32_t margin_y = console->y + 2;
    check(
        run_matches_layer(shown, layer, layer_width, layer_pitch, green, margin_x, margin_y, 24),
        "the console window's background is the green layer"
    );
    const int32_t rule_x = console->x + k_rule_x;
    const int32_t rule_top = console->y + k_rule_top;
    check(frame_pixel(shown, rule_x, rule_top) == k_rule_color, "the contents column rule");
    check(
        frame_pixel(shown, rule_x, console->y + k_rule_bottom) == k_rule_color,
        "the contents column rule reaches its foot"
    );
    check(
        run_matches_layer(shown, layer, layer_width, layer_pitch, green, rule_x - 1, rule_top, 1),
        "the contents image is keyed beside the rule"
    );
    check(
        run_matches_layer(
            shown, layer, layer_width, layer_pitch, green, rule_x, console->y + k_rule_top - 1, 1
        ),
        "the contents image is keyed above the rule"
    );

    // The page leaves its clear pixels unpainted, so the console's background shows there.
    int32_t page_x = 0;
    int32_t page_y = 0;
    check(find_clear_page_run(view, &page_x, &page_y), "a clear run of the page");
    check(
        run_matches_layer(shown, layer, layer_width, layer_pitch, green, page_x, page_y, 16),
        "the page's background is the green layer"
    );
    bool saw_green = false;
    for (int32_t i = 0; i < 16; ++i) {
        const int32_t lx = page_x + i - green->base.x;
        const int32_t ly = page_y - green->base.y;
        const uint8_t map =
            green->map
                ->pixels[static_cast<std::size_t>(ly) * layer_width + static_cast<std::size_t>(lx)];
        const uint8_t remapped =
            green->green
                ->pixels[static_cast<std::size_t>(ly) * layer_width + static_cast<std::size_t>(lx)];
        const uint8_t pixel = frame_pixel(shown, page_x + i, page_y);
        if (pixel == remapped && remapped == k_console_green_remap[map] &&
            ((remapped >= 42 && remapped <= 46) || remapped == 200))
            saw_green = true;
    }
    check(saw_green, "the page's background is the remapped war map");

    Window* last = list_box_item(list, list->count - 1);
    check(last != nullptr, "green background: last contents row");
    if (last != nullptr) {
        const int32_t gap_y = last->y + last->height + 2;
        const int32_t gap_x = list->base.x + 8;
        check(
            gap_y < list->base.y + list->base.height - 2,
            "the contents list has room below its rows"
        );
        check(
            run_matches_layer(shown, layer, layer_width, layer_pitch, green, gap_x, gap_y, 32),
            "the ContentsList's background is the green layer"
        );
    }

    Window* bar = &view->slider->base;
    int32_t bar_x = 0;
    int32_t bar_y = 0;
    int32_t bar_count = 0;
    check(
        bar->visible == 1 && scroll_track_run(bar, &bar_x, &bar_y, &bar_count),
        "the page scroll bar is shown"
    );
    check(
        bar_count > 0 &&
            run_matches_track(
                shown, layer, layer_width, layer_pitch, green, bar, bar_x, bar_y, bar_count
            ),
        "the page scroll bar's track line sits on the green layer"
    );
    {
        ScopedServices scope(widget);
        check(slider_track_art(bar), "the page scroll bar's track art");
        Window* list_bar = window_find_named(&list->base, "slider", nullptr);
        check(slider_track_art(list_bar), "the contents list's track art");
    }

    // Ladders and News are the same console. Their windows leave the same layer showing.
    for (ConsoleButton which : {ConsoleButton::ladders, ConsoleButton::news}) {
        click_window(widget, frame->console_buttons[static_cast<int32_t>(which)]);
        Window* other = frame->active_screen;
        const char* name = which == ConsoleButton::ladders ? "LadderConsole" : "NewsConsole";
        check(other != nullptr && window_is_named(other, name) && window_is_shown(other), name);
        if (other == nullptr || !window_is_shown(other))
            continue;
        boneyards_status_widget_tick(widget);
        draw_frame(widget, &shown);
        check(copy_green_layer(widget, green, &layer, &layer_width, &layer_pitch), name);
        check(
            run_matches_layer(
                shown, layer, layer_width, layer_pitch, green, other->x + 0x90, other->y + 2, 24
            ),
            "the console window's background is the green layer"
        );
        check(
            frame_pixel(shown, other->x + k_rule_x, other->y + k_rule_top) == k_rule_color,
            "the contents column rule"
        );
    }
    boneyards_status_widget_destroy(widget);
}

// ---- the contents tree -------------------------------------------------------

// The folder --frames names; each state of the contents tree is written there as a picture.
std::filesystem::path g_frames;

// Writes a drawn frame as a binary PPM in the widget's palette, when --frames names a folder.
void write_frame(const std::vector<uint8_t>& pixels, const char* name) {
    if (g_frames.empty() || g_widget == nullptr || g_widget->config.palette == nullptr)
        return;
    std::error_code error;
    std::filesystem::create_directories(g_frames, error);
    std::ofstream out(g_frames / name, std::ios::binary);
    out << "P6\n640 480\n255\n";
    for (uint8_t index : pixels) {
        const oa::PaletteEntry& entry = g_widget->config.palette->entries[index];
        const char rgb[3] = {
            static_cast<char>(entry.r), static_cast<char>(entry.g), static_cast<char>(entry.b)
        };
        out.write(rgb, sizeof rgb);
    }
    check(out.good(), name);
}

// The contents list's layout: rows 0x11 high, as wide as the list less its scroll bar, which is
// 0x11 wide and 0x12 in from the list's right edge, with arrows 9 high. A category row's arrow
// sits at the row's left edge 3 pixels below its top; every title starts 0xe in and ends at the
// row's right edge. The thumb is never shorter than the scrollbar art's long thumb.
constexpr int32_t k_list_row_height = 0x11;
constexpr int32_t k_list_bar_inset = 0x12;
constexpr int32_t k_list_bar_width = 0x11;
constexpr int32_t k_arrow_dy = 3;
constexpr int32_t k_arrow_size = 0xb;
constexpr int32_t k_title_x = 0xe;
constexpr int32_t k_bar_arrow_height = 9;
constexpr int32_t k_thumb_min = 0x1e;

// A category row holds its arrow, then its title.
bool is_category_row(Window* row) {
    return row->children.count == 2 && window_is_named(row->children.items[0], "CategoryIcon");
}

// A row's title: its last child.
Window* row_title(Window* row) {
    return row->children.count > 0 ? row->children.items[row->children.count - 1] : nullptr;
}

// The list's scroll bar.
Window* list_bar(ListBox* list) {
    return window_find_named(&list->base, "slider", nullptr);
}

// The rows a list of this height shows at once.
int32_t rows_shown(const ListBox* list) {
    return (list->base.height - 1) / k_list_row_height;
}

// The titles of the rows the list shows, top to bottom.
std::vector<std::string> shown_titles(ListBox* list) {
    std::vector<std::string> titles;
    for (int32_t i = 0; i < list->count; ++i) {
        Window* row = list_box_item(list, i);
        if (row != nullptr && row->visible != 0)
            titles.push_back(caption(row_title(row)));
    }
    return titles;
}

// Checks the rows' places: the shown rows stack from the list's top, one 0x11 row apart, from
// the list's first shown item; every other row is hidden. Each shown row holds one title inside
// it, and a category row its arrow centred on the title. No row reaches past the scroll bar's
// first column.
void check_rows(ListBox* list, const char* state) {
    const Window& box = list->base;
    const int32_t shown =
        list->count - list->top < rows_shown(list) ? list->count - list->top : rows_shown(list);
    const int32_t bar_x = box.x + box.width - k_list_bar_inset;
    int32_t visible = 0;
    for (int32_t i = 0; i < list->count; ++i) {
        Window* row = list_box_item(list, i);
        if (row == nullptr) {
            check(false, state);
            continue;
        }
        const bool expected = i >= list->top && i < list->top + shown;
        if (row->visible != (expected ? 1 : 0)) {
            std::fprintf(stderr, "FAIL: %s: row %d shown %d\n", state, i, row->visible);
            ++g_failures;
        }
        if (!expected)
            continue;
        ++visible;
        const int32_t y = box.y + (i - list->top) * k_list_row_height;
        Window* title = row_title(row);
        const bool category = is_category_row(row);
        const int32_t title_x = row->x + k_title_x;
        if (row->x != box.x || row->y != y || row->height != k_list_row_height ||
            row->x + row->width > bar_x + 1 || title == nullptr || title->x != title_x ||
            title->y != y || title->height != k_list_row_height ||
            title->x + title->width != row->x + row->width) {
            std::fprintf(
                stderr,
                "FAIL: %s: row %d \"%s\" at (%d, %d) %dx%d, title at (%d, %d) %dx%d\n",
                state,
                i,
                title != nullptr ? caption(title).c_str() : "",
                row->x,
                row->y,
                row->width,
                row->height,
                title != nullptr ? title->x : -1,
                title != nullptr ? title->y : -1,
                title != nullptr ? title->width : -1,
                title != nullptr ? title->height : -1
            );
            ++g_failures;
        }
        if (category) {
            const Window* arrow = row->children.items[0];
            if (arrow->x != row->x || arrow->y != y + k_arrow_dy || arrow->width != k_arrow_size ||
                arrow->height != k_arrow_size) {
                std::fprintf(
                    stderr,
                    "FAIL: %s: row %d's arrow at (%d, %d), row at (%d, %d)\n",
                    state,
                    i,
                    arrow->x,
                    arrow->y,
                    row->x,
                    y
                );
                ++g_failures;
            }
        } else if (row->children.count != 1) {
            std::fprintf(stderr, "FAIL: %s: headline row %d holds one title\n", state, i);
            ++g_failures;
        }
    }
    check(visible == shown, state);
}

// Checks the list's scroll bar: the list's first child, which the list lays out as no row; shown
// only while rows overflow, along the list's right edge and as tall as the list. Its thumb lies between the arrows, as long as the track's share of the rows
// in view (in whole pairs of pixels, never shorter than the art), and its place along the track
// is the list's first shown row's share of the rows out of view.
void check_bar(ListBox* list, const char* state) {
    Window* bar = list_bar(list);
    check(bar != nullptr, state);
    if (bar == nullptr)
        return;
    if (list->base.children.items[0] != bar) {
        std::fprintf(stderr, "FAIL: %s: the scroll bar is not the list's first child\n", state);
        ++g_failures;
    }
    const bool overflow = list->count > rows_shown(list);
    if (!overflow) {
        if (bar->visible != 0) {
            std::fprintf(stderr, "FAIL: %s: the scroll bar shows with every row in view\n", state);
            ++g_failures;
        }
        return;
    }
    const Window& box = list->base;
    if (bar->visible == 0 || bar->states.current == button_state::disabled ||
        bar->x != box.x + box.width - k_list_bar_inset || bar->width != k_list_bar_width ||
        bar->y != box.y || bar->height != box.height) {
        std::fprintf(
            stderr,
            "FAIL: %s: scroll bar shown %d at (%d, %d) %dx%d, list at (%d, %d) %dx%d\n",
            state,
            bar->visible,
            bar->x,
            bar->y,
            bar->width,
            bar->height,
            box.x,
            box.y,
            box.width,
            box.height
        );
        ++g_failures;
    }
    const Window* thumb = window_find_named(bar, "sbutton", nullptr);
    const Window* up = window_find_named(bar, "incbutton", nullptr);
    const Window* down = window_find_named(bar, "decbutton", nullptr);
    check(thumb != nullptr && up != nullptr && down != nullptr, state);
    if (thumb == nullptr || up == nullptr || down == nullptr)
        return;
    const int32_t track_top = bar->y + k_bar_arrow_height;
    const int32_t track = bar->height - 2 * k_bar_arrow_height;
    const int32_t hidden = list->count - rows_shown(list);
    int32_t size = track * rows_shown(list) / list->count;
    size -= size % 2;
    if (size < k_thumb_min)
        size = k_thumb_min;
    const int32_t travel = track - thumb->height;
    const int32_t y = track_top + (travel * list->top + hidden / 2) / hidden;
    if (up->y != bar->y || down->y + down->height != bar->y + bar->height || thumb->x != bar->x ||
        thumb->width != bar->width || thumb->height != size || thumb->y != y ||
        thumb->y < track_top || thumb->y + thumb->height > down->y) {
        std::fprintf(
            stderr,
            "FAIL: %s: thumb at (%d, %d) %dx%d, wanted (%d, %d) %dx%d (rows %d, first shown %d)\n",
            state,
            thumb->x,
            thumb->y,
            thumb->width,
            thumb->height,
            bar->x,
            y,
            bar->width,
            size,
            list->count,
            list->top
        );
        ++g_failures;
    }
}

// Checks what the list draws: with every shown row's title and arrow hidden, the list area
// (less its scroll bar) is the same as with the list hidden, so no row draws anything else and no
// hidden row draws at all; each shown title draws inside its own row.
void check_drawn(StatusWidget* widget, ListBox* list, const char* state) {
    std::vector<uint8_t> full;
    draw_frame(widget, &full);
    std::vector<Window*> parts;
    for (int32_t i = 0; i < list->count; ++i) {
        Window* row = list_box_item(list, i);
        if (row == nullptr || row->visible == 0)
            continue;
        for (int32_t c = 0; c < row->children.count; ++c)
            if (row->children.items[c]->visible != 0)
                parts.push_back(row->children.items[c]);
    }
    for (Window* part : parts)
        part->visible = 0;
    std::vector<uint8_t> bare;
    draw_frame(widget, &bare);
    for (Window* part : parts)
        part->visible = 1;
    list->base.visible = 0;
    std::vector<uint8_t> empty;
    draw_frame(widget, &empty);
    list->base.visible = 1;
    const Window& box = list->base;
    const Window* bar = list_bar(list);
    const int32_t bar_x = bar != nullptr && bar->visible != 0 ? bar->x : box.x + box.width;
    int32_t stray = 0;
    for (int32_t y = box.y; y < box.y + box.height; ++y)
        for (int32_t x = box.x; x < bar_x; ++x)
            stray += frame_pixel(bare, x, y) != frame_pixel(empty, x, y) ? 1 : 0;
    if (stray != 0) {
        std::fprintf(
            stderr, "FAIL: %s: %d pixels of the list drawn by no shown title\n", state, stray
        );
        ++g_failures;
    }
    for (Window* part : parts) {
        if (part->ops->is_class(class_name::slider) || window_is_named(part, "CategoryIcon"))
            continue;
        int32_t ink = 0;
        for (int32_t y = part->y; y < part->y + part->height; ++y)
            for (int32_t x = part->x; x < part->x + part->width; ++x)
                ink += frame_pixel(full, x, y) != frame_pixel(bare, x, y) ? 1 : 0;
        if (ink == 0) {
            std::fprintf(stderr, "FAIL: %s: \"%s\" not drawn\n", state, caption(part).c_str());
            ++g_failures;
        }
    }
}

void check_tree(StatusWidget* widget, ListBox* list, const char* state) {
    boneyards_status_widget_tick(widget);
    check_rows(list, state);
    check_bar(list, state);
    check_drawn(widget, list, state);
    std::vector<uint8_t> frame;
    draw_frame(widget, &frame);
    write_frame(frame, (std::string("contents-") + state + ".ppm").c_str());
}

// The highlighted headlines, joined by "|": those whose title a drawn frame shows in the light
// font.
std::string highlighted(ListBox* list) {
    std::vector<uint8_t> frame;
    draw_frame(g_widget, &frame);
    const screens::HatFonts& fonts = g_widget->system.hat_fonts;
    std::string found;
    for (int32_t i = 0; i < list->count; ++i) {
        Window* row = list_box_item(list, i);
        if (row == nullptr || is_category_row(row) || row->visible == 0)
            continue;
        Window* title = row_title(row);
        if (title->font == fonts.small_light)
            found += (found.empty() ? "" : "|") + caption(title);
    }
    return found;
}

// Shown point of the view's link to `target`.
bool find_link_to(HtmlView* view, const char* target, int32_t* x, int32_t* y) {
    const Window& w = view->base;
    for (int32_t py = w.y; py < w.y + w.height; ++py)
        for (int32_t px = w.x; px < w.x + w.width - 0x17; ++px) {
            const int32_t doc_y = ((py - w.y) - view->slider->index) + view->slider->range;
            const char* link = html::document_link_at(view->state->doc, px - w.x, doc_y);
            if (link != nullptr && std::strcmp(link, target) == 0) {
                *x = px;
                *y = py;
                return true;
            }
        }
    return false;
}

// Clicks the shown row titled `title`.
void click_row(StatusWidget* widget, ListBox* list, const char* title) {
    Window* row = row_titled(list, title);
    if (row == nullptr || row->visible == 0) {
        std::fprintf(stderr, "FAIL: no shown row \"%s\"\n", title);
        ++g_failures;
        return;
    }
    click_window(widget, row);
}

// The Help contents shipped with the game: open Help, open Galactic War Help, pick Battle
// results, scroll the list with its arrows and its thumb, close Galactic War Help and Getting
// Started, and follow a link. Every state shows one title per row, the arrows in their rows, the
// scroll bar along the list's right edge with its thumb sized and placed by the rows in view, and
// the open page's headline highlighted.
void test_contents_tree(Data* data) {
    // The pages shipped beside the game, in the source tree's data folder.
    const std::filesystem::path shipped = std::filesystem::path(OA_BY_CLIENT_TEST_DATA)
                                              .parent_path()
                                              .parent_path()
                                              .parent_path()
                                              .parent_path()
                                              .parent_path() /
                                          "data" / "boneyards" / "help";
    boneyards_console_set_help_override_directory(shipped.string().c_str());
    StatusWidgetConfig config{};
    config.files = FileReader{data, read_asset};
    config.palette = &data->palette;
    config.host.play_sound = record_sound;
    config.service.network_available = connected;
    config.service.activate = activate;
    config.service.connected = connected;
    StatusWidget* widget = boneyards_status_widget_create(config);
    g_widget = widget;
    check(widget != nullptr, "tree: status widget");
    if (widget == nullptr)
        return;
    click_window(widget, boneyards_status_widget_part(widget, OverlayPart::enter));
    MainFrame* frame = widget->system.main_frame;
    check(frame != nullptr, "tree: Main Frame");
    if (frame == nullptr)
        return;
    for (int32_t i = 0; frame->console_screen != console_screen::login && i < 160; ++i)
        boneyards_status_widget_tick(widget);
    {
        ScopedServices scope(widget);
        main_frame_enable_buttons(frame);
    }
    click_window(widget, frame->console_buttons[static_cast<int32_t>(ConsoleButton::help)]);
    Window* console = frame->active_screen;
    auto* view = console != nullptr
                     ? reinterpret_cast<HtmlView*>(window_find_named(console, "HTMLView", nullptr))
                     : nullptr;
    auto* list =
        console != nullptr
            ? reinterpret_cast<ListBox*>(window_find_named(console, "ContentsList", nullptr))
            : nullptr;
    check(view != nullptr && list != nullptr, "tree: Help view and contents list");
    if (view == nullptr || list == nullptr)
        return;

    // Getting Started open: six categories and its seven headlines, Where to begin highlighted.
    const std::vector<std::string> opened{
        "Getting Started",
        "Where to begin",
        "Roaming the galaxy",
        "Chat",
        "Ladders and ratings",
        "Waging the war",
        "When login fails",
        "Editing a profile",
        "Galactic War Help",
        "TA Help",
        "Screen Help",
        "Getting Addl. Help",
        "Rules of Conduct",
    };
    check(list->count == 13 && shown_titles(list) == opened, "tree: Getting Started open");
    check(highlighted(list) == "Where to begin", "tree: the first page's headline highlighted");
    check_tree(widget, list, "open");

    // Galactic War Help opens below its row: twenty rows, the first sixteen shown, the thumb at the
    // top of the bar. The open page's headline stays highlighted.
    click_row(widget, list, "Galactic War Help");
    const std::vector<std::string> expanded{
        "Getting Started",
        "Where to begin",
        "Roaming the galaxy",
        "Chat",
        "Ladders and ratings",
        "Waging the war",
        "When login fails",
        "Editing a profile",
        "Galactic War Help",
        "The war",
        "Register",
        "Fighting the war",
        "Battle results",
        "When the front moves",
        "Ranks",
        "The screens",
    };
    check(list->count == 20 && list->top == 0, "tree: Galactic War Help open");
    check(shown_titles(list) == expanded, "tree: the first sixteen rows shown");
    check(highlighted(list) == "Where to begin", "tree: an expand keeps the page's headline");
    check_tree(widget, list, "expanded");

    // Battle results opens its page and becomes the highlighted headline.
    click_row(widget, list, "Battle results");
    check(markup(view).find("Battle") != std::string_view::npos, "tree: Battle results page");
    check(highlighted(list) == "Battle results", "tree: the picked headline highlighted");
    check(shown_titles(list) == expanded, "tree: picking a headline keeps the rows in place");
    check_tree(widget, list, "battle-results");

    // The bar's lower arrow scrolls the list down one row; the thumb follows.
    Window* bar = list_bar(list);
    Window* down = bar != nullptr ? window_find_named(bar, "decbutton", nullptr) : nullptr;
    check(down != nullptr, "tree: the bar's lower arrow");
    if (down != nullptr)
        click_window(widget, down);
    check(list->top == 1, "tree: the lower arrow scrolls one row");
    check(shown_titles(list).front() == "Where to begin", "tree: Getting Started scrolled out");
    check_tree(widget, list, "scrolled-one");

    // The thumb dragged to the bottom of the track shows the last rows.
    Window* thumb = bar != nullptr ? window_find_named(bar, "sbutton", nullptr) : nullptr;
    if (thumb != nullptr) {
        const int32_t x = thumb->x + thumb->width / 2;
        boneyards_status_widget_event(
            widget, pointer(MessageCode::left_down, x, thumb->y + 1, button_left)
        );
        boneyards_status_widget_event(
            widget, pointer(MessageCode::mouse_move, x, bar->y + bar->height - 1, button_left)
        );
        boneyards_status_widget_event(
            widget, pointer(MessageCode::left_up, x, bar->y + bar->height - 1, 0)
        );
    }
    check(list->top == 4, "tree: the thumb at the bottom shows the last rows");
    check(shown_titles(list).back() == "Rules of Conduct", "tree: the last row shown");
    check_tree(widget, list, "scrolled-end");

    // Closing Galactic War Help from there: thirteen rows, all in view, no scroll bar, and nothing
    // left of the closed rows.
    click_row(widget, list, "Galactic War Help");
    check(list->count == 13 && list->top == 0, "tree: Galactic War Help closed");
    check(shown_titles(list) == opened, "tree: the rows after the close");
    check(highlighted(list).empty(), "tree: the closed page's headline is not listed");
    check_tree(widget, list, "collapsed");

    // Getting Started closed: six category rows.
    click_row(widget, list, "Getting Started");
    check(list->count == 6, "tree: every category closed");
    check_tree(widget, list, "closed");

    // A link opens its page, and the page's headline is highlighted where it is listed.
    click_row(widget, list, "Galactic War Help");
    click_row(widget, list, "Getting Started");
    click_row(widget, list, "The screens");
    check(highlighted(list) == "The screens", "tree: The screens highlighted");
    int32_t lx = 0;
    int32_t ly = 0;
    check(find_link_to(view, "Boneyards/Help/Help124.htm", &lx, &ly), "tree: link to Help124");
    click_at(widget, lx, ly);
    MainFrameProbe probe{};
    contents_console_probe(console, &probe);
    check(
        std::string(probe.contents_page) == "Boneyards/Help/Help124.htm", "tree: the link's page"
    );
    check(highlighted(list) == "When login fails", "tree: the linked page's headline highlighted");
    check_tree(widget, list, "link");
    boneyards_status_widget_destroy(widget);
    boneyards_console_set_help_override_directory(nullptr);
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--frames") == 0)
            g_frames = std::filesystem::path(argv[i + 1]);
    const std::filesystem::path root = oa::boneyards_test::require_by_install(
        "the contents console tests", {"Boneyards/Help/Help.def"}
    );
    Data data;
    data.store.mount(root / "by.ccx");
    data.store.mount(root / "totala1.hpi");
    const std::vector<uint8_t> raw = oa::test::netgame::base_palette(root);
    for (int i = 0; i < OA_PALETTE_COLORS; ++i)
        data.palette.entries[i] = oa::PaletteEntry{raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], 0};
    test_help_console(&data);
    test_help_host_fonts(&data);
    test_news_console(&data);
    test_ladders_console(&data);
    test_contents_green_background(&data);
    test_contents_tree(&data);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("contents console tests passed\n");
    return 0;
}

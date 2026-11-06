// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Slider, ListBox and container behaviour.
#include <cstdio>

#include "oa/boneyards/toolkit.hpp"

using namespace oa::boneyards;

namespace {

int g_failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++g_failures;
    }
}

Message pointer(MessageCode code, int32_t x, int32_t y, uint32_t buttons) {
    Message msg{};
    msg.code = code;
    msg.x = x;
    msg.y = y;
    msg.buttons = buttons;
    return msg;
}

// Colour service recording each colour's RGBA and releases.
constexpr int32_t k_color_log = 64;
Handle g_next_color = 0;
uint32_t g_color_rgba[k_color_log] = {};
bool g_color_released[k_color_log] = {};
int32_t g_color_releases = 0;

Handle log_color_create(void*, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const Handle color = g_next_color++;
    if (color < k_color_log) {
        g_color_rgba[color] = (uint32_t{r} << 24) | (uint32_t{g} << 16) | (uint32_t{b} << 8) | a;
    }
    return color;
}

void log_color_release(void*, Handle color) {
    ++g_color_releases;
    if (color >= 0 && color < k_color_log) {
        g_color_released[color] = true;
    }
}

void log_color_fill(void*, int32_t, int32_t, int32_t, int32_t, Handle) {
}

uint32_t rgba_of(Handle color) {
    return color >= 0 && color < k_color_log ? g_color_rgba[color] : 0;
}

bool released(Handle color) {
    return color >= 0 && color < k_color_log && g_color_released[color];
}

ColorService g_colors{nullptr, log_color_release, log_color_create, log_color_fill};

// Vertical scroll bar at (0,0) 10x100 with arrow buttons and a thumb.
Slider* make_scroll_bar(Window* parent, int32_t x, int32_t y) {
    Slider* bar = slider_create();
    window_create(&bar->base, "slider", x, y, 10, 100, parent, true);
    Window* dec = scroll_button_create();
    window_create(dec, "decbutton", x, y, 10, 10, &bar->base, true);
    Window* inc = scroll_button_create();
    window_create(inc, "incbutton", x, y + 90, 10, 10, &bar->base, true);
    Window* thumb = scroll_button_create();
    window_create(thumb, "sbutton", x, y + 10, 10, 10, &bar->base, true);
    return bar;
}

void test_slider(Desktop* desktop) {
    Slider* bar = make_scroll_bar(nullptr, 0, 0);
    check(slider_range(bar) == 100, "a new slider has 100 positions");
    check(slider_index(bar) == 50, "a new slider sits at 50");
    slider_set_range(bar, 11, false);
    check(slider_range(bar) == 11, "range read back through slider_range");
    slider_set_index(bar, 0);
    Window* thumb = window_find_named(&bar->base, "sbutton", nullptr);
    check(thumb->y == 15 - 5, "index 0 puts the thumb at the track start");
    slider_set_index(bar, 10);
    check(thumb->y == 85 - 5, "last index puts the thumb at the track end");
    slider_set_index(bar, 99);
    check(bar->index == 10, "index clamps to the range");
    Message down = pointer(MessageCode::left_down, 5, 5, button_left);
    Message up = pointer(MessageCode::left_up, 5, 5, 0);
    desktop_route(desktop, &down);
    check(bar->index == 9, "decrement button steps down once on press");
    for (int i = 0; i < 16; ++i) {
        desktop_pulse(desktop);
    }
    check(bar->index < 9, "held decrement button auto-repeats after the delay");
    desktop_route(desktop, &up);
    const int32_t held = bar->index;
    desktop_pulse(desktop);
    check(bar->index == held, "release stops the repeat");
    Message drag_start = pointer(MessageCode::left_down, 5, thumb->y + 2, button_left);
    desktop_route(desktop, &drag_start);
    Message drag = pointer(MessageCode::mouse_move, 5, 50, button_left);
    desktop_route(desktop, &drag);
    check(bar->index == 5, "dragging the thumb to the middle selects the middle");
    desktop_route(desktop, &up);

    // A track press pages once and holds Pressed; the pulse pages again only
    // while Pressed and past the 15-pulse delay.
    slider_set_index(bar, 0);
    Message track = pointer(MessageCode::left_down, 5, 70, button_left);
    desktop_route(desktop, &track);
    check(
        bar->base.states.current == button_state::pressed && slider_index(bar) == 1,
        "track press pages towards the pointer"
    );
    for (int i = 0; i < 16; ++i) {
        desktop_pulse(desktop);
    }
    check(slider_index(bar) == 1, "no repeat within the delay");
    desktop_pulse(desktop);
    check(slider_index(bar) == 2, "held track repeats after the delay");
    Message track_up = pointer(MessageCode::left_up, 5, 70, 0);
    desktop_route(desktop, &track_up);
    for (int i = 0; i < 20; ++i) {
        desktop_pulse(desktop);
    }
    check(slider_index(bar) == 2, "a released track does not repeat");
    window_destroy(&bar->base);
}

// The constructor sets both selection colours through the two setters
// (0x9E6E2D4B and 0x6F00CE64); a setter releases only a set colour, and the
// destructor releases the ones still set.
void test_list_box_colours() {
    ListBox* list = list_box_create();
    check(rgba_of(list->highlight) == 0x9E6E2D4Bu, "selection colour");
    check(rgba_of(list->focused_highlight) == 0x6F00CE64u, "focused selection colour");
    const Handle old = list->highlight;
    const int32_t releases = g_color_releases;
    list_box_set_highlight(list, 1, 2, 3, 4);
    check(g_color_releases == releases + 1 && released(old), "the old colour is released");
    check(rgba_of(list->highlight) == 0x01020304u, "new selection colour");
    const Handle focused = list->focused_highlight;
    const Handle highlight = list->highlight;
    list->highlight = k_no_handle;
    const int32_t before_destroy = g_color_releases;
    window_destroy(&list->base);
    check(released(focused) && !released(highlight), "destroy releases the set colour only");
    check(g_color_releases == before_destroy + 2, "focused colour and the window background");
}

void test_list_box() {
    ListBox* list = list_box_create();
    window_create(&list->base, "list", 0, 0, 100, 35, nullptr, true);
    make_scroll_bar(&list->base, 90, 0);
    Window* items[6];
    for (int32_t i = 0; i < 6; ++i) {
        items[i] = static_create();
        window_create(items[i], "item", 0, 0, 80, 10, nullptr, true);
        list_box_add(list, items[i]);
    }
    check(list->count == 6, "six items");
    check(
        items[0]->visible == 1 && items[2]->visible == 1 && items[3]->visible == 0, "three rows fit"
    );
    check(items[1]->y == 10 && items[2]->y == 20, "rows stack from the top");
    check(list->top == 0 && list->last == 2 && list->page == 3, "visible range");
    list_box_select(list, 4);
    check(list->selected == 4 && list->top == 2, "selection scrolls into view");
    check(items[4]->visible == 1 && items[1]->visible == 0, "scrolled rows");
    check(
        list_box_index_of(list, items[4]) == 4 && list_box_item(list, 4) == items[4],
        "index lookups skip the slider"
    );
    Message end{};
    end.code = MessageCode::key_down;
    end.key = key::home;
    window_dispatch(&list->base, &end);
    check(list->selected == 0 && list->top == 0, "home selects the first item");
    list_box_remove(list, items[0]);
    window_destroy(items[0]);
    check(list->count == 5 && list_box_item(list, 0) == items[1], "remove");
    // list_box_insert takes the child the item goes before, the slider being
    // child 0; a position past the children appends.
    Window* first = static_create();
    window_create(first, "first", 0, 0, 80, 10, nullptr, true);
    list_box_insert(list, first, 1);
    check(
        list->count == 6 && list_box_item(list, 0) == first && list_box_item(list, 1) == items[1],
        "insert ahead of the first item"
    );
    check(
        first->parent == &list->base && first->y == 0 && items[1]->y == 10,
        "an inserted item is laid out in its place"
    );
    Window* last = static_create();
    window_create(last, "last", 0, 0, 80, 10, nullptr, true);
    list_box_insert(list, last, 99);
    check(list->count == 7 && list_box_item(list, 6) == last, "insert past the end appends");
    list_box_clear(list);
    check(list->count == 0 && list->base.children.count == 1, "clear keeps the slider");
    window_destroy(&list->base);
}

void test_frames(Desktop* desktop) {
    IWnd* frame = iwnd_create();
    window_create(&frame->window.base, "frame", 100, 100, 200, 150, nullptr, true);
    Window* minimize = window_find_named(&frame->window.base, "MINIMIZE_BUTTON", nullptr);
    check(
        minimize != nullptr && minimize->x == 270 && minimize->y == 110,
        "minimise button in the corner"
    );
    Message down = pointer(MessageCode::left_down, 275, 115, button_left);
    Message up = pointer(MessageCode::left_up, 275, 115, 0);
    desktop_route(desktop, &down);
    desktop_route(desktop, &up);
    check(frame->window.minimized == 1, "minimise button minimises");
    check(frame->window.base.x == 20 && frame->window.base.width == 20, "icon rectangle");
    check(minimize->visible == 0, "children hidden while minimised");
    Message dbl = pointer(MessageCode::left_double, 25, 25, 0);
    desktop_route(desktop, &dbl);
    check(frame->window.minimized == 0 && frame->window.base.x == 100, "double click restores");
    Message grab = pointer(MessageCode::left_down, 150, 200, button_left);
    Message move = pointer(MessageCode::mouse_move, 160, 205, button_left);
    Message drop = pointer(MessageCode::left_up, 160, 205, 0);
    desktop_route(desktop, &grab);
    desktop_route(desktop, &move);
    desktop_route(desktop, &drop);
    check(
        frame->window.base.x == 110 && frame->window.base.y == 105 && minimize->x == 280,
        "dragging moves the frame and its children"
    );
    window_destroy(&frame->window.base);

    PageControl* pages = page_control_create();
    window_create(&pages->base, "pages", 0, 0, 100, 100, nullptr, true);
    Window* tab_a = button_create();
    Window* tab_b = button_create();
    Window* page_a = panel_create();
    Window* page_b = panel_create();
    window_create(tab_a, "tab_a", 0, 0, 20, 10, &pages->base, true);
    window_create(tab_b, "tab_b", 20, 0, 20, 10, &pages->base, true);
    window_create(page_a, "page_a", 0, 10, 100, 90, &pages->base, false);
    window_create(page_b, "page_b", 0, 10, 100, 90, &pages->base, false);
    page_control_add(pages, tab_a, page_a);
    page_control_add(pages, tab_b, page_b);
    Message click_b = pointer(MessageCode::left_down, 25, 5, button_left);
    Message release_b = pointer(MessageCode::left_up, 25, 5, 0);
    desktop_route(desktop, &click_b);
    desktop_route(desktop, &release_b);
    check(
        page_b->visible == 1 && page_a->visible == 0 && pages->selected == page_b,
        "tab shows its page"
    );
    window_destroy(&pages->base);

    Window* panel = panel_create();
    window_create(panel, "panel", 0, 0, 100, 100, nullptr, true);
    Window* a = button_create();
    Window* s = static_create();
    Window* b = button_create();
    window_create(a, "a", 0, 0, 10, 10, panel, true);
    window_create(s, "s", 20, 0, 10, 10, panel, true);
    window_create(b, "b", 40, 0, 10, 10, panel, true);
    desktop_set_focus(desktop, a);
    Message tab{};
    tab.code = MessageCode::key_down;
    tab.key = key::tab;
    desktop_route(desktop, &tab);
    check(desktop->focus == b, "tab skips the static to the next button");
    desktop_route(desktop, &tab);
    check(desktop->focus == a, "tab wraps around the panel");
    window_destroy(panel);
}

} // namespace

int main() {
    Desktop desktop{};
    desktop_init(&desktop, 0, 0, 640, 480);
    Services s{};
    s.desktop = &desktop;
    s.colors = &g_colors;
    services_install(s);
    test_slider(&desktop);
    test_list_box();
    test_list_box_colours();
    test_frames(&desktop);
    desktop_release(&desktop);
    services_install(Services{});
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("boneyards layout tests passed");
    return 0;
}

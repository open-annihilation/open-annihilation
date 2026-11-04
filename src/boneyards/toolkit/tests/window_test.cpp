// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Window tree, dispatch, focus and routing behaviour of the Boneyards toolkit.
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

// Recording services: colours are created as sequential handles, fills logged.
struct Recorder {
    int32_t next_color = 0;
    int32_t colors_live = 0;
    int32_t fills = 0;
    int32_t last_fill_x = -1;
    int32_t cursor = -99;
    bool shift = false;
    int32_t unset_releases = 0;
};

Recorder g_rec;

Handle rec_color_create(void*, uint8_t, uint8_t, uint8_t, uint8_t) {
    ++g_rec.colors_live;
    return g_rec.next_color++;
}

void rec_color_release(void*, Handle color) {
    if (color < 0)
        ++g_rec.unset_releases;
    else
        --g_rec.colors_live;
}

void rec_fill(void*, int32_t x, int32_t, int32_t, int32_t, Handle) {
    ++g_rec.fills;
    g_rec.last_fill_x = x;
}

void rec_cursor(void*, int32_t cursor) {
    g_rec.cursor = cursor;
}

bool rec_key_down(void*, int32_t key) {
    return key == key::shift && g_rec.shift;
}

ColorService g_colors{nullptr, rec_color_release, rec_color_create, rec_fill};
CursorService g_cursors{nullptr, nullptr, nullptr, nullptr, rec_cursor, nullptr};
InputService g_input{nullptr, rec_key_down};

// A class that records messages and draws its background.
struct Probe {
    Window base;
    int32_t left_downs;
    int32_t enters;
    int32_t leaves;
    int32_t notifies;
    NotifyCode last_notify;
    int32_t refreshes;
};

WindowOps g_probe_ops;

bool probe_left_down(Window* self, const Message*) {
    ++reinterpret_cast<Probe*>(self)->left_downs;
    return true;
}

void probe_enter(Window* self) {
    ++reinterpret_cast<Probe*>(self)->enters;
}

void probe_leave(Window* self) {
    ++reinterpret_cast<Probe*>(self)->leaves;
}

void probe_notify(Window* self, const Notify* notify) {
    auto* probe = reinterpret_cast<Probe*>(self);
    ++probe->notifies;
    probe->last_notify = notify->code;
}

bool probe_refresh(Window* self) {
    ++reinterpret_cast<Probe*>(self)->refreshes;
    return true;
}

void probe_destroy(Window* self) {
    window_release(self);
    std::free(self);
}

Probe* make_probe(const char* name, int32_t x, int32_t y, int32_t w, int32_t h, Window* parent) {
    auto* probe = static_cast<Probe*>(std::calloc(1, sizeof(Probe)));
    window_init(&probe->base, &g_probe_ops);
    window_create(&probe->base, name, x, y, w, h, parent, true);
    return probe;
}

void test_tree_and_hit_test(Desktop* desktop) {
    Probe* root = make_probe("root", 0, 0, 200, 100, nullptr);
    Probe* child = make_probe("child", 10, 10, 50, 20, &root->base);
    Probe* grandchild = make_probe("leaf", 15, 12, 10, 10, &child->base);
    check(desktop->windows.count == 1, "top-level window registered on the desktop");
    check(root->base.children.count == 1 && child->base.parent == &root->base, "child linked");
    check(window_find_named(&root->base, "leaf", nullptr) == &grandchild->base, "find by name");
    check(window_find_named(&root->base, "leaf", "Button") == nullptr, "class filter");
    check(window_hit_test(&root->base, 16, 13) == &grandchild->base, "deepest hit");
    check(window_hit_test(&root->base, 100, 50) == &root->base, "root hit");
    check(window_hit_test(&root->base, 300, 50) == nullptr, "miss");
    child->base.visible = 0;
    check(!window_is_shown(&grandchild->base), "hidden ancestor hides descendants");
    check(window_hit_test(&root->base, 16, 13) == &root->base, "hidden subtree not hit");
    child->base.visible = 1;

    window_move_to(&root->base, 20, 30);
    check(root->base.x == 20 && root->base.y == 30, "move_to");
    check(child->base.x == 30 && grandchild->base.y == 42, "children follow the move");
    window_move_by(&child->base, 1000, 0);
    check(
        child->base.x + child->base.width == root->base.x + root->base.width,
        "child clamped to the parent's right edge"
    );

    Message draw{};
    draw.code = MessageCode::draw;
    g_rec.fills = 0;
    window_dispatch(&root->base, &draw);
    check(g_rec.fills == 3, "background fill for each shown window");

    const int32_t live_before = g_rec.colors_live;
    window_destroy(&root->base);
    check(desktop->windows.count == 0, "destroy removes the window from the desktop");
    check(g_rec.colors_live == live_before - 3, "destroy releases every colour handle");
}

// Destroying a list runs the list destructor, which releases each selection
// colour only when one is set.
void test_list_destroy_releases_set_colours() {
    ListBox* list = list_box_create();
    const int32_t live = g_rec.colors_live;
    color_release(list->highlight);
    list->highlight = k_no_handle;
    g_rec.unset_releases = 0;
    window_destroy(&list->base);
    check(g_rec.unset_releases == 0, "an unset selection colour is not released");
    check(g_rec.colors_live == live - 3, "the focused colour and the window colour are released");
}

void test_routing(Desktop* desktop) {
    Probe* back = make_probe("back", 0, 0, 100, 100, nullptr);
    Probe* front = make_probe("front", 50, 50, 100, 100, nullptr);
    Message msg{};
    msg.code = MessageCode::left_down;
    msg.x = 60;
    msg.y = 60;
    check(desktop_route(desktop, &msg), "click routed");
    check(front->left_downs == 1 && back->left_downs == 0, "top-most window gets the click");
    check(desktop->focus == &front->base && front->base.focused == 1, "click focuses");

    msg.x = 10;
    msg.y = 10;
    desktop_route(desktop, &msg);
    check(back->left_downs == 1, "click reaches the back window");
    check(desktop->focus == &back->base && front->base.focused == 0, "focus moves");
    check(
        desktop->windows.items[desktop->windows.count - 1] == &back->base,
        "focused window is raised"
    );

    back->base.cursor = 7;
    msg.code = MessageCode::mouse_move;
    desktop_route(desktop, &msg);
    check(back->enters == 1 && desktop->hover == &back->base, "hover enter");
    check(g_rec.cursor == 7, "hover sets the window cursor");
    msg.x = 140;
    msg.y = 140;
    desktop_route(desktop, &msg);
    check(back->leaves == 1 && front->enters == 1, "hover moves between windows");

    Probe* a = make_probe("a", 0, 0, 10, 10, &back->base);
    Probe* b = make_probe("b", 20, 0, 10, 10, &back->base);
    window_notify_parent(&a->base, NotifyCode::activated, 0);
    check(back->notifies == 1 && back->last_notify == NotifyCode::activated, "notify parent");
    (void)b;

    window_destroy(&back->base);
    window_destroy(&front->base);
    check(desktop->hover == nullptr && desktop->focus == nullptr, "destroy clears desktop refs");
}

// A box may outlive the window it hands focus back to; only windows still in
// the desktop's tree count.
void test_desktop_holds(Desktop* desktop) {
    Probe* top = make_probe("top", 0, 0, 100, 100, nullptr);
    Probe* kid = make_probe("kid", 10, 10, 10, 10, &top->base);
    Probe* leaf = make_probe("leaf", 12, 12, 4, 4, &kid->base);
    check(desktop_holds(desktop, &top->base), "top-level window held");
    check(desktop_holds(desktop, &leaf->base), "descendant held");
    check(!desktop_holds(desktop, nullptr) && !desktop_holds(nullptr, &top->base), "null");
    const Window* gone = &kid->base;
    window_destroy(&kid->base);
    check(!desktop_holds(desktop, gone), "destroyed child not held");
    desktop_remove(desktop, &top->base);
    check(!desktop_holds(desktop, &top->base), "window off the desktop not held");
    window_destroy(&top->base);
}

void test_states() {
    Window* w = window_create_plain();
    check(w->states.count == 2 && w->states.current == 1, "two states, Normal current");
    check(w->ops->state_index(w, "Disabled") == 0, "Disabled index");
    check(w->ops->state_index(w, "Normal") == 1, "Normal index");
    check(w->ops->state_index(w, "Pressed") == -1, "unknown state");
    check(std::strcmp(w->ops->state_name(w, 0), "Disabled") == 0, "state name");
    check(w->ops->is_class("Window") && !w->ops->is_class("Button"), "is_class");
    window_set_state(w, 5);
    check(w->states.current == 1, "out-of-range state ignored");
    check(w->ops->draw(w), "plain window draw reports handled");
    window_destroy(w);
}

// The services_desktop_* calls act on the installed desktop: the area reads
// its four fields, raise/focus/capture forward to desktop_raise,
// desktop_set_focus and desktop_set_capture; without one the area is 0 and
// nothing happens.
void test_installed_desktop() {
    Desktop other{};
    desktop_init(&other, 3, 4, 320, 200);
    Services s = services();
    s.desktop = &other;
    const Services previous = services_install(s);
    check(
        services_desktop_x() == 3 && services_desktop_y() == 4 && services_desktop_width() == 320 &&
            services_desktop_height() == 200,
        "installed desktop area"
    );
    Probe* first = make_probe("first", 0, 0, 10, 10, nullptr);
    Probe* second = make_probe("second", 20, 0, 10, 10, nullptr);
    check(other.windows.items[other.windows.count - 1] == &second->base, "created on top");
    services_desktop_raise(&first->base);
    check(other.windows.items[other.windows.count - 1] == &first->base, "raise");
    services_desktop_focus(&second->base);
    check(other.focus == &second->base && second->base.focused == 1, "focus");
    services_desktop_focus(nullptr);
    check(other.focus == nullptr && second->base.focused == 0, "focus cleared");
    services_desktop_capture(&first->base);
    check(other.capture == &first->base, "capture");
    services_desktop_capture(nullptr);
    window_destroy(&first->base);
    window_destroy(&second->base);
    desktop_release(&other);

    Services none = s;
    none.desktop = nullptr;
    services_install(none);
    check(
        services_desktop_x() == 0 && services_desktop_y() == 0 && services_desktop_width() == 0 &&
            services_desktop_height() == 0,
        "no desktop reads an empty area"
    );
    Window* loose = window_create_plain();
    services_desktop_raise(loose);
    services_desktop_focus(loose);
    services_desktop_capture(loose);
    check(loose->focused == 0, "requests without a desktop do nothing");
    window_destroy(loose);
    services_install(previous);
}

// services_desktop_add/remove add and remove a top-level window,
// services_desktop_destroy_owned destroys the owned windows,
// services_desktop_release_capture clears the capture,
// services_desktop_captured reads it and services_desktop_hover/set_hover
// read and set the hover of the installed desktop; without one they
// read null and do nothing.
void test_installed_desktop_lists() {
    Desktop other{};
    desktop_init(&other, 0, 0, 100, 100);
    Services s = services();
    s.desktop = &other;
    const Services previous = services_install(s);
    Window* loose = window_create_plain();
    check(other.windows.count == 0, "a plain window is not listed");
    services_desktop_add(loose);
    services_desktop_add(loose);
    check(other.windows.count == 1 && other.windows.items[0] == loose, "add lists the window once");
    services_desktop_remove(loose);
    check(other.windows.count == 0, "remove unlists it");
    services_desktop_capture(loose);
    check(services_desktop_captured() == loose, "captured reads the capture");
    services_desktop_release_capture();
    check(
        other.capture == nullptr && services_desktop_captured() == nullptr,
        "release clears the capture"
    );
    services_desktop_set_hover(loose);
    check(other.hover == loose && services_desktop_hover() == loose, "set_hover sets the hover");
    services_desktop_capture(loose);
    window_destroy(loose);
    check(
        other.capture == nullptr && other.hover == nullptr, "destroy drops its capture and hover"
    );

    Probe* owned = make_probe("owned", 0, 0, 5, 5, nullptr);
    Probe* kept = make_probe("kept", 10, 0, 5, 5, nullptr);
    owned->base.owned = 1;
    services_desktop_destroy_owned();
    check(
        other.windows.count == 1 && other.windows.items[0] == &kept->base, "owned windows destroyed"
    );
    window_destroy(&kept->base);
    desktop_release(&other);

    Services none = s;
    none.desktop = nullptr;
    services_install(none);
    Window* orphan = window_create_plain();
    services_desktop_add(orphan);
    services_desktop_set_hover(orphan);
    services_desktop_release_capture();
    services_desktop_destroy_owned();
    services_desktop_remove(orphan);
    check(
        services_desktop_captured() == nullptr && services_desktop_hover() == nullptr,
        "no desktop reads null"
    );
    window_destroy(orphan);
    services_install(previous);
}

// names_equal: false when either name is null, else a case-sensitive match.
void test_names_equal() {
    check(names_equal("Button", "Button"), "equal names");
    check(!names_equal("Button", "button"), "case matters");
    check(!names_equal("Butto", "Button") && !names_equal("Button", "Butto"), "prefix differs");
    check(names_equal("", ""), "empty names match");
    check(!names_equal(nullptr, "Button") && !names_equal("Button", nullptr), "null never matches");
    check(!names_equal(nullptr, nullptr), "two nulls do not match");
}

// The geometry setters store the new geometry and then run the class's
// refresh slot once; nothing is clamped and children stay where they are.
void test_geometry_setters() {
    Probe* p = make_probe("sized", 30, 40, 50, 60, nullptr);
    Probe* kid = make_probe("kid", 35, 45, 5, 5, &p->base);
    p->refreshes = 0;
    window_set_width(&p->base, 700);
    check(p->base.width == 700 && p->base.height == 60 && p->refreshes == 1, "set width");
    window_set_height(&p->base, -3);
    check(p->base.height == -3 && p->refreshes == 2, "set height unclamped");
    window_set_size(&p->base, 11, 12);
    check(p->base.width == 11 && p->base.height == 12 && p->refreshes == 3, "set size");
    window_set_rect(&p->base, Rect32{100, 200, 109, 204});
    check(
        p->base.x == 100 && p->base.y == 200 && p->base.width == 10 && p->base.height == 5,
        "rect is inclusive"
    );
    check(p->refreshes == 4 && kid->base.x == 35 && kid->base.y == 45, "children not moved");
    window_destroy(&p->base);
}

} // namespace

int main() {
    g_probe_ops = k_window_ops;
    g_probe_ops.class_name = "Probe";
    g_probe_ops.destroy = probe_destroy;
    g_probe_ops.draw = window_draw_background;
    g_probe_ops.left_down = probe_left_down;
    g_probe_ops.mouse_enter = probe_enter;
    g_probe_ops.mouse_leave = probe_leave;
    g_probe_ops.on_notify = probe_notify;
    g_probe_ops.refresh = probe_refresh;

    Desktop desktop{};
    desktop_init(&desktop, 0, 0, 640, 480);
    Services s{};
    s.desktop = &desktop;
    s.colors = &g_colors;
    s.cursors = &g_cursors;
    s.input = &g_input;
    services_install(s);

    test_tree_and_hit_test(&desktop);
    test_routing(&desktop);
    test_list_destroy_releases_set_colours();
    test_desktop_holds(&desktop);
    test_states();
    test_geometry_setters();
    test_installed_desktop();
    test_installed_desktop_lists();
    test_names_equal();
    desktop_release(&desktop);
    services_install(Services{});

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("boneyards window tests passed");
    return 0;
}

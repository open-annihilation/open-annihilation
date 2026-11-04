// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// DockBar edge snapping, drag-and-drop docking and child stacking; GridBox
// cells and paging.
#include <cstdio>
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

Message pointer(MessageCode code, int32_t x, int32_t y, uint32_t buttons) {
    Message msg{};
    msg.code = code;
    msg.x = x;
    msg.y = y;
    msg.buttons = buttons;
    return msg;
}

bool outline_is(const DockBar* bar, int32_t x, int32_t y, int32_t w, int32_t h) {
    return bar->outline_x == x && bar->outline_y == y && bar->outline_width == w &&
           bar->outline_height == h;
}

void test_dock_bar(Desktop* desktop) {
    DockBar* bar = dock_bar_create();
    check(bar->base.ops->is_class(class_name::dock_bar), "class name");
    window_create(&bar->base, "tools", 100, 100, 80, 30, nullptr, true);
    bar->edges = 0xF;
    bar->spacing = 2;
    Window* tools[3];
    for (Window*& tool : tools) {
        tool = button_create();
        window_create(tool, "tool", 110, 105, 20, 20, &bar->base, true);
    }
    dock_bar_measure(bar);
    check(bar->dock_width == 24 && bar->dock_height == 24, "docked size from the children");
    check(bar->edge == 0xF, "layout edge restored from the mask");

    dock_bar_snap(bar, 320, 5);
    check(bar->drop_edge == dock_edge::top && outline_is(bar, 0, 0, 640, 24), "top edge");
    dock_bar_snap(bar, 5, 240);
    check(bar->drop_edge == dock_edge::left && outline_is(bar, 0, 0, 24, 480), "left edge");
    dock_bar_snap(bar, 635, 400);
    check(bar->drop_edge == dock_edge::right && outline_is(bar, 616, 0, 24, 480), "right edge");
    dock_bar_snap(bar, 320, 470);
    check(bar->drop_edge == dock_edge::bottom && outline_is(bar, 0, 456, 640, 24), "bottom edge");
    bar->edges = dock_edge::top | dock_edge::bottom;
    dock_bar_snap(bar, 5, 300);
    check(bar->drop_edge == dock_edge::bottom, "top/bottom bar picks by half");
    bar->edges = dock_edge::top;
    bar->drop_edge = 0;
    dock_bar_snap(bar, 5, 300);
    check(bar->drop_edge == 0, "single-edge bars never move");
    bar->edges = 0xF;

    Message down = pointer(MessageCode::left_down, 140, 115, button_left);
    window_dispatch(&bar->base, &down);
    check(bar->dragging == 1 && desktop->capture == &bar->base, "press starts a drag");
    Message move = pointer(MessageCode::mouse_move, 320, 5, button_left);
    window_dispatch(&bar->base, &move);
    check(bar->drag_x == 320 && bar->drop_edge == dock_edge::top, "drag tracks the edge");
    Message up = pointer(MessageCode::left_up, 320, 5, 0);
    window_dispatch(&bar->base, &up);
    check(bar->dragging == 0 && desktop->capture == nullptr, "release ends the drag");
    check(
        bar->base.x == 0 && bar->base.y == 0 && bar->base.width == 640 && bar->base.height == 24,
        "dropped into the top outline"
    );
    check(bar->edge == dock_edge::top, "laid out for the top edge");
    check(tools[0]->x == 2 && tools[0]->y == 2, "first tool inset by the spacing");
    check(
        tools[1]->x == 24 && tools[1]->y == 4 && tools[2]->x == 46, "later tools wrap into columns"
    );

    // DockBar table: the right and middle press and release handlers are the
    // left ones.
    Message right_down = pointer(MessageCode::right_down, 10, 10, button_right);
    window_dispatch(&bar->base, &right_down);
    check(bar->dragging == 1 && desktop->capture == &bar->base, "right press starts a drag");
    Message right_up = pointer(MessageCode::right_up, 10, 10, 0);
    window_dispatch(&bar->base, &right_up);
    check(bar->dragging == 0 && desktop->capture == nullptr, "right release ends the drag");
    Message middle_down = pointer(MessageCode::middle_down, 10, 10, button_middle);
    window_dispatch(&bar->base, &middle_down);
    check(bar->dragging == 1, "middle press starts a drag");
    Message middle_up = pointer(MessageCode::middle_up, 10, 10, 0);
    window_dispatch(&bar->base, &middle_up);
    check(bar->dragging == 0 && desktop->capture == nullptr, "middle release ends the drag");
    window_destroy(&bar->base);
}

void test_grid_box() {
    GridBox* grid = grid_box_create();
    grid->rows = 2;
    grid->columns = 2;
    grid->cell_width = 10;
    grid->cell_height = 12;
    window_create(&grid->base, "grid", 5, 7, 20, 24, nullptr, true);
    check(grid->base.children.count == 4, "rows x columns cells");
    Window* second = grid->base.children.items[1];
    Window* third = grid->base.children.items[2];
    check(
        second->x == 5 && second->y == 19 && third->x == 15 && third->y == 7,
        "cells fill columns top to bottom"
    );
    check(window_is_named(second, "empty") && second->visible == 1, "cells start empty");

    const Handle none[3] = {k_no_handle, k_no_handle, k_no_handle};
    const char* names[] = {"a", "b", "c", "d", "e"};
    for (int32_t i = 0; i < 5; ++i)
        grid_box_add_item(grid, names[i], i % 2, none);
    grid_box_fill(grid);
    check(
        window_is_named(grid->base.children.items[0], "a") &&
            window_is_named(grid->base.children.items[3], "d"),
        "first page"
    );
    check(grid->base.children.items[1]->states.current == 1, "item value is the cell state");
    grid_box_page_next(grid);
    check(
        grid->first == 4 && window_is_named(grid->base.children.items[0], "e") &&
            window_is_named(grid->base.children.items[1], "empty"),
        "second page pads with empty cells"
    );
    grid_box_page_next(grid);
    check(grid->first == 0, "paging past the end wraps to the start");
    grid_box_page_previous(grid);
    check(grid->first == 4, "paging back from the start goes to the last page");
    Notify page{};
    page.code = static_cast<NotifyCode>(4);
    page.value = grid_page::next;
    grid->base.ops->on_notify(&grid->base, &page);
    check(grid->first == 0, "page command");
    grid_box_clear(grid);
    check(grid->item_count == 0, "cleared");
    window_destroy(&grid->base);
}

} // namespace

int main() {
    Desktop desktop{};
    desktop_init(&desktop, 0, 0, 640, 480);
    Services s{};
    s.desktop = &desktop;
    const Services previous = services_install(s);
    test_dock_bar(&desktop);
    test_grid_box();
    desktop_release(&desktop);
    services_install(previous);
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("dock bar and grid box tests passed");
    return 0;
}

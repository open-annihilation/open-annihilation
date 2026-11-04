// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// GridBox: a page of equal cells showing a list of named, imaged items.
#include <cstdlib>
#include <cstring>

#include "oa/boneyards/toolkit.hpp"
#include "ops_defaults.hpp"

namespace oa::boneyards {
namespace {

constexpr int32_t k_cell_handles = 3;
constexpr int32_t k_page_notify = 4;
const char* const k_empty_cell = "empty";

GridBox* as_grid(Window* w) {
    return reinterpret_cast<GridBox*>(w);
}

void release_item(GridItem* item) {
    if (item == nullptr) {
        return;
    }
    for (Handle image : item->images) {
        image_release(image);
    }
    std::free(item->name);
    std::free(item);
}

void show_item(Window* cell, const char* name, int32_t value, const Handle* images) {
    window_set_name(cell, name);
    window_set_state(cell, value);
    for (int32_t i = 0; i < k_cell_handles; ++i) {
        window_set_state_image(cell, i, images[i]);
    }
}

/// Creates the grid's cells when it is placed (the on_bounds slot).
///
/// Makes rows x columns Gadget cells named "empty", column by column and top
/// to bottom within each column, each cell_width x cell_height at its place
/// in the grid, shown, and holding the grid's cell sounds (without adding a
/// reference). Nothing is created while the rows, columns or cell size are
/// zero; each placement adds another set of cells.
///
/// @param[in,out] self grid (a GridBox record) that was placed
void grid_box_create_cells(Window* self) {
    GridBox* grid = as_grid(self);
    if (grid->rows == 0 || grid->columns == 0 || grid->cell_width == 0 || grid->cell_height == 0) {
        return;
    }
    for (int32_t column = 0; column < grid->columns; ++column) {
        for (int32_t row = 0; row < grid->rows; ++row) {
            Window* cell = gadget_create();
            window_create(
                cell,
                k_empty_cell,
                grid->cell_width * column + self->x,
                grid->cell_height * row + self->y,
                grid->cell_width,
                grid->cell_height,
                self,
                false
            );
            for (int32_t i = 0; i < k_cell_handles; ++i) {
                if (grid->cell_sounds[i] >= 0) {
                    window_set_state_sound(cell, i, grid->cell_sounds[i]);
                }
            }
            cell->visible = 1;
        }
    }
}

/// Tests whether a class name is "GridBox".
///
/// @param name class name to test; may be null
/// @return true when `name` is "GridBox"
bool grid_box_is_class(const char* name) {
    return names_equal(name, class_name::grid_box);
}

/// Returns the GridBox class's type code, which nothing in the engine reads.
///
/// @return the same constant the Gadget class reports
int32_t grid_box_type_code() {
    return 0;
}

/// Passes events from the grid's cells on to its parent and turns the page
/// on a page command.
///
/// A page command is notify code 4 with value grid_page::previous or
/// grid_page::next; other events are ignored.
///
/// @param[in,out] self grid (a GridBox record) receiving the event
/// @param notify event; null is ignored
void grid_box_on_notify(Window* self, const Notify* notify) {
    if (notify == nullptr) {
        return;
    }
    if (notify->source != nullptr && notify->source->parent == self) {
        if (self->parent != nullptr) {
            self->parent->ops->on_notify(self->parent, notify);
        }
        return;
    }
    if (static_cast<int32_t>(notify->code) != k_page_notify) {
        return;
    }
    GridBox* grid = as_grid(self);
    if (notify->value == grid_page::previous) {
        grid_box_page_previous(grid);
    } else if (notify->value == grid_page::next) {
        grid_box_page_next(grid);
    }
}

/// Draws a grid: its state image, or else its background colour, then the
/// cells.
///
/// A hidden grid draws nothing.
///
/// @param self grid to draw
/// @return true (handled)
bool grid_box_draw(Window* self) {
    if (window_is_shown(self)) {
        const int32_t state = self->states.current;
        const Handle image =
            state >= 0 && state < self->states.count ? self->states.images[state] : k_no_handle;
        if (image >= 0) {
            image_draw(self->x, self->y, self->width, self->height, image);
        } else {
            color_fill(self->x, self->y, self->width, self->height, self->color);
        }
        window_draw_children(self);
    }
    return true;
}

/// Frees a GridBox: its items, the empty-cell image, the cell sounds that are
/// set and the item array, then runs the base teardown and frees the record.
///
/// @param self grid (a GridBox record) to free
void grid_box_destroy(Window* self) {
    GridBox* grid = as_grid(self);
    grid_box_clear(grid);
    image_release(grid->empty_image);
    for (Handle sound : grid->cell_sounds) {
        if (sound >= 0) {
            sound_release(sound);
        }
    }
    std::free(grid->items);
    window_release(self);
    std::free(self);
}

constexpr WindowOps make_grid_box_ops() {
    WindowOps ops = detail::plain_window_ops();
    ops.class_name = class_name::grid_box;
    ops.destroy = grid_box_destroy;
    ops.is_class = grid_box_is_class;
    ops.type_code = grid_box_type_code;
    ops.on_bounds = grid_box_create_cells;
    ops.on_notify = grid_box_on_notify;
    ops.draw = grid_box_draw;
    return ops;
}

} // namespace

constinit const WindowOps k_grid_box_ops = make_grid_box_ops();

GridBox* grid_box_create() {
    auto* grid = static_cast<GridBox*>(std::calloc(1, sizeof(GridBox)));
    if (grid == nullptr) {
        std::abort();
    }
    window_init(&grid->base, &k_grid_box_ops);
    grid->empty_image = k_no_handle;
    for (Handle& sound : grid->cell_sounds) {
        sound = k_no_handle;
    }
    return grid;
}

void grid_box_add_item(GridBox* grid, const char* name, int32_t value, const Handle images[3]) {
    auto* item = static_cast<GridItem*>(std::calloc(1, sizeof(GridItem)));
    const size_t length = std::strlen(name != nullptr ? name : "") + 1;
    char* copy = static_cast<char*>(std::malloc(length));
    if (item == nullptr || copy == nullptr) {
        std::abort();
    }
    std::memcpy(copy, name != nullptr ? name : "", length);
    item->name = copy;
    item->value = value;
    for (int32_t i = 0; i < k_cell_handles; ++i) {
        item->images[i] = images != nullptr ? images[i] : k_no_handle;
        image_add_ref(item->images[i]);
    }
    if (grid->item_count == grid->item_capacity) {
        const int32_t capacity = grid->item_capacity < 8 ? 8 : grid->item_capacity * 2;
        auto* grown = static_cast<GridItem**>(
            std::realloc(grid->items, sizeof(GridItem*) * static_cast<size_t>(capacity))
        );
        if (grown == nullptr) {
            std::abort();
        }
        grid->items = grown;
        grid->item_capacity = capacity;
    }
    grid->items[grid->item_count++] = item;
}

void grid_box_clear(GridBox* grid) {
    for (int32_t i = 0; i < grid->item_count; ++i) {
        release_item(grid->items[i]);
    }
    grid->item_count = 0;
}

void grid_box_page_previous(GridBox* grid) {
    const uint32_t page = static_cast<uint32_t>(grid->rows * grid->columns);
    grid->first -= static_cast<int32_t>(page);
    if (grid->first < 0 && page != 0) {
        const uint32_t count = static_cast<uint32_t>(grid->item_count);
        grid->first = static_cast<int32_t>((count / page) * page);
    }
    grid_box_fill(grid);
}

void grid_box_page_next(GridBox* grid) {
    const uint32_t next = static_cast<uint32_t>(grid->first + grid->columns * grid->rows);
    grid->first = static_cast<int32_t>(next);
    if (static_cast<uint32_t>(grid->item_count) < next) {
        grid->first = 0;
    }
    grid_box_fill(grid);
}

void grid_box_fill(GridBox* grid) {
    const int32_t count = grid->item_count;
    const uint32_t page = static_cast<uint32_t>(grid->rows * grid->columns);
    if (page == 0) {
        return;
    }
    int32_t padded = count;
    if (static_cast<uint32_t>(count) % page != 0) {
        padded = static_cast<int32_t>((static_cast<uint32_t>(count) / page + 1) * page);
    }
    const Handle empty[k_cell_handles] = {grid->empty_image, grid->empty_image, grid->empty_image};
    int32_t index = grid->first;
    const WindowList& cells = grid->base.children;
    for (int32_t i = 0; i < cells.count; ++i, ++index) {
        Window* cell = cells.items[i];
        if (cell == nullptr) {
            return;
        }
        if (index < count) {
            const GridItem* item = grid->items[index];
            if (item != nullptr) {
                show_item(cell, item->name, item->value, item->images);
            }
        } else if (index < padded || count == 0) {
            show_item(cell, k_empty_cell, 0, empty);
        } else if (index - padded >= 0 && index - padded < count) {
            const GridItem* item = grid->items[index - padded];
            if (item != nullptr) {
                show_item(cell, item->name, item->value, item->images);
            }
        }
    }
}

} // namespace oa::boneyards

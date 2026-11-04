// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Growable window pointer arrays.
#include <cstdlib>
#include <cstring>

#include "list.hpp"

namespace oa::boneyards {

void list_add_unique(WindowList* list, Window* window) {
    list_insert_unique(list, window, -1);
}

void list_insert_unique(WindowList* list, Window* window, int32_t position) {
    if (window == nullptr) {
        return;
    }
    for (int32_t i = 0; i < list->count; ++i) {
        if (list->items[i] == window) {
            return;
        }
    }
    if (list->count == list->capacity) {
        const int32_t capacity = list->capacity < 2 ? list->capacity + 1 : list->capacity * 2;
        auto* items = static_cast<Window**>(std::realloc(list->items, sizeof(Window*) * capacity));
        if (items == nullptr) {
            std::abort();
        }
        list->items = items;
        list->capacity = capacity;
    }
    if (position < 0 || position > list->count) {
        position = list->count;
    }
    std::memmove(
        list->items + position + 1,
        list->items + position,
        sizeof(Window*) * (list->count - position)
    );
    list->items[position] = window;
    ++list->count;
}

bool list_remove(WindowList* list, Window* window) {
    if (window == nullptr) {
        return false;
    }
    for (int32_t i = 0; i < list->count; ++i) {
        if (list->items[i] == window) {
            list_remove_at(list, i);
            return true;
        }
    }
    return false;
}

void list_remove_at(WindowList* list, int32_t index) {
    if (index < 0 || index >= list->count) {
        return;
    }
    std::memmove(
        list->items + index, list->items + index + 1, sizeof(Window*) * (list->count - index - 1)
    );
    --list->count;
}

void list_free(WindowList* list) {
    std::free(list->items);
    *list = WindowList{};
}

} // namespace oa::boneyards

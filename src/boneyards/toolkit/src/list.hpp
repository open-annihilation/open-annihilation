// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Internal helpers for WindowList (a growable array of window pointers).
#pragma once

#include "oa/boneyards/toolkit.hpp"

namespace oa::boneyards {

/// Appends a window unless it is already in the list.
///
/// @param[in,out] list list to extend
/// @param window window to add; null is ignored
void list_add_unique(WindowList* list, Window* window);
/// Inserts a window before index `position` unless it is already in the list.
///
/// @param[in,out] list list to extend
/// @param window window to add; null is ignored
/// @param position index to insert before; a position outside the list
///     appends
void list_insert_unique(WindowList* list, Window* window, int32_t position);
/// Removes the first occurrence of a window.
///
/// @param[in,out] list list to shrink
/// @param window window to remove; null is never present
/// @return true when the window was present
bool list_remove(WindowList* list, Window* window);
/// Removes the entry at an index; an index outside the list is ignored.
///
/// @param[in,out] list list to shrink
/// @param index entry index
void list_remove_at(WindowList* list, int32_t index);
/// Frees the list's storage and leaves it empty.
///
/// @param[in,out] list list to free
void list_free(WindowList* list);

} // namespace oa::boneyards

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Rover's own heap, drawn from memory private to the library: small blocks
// from its own reserved regions, larger ones from a private Win32 heap it
// creates.
// Every buffer Rover hands its host (string buffers above all) comes from
// there, laid out as 3.1c's Rover lays it out; the 32-bit Windows build keeps
// that allocator, other builds use malloc.
#ifndef OA_ROVER_HEAP_H
#define OA_ROVER_HEAP_H

#include <stddef.h>

/// Allocates a block from Rover's own heap.
///
/// On 32-bit Windows small requests come from the small-block regions and the rest, or any the
/// regions cannot serve, from the private heap in whole paragraphs; other builds use malloc.
///
/// @param size bytes wanted
/// @return the block, or null when the heap is exhausted
void* rover_heap_alloc(size_t size);

/// Returns a block to Rover's own heap.
///
/// On 32-bit Windows a small block goes back to its region and anything else to the private heap,
/// which is also where a buffer allocated by the host ends up.
///
/// @param block block to free, or null
void rover_heap_free(void* block);

#endif

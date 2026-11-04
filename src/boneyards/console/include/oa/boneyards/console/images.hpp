// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// GAF-backed image store for the Boneyards console screens.
//
// The console loads every graphic as one frame of a named GAF sequence
// ("Main_Console.Gaf" / "MM_Single_Button" / frame 2) and keeps it as an
// editable 8-bit sprite: the main-menu overlay darkens and restores its
// background in place, and clones button frames to make disabled states.
// The store also implements the toolkit ImageService so windows draw through
// it into the current target surface.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "oa/boneyards/toolkit.hpp"
#include "oa/present/surface.h"

namespace oa::boneyards::console {

// Reads a game resource ("anims/Main_Console.gaf") into `out`; returns false
// when the resource does not exist.
struct FileReader {
    void* user;
    bool (*read)(void* user, const char* path, std::vector<uint8_t>* out);
    // Called once when a base-disc map failed to load, before that same path
    // is read again. Null skips the retry.
    void (*rescan)(void* user) = nullptr;
};

// 32 rows of 256 palette remaps each (palette.shd / palette.lht layout).
inline constexpr std::size_t k_shade_levels = 32;
inline constexpr std::size_t k_shade_table_bytes = k_shade_levels * 256;

struct ShadeTables {
    const uint8_t* shade; // darkening rows, k_shade_table_bytes or null
    const uint8_t* light; // lightening rows, k_shade_table_bytes or null
};

// Directory every console GAF name is resolved under.
inline constexpr const char* k_console_gaf_directory = "anims/";

struct ConsoleImages;

/// Creates an empty console image store.
///
/// An archive the reader does not hold is read as the client session reads
/// a file (rover_session_read_file), where the new consoles' art is found
/// with their layouts when the profile does not mount coreprime.ccx. The
/// main console graphic is the exception: it falls back to the embedded copy.
///
/// @param reader reads game resources; archives are loaded and cached on first use
/// @return the store
ConsoleImages* console_images_create(FileReader reader);

/// Frees the image store with every image and cached archive.
///
/// @param images the store; may be null
void console_images_destroy(ConsoleImages* images);

/// Drops the parsed copy of the embedded main-menu graphic.
///
/// The bytes in the program stay. The next image load parses them again.
/// Pictures already copied into an image store are not affected.
void console_images_release_embedded_menu();

/// Loads one frame of a GAF sequence as a new image.
///
/// The sequence is matched case-insensitively in anims/<gaf_name>; the
/// image starts with the default delay.
///
/// @param images the store; may be null
/// @param gaf_name archive name under anims/
/// @param sequence sequence name
/// @param frame frame index in the sequence
/// @param key transparent palette index the image is drawn with
/// @return the image, or k_no_handle when the archive, sequence or frame does not exist
Handle console_images_load(
    ConsoleImages* images, const char* gaf_name, const char* sequence, int32_t frame, uint8_t key
);

/// Copies an image's picture into a new image drawn with another transparent index.
///
/// @param images the store; may be null
/// @param image image copied
/// @param key transparent palette index of the copy
/// @return the copy, or k_no_handle for a dead image
Handle console_images_clone(ConsoleImages* images, Handle image, uint8_t key);

/// Returns the editable sprite of a live image.
///
/// @param images the store; may be null
/// @param image the image
/// @return the sprite, or null for a dead handle
Sprite* console_images_sprite(ConsoleImages* images, Handle image);

/// Counts the live images, for leak checks.
///
/// @param images the store
/// @return number of live handles
int32_t console_images_live_count(const ConsoleImages* images);

/// Sets the surface the ImageService draw slot writes into.
///
/// @param[in,out] images the store
/// @param target surface drawn into; null disables drawing
void console_images_set_target(ConsoleImages* images, Surface* target);

/// Returns the surface the ImageService draw slot writes into.
///
/// @param images the store; may be null
/// @return the target, or null
Surface* console_images_target(const ConsoleImages* images);

/// Returns the toolkit ImageService backed by the store.
///
/// @param images the store the service's slots use
/// @return the service
ImageService console_images_service(ConsoleImages* images);

/// Remaps every pixel of an image through one shade row, the transparent index included.
///
/// @param images the store; may be null
/// @param image the image changed in place
/// @param level negative levels darken (clamped to -32), others lighten (clamped to 31)
/// @param tables the shade and light tables
/// @return false when the image is dead or the needed table is missing (the pixels stay
///     untouched)
bool console_images_shade(
    ConsoleImages* images, Handle image, int32_t level, const ShadeTables& tables
);

/// Copies a raw sprite keyed on its transparent index, its origin placed at (x, y), clipped to the
/// surface's clip rectangle.
///
/// @param[in,out] target surface drawn into; null draws nothing
/// @param sprite raw sprite; other encodings draw nothing
/// @param x where the sprite's origin lands, in target pixels
/// @param y where the sprite's origin lands, in target pixels
void blit_sprite(Surface* target, const Sprite& sprite, int32_t x, int32_t y);

} // namespace oa::boneyards::console

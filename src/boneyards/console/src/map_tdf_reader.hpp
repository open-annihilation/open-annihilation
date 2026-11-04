// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The planet map OTA behind the Recon screen's environment rows, read
// through the game's files.
#pragma once

#include "oa/boneyards/console/images.hpp"
#include "oa/boneyards/screens/galaxy.hpp"

namespace oa::boneyards::console {

// The loaded map OTA.
struct MapTdfFile;

/// Creates an empty map OTA holder reading through the game's files.
///
/// @param files game resource reader; its rescan, when set, runs once after a
///     base-disc map fails to load and before that same path is read again
/// @return the holder; allocation failure aborts
MapTdfFile* map_tdf_file_create(FileReader files);

/// Names the language folder tried before maps\ for this holder.
///
/// Null or empty clears the name, and the language the game is showing is
/// tried instead. A holder with neither leaves the map in maps\.
///
/// @param file the holder; null does nothing
/// @param language the language's word, as "German"
void map_tdf_file_set_language(MapTdfFile* file, const char* language);

/// Frees a map OTA holder and its document.
///
/// @param file the holder; null does nothing
void map_tdf_file_destroy(MapTdfFile* file);

/// Returns a reader over a map OTA holder for galaxy_load_environment and planet_info_fill.
///
/// Its open loads maps-<language>\<map name>.OTA when that file opens, else
/// maps\<map name>.OTA, and selects its GlobalHeader. A base-disc map that
/// fails is read again at the path already chosen, after the holder's archive
/// scan. Its get_int reads that section.
///
/// @param file the holder the reader uses
/// @return the reader
screens::MapTdfReader map_tdf_reader(MapTdfFile* file);

} // namespace oa::boneyards::console

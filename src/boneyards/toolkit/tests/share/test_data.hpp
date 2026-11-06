// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Installed-data fixtures shared by the toolkit tests: files are read from the
// by.ccx archive of the BY1117a install OA_GAME_DIR names, the palette from
// its totala1.hpi.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <utility>
#include <vector>

#include "oa/formats/hpi.hpp"
#include "oa/boneyards/toolkit.hpp"
#include "oa/boneyards_test/installs.hpp"

namespace oa::boneyards::testing {

struct InstalledData {
    std::unique_ptr<HpiArchive> archive{};
    Palette palette{};
};

/// Reads one file of by.ccx for the toolkit's file service.
///
/// @param user the InstalledData open_installed_data() filled
/// @param path path inside by.ccx
/// @param[out] size receives the file's length
/// @return a malloc'd copy of the file, or null when it is missing or memory runs out
inline uint8_t* read_from_archive(void* user, const char* path, size_t* size) {
    auto* data = static_cast<InstalledData*>(user);
    auto read = data->archive->read(path);
    if (!read.ok())
        return nullptr;
    const std::vector<uint8_t>& bytes = *read.value;
    auto* out = static_cast<uint8_t*>(std::malloc(bytes.size() + 1));
    if (out == nullptr) {
        return nullptr;
    }
    std::memcpy(out, bytes.data(), bytes.size());
    *size = bytes.size();
    return out;
}

/// Opens by.ccx and the base palette of the BY1117a install OA_GAME_DIR names.
///
/// Without that install the test ends as skipped (exit code 77) and this
/// does not return.
///
/// @param[out] data receives the archive and the palette
inline void open_installed_data(InstalledData* data) {
    const std::filesystem::path root =
        oa::boneyards_test::require_by_install("the toolkit's by.ccx cases");
    auto opened = oa::open_hpi_file(root / oa::boneyards_test::kBoneyardsArchive);
    if (!opened.ok()) {
        std::fprintf(
            stderr, "%s: %s\n", oa::boneyards_test::kBoneyardsArchive, opened.error.message
        );
        std::exit(EXIT_FAILURE);
    }
    data->archive = std::make_unique<HpiArchive>(std::move(*opened.value));
    const std::vector<uint8_t> raw = oa::test::netgame::base_palette(root);
    for (int i = 0; i < OA_PALETTE_COLORS; ++i) {
        data->palette.entries[i] = PaletteEntry{raw[i * 4], raw[i * 4 + 1], raw[i * 4 + 2], 0};
    }
}

/// Makes a toolkit configuration that draws with the installed art.
///
/// @param data the archive and palette open_installed_data() filled
/// @param width screen width in pixels
/// @param height screen height in pixels
/// @return the configuration
inline ToolkitConfig installed_config(InstalledData* data, int32_t width, int32_t height) {
    ToolkitConfig config{};
    config.width = width;
    config.height = height;
    config.palette = &data->palette;
    config.files = ToolkitFiles{data, read_from_archive};
    return config;
}

// FNV-1a over a surface region, for draw snapshots.
inline uint32_t surface_digest(const Surface* s) {
    uint32_t hash = 2166136261U;
    for (int32_t y = 0; y < s->height; ++y) {
        for (int32_t x = 0; x < s->width; ++x) {
            hash = (hash ^ s->pixels[y * s->pitch + x]) * 16777619U;
        }
    }
    return hash;
}

inline int32_t opaque_pixels(const Surface* s) {
    int32_t count = 0;
    for (int32_t y = 0; y < s->height; ++y) {
        for (int32_t x = 0; x < s->width; ++x) {
            count += s->pixels[y * s->pitch + x] != k_overlay_transparent ? 1 : 0;
        }
    }
    return count;
}

} // namespace oa::boneyards::testing

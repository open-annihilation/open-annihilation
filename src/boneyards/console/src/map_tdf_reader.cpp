// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "map_tdf_reader.hpp"

#include "oa/data/languages/translation.hpp"
#include "oa/formats/tdf.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace oa::boneyards::console {

struct MapTdfFile {
    FileReader files{};
    oa::formats::tdf::Document document{};
    bool header_selected{};
    // Set when a caller names the language folder; otherwise the language
    // the game is showing is used.
    bool language_set{};
    char language[32]{};
    screens::MapSource source{screens::MapSource::ta_disk};
};

namespace {

constexpr const char* kMapDirectory = "maps";
constexpr const char* kMapExtension = ".OTA";
constexpr const char* kHeaderSection = "GlobalHeader";
constexpr std::size_t kMapPathCapacity = 0x100;

MapTdfFile* as_file(void* user) {
    return static_cast<MapTdfFile*>(user);
}

// maps\<name>.OTA, or maps-<language>\<name>.OTA when a language is named.
// The name is cut at its last '.'; the extension is always .OTA.
bool format_map_path(char* path, std::size_t capacity, const char* language, const char* map_name) {
    int wrote;
    if (language != nullptr && language[0] != '\0')
        wrote = std::snprintf(path, capacity, "%s-%s\\%s", kMapDirectory, language, map_name);
    else
        wrote = std::snprintf(path, capacity, "%s\\%s", kMapDirectory, map_name);
    if (wrote < 0 || static_cast<std::size_t>(wrote) >= capacity)
        return false;
    if (char* dot = std::strrchr(path, '.'))
        *dot = '\0';
    const std::size_t used = std::strlen(path);
    if (used + std::strlen(kMapExtension) >= capacity)
        return false;
    std::snprintf(path + used, capacity - used, "%s", kMapExtension);
    return true;
}

bool parse_map(MapTdfFile* file, const std::vector<uint8_t>& bytes) {
    if (bytes.size() > oa::formats::tdf::max_input_bytes)
        return false;
    oa::formats::tdf::ParseError error{};
    return oa::formats::tdf::parse_text(
        &file->document,
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<uint32_t>(bytes.size()),
        true,
        &error
    );
}

/// Loads a planet's map OTA with the TDF loader and selects its GlobalHeader.
///
/// The path is chosen once: maps-<language>\<name>.OTA when that file opens,
/// otherwise maps\<name>.OTA. The language is the one the caller set, or the
/// language the game is showing. A base-disc map that then fails to load is
/// read again at that same path after the caller's archive scan; the path is
/// not chosen a second time, and a map from any other source is not retried.
///
/// @param user the MapTdfFile
/// @param map_name the planet's map name; null or empty fails
/// @return whether the map loaded and has a GlobalHeader
bool map_tdf_open(void* user, const char* map_name) {
    MapTdfFile* file = as_file(user);
    file->header_selected = false;
    if (map_name == nullptr || map_name[0] == '\0' || file->files.read == nullptr)
        return false;
    const char* language =
        file->language_set ? file->language : oa::data::languages::installed_word();
    char chosen[kMapPathCapacity]{};
    std::vector<uint8_t> bytes;
    bool opened = false;
    if (language != nullptr && language[0] != '\0' &&
        format_map_path(chosen, sizeof chosen, language, map_name) &&
        file->files.read(file->files.user, chosen, &bytes))
        opened = true;
    if (!opened) {
        bytes.clear();
        if (!format_map_path(chosen, sizeof chosen, nullptr, map_name))
            return false;
        opened = file->files.read(file->files.user, chosen, &bytes);
    }
    bool loaded = opened && parse_map(file, bytes);
    if (!loaded && file->source == screens::MapSource::ta_disk && file->files.rescan != nullptr) {
        file->files.rescan(file->files.user);
        bytes.clear();
        opened = file->files.read(file->files.user, chosen, &bytes);
        loaded = opened && parse_map(file, bytes);
    }
    if (!loaded)
        return false;
    oa::formats::tdf::reset_cursor(&file->document);
    file->header_selected = oa::formats::tdf::select_section(&file->document, kHeaderSection);
    return file->header_selected;
}

int32_t map_tdf_get_int(void* user, const char* key, int32_t fallback) {
    const MapTdfFile* file = as_file(user);
    if (!file->header_selected)
        return fallback;
    return oa::formats::tdf::get_int(oa::formats::tdf::cursor(&file->document), key, fallback);
}

void map_tdf_note_source(void* user, screens::MapSource source) {
    as_file(user)->source = source;
}

} // namespace

MapTdfFile* map_tdf_file_create(FileReader files) {
    auto* file = static_cast<MapTdfFile*>(std::calloc(1, sizeof(MapTdfFile)));
    if (file == nullptr)
        std::abort();
    file->files = files;
    oa::formats::tdf::document_init(&file->document);
    return file;
}

void map_tdf_file_set_language(MapTdfFile* file, const char* language) {
    if (file == nullptr)
        return;
    if (language == nullptr || language[0] == '\0') {
        file->language_set = false;
        file->language[0] = '\0';
        return;
    }
    std::snprintf(file->language, sizeof file->language, "%s", language);
    file->language_set = file->language[0] != '\0';
}

void map_tdf_file_destroy(MapTdfFile* file) {
    if (file == nullptr)
        return;
    oa::formats::tdf::document_free(&file->document);
    std::free(file);
}

screens::MapTdfReader map_tdf_reader(MapTdfFile* file) {
    return screens::MapTdfReader{file, map_tdf_open, map_tdf_get_int, map_tdf_note_source};
}

} // namespace oa::boneyards::console

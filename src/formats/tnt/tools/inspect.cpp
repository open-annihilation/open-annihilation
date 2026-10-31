// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/tnt.hpp"
#include "oa/formats/ota.hpp"
#include "oa/platform/files.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <vector>

namespace {
void hash_byte(uint64_t& hash, uint8_t value) {
    hash ^= value;
    hash *= 1099511628211ULL;
}

void hash_word(uint64_t& hash, uint16_t value) {
    hash_byte(hash, static_cast<uint8_t>(value));
    hash_byte(hash, static_cast<uint8_t>(value >> 8U));
}
} // namespace

int main(int argc, char** argv) {
    if (argc < 2)
        return 2;
    bool failed = false;
    for (int argument = 1; argument < argc; ++argument) {
        const std::filesystem::path path(argv[argument]);
        std::ifstream stream(path, std::ios::binary);
        const std::vector<uint8_t> bytes{
            std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()
        };
        auto extension = oa::platform::path_to_utf8(path.extension());
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (extension == ".ota") {
            const std::string text(bytes.begin(), bytes.end());
            const auto result = oa::formats::ota::parse(text);
            if (!result.ok()) {
                std::cerr << oa::platform::path_to_utf8(path) << '\t'
                          << static_cast<int>(result.error->code) << '\t' << result.error->offset
                          << '\t' << result.error->message << '\n';
                failed = true;
                continue;
            }
            std::size_t starts = 0;
            for (const auto& schema : result.metadata->schemas)
                starts += schema.start_positions.size();
            std::cout << oa::platform::path_to_utf8(path) << '\t' << result.metadata->schemas.size()
                      << '\t' << starts << '\t' << result.metadata->mission_name << '\n';
            continue;
        }
        const auto result = oa::formats::tnt::parse(bytes);
        if (!result.ok()) {
            std::cerr << oa::platform::path_to_utf8(path) << '\t'
                      << static_cast<int>(result.error->code) << '\t' << result.error->offset
                      << '\t' << result.error->message << '\n';
            failed = true;
            continue;
        }
        uint64_t hash = 14695981039346656037ULL;
        for (const auto tile : result.map->tile_indices)
            hash_word(hash, tile);
        for (const auto& attribute : result.map->attributes) {
            hash_byte(hash, attribute.height);
            hash_word(hash, attribute.feature);
            hash_byte(hash, attribute.padding);
        }
        for (const auto pixel : result.map->tile_palette_indices)
            hash_byte(hash, pixel);
        std::cout << oa::platform::path_to_utf8(path) << '\t' << result.map->attribute_width << '\t'
                  << result.map->attribute_height << '\t' << result.map->tile_count << '\t'
                  << result.map->features.size() << '\t'
                  << (result.map->minimap ? result.map->minimap->palette_indices.size() : 0U)
                  << '\t' << std::hex << std::setw(16) << std::setfill('0') << hash << std::dec
                  << '\n';
    }
    return failed ? 1 : 0;
}

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/gaf.hpp"
#include "oa/platform/files.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <iomanip>
#include <string>
#include <vector>

namespace {

void hash_byte(uint64_t& hash, uint8_t value) {
    constexpr uint64_t fnv_prime = 1099511628211ULL;
    hash ^= value;
    hash *= fnv_prime;
}

void hash_word(uint64_t& hash, uint16_t value) {
    hash_byte(hash, static_cast<uint8_t>(value));
    hash_byte(hash, static_cast<uint8_t>(value >> 8U));
}

void count_frame(
    const oa::formats::gaf::Frame& frame,
    std::size_t& records,
    std::size_t& simple,
    std::size_t& special
) {
    ++records;
    if (frame.layers.empty())
        ++simple;
    if (frame.special_render_flag != 0)
        ++special;
    for (const auto& layer : frame.layers)
        count_frame(layer, records, simple, special);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: oa-sprite-format-inspect FILE.gaf [...]\n";
        return 2;
    }
    bool failed = false;
    for (int argument = 1; argument < argc; ++argument) {
        const std::filesystem::path path(argv[argument]);
        std::ifstream stream(path, std::ios::binary);
        const std::vector<uint8_t> bytes{
            std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()
        };
        if (!stream.good() && !stream.eof()) {
            std::cerr << oa::platform::path_to_utf8(path) << "\tread_error\n";
            failed = true;
            continue;
        }
        const auto parsed = oa::formats::gaf::parse(bytes);
        if (!parsed.ok()) {
            std::cerr << oa::platform::path_to_utf8(path) << "\tparse_error\t"
                      << static_cast<int>(parsed.error->code) << '\t' << parsed.error->offset
                      << '\t' << parsed.error->message << '\n';
            failed = true;
            continue;
        }
        std::size_t records = 0;
        std::size_t simple = 0;
        std::size_t special = 0;
        std::size_t render_unsupported = 0;
        std::size_t top_frames = 0;
        uint64_t hash = 14695981039346656037ULL;
        for (const auto& sequence : parsed.archive->sequences) {
            for (const auto character : sequence.name) {
                hash_byte(hash, static_cast<uint8_t>(character));
            }
            hash_byte(hash, 0);
            for (const auto& frame : sequence.frames) {
                ++top_frames;
                count_frame(frame, records, simple, special);
                const auto rendered = oa::formats::gaf::render_normal(frame);
                if (!rendered.ok() && rendered.error->code ==
                                          oa::formats::gaf::ErrorCode::unsupported_special_render) {
                    ++render_unsupported;
                } else if (!rendered.ok()) {
                    std::cerr << oa::platform::path_to_utf8(path) << "\trender_error\t"
                              << rendered.error->message << '\n';
                    failed = true;
                } else {
                    hash_word(hash, frame.width);
                    hash_word(hash, frame.height);
                    hash_word(hash, static_cast<uint16_t>(frame.origin_x));
                    hash_word(hash, static_cast<uint16_t>(frame.origin_y));
                    hash_byte(hash, frame.transparency_index);
                    hash_word(hash, frame.duration);
                    for (const auto pixel : rendered.frame->pixels)
                        hash_byte(hash, pixel);
                }
            }
        }
        std::cout << oa::platform::path_to_utf8(path) << '\t' << parsed.archive->sequences.size()
                  << '\t' << records << '\t' << simple << '\t' << special << '\t'
                  << render_unsupported << '\t' << top_frames << '\t' << std::hex << std::setw(16)
                  << std::setfill('0') << hash << std::dec << '\n';
    }
    return failed ? 1 : 0;
}

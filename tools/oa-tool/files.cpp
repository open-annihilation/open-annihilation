// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "files.hpp"

#include "oa/formats/png.hpp"
#include "oa/platform/files.hpp"

#include <fstream>

namespace oa::tool {

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    if (size > 256 * 1024 * 1024)
        throw Failure("input exceeds 256 MiB limit");
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
        throw Failure("cannot read input: " + oa::platform::path_to_utf8(path));
    return bytes;
}

void write_file(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        ))
        throw Failure("cannot write output: " + oa::platform::path_to_utf8(path));
    out.close();
    if (!out)
        throw Failure("cannot finish output: " + oa::platform::path_to_utf8(path));
}

void write_image(const std::filesystem::path& path, const oa::Image& image, Output& output) {
    if (path.extension() == ".png") {
        std::vector<uint8_t> png;
        const oa::formats::png::Header header{
            image.width, image.height, 8, oa::formats::png::ColorType::rgb
        };
        if (!oa::formats::png::write(oa::formats::png::Image{header, {}, image.rgb}, &png))
            throw Failure("cannot encode image: " + oa::platform::path_to_utf8(path));
        write_file(path, png);
    } else {
        std::ofstream out(path, std::ios::binary);
        out << "P6\n" << image.width << ' ' << image.height << "\n255\n";
        out.write(
            reinterpret_cast<const char*>(image.rgb.data()),
            static_cast<std::streamsize>(image.rgb.size())
        );
        out.close();
        if (!out)
            throw Failure("cannot write image: " + oa::platform::path_to_utf8(path));
    }
    output.out << image.width << 'x' << image.height << " RGB -> "
               << oa::platform::path_to_utf8(path) << '\n';
}

} // namespace oa::tool

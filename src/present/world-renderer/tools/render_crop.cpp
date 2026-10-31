// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/platform/files.hpp"
#include "oa/present/world_renderer.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream input(path, std::ios::binary);
    if (!input.read(
            reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        )) {
        throw std::runtime_error("cannot read " + oa::platform::path_to_utf8(path));
    }
    return bytes;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 8) {
            std::cerr << "usage: oa-render-terrain TNT PALETTE.PAL X Y WIDTH HEIGHT OUTPUT.ppm\n";
            return 2;
        }
        const auto parsed = oa::formats::tnt::parse(read_file(argv[1]));
        if (!parsed.ok())
            throw std::runtime_error(parsed.error->message);
        const auto palette_bytes = read_file(argv[2]);
        if (palette_bytes.size() != oa::PaletteBytes{}.size()) {
            throw std::runtime_error("palette must contain 1024 bytes");
        }
        oa::PaletteBytes palette{};
        std::copy(palette_bytes.begin(), palette_bytes.end(), palette.begin());
        const oa::present::world_renderer::Viewport viewport{
            static_cast<uint32_t>(std::stoul(argv[3])),
            static_cast<uint32_t>(std::stoul(argv[4])),
            static_cast<uint32_t>(std::stoul(argv[5])),
            static_cast<uint32_t>(std::stoul(argv[6]))
        };
        const auto rendered =
            oa::present::world_renderer::render_viewport(*parsed.map, palette, viewport);
        if (!rendered.ok())
            throw std::runtime_error(rendered.error->message);
        std::ofstream output(argv[7], std::ios::binary);
        output << "P6\n" << rendered.surface->width << ' ' << rendered.surface->height << "\n255\n";
        output.write(
            reinterpret_cast<const char*>(rendered.surface->rgb.data()),
            static_cast<std::streamsize>(rendered.surface->rgb.size())
        );
        if (!output)
            throw std::runtime_error("cannot write output PPM");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "oa-render-terrain: " << error.what() << '\n';
        return 1;
    }
}

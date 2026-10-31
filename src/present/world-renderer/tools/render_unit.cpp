// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/objects3d.hpp"
#include "oa/platform/files.hpp"
#include "oa/sim/model_runtime/instance.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/tnt.hpp"
#include "oa/present/world_renderer/unit_renderer.hpp"
#include "oa/present/world_renderer.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
std::vector<uint8_t> read(const std::filesystem::path& path) {
    const auto size = std::filesystem::file_size(path);
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    if (!stream.read(
            reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        ))
        throw std::runtime_error("cannot read " + oa::platform::path_to_utf8(path));
    return bytes;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 5) {
            std::cerr << "usage: oa-render-unit MAP.tnt PALETTE.pal MODEL.3do OUTPUT.ppm\n";
            return 2;
        }
        const auto map_bytes = read(argv[1]);
        const auto parsed_map = oa::formats::tnt::parse(map_bytes);
        if (!parsed_map.ok())
            throw std::runtime_error(parsed_map.error->message);
        const auto palette_bytes = read(argv[2]);
        if (palette_bytes.size() != oa::PaletteBytes{}.size())
            throw std::runtime_error("palette must contain 1024 bytes");
        oa::PaletteBytes palette{};
        std::copy(palette_bytes.begin(), palette_bytes.end(), palette.begin());
        auto terrain = oa::present::world_renderer::render_viewport(
            *parsed_map.map, palette, {1232, 2128, 640, 480}
        );
        if (!terrain.ok())
            throw std::runtime_error(terrain.error->message);

        const auto model_bytes = read(argv[3]);
        auto loaded = oa::formats::objects3d::load_3do(std::as_bytes(std::span(model_bytes)));
        if (!loaded.ok())
            throw std::runtime_error(loaded.error.message);
        auto model = std::make_shared<oa::formats::objects3d::Model>(std::move(*loaded.value));
        auto instance = oa::sim::model_runtime::make_instance(model);
        instance.rebuild_transforms();
        constexpr int32_t fixed = oa::formats::objects3d::kThreeDoUnitsPerWorldUnit;
        const oa::present::world_renderer::UnitProjection projection{
            {(1232 + 256) * fixed, 0, (2128 + 224) * fixed}, 1232, 2128
        };
        oa::AssetStore assets(std::filesystem::path(argv[3]).parent_path().parent_path());
        const auto textures = oa::present::world_renderer::load_texture_catalog(assets);
        const auto rendered = oa::present::world_renderer::render_colored_instance(
            *terrain.surface, instance, palette, projection, &textures
        );
        if (!rendered.ok())
            throw std::runtime_error(rendered.error->message);

        std::ofstream output(argv[4], std::ios::binary);
        output << "P6\n640 480\n255\n";
        output.write(
            reinterpret_cast<const char*>(terrain.surface->rgb.data()),
            static_cast<std::streamsize>(terrain.surface->rgb.size())
        );
        if (!output)
            throw std::runtime_error("cannot write output PPM");
        std::cout << "colored=" << rendered.stats->colored_primitives
                  << " textured=" << rendered.stats->textured_primitives
                  << " textured-deferred=" << rendered.stats->textured_primitives_deferred << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "oa-render-unit: " << error.what() << '\n';
        return 1;
    }
}

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "renderer_proof_scene.hpp"

#include "oa/formats/gaf.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/objects3d.hpp"
#include "oa/formats/tnt.hpp"
#include "oa/present/world_renderer.hpp"
#include "oa/present/world_renderer/unit_renderer.hpp"
#include "oa/sim/model_runtime/instance.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <memory>
#include <span>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace oa::app::renderer_proof_scene {
namespace {
namespace wr = oa::present::world_renderer;
namespace models = oa::formats::objects3d;
using renderer_proof_gpu::Vertex;

constexpr uint32_t atlas_width = 1024;
constexpr uint32_t atlas_height = 2048;
constexpr uint32_t channels = 4;
constexpr uint32_t cell_pixels = 16;
constexpr uint32_t palette_row = scene_height;
constexpr uint32_t texture_first_row = palette_row + 1;
constexpr float fixed_unit = 65536.0F;
constexpr uint32_t solid_color_flag = 1;

/// A material's pixel rectangle in the common atlas.
struct AtlasRect {
    uint32_t x{};
    uint32_t y{};
    uint32_t width{};
    uint32_t height{};
};

/// Tracks bounded shelf placement of model textures below the terrain.
struct AtlasPacking {
    uint32_t x{};
    uint32_t y{texture_first_row};
    uint32_t row_height{};
    std::unordered_map<std::string, AtlasRect> materials{};
};

/// Folds ASCII asset names for the material catalogue and deterministic sorting.
std::string folded(std::string name) {
    for (char& character : name)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character + ('a' - 'A'));
    return name;
}

/// Converts an existing rasteriser's RGB image to opaque RGBA.
std::vector<uint8_t> rgba(const wr::Surface& surface) {
    std::vector<uint8_t> result(
        static_cast<std::size_t>(surface.width) * surface.height * channels
    );
    for (std::size_t pixel = 0; pixel < result.size() / channels; ++pixel) {
        for (std::size_t channel = 0; channel < 3; ++channel)
            result[pixel * channels + channel] = surface.rgb[pixel * 3 + channel];
        result[pixel * channels + 3] = 255;
    }
    return result;
}

/// Writes one palette entry to the atlas, with explicit coverage.
void put_palette_pixel(
    Scene& scene, uint32_t x, uint32_t y, uint8_t index, const PaletteBytes& palette, bool covered
) {
    const std::size_t target = (static_cast<std::size_t>(y) * atlas_width + x) * channels;
    const std::size_t source = static_cast<std::size_t>(index) * palette_entry_bytes;
    for (std::size_t channel = 0; channel < 3; ++channel)
        scene.texture_rgba[target + channel] = palette[source + channel];
    scene.texture_rgba[target + 3] = covered ? 255 : 0;
}

/// Samples an attribute height at the crop's painted pixel, clamped at map edges.
float terrain_height(const formats::tnt::Map& map, uint32_t x, uint32_t y) {
    const uint32_t column = std::min(x / cell_pixels, map.attribute_width - 1);
    const uint32_t row = std::min(y / cell_pixels, map.attribute_height - 1);
    return map.attributes[static_cast<std::size_t>(row) * map.attribute_width + column].height;
}

/// Adds terrain triangles whose classic projection preserves the crop exactly.
void add_terrain(Scene& scene, const formats::tnt::Map& map, uint32_t crop_x, uint32_t crop_y) {
    const auto vertex = [&](uint32_t x, uint32_t y) {
        const float height = terrain_height(map, crop_x + x, crop_y + y);
        return Vertex{
            static_cast<float>(x),
            height,
            static_cast<float>(y) + height * 0.5F,
            static_cast<float>(x) / atlas_width,
            static_cast<float>(y) / atlas_height
        };
    };
    for (uint32_t y = 0; y < scene_height; y += cell_pixels) {
        for (uint32_t x = 0; x < scene_width; x += cell_pixels) {
            const Vertex top_left = vertex(x, y);
            const Vertex top_right = vertex(x + cell_pixels, y);
            const Vertex bottom_left = vertex(x, y + cell_pixels);
            const Vertex bottom_right = vertex(x + cell_pixels, y + cell_pixels);
            scene.vertices.insert(
                scene.vertices.end(),
                {top_left, top_right, bottom_right, top_left, bottom_right, bottom_left}
            );
        }
    }
    scene.terrain_vertex_count = scene.vertices.size();
}

/// Places a material's first frame once, rejecting atlas overflow.
AtlasRect place_texture(
    Scene& scene,
    AtlasPacking& packing,
    const std::string& name,
    const formats::gaf::RenderedFrame& frame,
    const PaletteBytes& palette
) {
    if (const auto found = packing.materials.find(name); found != packing.materials.end())
        return found->second;
    if (frame.width == 0 || frame.height == 0 || frame.width > atlas_width)
        throw std::runtime_error("model texture cannot fit proof atlas");
    if (packing.x + frame.width > atlas_width) {
        packing.x = 0;
        packing.y += packing.row_height;
        packing.row_height = 0;
    }
    if (packing.y + frame.height > atlas_height)
        throw std::runtime_error("model textures exceed proof atlas capacity");
    const AtlasRect rectangle{packing.x, packing.y, frame.width, frame.height};
    for (uint32_t y = 0; y < frame.height; ++y) {
        for (uint32_t x = 0; x < frame.width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * frame.width + x;
            const bool covered = frame.coverage.empty()
                                     ? frame.pixels[index] != frame.transparency_index
                                     : frame.coverage[index] != 0;
            put_palette_pixel(
                scene, rectangle.x + x, rectangle.y + y, frame.pixels[index], palette, covered
            );
        }
    }
    packing.x += frame.width;
    packing.row_height = std::max(packing.row_height, static_cast<uint32_t>(frame.height));
    packing.materials.emplace(name, rectangle);
    return rectangle;
}

/// Appends one real transformed model and draws its independent classic reference.
void add_model(
    Scene& scene,
    AssetStore& assets,
    const std::string& path,
    float x,
    float painted_y,
    float height,
    const PaletteBytes& palette,
    const wr::TextureCatalog& textures,
    AtlasPacking& packing,
    wr::Surface& reference
) {
    const auto bytes = assets.read(path).bytes;
    auto decoded = models::load_3do(std::as_bytes(std::span(bytes)));
    if (!decoded.ok())
        throw std::runtime_error("cannot decode " + path);
    auto handle = std::make_shared<const models::Model>(std::move(*decoded.value));
    auto instance = sim::model_runtime::make_instance(handle);
    if (instance.pieces().empty())
        throw std::runtime_error("cannot instance " + path);
    instance.rebuild_transforms();
    const float z = painted_y + height * 0.5F;
    for (const auto& piece : instance.pieces()) {
        const auto& object = instance.model().objects[piece.object_index];
        for (std::size_t primitive_index = 0; primitive_index < object.primitives.size();
             ++primitive_index) {
            if (static_cast<int32_t>(primitive_index) == object.selection_primitive)
                continue;
            const auto& primitive = object.primitives[primitive_index];
            if (primitive.vertex_indices.size() < 3)
                continue;
            AtlasRect rectangle{static_cast<uint8_t>(primitive.color_index), palette_row, 1, 1};
            const bool colored =
                (static_cast<uint32_t>(primitive.is_colored) & solid_color_flag) != 0;
            if (!colored) {
                const std::string name = folded(primitive.texture_name);
                const auto material = textures.materials.find(name);
                if (material == textures.materials.end() || primitive.vertex_indices.size() != 4)
                    continue;
                const auto* frame =
                    wr::select_material_frame(material->second, {0, true, uint8_t{0}});
                if (frame == nullptr)
                    continue;
                rectangle = place_texture(scene, packing, name, *frame, palette);
            }
            std::vector<Vertex> polygon;
            constexpr std::array<float, 4> corner_u{0, 1, 1, 0};
            constexpr std::array<float, 4> corner_v{0, 0, 1, 1};
            for (std::size_t corner = 0; corner < primitive.vertex_indices.size(); ++corner) {
                const auto& point = piece.transformed_vertices.at(primitive.vertex_indices[corner]);
                const float u = rectangle.x + 0.5F +
                                (colored ? 0.0F : corner_u[corner] * (rectangle.width - 1));
                const float v = rectangle.y + 0.5F +
                                (colored ? 0.0F : corner_v[corner] * (rectangle.height - 1));
                polygon.push_back(
                    {x + point.x / fixed_unit,
                     height + point.y / fixed_unit,
                     z - point.z / fixed_unit,
                     u / atlas_width,
                     v / atlas_height}
                );
            }
            for (std::size_t corner = 1; corner + 1 < polygon.size(); ++corner)
                scene.vertices.insert(
                    scene.vertices.end(), {polygon[0], polygon[corner], polygon[corner + 1]}
                );
        }
    }
    wr::UnitProjection projection{
        {static_cast<int32_t>(x * fixed_unit),
         static_cast<int32_t>(height * fixed_unit),
         static_cast<int32_t>(z * fixed_unit)},
        0,
        0,
        0,
        0
    };
    wr::UnitMaterialState material;
    material.force_first_frame = true;
    material.owner_team_frame = 0;
    const auto result =
        wr::render_colored_instance(reference, instance, palette, projection, &textures, material);
    if (!result.ok())
        throw std::runtime_error("classic model raster failed for " + path);
}

/// Chooses the first sampled crop with the widest terrain height range.
std::array<uint32_t, 4> relief_crop(const formats::tnt::Map& map) {
    std::array<uint32_t, 4> best{};
    const uint32_t maximum_x = map.attribute_width * cell_pixels - scene_width;
    const uint32_t maximum_y = map.attribute_height * cell_pixels - scene_height;
    constexpr uint32_t stride = 128;
    for (uint32_t y = 0; y <= maximum_y; y += stride) {
        for (uint32_t x = 0; x <= maximum_x; x += stride) {
            uint32_t low = 255;
            uint32_t high = 0;
            for (uint32_t row = 0; row <= scene_height; row += cell_pixels) {
                for (uint32_t column = 0; column <= scene_width; column += cell_pixels) {
                    const auto height =
                        static_cast<uint32_t>(terrain_height(map, x + column, y + row));
                    low = std::min(low, height);
                    high = std::max(high, height);
                }
            }
            if (high - low > best[3] - best[2])
                best = {x, y, low, high};
        }
    }
    return best;
}

/// Adds the first tree frame as a vertical cutout and independently composites its pixels.
std::string add_tree(
    Scene& scene,
    AssetStore& assets,
    const PaletteBytes& palette,
    AtlasPacking& packing,
    wr::Surface& reference,
    float ground_height
) {
    const auto archive = formats::gaf::parse(assets.read("anims/trees.gaf").bytes);
    if (!archive.archive || archive.archive->sequences.empty() ||
        archive.archive->sequences.front().frames.empty())
        throw std::runtime_error("no tree frame in anims/trees.gaf");
    const auto& sequence = archive.archive->sequences.front();
    const auto rendered = formats::gaf::render_normal(sequence.frames.front());
    if (!rendered.frame)
        throw std::runtime_error("cannot render tree frame");
    const auto& frame = *rendered.frame;
    const auto rectangle = place_texture(scene, packing, "proof-tree", frame, palette);
    constexpr int32_t tree_x = 520;
    constexpr int32_t tree_y = 180;
    const int32_t left = tree_x - frame.origin_x;
    const int32_t top = tree_y - frame.origin_y;
    const auto vertex = [&](float column, float row) {
        return Vertex{
            left + column,
            ground_height + 2.0F * (frame.origin_y - row),
            tree_y + ground_height * 0.5F,
            (rectangle.x + column) / atlas_width,
            (rectangle.y + row) / atlas_height
        };
    };
    const Vertex a = vertex(0, 0);
    const Vertex b = vertex(frame.width, 0);
    const Vertex c = vertex(frame.width, frame.height);
    const Vertex d = vertex(0, frame.height);
    scene.vertices.insert(scene.vertices.end(), {a, b, c, a, c, d});
    for (int32_t y = 0; y < frame.height; ++y) {
        for (int32_t x = 0; x < frame.width; ++x) {
            const auto source = static_cast<std::size_t>(y) * frame.width + x;
            const bool covered = frame.coverage.empty()
                                     ? frame.pixels[source] != frame.transparency_index
                                     : frame.coverage[source] != 0;
            if (!covered || left + x < 0 || top + y < 0 ||
                left + x >= static_cast<int32_t>(reference.width) ||
                top + y >= static_cast<int32_t>(reference.height))
                continue;
            const auto target =
                (static_cast<std::size_t>(top + y) * reference.width + left + x) * 3;
            for (std::size_t channel = 0; channel < 3; ++channel)
                reference.rgb[target + channel] =
                    palette[frame.pixels[source] * palette_entry_bytes + channel];
        }
    }
    return sequence.name;
}

/// Chooses Greenhaven when present, otherwise the first sufficiently large map.
std::pair<std::string, formats::tnt::Map> load_map(AssetStore& assets) {
    std::vector<std::string> names;
    for (const auto& found : assets.find("maps/*.tnt"))
        if (!found.directory)
            names.push_back(
                found.name.starts_with("maps/") || found.name.starts_with("maps\\")
                    ? found.name
                    : "maps/" + found.name
            );
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    const auto preferred = std::find_if(names.begin(), names.end(), [](const std::string& name) {
        const auto lower = folded(name);
        return lower.ends_with("greenhaven.tnt");
    });
    if (preferred != names.end())
        std::rotate(names.begin(), preferred, preferred + 1);
    for (const auto& name : names) {
        auto parsed = formats::tnt::parse(assets.read(name).bytes);
        if (parsed.map && parsed.map->attribute_width * cell_pixels >= scene_width &&
            parsed.map->attribute_height * cell_pixels >= scene_height)
            return {name, std::move(*parsed.map)};
    }
    throw std::runtime_error("no installed TNT map fits the 640x480 proof scene");
}
} // namespace

bool build(const std::filesystem::path& game_dir, Scene& scene, std::string& error) {
    try {
        AssetStore assets(game_dir);
        static_cast<void>(assets.discover("31"));
        const auto palette_data = assets.read("palettes/palette.pal").bytes;
        PaletteBytes palette{};
        if (palette_data.size() != palette.size())
            throw std::runtime_error("game palette has an unexpected size");
        std::copy(palette_data.begin(), palette_data.end(), palette.begin());
        auto [map_name, map] = load_map(assets);
        const auto crop = relief_crop(map);
        const uint32_t crop_x = crop[0];
        const uint32_t crop_y = crop[1];
        auto terrain =
            wr::render_viewport(map, palette, {crop_x, crop_y, scene_width, scene_height});
        if (!terrain.ok())
            throw std::runtime_error("classic terrain raster failed");
        Scene result;
        result.texture_width = atlas_width;
        result.texture_height = atlas_height;
        result.texture_rgba.resize(static_cast<std::size_t>(atlas_width) * atlas_height * channels);
        result.classic_terrain_rgba = rgba(*terrain.surface);
        for (uint32_t row = 0; row < scene_height; ++row)
            std::copy_n(
                result.classic_terrain_rgba.data() + row * scene_width * channels,
                scene_width * channels,
                result.texture_rgba.data() + row * atlas_width * channels
            );
        for (uint32_t index = 0; index < 256; ++index)
            put_palette_pixel(
                result, index, palette_row, static_cast<uint8_t>(index), palette, true
            );
        add_terrain(result, map, crop_x, crop_y);
        const auto textures = wr::load_texture_catalog(assets);
        AtlasPacking packing;
        constexpr uint32_t unit_x = 240;
        constexpr uint32_t unit_y = 240;
        constexpr uint32_t wreck_x = 400;
        constexpr uint32_t wreck_y = 280;
        add_model(
            result,
            assets,
            "objects3d/armflash.3do",
            unit_x,
            unit_y,
            terrain_height(map, crop_x + unit_x, crop_y + unit_y),
            palette,
            textures,
            packing,
            *terrain.surface
        );
        add_model(
            result,
            assets,
            "objects3d/armflash_dead.3do",
            wreck_x,
            wreck_y,
            terrain_height(map, crop_x + wreck_x, crop_y + wreck_y),
            palette,
            textures,
            packing,
            *terrain.surface
        );
        const std::string tree_name = add_tree(
            result,
            assets,
            palette,
            packing,
            *terrain.surface,
            terrain_height(map, crop_x + 520, crop_y + 180)
        );
        result.classic_rgba = rgba(*terrain.surface);
        result.description = map_name + " crop " + std::to_string(crop_x) + "," +
                             std::to_string(crop_y) + "; heights " + std::to_string(crop[2]) +
                             ".." + std::to_string(crop[3]) + "; ARM Flash, Flash wreck, tree " +
                             tree_name +
                             "; painted-coordinate height reconstruction; shadows omitted";
        scene = std::move(result);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}
} // namespace oa::app::renderer_proof_scene

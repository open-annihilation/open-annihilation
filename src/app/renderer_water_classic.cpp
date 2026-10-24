// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#include "renderer_water_classic.hpp"
#include "renderer_water_appearance.hpp"
#include "renderer_water_shading.hpp"
#include "renderer_water_surface.hpp"

#include <algorithm>
#include <cmath>

namespace oa::app::renderer_water_classic {
namespace {
constexpr float cell = 16;
constexpr float fixed = 65536;
constexpr std::size_t maximum_surface_cells = 4096;
constexpr std::size_t maximum_reflected_vertices = 24576;

/// Fade effects before water detail becomes smaller than a screen pixel.
float detail(const Inputs& in) {
    return std::clamp((in.view.zoom - 0.25F) / 0.25F, 0.0F, 1.0F);
}

/// Water depth at the terrain attribute cell, negative outside the map.
float depth(const Inputs& in, float x, float z) {
    if (!in.map || x < 0 || z < 0 || x >= in.map->attribute_width * cell ||
        z >= in.map->attribute_height * cell)
        return -1;
    const auto index = static_cast<std::size_t>(z / cell) * in.map->attribute_width +
                       static_cast<std::size_t>(x / cell);
    return index < in.map->attributes.size()
               ? float(in.map->sea_level) - in.map->attributes[index].height
               : -1;
}

/// Require both water and current sight at the projected point.
bool wet(const Inputs& in, float x, float z) {
    return depth(in, x, z) > 0 && in.visible && in.visible(x, z - in.map->sea_level * 0.5F);
}

/// Place a projected map point in the current render target.
std::array<float, 2> screen(const Inputs& in, float x, float y) {
    const auto& v = in.view;
    return {
        v.origin_x + (x - v.camera_x - float(v.offset.x)) * v.zoom * v.scale,
        v.origin_y + (y - v.camera_y - float(v.offset.y)) * v.zoom * v.scale
    };
}

/// Recover a water-plane point from target geometry.
std::array<float, 2> water_position(const Inputs& in, const card::Vertex& p) {
    const auto& v = in.view;
    return {
        (p.x - v.origin_x) / (v.zoom * v.scale) + v.camera_x + float(v.offset.x),
        (p.y - v.origin_y) / (v.zoom * v.scale) + v.camera_y + float(v.offset.y) +
            in.map->sea_level * 0.5F
    };
}

/// Alpha water geometry shares the battlefield target and scissor.
card::Batch batch(const Inputs& in, std::size_t first) {
    card::Batch out;
    out.target = in.view.target;
    out.blend = card::Blend::alpha;
    out.first_index = static_cast<card::Index>(first);
    out.scissored = true;
    out.scissor = {
        int32_t(in.view.origin_x),
        int32_t(in.view.origin_y),
        int32_t(in.view.width * in.view.scale),
        int32_t(in.view.height * in.view.scale)
    };
    return out;
}
} // namespace

void surface(card::CardFrame& frame, const Inputs& in) {
    if (!in.map || in.map->sea_level == 0 || detail(in) == 0 || (in.ripples <= 0 && in.coast <= 0))
        return;
    const float scale = in.view.zoom * in.view.scale;
    if (!std::isfinite(scale) || scale <= 0)
        return;
    auto out = batch(in, frame.indices.size());
    const float sea = float(in.map->sea_level);
    const int left = std::max(0, int(in.view.camera_x / cell) - 1);
    const int top = std::max(0, int((in.view.camera_y + sea * 0.5F) / cell) - 1);
    const int right = std::min(
        int(in.map->attribute_width),
        int((in.view.camera_x + in.view.width / in.view.zoom) / cell) + 2
    );
    const int bottom = std::min(
        int(in.map->attribute_height),
        int((in.view.camera_y + sea * 0.5F + in.view.height / in.view.zoom) / cell) + 2
    );
    const int stride = std::max(
        1,
        int(std::ceil(
            std::sqrt(
                double(std::max(0, right - left)) * std::max(0, bottom - top) /
                maximum_surface_cells
            )
        ))
    );
    for (int row = top; row < bottom; row += stride) {
        for (int col = left; col < right; col += stride) {
            const float x = col * cell;
            const float z = row * cell;
            if (!wet(in, x, z) || !wet(in, x + 15.999F, z) || !wet(in, x, z + 15.999F) ||
                !wet(in, x + 15.999F, z + 15.999F))
                continue;
            const float d = depth(in, x + 8, z + 8);
            const float phase = float(in.seconds) * 1.8F + x * 0.045F + z * 0.073F;
            const float wave = 0.5F + 0.5F * std::sin(phase);
            const float fetch = in.water_surface ? in.water_surface->fetch(x + 8, z + 8) : 0.25F;
            const float coastal = std::clamp(1 - d / 24, 0.0F, 1.0F) * fetch;
            const float coast_crest = 0.5F + 0.5F * std::sin(d * 0.7F + float(in.seconds) * 1.4F);
            // Nine shared samples form broad, softly varying patches. Only
            // wind drift moves them; calm water has no independent time animation.
            if (in.ripples > 0) {
                std::array<card::Vertex, 9> patch{};
                float maximum_alpha = 0;
                for (int rz = 0; rz < 3; ++rz) {
                    for (int rx = 0; rx < 3; ++rx) {
                        const float px = x + rx * 8, pz = z + rz * 8;
                        const float alpha =
                            renderer_water_surface::ripple_alpha(px, pz, in.wind, in.view.zoom) *
                            in.ripples * detail(in);
                        const auto point = screen(in, px, pz - sea * 0.5F);
                        patch[rz * 3 + rx] = {
                            point[0], point[1], {0.72F, 0.86F, 0.90F, alpha}, 0, 0
                        };
                        maximum_alpha = std::max(maximum_alpha, alpha);
                    }
                }
                if (maximum_alpha >= 0.002F) {
                    const auto first = static_cast<card::Index>(frame.vertices.size());
                    frame.vertices.insert(frame.vertices.end(), patch.begin(), patch.end());
                    for (uint32_t rz = 0; rz < 2; ++rz)
                        for (uint32_t rx = 0; rx < 2; ++rx) {
                            const auto corner = first + rz * 3 + rx;
                            for (const auto offset : {0U, 1U, 4U, 0U, 4U, 3U})
                                frame.indices.push_back(corner + offset);
                        }
                }
            }
            const float foam = detail(in) * 0.070F * in.coast * coastal *
                               (0.12F + 0.88F * in.wind.strength) * std::pow(coast_crest, 6);
            if (foam >= 0.002F) {
                const auto p = screen(in, x + 2, z + 4 + wave * 6 - sea * 0.5F);
                card::append_quad(
                    frame,
                    p[0],
                    p[1],
                    12 * scale,
                    (0.6F + coastal) * scale,
                    0,
                    0,
                    1,
                    1,
                    {0.65F, 0.86F, 0.95F, std::min(foam, 0.3F)}
                );
            }
        }
    }
    // Local impacts leave expanding, shallow rings; every segment is clipped
    // conservatively to visible water, never to a unit's current visibility alone.
    if (in.ripples > 0) {
        std::size_t events = 0;
        for (const auto& disturbance : in.disturbances) {
            if (events >= 48)
                break;
            const float age = float(in.seconds) - disturbance.birth / 30.0F;
            if (age < 0 || age >= 2 || !wet(in, disturbance.x, disturbance.z))
                continue;
            const bool firing = disturbance.kind == renderer_water::Kind::firing;
            const float radius = firing ? 3 + age * 12 : 5 + age * 24;
            const float alpha = (firing ? 0.035F : 0.14F) * disturbance.strength * in.ripples *
                                detail(in) * (1 - age / 2);
            if (radius * in.view.zoom < 2 || alpha < 0.002F)
                continue;
            ++events;
            constexpr int segments = 24;
            constexpr float tau = 6.283185307F;
            constexpr float splash_seconds = 16.0F / 30.0F;
            constexpr int splash_droplets = 6;
            if (!firing && age < splash_seconds) {
                const float progress = age / splash_seconds;
                const float splash_alpha = std::min(
                    0.35F, 0.22F * disturbance.strength * in.ripples * detail(in) * (1 - progress)
                );
                for (int droplet = 0; droplet < splash_droplets; ++droplet) {
                    const float angle = tau * droplet / splash_droplets;
                    const float spread = 2 + age * (12 + 2 * (droplet % 3));
                    const float x = disturbance.x + std::cos(angle) * spread;
                    const float z = disturbance.z + std::sin(angle) * spread;
                    const float height =
                        12 * std::sin(progress * tau * 0.5F) * (0.7F + 0.1F * (droplet % 3));
                    const float projected_z = z - height * 0.5F;
                    // Check the entire small projected footprint, as well as
                    // the landing point, so droplets cannot reveal unseen shore.
                    constexpr float half_width = 0.8F;
                    constexpr float half_height = 1.2F;
                    if (!wet(in, x, z) || !wet(in, x - half_width, projected_z - half_height) ||
                        !wet(in, x + half_width, projected_z + half_height))
                        continue;
                    const auto point =
                        screen(in, x - half_width, projected_z - sea * 0.5F - half_height);
                    card::append_quad(
                        frame,
                        point[0],
                        point[1],
                        2 * half_width * scale,
                        2 * half_height * scale,
                        0,
                        0,
                        1,
                        1,
                        {0.72F, 0.88F, 0.96F, splash_alpha}
                    );
                }
            }
            for (int segment = 0; segment < segments; ++segment) {
                const float a = tau * segment / segments;
                const float b = tau * (segment + 1) / segments;
                const float x0 = disturbance.x + std::cos(a) * radius;
                const float z0 = disturbance.z + std::sin(a) * radius;
                const float x1 = disturbance.x + std::cos(b) * radius;
                const float z1 = disturbance.z + std::sin(b) * radius;
                const float width = 0.7F + 0.4F * (1 - age / 2);
                if (!wet(in, x0 - width, z0 - width) || !wet(in, x0 + width, z0 + width) ||
                    !wet(in, x1 - width, z1 - width) || !wet(in, x1 + width, z1 + width))
                    continue;
                const auto first = static_cast<card::Index>(frame.vertices.size());
                const auto emit = [&](float x, float z) {
                    const auto point = screen(in, x, z - sea * 0.5F);
                    frame.vertices.push_back(
                        {point[0], point[1], {0.65F, 0.84F, 0.92F, std::min(alpha, 0.3F)}, 0, 0}
                    );
                };
                emit(x0, z0);
                emit(x1, z1);
                emit(x1 + std::cos(b) * width, z1 + std::sin(b) * width);
                emit(x0 + std::cos(a) * width, z0 + std::sin(a) * width);
                for (const auto index : {0U, 1U, 2U, 0U, 2U, 3U})
                    frame.indices.push_back(first + index);
            }
        }
    }
    out.index_count = static_cast<uint32_t>(frame.indices.size() - out.first_index);
    if (out.index_count)
        frame.batches.push_back(out);
}

void objects(
    card::CardFrame& frame,
    Inputs& in,
    const WorldDrawList& draws,
    const WorldDraw& draw,
    std::size_t first_vertex,
    std::size_t first_index
) {
    if (!in.map || detail(in) == 0 || first_vertex >= frame.vertices.size())
        return;
    std::optional<FixedVec3> position;
    bool naval = false;
    float hull_waterline = 0;
    if (draw.kind == WorldDrawKind::model && draw.index < draws.models.size()) {
        const auto& model = draws.models[draw.index];
        const auto* unit = model.model.unit;
        if (model.stand_in >= 0 && std::size_t(model.stand_in) < draws.stand_ins.size())
            unit = &draws.stand_ins[model.stand_in];
        if (unit)
            position = model.shown_position.value_or(unit->position);
        naval = model.stand_in < 0 && model.model.def &&
                (model.model.def->flags & OA_UNIT_DEF_FLAG_FLOATER) != 0 &&
                (model.model.def->flags &
                 (OA_UNIT_DEF_FLAG_CAN_FLY | OA_UNIT_DEF_FLAG_CAN_HOVER)) == 0 &&
                unit && unit->build_remaining == 0;
        if (model.model.def)
            hull_waterline = static_cast<uint8_t>(model.model.def->water_line);
    } else if (
        (draw.kind == WorldDrawKind::sprite || draw.kind == WorldDrawKind::blended_sprite ||
         draw.kind == WorldDrawKind::lit_sprite) &&
        draw.index < draws.sprites.size() && !draws.sprites[draw.index].shadow
    ) {
        position = draws.sprites[draw.index].world_position;
    }
    if (!position)
        return;
    const float x = position->x / fixed, y = position->y / fixed, z = position->z / fixed;
    const float sea = float(in.map->sea_level);
    if (!wet(in, x, z))
        return;
    if (draw.kind != WorldDrawKind::model && y < sea && in.shimmer > 0) {
        // Terrain decals lie at their world height; upright feature sprites
        // rise from their anchor, so their dry tops retain their original colour.
        // Effect artwork is an optical halo at its actual event height, not
        // a tall object whose upper pixels emerge through the water surface.
        const auto& sprite = draws.sprites[draw.index];
        const auto anchor = screen(in, x, z - y * 0.5F);
        for (std::size_t i = first_vertex; i < frame.vertices.size(); ++i) {
            auto& vertex = frame.vertices[i];
            const float height =
                (sprite.terrain_decal || !sprite.on_map || draw.kind == WorldDrawKind::lit_sprite)
                    ? y
                    : y + std::max(
                              0.0F, (anchor[1] - vertex.y) / (in.view.zoom * in.view.scale) * 2
                          );
            const auto point = water_position(in, vertex);
            renderer_water_shading::apply(
                vertex,
                point[0],
                point[1],
                sea - height,
                in.seconds,
                in.shimmer * detail(in),
                in.view.scale * in.view.zoom
            );
        }
    }
    const auto end_vertex = frame.vertices.size(), end_index = frame.indices.size();
    if (!naval || std::abs(y + hull_waterline - sea) > 12 || in.reflections <= 0 ||
        end_vertex - first_vertex + in.reflected_vertices > maximum_reflected_vertices)
        return;
    // The classic view has no per-pixel model depth. Attach its flattened
    // silhouette to the near hull edge, rather than the unit centre where
    // the original hull would cover almost the entire reflected picture.
    float axis = screen(in, x, z - sea * 0.5F)[1];
    float top = axis;
    for (std::size_t i = first_vertex; i < end_vertex; ++i) {
        axis = std::max(axis, frame.vertices[i].y);
        top = std::min(top, frame.vertices[i].y);
    }
    const float reflection_height =
        std::max(1.0F, std::min((axis - top) * 0.24F, 10 * in.view.zoom * in.view.scale));
    std::vector<card::Vertex> reflected;
    reflected.reserve(end_vertex - first_vertex);
    for (std::size_t i = first_vertex; i < end_vertex; ++i) {
        auto v = frame.vertices[i];
        v.y = axis + (axis - v.y) * 0.4F;
        v.x += std::sin((v.y - axis) * 0.16F + float(in.seconds) * 2) * 0.7F * in.view.scale *
               in.view.zoom;
        const float opacity = std::min(
                                  renderer_water_appearance::reflection_cap,
                                  renderer_water_appearance::reflection_opacity * in.reflections
                              ) *
                              detail(in);
        v.colour.red *= renderer_water_appearance::reflection_brightness;
        v.colour.green *= renderer_water_appearance::reflection_brightness;
        v.colour.blue *= renderer_water_appearance::reflection_brightness;
        v.colour.alpha *= opacity;
        reflected.push_back(v);
    }
    // Reject any triangle whose water footprint crosses a dry or unseen cell.
    // Conservative rejection leaves the original coast and fog entirely untouched.
    std::vector<card::Vertex> clipped_vertices;
    std::vector<card::Batch> reflected_batches;
    std::vector<card::Batch> original_batches;
    std::vector<std::size_t> original_slots;
    const auto old_batches = frame.batches.size();
    for (std::size_t b = 0; b < old_batches; ++b) {
        const auto& original = frame.batches[b];
        const auto begin = std::max(first_index, std::size_t(original.first_index));
        const auto end =
            std::min(end_index, std::size_t(original.first_index) + original.index_count);
        if (begin >= end || original.operation != card::Operation::draw)
            continue;
        auto original_part = original;
        original_part.first_index = static_cast<card::Index>(begin);
        original_part.index_count = static_cast<uint32_t>(end - begin);
        original_batches.push_back(original_part);
        original_slots.push_back(b);
        auto copy = original;
        copy.first_index = static_cast<card::Index>(frame.indices.size());
        // Model pages hold straight colours. Straight alpha also works
        // on the software card's textured, non-axis-aligned triangles.
        copy.blend = card::Blend::alpha;
        copy.sampling = card::Sampling::linear;
        for (auto i = begin; i + 2 < end; i += 3) {
            std::array<card::Vertex, 3> triangle{};
            bool valid = true;
            for (int k = 0; k < 3; ++k) {
                const auto index = frame.indices[i + k];
                if (index < first_vertex || index >= end_vertex) {
                    valid = false;
                    break;
                }
                triangle[k] = reflected[index - first_vertex];
            }
            if (!valid)
                continue;
            // Keep only a short band beside the near hull. Tall superstructure
            // and the distant side cannot become a second complete ship.
            std::array<card::Vertex, 5> clipped{};
            std::size_t count = 0;
            const float limit = axis + reflection_height;
            for (int k = 0; k < 3; ++k) {
                const auto& a = triangle[k];
                const auto& b = triangle[(k + 1) % 3];
                const bool inside_a = a.y <= limit, inside_b = b.y <= limit;
                if (inside_a)
                    clipped[count++] = a;
                if (inside_a != inside_b) {
                    const float t = (limit - a.y) / (b.y - a.y);
                    auto v = a;
                    v.x += (b.x - a.x) * t;
                    v.y = limit;
                    v.u += (b.u - a.u) * t;
                    v.v += (b.v - a.v) * t;
                    v.colour.red += (b.colour.red - a.colour.red) * t;
                    v.colour.green += (b.colour.green - a.colour.green) * t;
                    v.colour.blue += (b.colour.blue - a.colour.blue) * t;
                    v.colour.alpha += (b.colour.alpha - a.colour.alpha) * t;
                    clipped[count++] = v;
                }
            }
            if (count < 3 || in.reflected_vertices + clipped_vertices.size() + count >
                                 maximum_reflected_vertices)
                continue;
            std::array<std::array<float, 2>, 5> p{};
            for (std::size_t k = 0; k < count; ++k)
                p[k] = water_position(in, clipped[k]);
            float left = p[0][0], right = left, top = p[0][1], bottom = top;
            for (const auto& point : std::span(p.data(), count)) {
                left = std::min(left, point[0]);
                right = std::max(right, point[0]);
                top = std::min(top, point[1]);
                bottom = std::max(bottom, point[1]);
            }
            if (right - left > 256 || bottom - top > 256)
                continue;
            for (float row = std::floor(top / cell) * cell; row <= bottom && valid; row += cell)
                for (float col = std::floor(left / cell) * cell; col <= right; col += cell)
                    if (!wet(in, col + 0.5F, row + 0.5F) || !wet(in, col + 15.5F, row + 15.5F)) {
                        valid = false;
                        break;
                    }
            if (valid) {
                const auto first = static_cast<card::Index>(end_vertex + clipped_vertices.size());
                for (std::size_t k = 0; k < count; ++k) {
                    auto v = clipped[k];
                    const float fade = std::clamp(1 - (v.y - axis) / reflection_height, 0.0F, 1.0F);
                    v.colour.alpha *= fade * fade;
                    clipped_vertices.push_back(v);
                }
                for (uint32_t k = 1; k + 1 < count; ++k) {
                    frame.indices.push_back(first);
                    frame.indices.push_back(first + k);
                    frame.indices.push_back(first + k + 1);
                }
            }
        }
        copy.index_count = static_cast<uint32_t>(frame.indices.size() - copy.first_index);
        if (copy.index_count)
            reflected_batches.push_back(copy);
    }
    if (!reflected_batches.empty()) {
        in.reflected_vertices += clipped_vertices.size();
        frame.vertices.insert(
            frame.vertices.end(), clipped_vertices.begin(), clipped_vertices.end()
        );
        // Keep existing batch indices stable: sprite pages remember their slots
        // until finish(). Move only this draw's ranges to after its reflection.
        for (auto slot : original_slots) {
            auto& old = frame.batches[slot];
            old.index_count = old.first_index < first_index
                                  ? static_cast<uint32_t>(first_index - old.first_index)
                                  : 0;
        }
        frame.batches.insert(
            frame.batches.end(), reflected_batches.begin(), reflected_batches.end()
        );
        frame.batches.insert(frame.batches.end(), original_batches.begin(), original_batches.end());
    }
}
} // namespace oa::app::renderer_water_classic

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#include "renderer_water_classic.hpp"
#include "renderer_water_appearance.hpp"
#include "renderer_water_shading.hpp"
#include "renderer_water_surface.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace oa::app::renderer_water_classic {
namespace {
constexpr float cell = 16;
constexpr float fixed = 65536;
/// Most cells of foam a frame holds: far more shore than any view shows.
constexpr std::size_t maximum_foam_cells = 16384;
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

/// Water depth between the terrain attribute cells' centres, blended
/// across them so that the art's motion grows evenly from the shore;
/// negative outside the map.
float smooth_depth(const Inputs& in, float x, float z) {
    const auto width = int32_t(in.map->attribute_width);
    const auto height = int32_t(in.map->attribute_height);
    if (x < 0 || z < 0 || x >= width * cell || z >= height * cell ||
        in.map->attributes.size() < std::size_t(width) * std::size_t(height))
        return -1;
    const float across = x / cell - 0.5F, down = z / cell - 0.5F;
    const int32_t column = int32_t(std::floor(across)), row = int32_t(std::floor(down));
    const float tx = across - float(column), ty = down - float(row);
    const auto at = [&](int32_t c, int32_t r) {
        c = std::clamp(c, 0, width - 1);
        r = std::clamp(r, 0, height - 1);
        return float(
            in.map->attributes[std::size_t(r) * std::size_t(width) + std::size_t(c)].height
        );
    };
    const float upper = at(column, row) + (at(column + 1, row) - at(column, row)) * tx;
    const float lower = at(column, row + 1) + (at(column + 1, row + 1) - at(column, row + 1)) * tx;
    return float(in.map->sea_level) - (upper + (lower - upper) * ty);
}

/// Water depth smoothed over the four by four terrain attribute cells
/// around a point by cubic B-spline weights: no corner where the cells
/// step, so the coastal foam's crests run along the shore's line rather than
/// each cell's edge. Its shoreline lies a little off the cells' own, so the
/// foam's wash and dry ground keep smooth_depth.
float wave_depth(const Inputs& in, float x, float z) {
    const auto width = int32_t(in.map->attribute_width);
    const auto height = int32_t(in.map->attribute_height);
    if (x < 0 || z < 0 || x >= width * cell || z >= height * cell ||
        in.map->attributes.size() < std::size_t(width) * std::size_t(height))
        return -1;
    const float across = x / cell - 0.5F, down = z / cell - 0.5F;
    const int32_t column = int32_t(std::floor(across)), row = int32_t(std::floor(down));
    const auto weights = [](float t) {
        const float s = 1 - t;
        return std::array<float, 4>{
            s * s * s / 6,
            (3 * t * t * t - 6 * t * t + 4) / 6,
            (-3 * t * t * t + 3 * t * t + 3 * t + 1) / 6,
            t * t * t / 6
        };
    };
    const auto wx = weights(across - float(column)), wz = weights(down - float(row));
    float total = 0;
    for (int32_t j = 0; j < 4; ++j)
        for (int32_t i = 0; i < 4; ++i) {
            const int32_t c = std::clamp(column - 1 + i, 0, width - 1);
            const int32_t r = std::clamp(row - 1 + j, 0, height - 1);
            total +=
                wx[std::size_t(i)] * wz[std::size_t(j)] *
                float(
                    in.map->attributes[std::size_t(r) * std::size_t(width) + std::size_t(c)].height
                );
        }
    return float(in.map->sea_level) - total;
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

/// The sprite of a draw, where it is one: a feature, its shadow or an effect.
const SpriteDraw* sprite_of(const WorldDrawList& draws, const WorldDraw& draw) {
    const bool sprite = draw.kind == WorldDrawKind::sprite ||
                        draw.kind == WorldDrawKind::blended_sprite ||
                        draw.kind == WorldDrawKind::lit_sprite;
    return sprite && draw.index < draws.sprites.size() ? &draws.sprites[draw.index] : nullptr;
}

/// Moves a sprite under the water with the water's art the ripples move
/// beneath it, so that what lies on the sea bed stays where the art shows
/// it: a feature or shadow lying on the ground by the art's motion at each
/// corner; an upright feature by the motion at each corner as deep as the
/// corner lies, so that a top standing out of the water stays. Effects of
/// light are halos at their event, which stay.
void move_with_art(
    card::CardFrame& frame,
    const Inputs& in,
    const WorldDrawList& draws,
    const WorldDraw& draw,
    std::size_t first_vertex
) {
    const SpriteDraw* sprite = sprite_of(draws, draw);
    if (!in.art || in.art->offsets.empty() || !sprite || !sprite->world_position ||
        draw.kind == WorldDrawKind::lit_sprite)
        return;
    const float y = sprite->world_position->y / fixed;
    const float sea = float(in.map->sea_level);
    if (!(y < sea))
        return;
    const float scale = in.view.zoom * in.view.scale;
    const auto& v = in.view;
    const auto anchor =
        screen(in, sprite->world_position->x / fixed, sprite->world_position->z / fixed - y * 0.5F);
    const bool lying = sprite->terrain_decal || sprite->shadow || !sprite->on_map;
    for (std::size_t i = first_vertex; i < frame.vertices.size(); ++i) {
        auto& vertex = frame.vertices[i];
        float share = 1;
        if (!lying) {
            const float height = y + std::max(0.0F, (anchor[1] - vertex.y) / scale * 2);
            share = renderer_water_surface::art_depth_weight(sea - height);
        }
        // The picture's map pixel under the corner, where the terrain's
        // moved art was cut.
        const auto offset = in.art->at(
            (vertex.x - v.origin_x) / scale + v.camera_x + float(v.offset.x),
            (vertex.y - v.origin_y) / scale + v.camera_y + float(v.offset.y)
        );
        vertex.x += offset[0] * share * scale;
        vertex.y += offset[1] * share * scale;
    }
}
} // namespace

bool on_sea_bed(const Inputs& in, const WorldDrawList& draws, const WorldDraw& draw) {
    const SpriteDraw* sprite = sprite_of(draws, draw);
    return in.map && in.map->sea_level > 0 && sprite && draw.kind != WorldDrawKind::lit_sprite &&
           (sprite->terrain_decal || sprite->shadow) && sprite->world_position &&
           sprite->world_position->y / fixed < float(in.map->sea_level);
}

bool surface_drawn(const Inputs& in) {
    return in.map && in.map->sea_level > 0 && detail(in) > 0 && (in.ripples > 0 || in.coast > 0);
}

full_terrain::ArtMotion
art_motion(const Inputs& in, double left, double top, double right, double bottom) {
    full_terrain::ArtMotion motion;
    const float zoom = in.view.zoom;
    if (!in.map || in.map->sea_level == 0 || !(in.ripples > 0) || detail(in) == 0 ||
        !(in.wind.strength > 0) || !std::isfinite(zoom))
        return motion;
    // The grid's points 8 to 16 window pixels apart: the waves' shortest,
    // 23 map pixels, keeps three points a wavelength wherever it shows.
    uint32_t spacing = 32;
    while (spacing > 4 && float(spacing) * zoom > 16)
        spacing /= 2;
    const double map_width = double(in.map->attribute_width) * cell;
    const double map_height = double(in.map->attribute_height) * cell;
    left = std::max(left, 0.0);
    top = std::max(top, 0.0);
    right = std::min(right, map_width);
    bottom = std::min(bottom, map_height);
    if (!(left < right) || !(top < bottom))
        return motion;
    motion.spacing = spacing;
    motion.first_x = int32_t(std::floor(left / spacing)) * int32_t(spacing);
    motion.first_y = int32_t(std::floor(top / spacing)) * int32_t(spacing);
    motion.columns = uint32_t(std::ceil((right - motion.first_x) / spacing)) + 1;
    motion.rows = uint32_t(std::ceil((bottom - motion.first_y) / spacing)) + 1;
    motion.offsets.assign(std::size_t(motion.columns) * motion.rows, {});
    // Each wave's sine splits into a part across and a part down, so the
    // grid takes two sines a wave for each of its columns and rows, not for
    // every point. The picture's row y shows the water plane at y + sea / 2.
    const float sea = float(in.map->sea_level);
    const float fine = renderer_water_surface::fine_detail(zoom);
    constexpr std::size_t waves = renderer_water_surface::art_waves.size();
    std::array<std::array<float, 2>, waves> headings{};
    std::array<float, waves> amplitudes{};
    std::vector<std::array<float, 2>> across(std::size_t(motion.columns) * waves);
    std::vector<std::array<float, 2>> down(std::size_t(motion.rows) * waves);
    for (std::size_t wave = 0; wave < waves; ++wave) {
        const auto& form = renderer_water_surface::art_waves[wave];
        headings[wave] = renderer_water_surface::art_heading(form, in.wind);
        amplitudes[wave] = form.amplitude * in.wind.strength * (form.fine ? fine : 1.0F);
        const double number = 2 * std::numbers::pi / form.wavelength;
        const double travel = std::fmod(
            number * form.speed *
                (double(headings[wave][0]) * in.wind.drift_x +
                 double(headings[wave][1]) * in.wind.drift_z),
            2 * std::numbers::pi
        );
        for (uint32_t column = 0; column < motion.columns; ++column) {
            const double x = double(motion.first_x) + double(column) * spacing;
            const double phase = number * headings[wave][0] * x;
            across[column * waves + wave] = {float(std::sin(phase)), float(std::cos(phase))};
        }
        for (uint32_t row = 0; row < motion.rows; ++row) {
            const double z = double(motion.first_y) + double(row) * spacing + sea * 0.5;
            const double phase = number * headings[wave][1] * z - travel;
            down[row * waves + wave] = {float(std::sin(phase)), float(std::cos(phase))};
        }
    }
    bool moves = false;
    for (uint32_t row = 0; row < motion.rows; ++row) {
        const float y = float(motion.first_y) + float(row * spacing);
        for (uint32_t column = 0; column < motion.columns; ++column) {
            const float x = float(motion.first_x) + float(column * spacing);
            const float weight =
                renderer_water_surface::art_depth_weight(smooth_depth(in, x, y + sea * 0.5F)) *
                in.ripples * detail(in);
            if (weight <= 0)
                continue;
            std::array<float, 2> offset{};
            for (std::size_t wave = 0; wave < waves; ++wave) {
                const auto& a = across[column * waves + wave];
                const auto& b = down[row * waves + wave];
                const float shift = amplitudes[wave] * (a[0] * b[1] + a[1] * b[0]);
                offset[0] += headings[wave][0] * shift;
                offset[1] += headings[wave][1] * shift;
            }
            motion.offsets[std::size_t(row) * motion.columns + column] = {
                offset[0] * weight, offset[1] * weight
            };
            moves = true;
        }
    }
    if (!moves)
        motion = {};
    return motion;
}

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
    // Every cell the view shows, however far out: the foam lies only on
    // the shallows, so the cells under deep water and inland are passed over
    // by their depth alone, and the shore's foam never breaks into a cell
    // here and there as the view zooms out. A cell of the shore is cut into
    // squares at least 4 window pixels a side, each corner taking the foam's
    // cover at its own depth, its crests along the smoother wave_depth, and
    // the water's fetch blended there, so that the crests show as bands
    // running in along the shore and the foam changes evenly from cell to
    // cell. A corner on dry ground takes no foam, as the
    // waterline does, so the foam thins out to the shore however it runs
    // across the squares; a square with a corner on unseen ground is left
    // out.
    using renderer_water_surface::foam_depth;
    int spacing = 4;
    while (spacing < 16 && float(spacing) * scale < 4)
        spacing *= 2;
    const int across = int(cell) / spacing;

    struct FoamCorner {
        float alpha{};
        bool shown{};
    };

    std::vector<FoamCorner> corners(std::size_t(across + 1) * std::size_t(across + 1));
    const card::Colour foam_colour{
        renderer_water_appearance::foam_red,
        renderer_water_appearance::foam_green,
        renderer_water_appearance::foam_blue,
        1
    };
    // Each cell's depth once, a cell past the view's on each side too: the
    // depths blended at a cell's corners lie between those of the cells
    // around it, so a cell takes foam only where they hold some water
    // shallower than the foam's depth.
    const int map_width = int(in.map->attribute_width);
    const int map_height = int(in.map->attribute_height);
    const int depths_across = right - left + 2;
    std::vector<float> depths(
        in.coast > 0
            ? std::size_t(std::max(0, depths_across)) * std::size_t(std::max(0, bottom - top + 2))
            : 0
    );
    for (std::size_t k = 0; k < depths.size(); ++k) {
        const int c = std::clamp(left - 1 + int(k % std::size_t(depths_across)), 0, map_width - 1);
        const int r = std::clamp(top - 1 + int(k / std::size_t(depths_across)), 0, map_height - 1);
        depths[k] = depth(in, float(c) * cell + 8, float(r) * cell + 8);
    }
    const auto may_foam = [&](int col, int row) {
        float shallowest = foam_depth, deepest = 0;
        for (int dr = 0; dr < 3; ++dr)
            for (int dc = 0; dc < 3; ++dc) {
                const float d = depths
                    [std::size_t(row - top + dr) * std::size_t(depths_across) +
                     std::size_t(col - left + dc)];
                shallowest = std::min(shallowest, d);
                deepest = std::max(deepest, d);
            }
        return shallowest < foam_depth && deepest > 0;
    };
    std::size_t foam_cells = 0;
    for (int row = top; row < bottom && in.coast > 0; ++row) {
        for (int col = left; col < right && foam_cells < maximum_foam_cells; ++col) {
            const float x = col * cell;
            const float z = row * cell;
            if (!may_foam(col, row))
                continue;
            float most = 0;
            for (int down = 0; down <= across; ++down)
                for (int along = 0; along <= across; ++along) {
                    const float px = x + float(along * spacing);
                    const float pz = z + float(down * spacing);
                    const float corner_depth = smooth_depth(in, px, pz);
                    auto& corner = corners[std::size_t(down) * (across + 1) + along];
                    corner.shown = in.visible && in.visible(px, pz - sea * 0.5F);
                    const float fetch =
                        in.water_surface ? in.water_surface->smooth_fetch(px, pz) : 0.25F;
                    corner.alpha =
                        corner.shown
                            ? renderer_water_surface::foam_alpha(
                                  renderer_water_surface::foam_cover(
                                      corner_depth,
                                      corner_depth > 0 ? wave_depth(in, px, pz) : corner_depth,
                                      float(in.seconds)
                                  ),
                                  in.coast,
                                  fetch,
                                  in.wind.strength
                              ) * detail(in)
                            : 0;
                    most = std::max(most, corner.alpha);
                }
            if (most < 0.002F)
                continue;
            ++foam_cells;
            const auto first = static_cast<card::Index>(frame.vertices.size());
            for (int down = 0; down <= across; ++down)
                for (int along = 0; along <= across; ++along) {
                    const auto point = screen(
                        in, x + float(along * spacing), z + float(down * spacing) - sea * 0.5F
                    );
                    auto colour = foam_colour;
                    colour.alpha = corners[std::size_t(down) * (across + 1) + along].alpha;
                    frame.vertices.push_back({point[0], point[1], colour, 0, 0});
                }
            for (int down = 0; down < across; ++down)
                for (int along = 0; along < across; ++along) {
                    const auto corner = [&](int a, int b) {
                        return std::size_t(down + b) * (across + 1) + std::size_t(along + a);
                    };
                    const std::array<std::size_t, 4> square{
                        corner(0, 0), corner(1, 0), corner(1, 1), corner(0, 1)
                    };
                    if (!std::all_of(
                            square.begin(),
                            square.end(),
                            [&](std::size_t k) { return corners[k].shown; }
                        ) ||
                        std::all_of(square.begin(), square.end(), [&](std::size_t k) {
                            return corners[k].alpha < 0.002F;
                        }))
                        continue;
                    constexpr std::array<std::size_t, 6> triangles{0, 1, 2, 0, 2, 3};
                    for (const std::size_t k : triangles)
                        frame.indices.push_back(first + static_cast<card::Index>(square[k]));
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
    move_with_art(frame, in, draws, draw, first_vertex);
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
        in.vertex_heights.size() <= first_vertex ||
        end_vertex - first_vertex + in.reflected_vertices > maximum_reflected_vertices)
        return;
    // The hull mirrored in the water's surface: a point standing above the
    // water shows as far below its waterline as it stands above it, which
    // in the game's projection, where a point h higher shows h / 2 higher,
    // is its height above the water down the screen. Each corner takes the
    // height the model stage drew it at; the parts under the water are cut
    // off at the waterline, as they mirror nothing.
    const float scale = in.view.zoom * in.view.scale;
    const float opacity = std::min(
                              renderer_water_appearance::reflection_cap,
                              renderer_water_appearance::reflection_opacity * in.reflections
                          ) *
                          detail(in);

    struct Corner {
        card::Vertex vertex{};
        float height{};
    };

    const auto between = [](const Corner& a, const Corner& b, float t) {
        const auto mix = [t](float from, float to) { return from + (to - from) * t; };
        Corner corner = a;
        corner.vertex.x = mix(a.vertex.x, b.vertex.x);
        corner.vertex.y = mix(a.vertex.y, b.vertex.y);
        corner.vertex.u = mix(a.vertex.u, b.vertex.u);
        corner.vertex.v = mix(a.vertex.v, b.vertex.v);
        corner.vertex.colour = {
            mix(a.vertex.colour.red, b.vertex.colour.red),
            mix(a.vertex.colour.green, b.vertex.colour.green),
            mix(a.vertex.colour.blue, b.vertex.colour.blue),
            mix(a.vertex.colour.alpha, b.vertex.colour.alpha)
        };
        corner.height = mix(a.height, b.height);
        return corner;
    };
    const auto mirrored = [&](const Corner& corner) {
        auto v = corner.vertex;
        const float altitude = std::max(0.0F, corner.height - sea);
        v.y += altitude * scale;
        const auto water = water_position(in, v);
        v.x += std::sin(water[1] * 0.12F + float(in.seconds) * 1.3F) * 0.65F * scale;
        v.colour.red *= renderer_water_appearance::reflection_brightness;
        v.colour.green *= renderer_water_appearance::reflection_brightness;
        v.colour.blue *= renderer_water_appearance::reflection_brightness;
        // Fainter the higher the point it mirrors.
        v.colour.alpha *= opacity * std::clamp(1 - altitude / 100, 0.0F, 1.0F);
        return v;
    };
    // Reject any triangle whose water footprint crosses a dry or unseen cell.
    // Conservative rejection leaves the original coast and fog entirely untouched.
    std::vector<card::Vertex> clipped_vertices;
    std::vector<card::Batch> reflected_batches;
    std::vector<card::Batch> model_batches;
    std::vector<std::size_t> model_slots;
    const auto old_batches = frame.batches.size();
    for (std::size_t batch_slot = 0; batch_slot < old_batches; ++batch_slot) {
        const auto& original = frame.batches[batch_slot];
        const auto begin = std::max(first_index, std::size_t(original.first_index));
        const auto end =
            std::min(end_index, std::size_t(original.first_index) + original.index_count);
        if (begin >= end || original.operation != card::Operation::draw)
            continue;
        auto model_part = original;
        model_part.first_index = static_cast<card::Index>(begin);
        model_part.index_count = static_cast<uint32_t>(end - begin);
        model_batches.push_back(model_part);
        model_slots.push_back(batch_slot);
        auto copy = original;
        copy.first_index = static_cast<card::Index>(frame.indices.size());
        // Model pages hold straight colours. Straight alpha also works
        // on the software card's textured, non-axis-aligned triangles.
        copy.blend = card::Blend::alpha;
        copy.sampling = card::Sampling::linear;
        for (auto i = begin; i + 2 < end; i += 3) {
            std::array<Corner, 3> triangle{};
            bool valid = true;
            for (int k = 0; k < 3; ++k) {
                const auto index = frame.indices[i + k];
                if (index < first_vertex || index >= end_vertex ||
                    index >= in.vertex_heights.size() || !std::isfinite(in.vertex_heights[index])) {
                    valid = false;
                    break;
                }
                triangle[k] = {frame.vertices[index], in.vertex_heights[index]};
            }
            if (!valid)
                continue;
            // The part above the water.
            std::array<card::Vertex, 4> clipped{};
            std::size_t count = 0;
            for (int k = 0; k < 3; ++k) {
                const auto& a = triangle[k];
                const auto& b = triangle[(k + 1) % 3];
                const bool dry_a = a.height >= sea, dry_b = b.height >= sea;
                if (dry_a)
                    clipped[count++] = mirrored(a);
                if (dry_a != dry_b)
                    clipped[count++] =
                        mirrored(between(a, b, (sea - a.height) / (b.height - a.height)));
            }
            if (count < 3 || in.reflected_vertices + clipped_vertices.size() + count >
                                 maximum_reflected_vertices)
                continue;
            std::array<std::array<float, 2>, 4> p{};
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
                clipped_vertices.insert(
                    clipped_vertices.end(), clipped.begin(), clipped.begin() + std::ptrdiff_t(count)
                );
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
        for (auto slot : model_slots) {
            auto& old = frame.batches[slot];
            old.index_count = old.first_index < first_index
                                  ? static_cast<uint32_t>(first_index - old.first_index)
                                  : 0;
        }
        frame.batches.insert(
            frame.batches.end(), reflected_batches.begin(), reflected_batches.end()
        );
        frame.batches.insert(frame.batches.end(), model_batches.begin(), model_batches.end());
    }
}
} // namespace oa::app::renderer_water_classic

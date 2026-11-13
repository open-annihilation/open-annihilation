// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "body_lod_geometry.hpp"

#include <array>
#include <limits>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace {

namespace coarse_body = oa::app::body_lod;
namespace coarse_model = oa::formats::objects3d;

/// Makes a cube whose six faces have an additional material-centre vertex.
coarse_model::Object coarse_cube(int32_t scale = coarse_model::kThreeDoUnitsPerWorldUnit) {
    coarse_model::Object object;
    for (uint32_t corner = 0; corner < 8; ++corner)
        object.vertices.push_back(
            {corner & 1 ? scale : -scale, corner & 2 ? scale : -scale, corner & 4 ? scale : -scale}
        );
    constexpr std::array faces{
        std::array<uint16_t, 4>{0, 4, 6, 2},
        std::array<uint16_t, 4>{1, 3, 7, 5},
        std::array<uint16_t, 4>{0, 1, 5, 4},
        std::array<uint16_t, 4>{2, 6, 7, 3},
        std::array<uint16_t, 4>{0, 2, 3, 1},
        std::array<uint16_t, 4>{4, 5, 7, 6}
    };
    for (const auto& face : faces) {
        coarse_model::FixedVector3 center{};
        for (const auto vertex : face) {
            center.x += object.vertices[vertex].x / 4;
            center.y += object.vertices[vertex].y / 4;
            center.z += object.vertices[vertex].z / 4;
        }
        const auto middle = static_cast<uint16_t>(object.vertices.size());
        object.vertices.push_back(center);
        for (std::size_t corner = 0; corner < face.size(); ++corner) {
            coarse_model::Primitive primitive;
            primitive.is_colored = 1;
            primitive.color_index = static_cast<int32_t>(object.primitives.size());
            primitive.vertex_indices = {face[corner], face[(corner + 1) % face.size()], middle};
            object.primitives.push_back(primitive);
        }
    }
    return object;
}

/// Lists every polygon index in its ordinary drawing order.
std::vector<uint32_t> coarse_primitives(const coarse_model::Object& object) {
    std::vector<uint32_t> result;
    for (uint32_t index = 0; index < object.primitives.size(); ++index)
        result.push_back(index);
    return result;
}

/// Independently checks the hull's manifold edges, materials and outward faces.
void check_coarse_closed(const coarse_model::Object& object, const coarse_body::PieceMesh& mesh) {
    std::map<std::pair<uint16_t, uint16_t>, uint32_t> directed;
    std::set<uint16_t> retained;
    for (const auto& triangle : mesh.triangles) {
        OA_CHECK(triangle.primitive < object.primitives.size());
        for (std::size_t corner = 0; corner < triangle.vertices.size(); ++corner) {
            const auto from = triangle.vertices[corner];
            const auto to = triangle.vertices[(corner + 1) % triangle.vertices.size()];
            OA_CHECK(from < object.vertices.size());
            ++directed[{from, to}];
            retained.insert(from);
        }
    }
    for (const auto& [edge, count] : directed) {
        OA_CHECK(count == 1);
        const auto reversed = directed.find({edge.second, edge.first});
        OA_CHECK(reversed != directed.end());
        if (reversed != directed.end())
            OA_CHECK(reversed->second == 1);
    }
    OA_CHECK(retained.size() + mesh.triangles.size() == directed.size() / 2 + 2);
    for (const auto& triangle : mesh.triangles) {
        const auto& a = object.vertices[triangle.vertices[0]];
        const auto& b = object.vertices[triangle.vertices[1]];
        const auto& c = object.vertices[triangle.vertices[2]];
        const int64_t ab_x = int64_t{b.x} - a.x, ab_y = int64_t{b.y} - a.y;
        const int64_t ab_z = int64_t{b.z} - a.z;
        const int64_t ac_x = int64_t{c.x} - a.x, ac_y = int64_t{c.y} - a.y;
        const int64_t ac_z = int64_t{c.z} - a.z;
        const auto nx = ab_y * ac_z - ab_z * ac_y;
        const auto ny = ab_z * ac_x - ab_x * ac_z;
        const auto nz = ab_x * ac_y - ab_y * ac_x;
        OA_CHECK(nx * a.x + ny * a.y + nz * a.z > 0);
    }
}

/// Exercises real source-index hulls, safe fallbacks and invisible geometry.
void body_lod_geometry_tests() {
    auto cube = coarse_cube();
    const auto primitives = coarse_primitives(cube);
    const auto low = coarse_body::make_piece(cube, primitives, coarse_body::Quality::low);
    const auto medium = coarse_body::make_piece(cube, primitives, coarse_body::Quality::medium);
    OA_CHECK(low.valid && low.reduced && low.triangles.size() == 4);
    OA_CHECK(medium.valid && medium.reduced && medium.triangles.size() == 12);
    check_coarse_closed(cube, low);
    check_coarse_closed(cube, medium);
    OA_CHECK(
        coarse_body::make_piece(cube, primitives, coarse_body::Quality::medium).triangles ==
        medium.triangles
    );
    for (const auto& triangle : medium.triangles)
        for (const auto vertex : triangle.vertices)
            OA_CHECK(vertex < 8); // Face centres are interior to the selected cube boundary.

    // Neither unused geometry nor the selection polygon may change the hull.
    auto outliers = cube;
    const auto far = static_cast<uint16_t>(outliers.vertices.size());
    outliers.vertices.push_back({std::numeric_limits<int32_t>::max(), 0, 0});
    coarse_model::Primitive selection;
    selection.is_colored = 1;
    selection.vertex_indices = {far, 1, 2};
    outliers.selection_primitive = static_cast<int32_t>(outliers.primitives.size());
    outliers.primitives.push_back(selection);
    const auto selection_hull = coarse_body::make_piece(
        outliers, coarse_primitives(outliers), coarse_body::Quality::medium
    );
    OA_CHECK(selection_hull.valid && selection_hull.triangles == medium.triangles);
    outliers.selection_primitive = -1;
    outliers.primitives.back().vertex_indices = {0, 1, far}; // Collinear; draws no area.
    outliers.vertices.back() = {std::numeric_limits<int32_t>::max(), -65536, -65536};
    const auto line_hull = coarse_body::make_piece(
        outliers, coarse_primitives(outliers), coarse_body::Quality::medium
    );
    OA_CHECK(line_hull.valid && line_hull.triangles == medium.triangles);

    // A collection of zero-area lines must not acquire an opaque body.
    coarse_model::Object lines;
    lines.vertices = {
        {0, 0, 0},
        {65536, 0, 0},
        {131072, 0, 0},
        {0, 65536, 0},
        {0, 131072, 0},
        {0, 0, 65536},
        {0, 0, 131072}
    };
    for (const auto vertices :
         {std::array<uint16_t, 3>{0, 1, 2},
          std::array<uint16_t, 3>{0, 3, 4},
          std::array<uint16_t, 3>{0, 5, 6}}) {
        coarse_model::Primitive primitive;
        primitive.vertex_indices.assign(vertices.begin(), vertices.end());
        lines.primitives.push_back(primitive);
    }
    const auto empty_area =
        coarse_body::make_piece(lines, coarse_primitives(lines), coarse_body::Quality::medium);
    OA_CHECK(empty_area.valid && !empty_area.reduced && empty_area.triangles.size() == 3);

    auto planar = cube;
    for (auto& vertex : planar.vertices)
        vertex.z = 0;
    const auto flat = coarse_body::make_piece(planar, primitives, coarse_body::Quality::low);
    OA_CHECK(flat.valid && !flat.reduced && flat.triangles.size() == cube.primitives.size());
    auto thin = cube;
    for (auto& vertex : thin.vertices)
        vertex.z = vertex.z < 0 ? -1 : (vertex.z > 0 ? 1 : 0);
    const auto narrow = coarse_body::make_piece(thin, primitives, coarse_body::Quality::low);
    OA_CHECK(narrow.valid && !narrow.reduced && narrow.triangles == flat.triangles);

    // The large signed coordinate span keeps the exact original fans.
    auto wrapped = cube;
    for (auto& vertex : wrapped.vertices) {
        vertex.x = vertex.x < 0 ? std::numeric_limits<int32_t>::min()
                                : std::numeric_limits<int32_t>::max();
    }
    const auto wide = coarse_body::make_piece(wrapped, primitives, coarse_body::Quality::low);
    OA_CHECK(wide.valid && !wide.reduced && wide.triangles == flat.triangles);
    auto invalid = cube;
    invalid.primitives[0].vertex_indices[0] = std::numeric_limits<uint16_t>::max();
    OA_CHECK(!coarse_body::make_piece(invalid, primitives, coarse_body::Quality::low).valid);
    constexpr std::array<uint32_t, 1> absent{std::numeric_limits<uint32_t>::max()};
    OA_CHECK(!coarse_body::make_piece(cube, absent, coarse_body::Quality::low).valid);
    const auto none = coarse_body::make_piece(cube, {}, coarse_body::Quality::low);
    OA_CHECK(none.valid && !none.reduced && none.triangles.empty());
}

} // namespace

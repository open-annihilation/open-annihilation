// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// 3DO models as card-ready meshes: triangles, textures, piece hierarchy,
// the vertex normals and the projection.
#include "oa/present/gpu_world/model_meshes.hpp"

#include "oa/base/game_math.hpp"
#include "oa/base/geometry.hpp"
#include "oa/present/palette_tables.hpp"
#include "oa/sim/model_runtime/instance.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace oa::present::gpu_world {
namespace {

using formats::objects3d::FixedVector3;
using formats::objects3d::kNoObject;

constexpr std::size_t quad_corners = 4;
constexpr int32_t shade_rows = 32;
constexpr uint8_t opaque = 255;
constexpr uint8_t white = 255;
constexpr uint32_t fan_from_first_corner = 0;
constexpr uint32_t fan_from_second_corner = 1;

/// Folds ASCII upper case to lower case, as the texture library keys its sequences.
///
/// @param text sequence name
/// @return the folded name
std::string folded(std::string_view text) {
    std::string out(text);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
    return out;
}

/// Negates a 16.16 coordinate with 32-bit wrap, as the loader puts a model on its axes.
///
/// @param value coordinate
/// @return the negated coordinate; -2^31 stays -2^31
int32_t negate(int32_t value) noexcept {
    return std::bit_cast<int32_t>(0U - std::bit_cast<uint32_t>(value));
}

/// Converts a 16.16 coordinate to world units, counting a value a float cannot hold.
///
/// @param value 16.16 coordinate
/// @param[in,out] inexact count of coordinates rounded by the conversion
/// @return the coordinate in world units
float to_world(int32_t value, uint32_t& inexact) noexcept {
    const auto as_float = static_cast<float>(value);
    if (static_cast<double>(as_float) != static_cast<double>(value))
        ++inexact;
    return as_float / fixed_units_per_world_unit;
}

/// Floors a projected coordinate to a whole pixel.
///
/// @param value coordinate in pixels
/// @return the pixel
int32_t floor_pixel(float value) noexcept {
    return static_cast<int32_t>(std::floor(value));
}

/// Returns the whole part of a 16.16 coordinate, rounding toward negative infinity.
///
/// @param value 16.16 coordinate
/// @return the whole part
int32_t whole(int32_t value) noexcept {
    return value >> 16;
}

/// The frame corner each corner of a textured primitive maps to, 0 to 1 along each axis.
///
/// @param corner corner index, 0 to 3
/// @return u, then v
std::pair<float, float> texture_corner(std::size_t corner) noexcept {
    switch (corner) {
    case 0:
        return {0.0F, 0.0F};
    case 1:
        return {1.0F, 0.0F};
    case 2:
        return {1.0F, 1.0F};
    default:
        return {0.0F, 1.0F};
    }
}

/// A quad corner as today's whole walk sees it at rest: its pixel, and the
/// weights of the frame corner it carries.
struct WalkCorner {
    double x{};
    double y{};
    double u{};
    double v{};
};

/// What a chain of today's whole walk gives at a row: the edge's column and
/// its weights there.
struct WalkSide {
    double x{};
    double u{};
    double v{};
    bool found{};
};

/// Interpolates the corner weights at a point as today's whole walk does:
/// down the left and right chains of edges from the topmost corner to the
/// point's row, then along the row.
///
/// @param corners the quad's corners in order
/// @param x point, in pixels
/// @param y point
/// @param[out] u the weights at the point
/// @param[out] v
/// @return false when a chain turns back up (the quad is not convex at
///     rest) or the point's row has no span
bool whole_walk_at(
    const std::array<WalkCorner, quad_corners>& corners, double x, double y, double& u, double& v
) noexcept {
    std::size_t top = 0;
    std::size_t bottom = 0;
    for (std::size_t i = 1; i < quad_corners; ++i) {
        if (corners[i].y < corners[top].y)
            top = i;
        if (corners[bottom].y < corners[i].y)
            bottom = i;
    }
    const auto chain = [&](bool right, WalkSide& side) {
        side = {};
        for (std::size_t i = top; i != bottom;) {
            const std::size_t j =
                right ? (i + 1) % quad_corners : (i + quad_corners - 1) % quad_corners;
            const WalkCorner& a = corners[i];
            const WalkCorner& b = corners[j];
            if (a.y > b.y)
                return false;
            if (!side.found && a.y <= y && y < b.y) {
                const double t = (y - a.y) / (b.y - a.y);
                side = {a.x + t * (b.x - a.x), a.u + t * (b.u - a.u), a.v + t * (b.v - a.v), true};
            }
            i = j;
        }
        return side.found;
    };
    WalkSide left;
    WalkSide right;
    if (!chain(false, left) || !chain(true, right))
        return false;
    const double span = right.x - left.x;
    if (span <= 0.0)
        return false;
    const double s = (x - left.x) / span;
    u = left.u + s * (right.u - left.u);
    v = left.v + s * (right.v - left.v);
    return true;
}

/// Chooses the corner a quad's triangles fan from: the diagonal whose
/// midpoint today's whole walk interpolates nearer the mean of its corners.
///
/// @param corners the quad's corners at rest, in order
/// @return fan_from_first_corner to join corners 0 and 2,
///     fan_from_second_corner to join 1 and 3
uint32_t fan_corner(const std::array<WalkCorner, quad_corners>& corners) noexcept {
    double deviation[2] = {};
    bool walked[2] = {};
    for (std::size_t d = 0; d < 2; ++d) {
        const WalkCorner& a = corners[d];
        const WalkCorner& b = corners[d + 2];
        double u = 0.0;
        double v = 0.0;
        walked[d] = whole_walk_at(corners, (a.x + b.x) / 2, (a.y + b.y) / 2, u, v);
        deviation[d] = std::max(std::fabs(u - (a.u + b.u) / 2), std::fabs(v - (a.v + b.v) / 2));
    }
    if (!walked[1])
        return fan_from_first_corner;
    if (!walked[0])
        return fan_from_second_corner;
    return deviation[1] < deviation[0] ? fan_from_second_corner : fan_from_first_corner;
}

/// A piece's origin at rest in model space, and whether the processor's
/// transforms place the piece at all.
struct RestOrigin {
    float x{};
    float y{};
    float z{};
    bool under_root{};
};

/// What a build keeps while it walks a model.
struct Builder {
    const formats::objects3d::Model& model;
    const model::PreparedModel& prepared;
    const model::TextureLibrary& textures;
    const Palette& palette;
    float gamma{};
    ModelMesh& mesh;
    std::unordered_map<std::string, uint32_t> texture_index;
    std::vector<FixedVector3> points;
    std::vector<VertexNormal> normals;
    std::vector<RestOrigin> rest;

    /// Refuses the model with a message.
    static MeshBuildError refuse(const char* message, uint32_t object, uint32_t primitive) {
        return {message, object, primitive};
    }

    /// Checks a model's hierarchy as the model runtime checks it before instancing.
    ///
    /// @return the fault, or null
    const char* hierarchy_fault() const {
        // A non-owning handle: the check only reads the model.
        const std::shared_ptr<const formats::objects3d::Model> borrowed(
            std::shared_ptr<const formats::objects3d::Model>{}, &model
        );
        return sim::model_runtime::model_hierarchy_error(borrowed);
    }

    /// Checks every primitive of an object the mesh will walk.
    ///
    /// @param oi object index
    /// @return a null message, or why the object was refused
    MeshBuildError check_object(uint32_t oi) const {
        const auto& object = model.objects[oi];
        const auto& prep = prepared.objects[oi];
        if (object.vertices.size() > max_object_vertices)
            return refuse("3DO object has too many vertices", oi, 0);
        if (object.primitives.size() > max_object_primitives)
            return refuse("3DO object has too many primitives", oi, 0);
        if (prep.primitives.size() != object.primitives.size())
            return refuse("prepared object does not match the model's", oi, 0);
        for (std::size_t pi = first_primitive(prep); pi < prep.primitives.size(); ++pi) {
            const auto& primitive = prep.primitives[pi];
            if (primitive.source_index >= object.primitives.size())
                return refuse(
                    "prepared primitive names no primitive of the object",
                    oi,
                    static_cast<uint32_t>(pi)
                );
            const auto& source = object.primitives[primitive.source_index];
            if (source.vertex_indices.size() > max_primitive_corners)
                return refuse("3DO primitive has too many corners", oi, primitive.source_index);
            for (const uint16_t index : source.vertex_indices)
                if (index >= object.vertices.size())
                    return refuse(
                        "3DO primitive names a vertex past the object's", oi, primitive.source_index
                    );
        }
        return {};
    }

    /// Returns the first primitive drawn: the selection primitive, when there is one, is skipped.
    static std::size_t first_primitive(const model::PreparedObject& prep) noexcept {
        return prep.skips_first ? 1 : 0;
    }

    /// Resets the object's vertices to the loaded orientation.
    void load_points(const formats::objects3d::Object& object) {
        points.clear();
        points.reserve(object.vertices.size());
        for (const FixedVector3& vertex : object.vertices)
            points.push_back({negate(vertex.x), vertex.y, negate(vertex.z)});
    }

    /// Places a piece's origin at rest: its parent's origin plus its offset,
    /// as the processor's transforms add them with no turn; a piece outside
    /// the root's subtree, which they leave where it is, stays at the origin.
    void place_at_rest(uint32_t oi, const MeshPiece& piece) {
        RestOrigin origin;
        origin.under_root = oi == 0 || (piece.parent != no_piece && rest[piece.parent].under_root);
        if (origin.under_root) {
            const RestOrigin parent = piece.parent == no_piece ? RestOrigin{} : rest[piece.parent];
            origin.x = parent.x + piece.offset_x;
            origin.y = parent.y + piece.offset_y;
            origin.z = parent.z + piece.offset_z;
        }
        rest[oi] = origin;
    }

    /// Finds or adds the mesh's entry for a prepared primitive's texture.
    ///
    /// @param primitive prepared primitive with a texture
    /// @param source the 3DO primitive, whose texture name keys the library
    /// @param[out] index the entry's index
    /// @return false when the library has no such sequence
    bool texture_of(
        const model::PreparedPrimitive& primitive,
        const formats::objects3d::Primitive& source,
        uint32_t& index
    ) {
        std::string key = folded(source.texture_name);
        if (const auto found = texture_index.find(key); found != texture_index.end()) {
            index = found->second;
            return true;
        }
        const auto sequence = textures.sequences.find(key);
        if (sequence == textures.sequences.end())
            return false;
        const model::TextureSequence& entry = sequence->second;
        MeshTexture texture;
        texture.name = key;
        texture.sequence = &entry;
        texture.frame_count = static_cast<uint16_t>(entry.frames.size());
        if (!entry.frames.empty()) {
            texture.width = entry.frames.front().width;
            texture.height = entry.frames.front().height;
            texture.readable = entry.frames.front().data != nullptr;
            for (const Sprite& frame : entry.frames)
                if (frame.width != texture.width || frame.height != texture.height)
                    texture.frame_sizes_vary = true;
        }
        if ((primitive.flags & model::primitive_team) != 0)
            texture.kind = TextureKind::team;
        else if ((primitive.flags & model::primitive_animated) != 0)
            texture.kind = TextureKind::animated;
        else
            texture.kind = TextureKind::fixed;
        index = static_cast<uint32_t>(mesh.textures.size());
        mesh.textures.push_back(std::move(texture));
        texture_index.emplace(std::move(key), index);
        return true;
    }

    /// Chooses the corner a four-cornered primitive's triangles fan from,
    /// from the quad as today's whole walk sees it at rest.
    ///
    /// @param oi object index
    /// @param base the primitive's first corner in the mesh
    /// @return the corner, 0 or 1
    uint32_t quad_fan_corner(uint32_t oi, uint32_t base) const {
        std::array<WalkCorner, quad_corners> corners{};
        for (std::size_t k = 0; k < quad_corners; ++k) {
            const MeshVertex& vertex = mesh.vertices[base + k];
            const PixelPoint pixel = pixel_of_model_point(
                vertex.x + rest[oi].x, vertex.y + rest[oi].y, vertex.z + rest[oi].z, 0
            );
            const auto [u, v] = texture_corner(k);
            corners[k] = {static_cast<double>(pixel.x), static_cast<double>(pixel.y), u, v};
        }
        return fan_corner(corners);
    }

    /// Adds one object's piece, corners and triangles.
    ///
    /// @param oi object index
    /// @return a null message, or why the object was refused
    MeshBuildError add_object(uint32_t oi) {
        const auto& object = model.objects[oi];
        const auto& prep = prepared.objects[oi];
        MeshPiece piece;
        piece.object_index = oi;
        piece.parent = object.parent == kNoObject ? no_piece : object.parent;
        piece.offset_x = to_world(negate(object.offset_from_parent.x), mesh.inexact_positions);
        piece.offset_y = to_world(object.offset_from_parent.y, mesh.inexact_positions);
        piece.offset_z = to_world(negate(object.offset_from_parent.z), mesh.inexact_positions);
        piece.first_vertex = static_cast<uint32_t>(mesh.vertices.size());
        piece.first_primitive = static_cast<uint32_t>(mesh.primitives.size());
        place_at_rest(oi, piece);
        load_points(object);
        vertex_normals(object, prep, points, normals);
        for (std::size_t pi = first_primitive(prep); pi < prep.primitives.size(); ++pi) {
            const model::PreparedPrimitive& primitive = prep.primitives[pi];
            const formats::objects3d::Primitive& source = object.primitives[primitive.source_index];
            const std::size_t count = source.vertex_indices.size();
            MeshPrimitive run;
            run.source_index = primitive.source_index;
            run.prepared_index = static_cast<uint32_t>(pi);
            if ((primitive.flags & model::primitive_colored) != 0) {
                run.palette_index = primitive.color;
                if (count < 3) {
                    ++mesh.dropped_short_primitives;
                    continue;
                }
            } else {
                if (source.texture_name.empty() ||
                    (primitive.frame == nullptr && primitive.texture == nullptr)) {
                    ++mesh.dropped_invisible_primitives;
                    continue;
                }
                if (count != quad_corners) {
                    ++mesh.dropped_textured_primitives;
                    continue;
                }
                if (!texture_of(primitive, source, run.texture))
                    return refuse(
                        "prepared primitive names a texture the library lacks",
                        oi,
                        primitive.source_index
                    );
                run.flags = primitive_flag_textured;
                if ((primitive.flags & model::primitive_team) != 0)
                    run.flags |= primitive_flag_team;
                else if ((primitive.flags & model::primitive_animated) != 0)
                    run.flags |= primitive_flag_animated;
            }
            if (mesh.vertices.size() + count > max_mesh_vertices)
                return refuse("mesh has too many vertices", oi, primitive.source_index);
            const auto base = static_cast<uint32_t>(mesh.vertices.size());
            run.first_vertex = base;
            run.corner_count = static_cast<uint16_t>(count);
            const PaletteEntry& colour = palette.entries[run.palette_index];
            for (std::size_t k = 0; k < count; ++k) {
                const uint16_t index = source.vertex_indices[k];
                const FixedVector3& point = points[index];
                MeshVertex vertex;
                vertex.x = to_world(point.x, mesh.inexact_positions);
                vertex.y = to_world(point.y, mesh.inexact_positions);
                vertex.z = to_world(point.z, mesh.inexact_positions);
                vertex.normal_x = normals[index].x;
                vertex.normal_y = normals[index].y;
                vertex.normal_z = normals[index].z;
                vertex.piece = static_cast<uint16_t>(oi);
                vertex.flags = run.flags;
                vertex.alpha = opaque;
                if ((run.flags & primitive_flag_textured) != 0) {
                    const auto [u, v] = texture_corner(k);
                    vertex.u = u;
                    vertex.v = v;
                    vertex.red = white;
                    vertex.green = white;
                    vertex.blue = white;
                } else {
                    vertex.palette_index = run.palette_index;
                    vertex.red = gamma_channel(colour.r, gamma);
                    vertex.green = gamma_channel(colour.g, gamma);
                    vertex.blue = gamma_channel(colour.b, gamma);
                }
                mesh.vertices.push_back(vertex);
                mesh.source_vertex.push_back(index);
            }
            const uint32_t apex =
                count == quad_corners ? quad_fan_corner(oi, base) : fan_from_first_corner;
            run.first_index = static_cast<uint32_t>(mesh.indices.size());
            for (uint32_t k = 1; k + 1 < count; ++k) {
                mesh.indices.push_back(base + apex);
                mesh.indices.push_back(base + (apex + k) % static_cast<uint32_t>(count));
                mesh.indices.push_back(base + (apex + k + 1) % static_cast<uint32_t>(count));
            }
            run.index_count = static_cast<uint32_t>(mesh.indices.size()) - run.first_index;
            mesh.primitives.push_back(run);
        }
        piece.vertex_count = static_cast<uint32_t>(mesh.vertices.size()) - piece.first_vertex;
        piece.primitive_count =
            static_cast<uint32_t>(mesh.primitives.size()) - piece.first_primitive;
        mesh.pieces.push_back(piece);
        return {};
    }
};

} // namespace

PixelPoint pixel_of_model_point(float x, float y, float z, int32_t depth_base) noexcept {
    const int32_t height = floor_pixel(y);
    PixelPoint pixel;
    pixel.x = floor_pixel(x);
    pixel.y = floor_pixel(-z) - (height >> 1);
    // The depth keeps the height's low 16 bits, as today's raster reads them.
    pixel.depth = depth_base + static_cast<int16_t>(static_cast<uint16_t>(height));
    return pixel;
}

PixelPoint pixel_of_model_point(const FixedVector3& point, int32_t depth_base) noexcept {
    const int32_t height = whole(point.y);
    PixelPoint pixel;
    pixel.x = whole(point.x);
    pixel.y = whole(negate(point.z)) - (height >> 1);
    pixel.depth = depth_base + static_cast<int16_t>(static_cast<uint16_t>(height));
    return pixel;
}

MeshBuildError build_model_mesh(
    const formats::objects3d::Model& model,
    const model::PreparedModel& prepared,
    const model::TextureLibrary& textures,
    const Palette& palette,
    float gamma,
    ModelMesh& mesh
) {
    mesh = {};
    if (model.objects.empty())
        return Builder::refuse("3DO model has no objects", 0, 0);
    if (model.objects.size() > max_mesh_pieces)
        return Builder::refuse("3DO model has too many objects", 0, 0);
    if (prepared.model != &model)
        return Builder::refuse("prepared model was prepared for another model", 0, 0);
    if (prepared.owner.expired())
        return Builder::refuse("prepared model's model has been freed", 0, 0);
    if (prepared.objects.size() != model.objects.size())
        return Builder::refuse("prepared model does not match the model", 0, 0);
    Builder builder{model, prepared, textures, palette, gamma, mesh, {}, {}, {}, {}};
    if (const char* fault = builder.hierarchy_fault(); fault != nullptr)
        return Builder::refuse(fault, 0, 0);
    for (uint32_t oi = 0; oi < model.objects.size(); ++oi) {
        if (const MeshBuildError error = builder.check_object(oi); error.message != nullptr) {
            mesh = {};
            return error;
        }
    }
    mesh.pieces.reserve(model.objects.size());
    builder.rest.resize(model.objects.size());
    for (uint32_t oi = 0; oi < model.objects.size(); ++oi) {
        if (const MeshBuildError error = builder.add_object(oi); error.message != nullptr) {
            mesh = {};
            return error;
        }
    }
    return {};
}

std::size_t mesh_bytes(const ModelMesh& mesh) noexcept {
    std::size_t bytes =
        mesh.vertices.size() * sizeof(MeshVertex) + mesh.source_vertex.size() * sizeof(uint16_t) +
        mesh.indices.size() * sizeof(uint32_t) + mesh.primitives.size() * sizeof(MeshPrimitive) +
        mesh.pieces.size() * sizeof(MeshPiece);
    for (const MeshTexture& texture : mesh.textures)
        bytes += sizeof(MeshTexture) + texture.name.size();
    return bytes;
}

void vertex_normals(
    const formats::objects3d::Object& object,
    const model::PreparedObject& prepared,
    std::span<const FixedVector3> points,
    std::vector<VertexNormal>& normals
) {
    // Kept from call to call, so that a frame's lit pieces take no memory.
    thread_local std::vector<int32_t> uses;
    thread_local std::vector<VertexNormal> faces;
    const std::size_t first = Builder::first_primitive(prepared);
    uses.assign(points.size(), 0);
    faces.assign(prepared.primitives.size(), VertexNormal{0.0F, 1.0F, 0.0F});
    normals.assign(points.size(), VertexNormal{0.0F, 0.0F, 0.0F});
    for (std::size_t i = first; i < prepared.primitives.size(); ++i) {
        const uint32_t source = prepared.primitives[i].source_index;
        if (source >= object.primitives.size())
            continue;
        const auto& indices = object.primitives[source].vertex_indices;
        if (indices.size() < 3 || indices[0] == indices[1] || indices[1] == indices[2] ||
            indices[2] == indices[0] || indices[0] >= points.size() ||
            indices[1] >= points.size() || indices[2] >= points.size())
            continue;
        const FixedVector3& v0 = points[indices[0]];
        const FixedVector3& v1 = points[indices[1]];
        const FixedVector3& v2 = points[indices[2]];
        const base::geometry::Vec3f a =
            base::geometry::vec3f_int_delta(v1.x, v1.y, v1.z, v0.x, v0.y, v0.z);
        const base::geometry::Vec3f b =
            base::geometry::vec3f_int_delta(v1.x, v1.y, v1.z, v2.x, v2.y, v2.z);
        const VertexNormal cross{
            static_cast<float>(static_cast<double>(b.z) * a.y - static_cast<double>(a.z) * b.y),
            static_cast<float>(static_cast<double>(a.z) * b.x - static_cast<double>(b.z) * a.x),
            static_cast<float>(static_cast<double>(b.y) * a.x - static_cast<double>(b.x) * a.y)
        };
        const double length = std::sqrt(
            static_cast<double>(cross.x) * cross.x + static_cast<double>(cross.y) * cross.y +
            static_cast<double>(cross.z) * cross.z
        );
        faces[i] = {
            static_cast<float>(cross.x / length),
            static_cast<float>(cross.y / length),
            static_cast<float>(cross.z / length)
        };
    }
    for (std::size_t i = first; i < prepared.primitives.size(); ++i) {
        const uint32_t source = prepared.primitives[i].source_index;
        if (source >= object.primitives.size())
            continue;
        for (const uint16_t index : object.primitives[source].vertex_indices) {
            if (index >= points.size())
                continue;
            ++uses[index];
            normals[index].x += faces[i].x;
            normals[index].y += faces[i].y;
            normals[index].z += faces[i].z;
        }
    }
    for (std::size_t v = 0; v < points.size(); ++v) {
        if (uses[v] == 0)
            continue;
        const auto count = static_cast<float>(uses[v]);
        normals[v] = {normals[v].x / count, normals[v].y / count, normals[v].z / count};
    }
}

int32_t shade_row(
    float normal_x, float normal_y, float normal_z, const float light[3], float light_scale
) noexcept {
    const double dot =
        (static_cast<double>(light[2]) * normal_z + static_cast<double>(light[1]) * normal_y) +
        static_cast<double>(light[0]) * normal_x;
    const uint32_t truncated =
        static_cast<uint32_t>(base::game_math::truncate_low32(dot * light_scale));
    return static_cast<int32_t>(truncated & static_cast<uint32_t>(shade_rows - 1));
}

} // namespace oa::present::gpu_world

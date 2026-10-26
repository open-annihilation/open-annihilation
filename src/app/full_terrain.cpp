// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "full_terrain.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app::full_terrain {

namespace gw = oa::present::gpu_world;

namespace {

/// Indices one tile's quad takes: two triangles.
constexpr uint32_t quad_indices = 6;

/// The levels of the zoomed-out blend at a zoom below 1.
struct ZoomedOutLevels {
    uint8_t far{};      ///< the level drawn first, alone or under the nearer level
    float near_alpha{}; ///< the alpha the level above it is blended over at; 0 for none
};

/// Returns the levels of the zoomed-out blend: with t = log2(1 / zoom), the
/// two levels that bracket t, the nearer blended over the farther at alpha
/// 1 - (t - near); at a whole t the far level alone; at and beyond the last
/// tile level that level alone, reduced.
///
/// @param zoom window pixels per map pixel, below 1
/// @return the levels
ZoomedOutLevels zoomed_out_levels(float zoom) noexcept {
    const double t = std::log2(1.0 / static_cast<double>(zoom));
    constexpr double last_level = gw::tile_level_count - 1;
    const double far = std::min(std::ceil(t), last_level);
    const double near_alpha = t < far ? std::clamp(1.0 - (t - (far - 1.0)), 0.0, 1.0) : 0.0;
    return {static_cast<uint8_t>(far), static_cast<float>(near_alpha)};
}

} // namespace

TerrainDrawPlan plan_terrain_draw(float zoom, bool pixel_art_sampling) noexcept {
    TerrainDrawPlan plan;
    if (!(zoom > 0.0F))
        zoom = 1.0F;
    if (zoom < 1.0F) {
        const ZoomedOutLevels levels = zoomed_out_levels(zoom);
        plan.passes[0] = {levels.far, card::Sampling::linear, card::Blend::none, 1.0F};
        plan.pass_count = 1;
        if (levels.near_alpha > 0.0F) {
            plan.passes[1] = {
                static_cast<uint8_t>(levels.far - 1U),
                card::Sampling::linear,
                card::Blend::alpha,
                levels.near_alpha
            };
            plan.pass_count = 2;
        }
        return plan;
    }
    const bool whole = std::floor(zoom) == zoom;
    plan.pass_count = 1;
    if (whole || !pixel_art_sampling) {
        plan.passes[0] = {0, card::Sampling::nearest, card::Blend::none, 1.0F};
        plan.through_target = !whole;
        plan.target_zoom = static_cast<uint32_t>(std::ceil(zoom));
        return plan;
    }
    plan.passes[0] = {0, card::Sampling::pixel_art, card::Blend::none, 1.0F};
    return plan;
}

TileRange visible_tiles(const gw::TerrainAtlas& atlas, const TerrainView& view) noexcept {
    TileRange range;
    if (atlas.grid_width == 0 || atlas.grid_height == 0 || !(view.scale > 0.0F))
        return range;
    // The tile a map pixel lies in, rounded toward negative infinity, so
    // that a camera before the map counts its tiles as one on it does.
    const auto tile_of = [](int64_t pixel) {
        constexpr auto edge = static_cast<int64_t>(gw::tile_edge);
        return pixel >= 0 ? pixel / edge : -((edge - 1 - pixel) / edge);
    };
    // The map pixels the margin reaches before the camera.
    const auto before = static_cast<int64_t>(
        std::ceil(static_cast<double>(view.margin) / static_cast<double>(view.scale))
    );
    const auto columns = [&](int32_t camera, uint32_t extent, uint32_t cells) {
        const auto map_pixels = static_cast<int64_t>(
            std::ceil(static_cast<double>(extent) / static_cast<double>(view.scale))
        );
        const auto last = static_cast<int64_t>(cells);
        const int64_t first = std::clamp<int64_t>(tile_of(camera - before), 0, last);
        const int64_t end = std::clamp<int64_t>(tile_of(camera + map_pixels) + 1, 0, last);
        return std::pair{static_cast<uint32_t>(first), static_cast<uint32_t>(std::max(first, end))};
    };
    std::tie(range.first_column, range.end_column) =
        columns(view.camera_x, view.width, atlas.grid_width);
    std::tie(range.first_row, range.end_row) =
        columns(view.camera_y, view.height, atlas.grid_height);
    return range;
}

uint32_t append_terrain_tiles(
    card::CardFrame& frame,
    const gw::TerrainAtlas& atlas,
    std::span<const card::PageHandle> pages,
    const TerrainView& view,
    const TerrainPass& pass,
    card::TargetHandle target,
    const card::Rect* scissor
) {
    const TileRange range = visible_tiles(atlas, view);
    if (pass.level >= gw::tile_level_count || pages.size() < atlas.pages.size())
        return 0;
    // The tiles of one pass never overlap, so their order is free: the
    // pass's quads are grouped by page, one batch for each page, so that
    // the card draws a view in as many calls as the atlas has pages, not as
    // many as the grid changes page. The tiles are counted by page first,
    // each page is given one run of the frame's indices, and each tile's
    // six indices are then written into its page's run; the vertices stay
    // in grid order.
    std::vector<uint32_t> page_tiles(atlas.pages.size(), 0);
    uint32_t quads = 0;
    for (uint32_t row = range.first_row; row < range.end_row; ++row)
        for (uint32_t column = range.first_column; column < range.end_column; ++column) {
            const auto cell = static_cast<std::size_t>(row) * atlas.grid_width + column;
            const auto rect = gw::tile_rect(atlas, atlas.grid[cell], pass.level);
            if (!rect || rect->page >= atlas.pages.size())
                continue;
            ++page_tiles[rect->page];
            ++quads;
        }
    if (quads == 0)
        return 0;
    // Where each page's run of indices goes next.
    std::vector<card::Index> run_next(atlas.pages.size(), 0);
    auto next = static_cast<card::Index>(frame.indices.size());
    for (std::size_t page = 0; page < atlas.pages.size(); ++page) {
        if (page_tiles[page] == 0)
            continue;
        card::Batch batch;
        batch.operation = card::Operation::draw;
        batch.target = target;
        batch.page = pages[page];
        batch.level = pass.level;
        batch.blend = pass.blend;
        batch.sampling = pass.sampling;
        batch.scissored = scissor != nullptr;
        if (scissor != nullptr)
            batch.scissor = *scissor;
        batch.first_index = next;
        batch.index_count = quad_indices * page_tiles[page];
        frame.batches.push_back(batch);
        run_next[page] = next;
        next += batch.index_count;
    }
    frame.indices.resize(next);
    const card::Colour colour{1.0F, 1.0F, 1.0F, pass.alpha};
    const auto tile_pixels = static_cast<float>(static_cast<double>(gw::tile_edge) * view.scale);
    // Where a tile edge lands, and, on whole pixels, rounded to the nearest.
    const auto edge_at = [&](uint32_t tile, float origin, int32_t camera) {
        const auto at = static_cast<float>(
            static_cast<double>(origin) +
            (static_cast<double>(tile) * gw::tile_edge - static_cast<double>(camera)) * view.scale
        );
        return view.whole_pixels ? std::round(at) : at;
    };
    for (uint32_t row = range.first_row; row < range.end_row; ++row) {
        const float y = edge_at(row, view.origin_y, view.camera_y);
        const float high =
            view.whole_pixels ? edge_at(row + 1, view.origin_y, view.camera_y) - y : tile_pixels;
        for (uint32_t column = range.first_column; column < range.end_column; ++column) {
            const auto cell = static_cast<std::size_t>(row) * atlas.grid_width + column;
            const auto rect = gw::tile_rect(atlas, atlas.grid[cell], pass.level);
            if (!rect || rect->page >= atlas.pages.size())
                continue;
            const auto& level = atlas.pages[rect->page].levels[pass.level];
            const float x = edge_at(column, view.origin_x, view.camera_x);
            const float wide = view.whole_pixels
                                   ? edge_at(column + 1, view.origin_x, view.camera_x) - x
                                   : tile_pixels;
            const float level_width = static_cast<float>(level.width);
            const float level_height = static_cast<float>(level.height);
            const float u0 = static_cast<float>(rect->x) / level_width;
            const float v0 = static_cast<float>(rect->y) / level_height;
            const float u1 = static_cast<float>(rect->x + rect->edge) / level_width;
            const float v1 = static_cast<float>(rect->y + rect->edge) / level_height;
            // The quad's corners and its two triangles as card::append_quad
            // lays them out, the diagonal from the top-left to the
            // bottom-right corner, with the indices in the page's run.
            const auto first = static_cast<card::Index>(frame.vertices.size());
            frame.vertices.push_back({x, y, colour, u0, v0});
            frame.vertices.push_back({x + wide, y, colour, u1, v0});
            frame.vertices.push_back({x + wide, y + high, colour, u1, v1});
            frame.vertices.push_back({x, y + high, colour, u0, v1});
            card::Index* run = frame.indices.data() + run_next[rect->page];
            run[0] = first;
            run[1] = first + 1;
            run[2] = first + 2;
            run[3] = first;
            run[4] = first + 2;
            run[5] = first + 3;
            run_next[rect->page] += quad_indices;
        }
    }
    return quads;
}

namespace {

/// The least texels a side a mosaic chunk may span: a texel it draws and
/// one of each neighbour's.
constexpr uint32_t least_chunk_limit = 3;

/// One axis of a chunk: its texture's texels and the part of them drawn.
struct ChunkSpan {
    uint32_t start{};
    uint32_t size{};
    uint32_t drawn_start{};
    uint32_t drawn_size{};
};

/// Cuts one axis of a picture into spans of at most `limit` texels, each
/// drawn part followed by the next, with one texel of each neighbour held
/// beyond a drawn part that meets one.
///
/// @param total texels along the axis, above 0
/// @param limit the most texels a span may hold, at least least_chunk_limit
/// @return the spans, in order
std::vector<ChunkSpan> cut_axis(uint32_t total, uint32_t limit) {
    if (total <= limit)
        return {{0, total, 0, total}};
    std::vector<ChunkSpan> spans;
    const uint32_t stride = limit - 2U;
    for (uint32_t drawn = 0; drawn < total; drawn += stride) {
        const uint32_t end = std::min(total, drawn + stride);
        const uint32_t start = drawn == 0 ? 0U : drawn - 1U;
        const uint32_t stop = end == total ? total : end + 1U;
        spans.push_back({start, stop - start, drawn, end - drawn});
    }
    return spans;
}

} // namespace

MosaicLevel
plan_mosaic_level(uint32_t grid_width, uint32_t grid_height, uint8_t level, uint32_t chunk_limit) {
    MosaicLevel mosaic;
    mosaic.level = level;
    if (grid_width == 0 || grid_height == 0 || level < first_mosaic_level ||
        level >= gw::tile_level_count || chunk_limit < least_chunk_limit)
        return mosaic;
    const uint32_t tile_texels = gw::tile_edge >> level;
    mosaic.width = grid_width * tile_texels;
    mosaic.height = grid_height * tile_texels;
    const std::vector<ChunkSpan> columns = cut_axis(mosaic.width, chunk_limit);
    const std::vector<ChunkSpan> rows = cut_axis(mosaic.height, chunk_limit);
    mosaic.chunks.reserve(columns.size() * rows.size());
    for (const ChunkSpan& row : rows)
        for (const ChunkSpan& column : columns)
            mosaic.chunks.push_back(
                {column.start,
                 row.start,
                 column.size,
                 row.size,
                 column.drawn_start,
                 row.drawn_start,
                 column.drawn_size,
                 row.drawn_size}
            );
    return mosaic;
}

uint32_t append_terrain_mosaic(
    card::CardFrame& frame,
    const MosaicLevel& mosaic,
    std::span<const card::PageHandle> pages,
    const TerrainView& view,
    const TerrainPass& pass,
    card::TargetHandle target,
    const card::Rect* scissor
) {
    if (pass.level != mosaic.level || pages.size() < mosaic.chunks.size() || !(view.scale > 0.0F))
        return 0;
    const double scale = view.scale;
    // Map pixels a texel of the level spans.
    const double texel_pixels = static_cast<double>(1U << mosaic.level);
    // The map pixels the view shows, its margin before the camera included.
    const double view_left = view.camera_x - static_cast<double>(view.margin) / scale;
    const double view_top = view.camera_y - static_cast<double>(view.margin) / scale;
    const double view_right = view.camera_x + static_cast<double>(view.width) / scale;
    const double view_bottom = view.camera_y + static_cast<double>(view.height) / scale;
    // Where a map pixel lands in the target, and, on whole pixels, rounded
    // to the nearest, so that chunks meeting at a map pixel meet there.
    const auto land = [&](double map_pixel, float origin, int32_t camera) {
        const auto at = static_cast<float>(origin + (map_pixel - camera) * scale);
        return view.whole_pixels ? std::round(at) : at;
    };
    const card::Colour colour{1.0F, 1.0F, 1.0F, pass.alpha};
    uint32_t quads = 0;
    for (std::size_t index = 0; index < mosaic.chunks.size(); ++index) {
        const MosaicChunk& chunk = mosaic.chunks[index];
        const double left = std::max(view_left, static_cast<double>(chunk.drawn_x) * texel_pixels);
        const double top = std::max(view_top, static_cast<double>(chunk.drawn_y) * texel_pixels);
        const double right = std::min(
            view_right, static_cast<double>(chunk.drawn_x + chunk.drawn_width) * texel_pixels
        );
        const double bottom = std::min(
            view_bottom, static_cast<double>(chunk.drawn_y + chunk.drawn_height) * texel_pixels
        );
        if (!(left < right) || !(top < bottom))
            continue;
        const float x0 = land(left, view.origin_x, view.camera_x);
        const float y0 = land(top, view.origin_y, view.camera_y);
        const float x1 = land(right, view.origin_x, view.camera_x);
        const float y1 = land(bottom, view.origin_y, view.camera_y);
        if (!(x0 < x1) || !(y0 < y1))
            continue;
        // The chunk's texture coordinate of a map pixel.
        const auto u_of = [&](double map_pixel) {
            return static_cast<float>(
                (map_pixel / texel_pixels - static_cast<double>(chunk.x)) /
                static_cast<double>(chunk.width)
            );
        };
        const auto v_of = [&](double map_pixel) {
            return static_cast<float>(
                (map_pixel / texel_pixels - static_cast<double>(chunk.y)) /
                static_cast<double>(chunk.height)
            );
        };
        card::Batch batch;
        batch.operation = card::Operation::draw;
        batch.target = target;
        batch.page = pages[index];
        batch.level = 0;
        batch.blend = pass.blend;
        batch.sampling = pass.sampling;
        batch.scissored = scissor != nullptr;
        if (scissor != nullptr)
            batch.scissor = *scissor;
        batch.first_index = static_cast<card::Index>(frame.indices.size());
        batch.index_count = quad_indices;
        card::append_quad(
            frame,
            x0,
            y0,
            x1 - x0,
            y1 - y0,
            u_of(left),
            v_of(top),
            u_of(right),
            v_of(bottom),
            colour
        );
        frame.batches.push_back(batch);
        ++quads;
    }
    return quads;
}

namespace {

/// Where a pass landed the map pixels of one axis in its target: evenly
/// between the target pixels `start` and `end` it landed the map pixels
/// `from` and `to` at, and past them at the view's scale.
struct Landing {
    double from{};
    double to{};
    float start{};
    float end{};
    float origin{};
    int32_t camera{};
    double scale{1.0};

    [[nodiscard]] float at(double map_pixel) const noexcept {
        if (!(from < to) || map_pixel < from || map_pixel > to)
            return static_cast<float>(origin + (map_pixel - camera) * scale);
        return static_cast<float>(start + (end - start) * (map_pixel - from) / (to - from));
    }
};

/// A rectangle of the terrain's picture to draw again moved: its map
/// pixels, where the pass landed them and its texture coordinates there.
struct MovedRect {
    double left{};
    double top{};
    double right{};
    double bottom{};
    Landing across;
    Landing down;
    float u0{};
    float v0{};
    float u1{};
    float v1{};
};

/// Cuts one axis of a moved rectangle: its edges, any lines the pass drew
/// on between them, and the motion grid's lines.
void cut_axis_lines(
    double from,
    double to,
    uint32_t spacing,
    std::span<const double> drawn,
    std::vector<double>& lines
) {
    lines.clear();
    lines.push_back(from);
    for (double line = (std::floor(from / spacing) + 1.0) * spacing; line < to; line += spacing)
        lines.push_back(line);
    for (const double line : drawn)
        if (line > from && line < to)
            lines.push_back(line);
    lines.push_back(to);
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
}

/// The scratch arrays one moved rectangle is cut into.
struct MovedScratch {
    std::vector<double> across;
    std::vector<double> down;
    std::vector<std::array<float, 2>> offsets;
    std::vector<card::Index> vertices;
};

/// Appends the squares of a moved rectangle that move: each corner where
/// the pass drew it, moved by the motion there in target pixels, its
/// texture coordinate that of its unmoved place; a corner the squares
/// share is one vertex.
///
/// @return squares appended
uint32_t append_moved_rect(
    card::CardFrame& frame,
    const ArtMotion& motion,
    const MovedRect& rect,
    std::span<const double> drawn_across,
    std::span<const double> drawn_down,
    float scale,
    const card::Colour& colour,
    std::vector<card::Index>& indices,
    MovedScratch& scratch
) {
    if (!(rect.left < rect.right) || !(rect.top < rect.bottom))
        return 0;
    cut_axis_lines(rect.left, rect.right, motion.spacing, drawn_across, scratch.across);
    cut_axis_lines(rect.top, rect.bottom, motion.spacing, drawn_down, scratch.down);
    const std::size_t columns = scratch.across.size();
    const std::size_t rows = scratch.down.size();
    scratch.offsets.resize(columns * rows);
    bool moves = false;
    for (std::size_t row = 0; row < rows; ++row)
        for (std::size_t column = 0; column < columns; ++column) {
            const auto offset = motion.at(scratch.across[column], scratch.down[row]);
            scratch.offsets[row * columns + column] = offset;
            moves = moves || offset[0] != 0.0F || offset[1] != 0.0F;
        }
    if (!moves)
        return 0;
    constexpr card::Index none = ~card::Index{0};
    scratch.vertices.assign(columns * rows, none);
    const auto vertex = [&](std::size_t column, std::size_t row) {
        auto& index = scratch.vertices[row * columns + column];
        if (index != none)
            return index;
        const double x = scratch.across[column];
        const double y = scratch.down[row];
        const auto& offset = scratch.offsets[row * columns + column];
        const auto u = static_cast<float>(
            rect.u0 + (rect.u1 - rect.u0) * (x - rect.left) / (rect.right - rect.left)
        );
        const auto v = static_cast<float>(
            rect.v0 + (rect.v1 - rect.v0) * (y - rect.top) / (rect.bottom - rect.top)
        );
        index = static_cast<card::Index>(frame.vertices.size());
        frame.vertices.push_back(
            {rect.across.at(x) + offset[0] * scale,
             rect.down.at(y) + offset[1] * scale,
             colour,
             u,
             v}
        );
        return index;
    };
    const auto moved = [&](std::size_t column, std::size_t row) {
        const auto& offset = scratch.offsets[row * columns + column];
        return offset[0] != 0.0F || offset[1] != 0.0F;
    };
    uint32_t squares = 0;
    for (std::size_t row = 0; row + 1 < rows; ++row)
        for (std::size_t column = 0; column + 1 < columns; ++column) {
            if (!moved(column, row) && !moved(column + 1, row) && !moved(column, row + 1) &&
                !moved(column + 1, row + 1))
                continue;
            // The diagonal from the top-left corner to the bottom-right, as
            // card::append_quad lays a quad out.
            const card::Index top_left = vertex(column, row);
            const card::Index top_right = vertex(column + 1, row);
            const card::Index bottom_right = vertex(column + 1, row + 1);
            const card::Index bottom_left = vertex(column, row + 1);
            indices.insert(
                indices.end(),
                {top_left, top_right, bottom_right, top_left, bottom_right, bottom_left}
            );
            ++squares;
        }
    return squares;
}

/// Appends a draw batch of a run of indices just appended to the frame.
void append_moved_batch(
    card::CardFrame& frame,
    card::PageHandle page,
    uint8_t level,
    const TerrainPass& pass,
    std::span<const card::Index> indices,
    card::TargetHandle target,
    const card::Rect* scissor
) {
    if (indices.empty())
        return;
    card::Batch batch;
    batch.operation = card::Operation::draw;
    batch.target = target;
    batch.page = page;
    batch.level = level;
    batch.blend = pass.blend;
    batch.sampling = pass.sampling;
    batch.scissored = scissor != nullptr;
    if (scissor != nullptr)
        batch.scissor = *scissor;
    batch.first_index = static_cast<card::Index>(frame.indices.size());
    batch.index_count = static_cast<uint32_t>(indices.size());
    frame.indices.insert(frame.indices.end(), indices.begin(), indices.end());
    frame.batches.push_back(batch);
}

/// Tells whether a motion can move the art: a grid on the tile edge's
/// divisors with an offset for every point, not all of them zero.
bool can_move(const ArtMotion& motion) noexcept {
    return motion.spacing != 0 && gw::tile_edge % motion.spacing == 0 && motion.columns != 0 &&
           motion.rows != 0 &&
           motion.offsets.size() == static_cast<std::size_t>(motion.columns) * motion.rows &&
           !motion.still();
}

} // namespace

uint32_t append_moved_terrain_tiles(
    card::CardFrame& frame,
    const gw::TerrainAtlas& atlas,
    std::span<const card::PageHandle> pages,
    const TerrainView& view,
    const TerrainPass& pass,
    const ArtMotion& motion,
    card::TargetHandle target,
    const card::Rect* scissor
) {
    if (pass.level >= gw::tile_level_count || pages.size() < atlas.pages.size() ||
        !can_move(motion))
        return 0;
    TileRange range = visible_tiles(atlas, view);
    if (range.first_column >= range.end_column || range.first_row >= range.end_row)
        return 0;
    // A tile past the view's edge can move its art into it.
    range.first_column -= range.first_column > 0 ? 1U : 0U;
    range.first_row -= range.first_row > 0 ? 1U : 0U;
    range.end_column = std::min(range.end_column + 1U, atlas.grid_width);
    range.end_row = std::min(range.end_row + 1U, atlas.grid_height);
    const auto edge_at = [&](double map_pixel, float origin, int32_t camera) {
        const auto at = static_cast<float>(
            static_cast<double>(origin) + (map_pixel - static_cast<double>(camera)) * view.scale
        );
        return view.whole_pixels ? std::round(at) : at;
    };
    const card::Colour colour{1.0F, 1.0F, 1.0F, pass.alpha};
    std::vector<std::vector<card::Index>> page_indices(atlas.pages.size());
    MovedScratch scratch;
    uint32_t squares = 0;
    constexpr double edge = gw::tile_edge;
    for (uint32_t row = range.first_row; row < range.end_row; ++row)
        for (uint32_t column = range.first_column; column < range.end_column; ++column) {
            const auto cell = static_cast<std::size_t>(row) * atlas.grid_width + column;
            const auto rect = gw::tile_rect(atlas, atlas.grid[cell], pass.level);
            if (!rect || rect->page >= atlas.pages.size())
                continue;
            const auto& level = atlas.pages[rect->page].levels[pass.level];
            MovedRect moved;
            moved.left = column * edge;
            moved.top = row * edge;
            moved.right = moved.left + edge;
            moved.bottom = moved.top + edge;
            // Within the tile, the pass's quad lands its art evenly between
            // the corners it landed.
            moved.across = {
                moved.left,
                moved.right,
                edge_at(moved.left, view.origin_x, view.camera_x),
                edge_at(moved.right, view.origin_x, view.camera_x),
                view.origin_x,
                view.camera_x,
                view.scale
            };
            moved.down = {
                moved.top,
                moved.bottom,
                edge_at(moved.top, view.origin_y, view.camera_y),
                edge_at(moved.bottom, view.origin_y, view.camera_y),
                view.origin_y,
                view.camera_y,
                view.scale
            };
            moved.u0 = static_cast<float>(rect->x) / static_cast<float>(level.width);
            moved.v0 = static_cast<float>(rect->y) / static_cast<float>(level.height);
            moved.u1 = static_cast<float>(rect->x + rect->edge) / static_cast<float>(level.width);
            moved.v1 = static_cast<float>(rect->y + rect->edge) / static_cast<float>(level.height);
            squares += append_moved_rect(
                frame, motion, moved, {}, {}, view.scale, colour, page_indices[rect->page], scratch
            );
        }
    for (std::size_t page = 0; page < atlas.pages.size(); ++page)
        append_moved_batch(
            frame, pages[page], pass.level, pass, page_indices[page], target, scissor
        );
    return squares;
}

uint32_t append_moved_terrain_mosaic(
    card::CardFrame& frame,
    const MosaicLevel& mosaic,
    std::span<const card::PageHandle> pages,
    const TerrainView& view,
    const TerrainPass& pass,
    const ArtMotion& motion,
    card::TargetHandle target,
    const card::Rect* scissor
) {
    if (pass.level != mosaic.level || pages.size() < mosaic.chunks.size() || !(view.scale > 0.0F) ||
        !can_move(motion))
        return 0;
    const double scale = view.scale;
    const double texel_pixels = static_cast<double>(1U << mosaic.level);
    // The view's map pixels, as append_terrain_mosaic clips the chunks to
    // them, and a grid spacing past them, from where art moves into view.
    const double view_left = view.camera_x - static_cast<double>(view.margin) / scale;
    const double view_top = view.camera_y - static_cast<double>(view.margin) / scale;
    const double view_right = view.camera_x + static_cast<double>(view.width) / scale;
    const double view_bottom = view.camera_y + static_cast<double>(view.height) / scale;
    const double reach = motion.spacing;
    const auto land = [&](double map_pixel, float origin, int32_t camera) {
        const auto at = static_cast<float>(origin + (map_pixel - camera) * scale);
        return view.whole_pixels ? std::round(at) : at;
    };
    const card::Colour colour{1.0F, 1.0F, 1.0F, pass.alpha};
    MovedScratch scratch;
    std::vector<card::Index> indices;
    uint32_t squares = 0;
    for (std::size_t index = 0; index < mosaic.chunks.size(); ++index) {
        const MosaicChunk& chunk = mosaic.chunks[index];
        const double drawn_left = static_cast<double>(chunk.drawn_x) * texel_pixels;
        const double drawn_top = static_cast<double>(chunk.drawn_y) * texel_pixels;
        const double drawn_right =
            static_cast<double>(chunk.drawn_x + chunk.drawn_width) * texel_pixels;
        const double drawn_bottom =
            static_cast<double>(chunk.drawn_y + chunk.drawn_height) * texel_pixels;
        MovedRect moved;
        moved.left = std::max(view_left - reach, drawn_left);
        moved.top = std::max(view_top - reach, drawn_top);
        moved.right = std::min(view_right + reach, drawn_right);
        moved.bottom = std::min(view_bottom + reach, drawn_bottom);
        if (!(moved.left < moved.right) || !(moved.top < moved.bottom))
            continue;
        // The part append_terrain_mosaic drew lands its art evenly between
        // the edges it landed; past them, at the view's scale.
        const double base_left = std::max(view_left, drawn_left);
        const double base_top = std::max(view_top, drawn_top);
        const double base_right = std::min(view_right, drawn_right);
        const double base_bottom = std::min(view_bottom, drawn_bottom);
        moved.across = {
            base_left,
            base_right,
            land(base_left, view.origin_x, view.camera_x),
            land(base_right, view.origin_x, view.camera_x),
            view.origin_x,
            view.camera_x,
            scale
        };
        moved.down = {
            base_top,
            base_bottom,
            land(base_top, view.origin_y, view.camera_y),
            land(base_bottom, view.origin_y, view.camera_y),
            view.origin_y,
            view.camera_y,
            scale
        };
        moved.u0 = static_cast<float>(
            (moved.left / texel_pixels - static_cast<double>(chunk.x)) / chunk.width
        );
        moved.v0 = static_cast<float>(
            (moved.top / texel_pixels - static_cast<double>(chunk.y)) / chunk.height
        );
        moved.u1 = static_cast<float>(
            (moved.right / texel_pixels - static_cast<double>(chunk.x)) / chunk.width
        );
        moved.v1 = static_cast<float>(
            (moved.bottom / texel_pixels - static_cast<double>(chunk.y)) / chunk.height
        );
        const std::array<double, 2> drawn_across{base_left, base_right};
        const std::array<double, 2> drawn_down{base_top, base_bottom};
        indices.clear();
        squares += append_moved_rect(
            frame, motion, moved, drawn_across, drawn_down, view.scale, colour, indices, scratch
        );
        append_moved_batch(frame, pages[index], 0, pass, indices, target, scissor);
    }
    return squares;
}

} // namespace oa::app::full_terrain

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A photo's look on the preview and on the picture that is saved. The
// filter runs after the player's gamma. With no look on, nothing here runs.
#include "oa/app/runtime.hpp"

#include "full_effects.hpp"
#include "photo_state.hpp"

#include "oa/formats/tnt.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/present/photo/looks.hpp"
#include "oa/present/world_renderer/world_fog.hpp"
#include "oa/sim/visibility_state.hpp"
#include "oa/ui/photo_panel/crop.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace photo = oa::present::photo;

/// Two digits, or more when the number needs them. A negative value is 00.
std::string two_digits(int value) {
    if (value < 0)
        value = 0;
    std::string text = std::to_string(value);
    if (text.size() < 2)
        text.insert(0, 2 - text.size(), '0');
    return text;
}

/// Four digits for a year. A negative year is 0000.
std::string four_digits(int value) {
    if (value < 0)
        value = 0;
    std::string text = std::to_string(value);
    if (text.size() < 4)
        text.insert(0, 4 - text.size(), '0');
    return text;
}

/// The local date as YYYY-MM-DD. The filter draws this text; it does not
/// format a date of its own.
std::string iso_date() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#if defined(_WIN32)
    localtime_s(&parts, &now);
#else
    localtime_r(&now, &parts);
#endif
    return four_digits(parts.tm_year + 1900) + "-" + two_digits(parts.tm_mon + 1) + "-" +
           two_digits(parts.tm_mday);
}

/// The game clock as hh:mm:ss. Thirty ticks are one second.
std::string clock_text(uint32_t tick) {
    const uint32_t seconds = tick / 30U;
    const auto two = [](uint32_t part) {
        const std::string digits = std::to_string(part);
        return part < 10U ? "0" + digits : digits;
    };
    return two(seconds / 3600U) + ":" + two(seconds / 60U % 60U) + ":" + two(seconds % 60U);
}

/// The faces a caption bar draws with. One stack for the process, opened
/// from the faces that ship with the engine. Null when they did not open:
/// the bar is still painted, with no letters.
oa::platform::text_font::FontStack* caption_fonts() {
    static std::unique_ptr<oa::platform::text_font::FontStack> stack =
        oa::platform::text_font::FontStack::open(oa::platform::text_font::bundled_font_directory());
    return stack.get();
}

/// Mixes one value into a preview key.
void mix_key(uint64_t& hash, uint64_t value) noexcept {
    hash ^= value + 0x9E3779B97F4A7C15ULL + (hash << 6U) + (hash >> 2U);
}

/// The bits of a float, so a zoom or an angle can join the preview key.
uint32_t float_bits(float value) noexcept {
    static_assert(sizeof(uint32_t) == sizeof(float));
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

/// The map's height at one pixel, bilinearly, as the ground under a unit is
/// read. Below 0 is off the grid. Water is not raised: a low shore stays low.
int32_t sampled_height(const oa::formats::tnt::Map& map, int32_t x, int32_t z) noexcept {
    constexpr int kSpacing = 16;
    if (map.attribute_width < 2 || map.attribute_height < 2 || x < 0 || z < 0)
        return -1;
    const int cell_x = x >> 4;
    const int cell_z = z >> 4;
    if (cell_x < 0 || cell_z < 0 || static_cast<uint32_t>(cell_x) + 1U >= map.attribute_width ||
        static_cast<uint32_t>(cell_z) + 1U >= map.attribute_height)
        return -1;
    const std::size_t at =
        static_cast<std::size_t>(cell_z) * map.attribute_width + static_cast<std::size_t>(cell_x);
    if (at + map.attribute_width + 1U >= map.attributes.size())
        return -1;
    const int fx = x & 15;
    const int fz = z & 15;
    const int top_left = map.attributes[at].height;
    const int top_right = map.attributes[at + 1U].height;
    const int bottom_left = map.attributes[at + map.attribute_width].height;
    const int bottom_right = map.attributes[at + map.attribute_width + 1U].height;
    const int top = top_left + (top_right - top_left) * fx / kSpacing;
    const int bottom = bottom_left + (bottom_right - bottom_left) * fx / kSpacing;
    return top + (bottom - top) * fz / kSpacing;
}

namespace world_renderer = oa::present::world_renderer;

/// The map the haze samples, and the fog of war the battlefield draws. One
/// column at a time.
struct GroundColumn {
    const oa::formats::tnt::Map* map{nullptr};
    /// 1 for each sight cell the fog draws as never mapped, row by row. Null
    /// when the view draws no such fog.
    const uint8_t* unmapped{nullptr};
    int32_t fog_width{0};  ///< sight cells across
    int32_t fog_height{0}; ///< sight cells down
    /// The black tiles' art, for the ragged edge of a tile with some corners
    /// never mapped. Without it those tiles draw nothing, as in the view.
    const world_renderer::FogTileSet* fog_tiles{nullptr};
};

/// Writes one column of ground heights for the haze, from map row `first_row`.
void write_ground_column(
    void* user, int32_t map_x, int32_t first_row, int32_t* heights, int32_t count
) {
    const auto* ground = static_cast<const GroundColumn*>(user);
    if (ground == nullptr || ground->map == nullptr || heights == nullptr)
        return;
    for (int32_t row = 0; row < count; ++row)
        heights[row] = sampled_height(*ground->map, map_x, first_row + row);
}

/// `value` divided by `by`, rounded toward negative infinity.
int32_t floor_divided(int32_t value, int32_t by) noexcept {
    const int32_t quotient = value / by;
    return (value % by != 0 && (value < 0) != (by < 0)) ? quotient - 1 : quotient;
}

/// Marks the screen rows of map column `map_x`, from `first_row`, that the
/// fog of war draws black over, as draw_fog_grid draws them: the fog's tiles
/// sit half a sight cell off the cells, each corner on a cell's centre; a
/// tile whose four corners were never mapped is filled, and one with some is
/// drawn through its black FOG.GAF tile. A corner past the map's edge reads
/// the edge's cell, as the view copies the border tiles' corners outward.
void write_fog_column(void* user, int32_t map_x, int32_t first_row, uint8_t* black, int32_t count) {
    const auto* ground = static_cast<const GroundColumn*>(user);
    if (ground == nullptr || ground->unmapped == nullptr || black == nullptr ||
        ground->fog_width <= 0 || ground->fog_height <= 0)
        return;
    constexpr int32_t kCell = world_renderer::fog_cell_pixels;
    constexpr int32_t kHalf = kCell / 2;
    const auto never_mapped = [ground](int32_t cell_x, int32_t cell_z) {
        const int32_t x = std::clamp(cell_x, int32_t{0}, ground->fog_width - 1);
        const int32_t z = std::clamp(cell_z, int32_t{0}, ground->fog_height - 1);
        return ground->unmapped
                   [static_cast<std::size_t>(z) * static_cast<std::size_t>(ground->fog_width) +
                    static_cast<std::size_t>(x)] != 0;
    };
    const bool art = ground->fog_tiles != nullptr && ground->fog_tiles->loaded();
    const int32_t tile_x = floor_divided(map_x - kHalf, kCell);
    const int32_t texel_x = map_x - kHalf - tile_x * kCell;
    int32_t row = 0;
    while (row < count) {
        const int32_t map_z = first_row + row;
        const int32_t tile_z = floor_divided(map_z - kHalf, kCell);
        const int32_t tile_top = tile_z * kCell + kHalf;
        const int32_t rows = std::min(count - row, tile_top + kCell - map_z);
        uint8_t mask = 0;
        if (never_mapped(tile_x, tile_z))
            mask |= world_renderer::fog_corner_top_left;
        if (never_mapped(tile_x + 1, tile_z))
            mask |= world_renderer::fog_corner_top_right;
        if (never_mapped(tile_x, tile_z + 1))
            mask |= world_renderer::fog_corner_bottom_left;
        if (never_mapped(tile_x + 1, tile_z + 1))
            mask |= world_renderer::fog_corner_bottom_right;
        if (mask == world_renderer::fog_mask_full) {
            std::fill(black + row, black + row + rows, uint8_t{1});
        } else if (mask != 0 && art) {
            // The view's variant is (row + column + phase) & 3 counted from
            // its first tile, which is the tile's own (x + z + 2) & 3.
            const int32_t variant = (tile_x + tile_z + 2) & (world_renderer::fog_tile_variants - 1);
            const world_renderer::FogTileArt& tile =
                ground->fog_tiles->at(world_renderer::FogTileSet::black, variant, mask);
            for (int32_t at = 0; at < rows; ++at) {
                const int32_t texel_z = map_z + at - tile_top;
                black[row + at] = tile.opaque[static_cast<std::size_t>(texel_z * kCell + texel_x)];
            }
        }
        row += rows;
    }
}

/// Points the haze at the fog it keeps.
///
/// @param[in,out] ground the field's user
/// @param[in,out] field the field, given the fog's callback when there is a fog
/// @param cells 1 for each sight cell drawn never mapped
/// @param width cells across; 0 for no fog
/// @param height cells down
/// @param tiles the black tiles' art
void keep_fog(
    GroundColumn& ground,
    photo::HazeField& field,
    const std::vector<uint8_t>& cells,
    int32_t width,
    int32_t height,
    const world_renderer::FogTileSet& tiles
) noexcept {
    if (width <= 0 || height <= 0 ||
        cells.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height))
        return;
    ground.unmapped = cells.data();
    ground.fog_width = width;
    ground.fog_height = height;
    ground.fog_tiles = &tiles;
    field.fog = write_fog_column;
}

/// How many map pixels the height grid covers. The last cell has no neighbour
/// to blend with, so it is not a pixel the ground can answer.
int32_t map_pixels(uint32_t attributes) noexcept {
    if (attributes < 2U)
        return 0;
    const uint64_t cells = static_cast<uint64_t>(attributes) - 1U;
    if (cells > static_cast<uint64_t>(std::numeric_limits<int32_t>::max() / 16))
        return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(cells * 16U);
}

/// Map pixels per picture pixel, in 256ths. A factor or a zoom below 1 counts
/// as 1, and the step never falls below one 256th.
int32_t picture_step(int size_factor, float zoom) noexcept {
    const int factor = size_factor < 1 ? 1 : size_factor;
    const double times = static_cast<double>(factor) * static_cast<double>(zoom);
    if (!(times > 0.0))
        return 256;
    double step = 256.0 / times;
    if (step < 1.0)
        step = 1.0;
    if (step > static_cast<double>(std::numeric_limits<int32_t>::max()))
        return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(step + 0.5);
}

/// Map position of picture pixel 0, in 256ths.
int32_t picture_origin(int32_t camera, int32_t crop, int32_t step) noexcept {
    const int64_t value = static_cast<int64_t>(camera) * 256 +
                          static_cast<int64_t>(crop) * static_cast<int64_t>(step);
    if (value > std::numeric_limits<int32_t>::max())
        return std::numeric_limits<int32_t>::max();
    if (value < std::numeric_limits<int32_t>::min())
        return std::numeric_limits<int32_t>::min();
    return static_cast<int32_t>(value);
}

/// A look built from the visit. The caption names the map, the clock and
/// the date, joined with a middle dot. An idle look is still built: the
/// caller decides whether to paint it. The fields are passed one by one
/// because the visit's state is private to Runtime. Width and radius are
/// already in this picture's pixels.
photo::Look make_look(
    uint8_t preset_index,
    int32_t vignette,
    int32_t roundness,
    int32_t grain,
    int32_t grain_size,
    uint8_t frame_index,
    uint32_t seed,
    const std::string& map,
    uint32_t tick,
    int32_t tilt_centre,
    int32_t tilt_width,
    int32_t tilt_radius,
    int32_t haze
) {
    photo::Look look;
    const int preset = std::clamp(static_cast<int>(preset_index), 0, 5);
    const int frame = std::clamp(static_cast<int>(frame_index), 0, 3);
    look.preset = static_cast<photo::Preset>(preset);
    look.vignette = std::clamp(vignette, 0, 100);
    look.roundness = std::clamp(roundness, 0, 100);
    look.grain = std::clamp(grain, 0, 100);
    look.grain_size = std::max(grain_size, 1);
    look.seed = seed;
    look.frame = static_cast<photo::Frame>(frame);
    look.tilt_centre = std::clamp(tilt_centre, 0, 100);
    look.tilt_width = std::max(tilt_width, 0);
    look.tilt_radius = std::max(tilt_radius, 0);
    look.haze = std::clamp(haze, 0, 100);
    if (look.frame != photo::Frame::caption)
        return look;
    const std::string clock = clock_text(tick);
    const std::string date = iso_date();
    if (!map.empty())
        look.caption = map + " \xC2\xB7 " + clock + " \xC2\xB7 " + date;
    else
        look.caption = clock + " \xC2\xB7 " + date;
    return look;
}

/// Fills the ground haze. Nothing is filled when the amount is 0 or the map
/// has no height grid. The caller passes 0 in the turned view.
///
/// @param[out] field the field apply_look reads
/// @param[out] ground the map the field's callback reads; it must outlive the paint
/// @param map the loaded map, or null
/// @param camera_x the camera, in map pixels
/// @param camera_z the camera, in map pixels
/// @param zoom the camera's zoom
/// @param size_factor the picture's size factor
/// @param crop_x the crop's left edge in the picture before it was cut
/// @param crop_y the crop's top edge
/// @param amount the haze, 0 to 100
/// @return true when `field` should be passed to apply_look
bool prepare_haze(
    photo::HazeField& field,
    GroundColumn& ground,
    const oa::formats::tnt::Map* map,
    int32_t camera_x,
    int32_t camera_z,
    float zoom,
    int size_factor,
    int32_t crop_x,
    int32_t crop_y,
    int32_t amount
) noexcept {
    if (amount <= 0 || map == nullptr)
        return false;
    const int32_t columns = map_pixels(map->attribute_width);
    const int32_t rows = map_pixels(map->attribute_height);
    if (columns <= 0 || rows <= 0)
        return false;
    ground.map = map;
    const int32_t step = picture_step(size_factor, zoom);
    field = {};
    field.amount = amount;
    field.column = write_ground_column;
    field.user = &ground;
    field.map_columns = columns;
    field.map_rows = rows;
    field.origin_x_256 = picture_origin(camera_x, crop_x, step);
    field.origin_row_256 = picture_origin(camera_z, crop_y, step);
    field.step_256 = step;
    return true;
}

/// True when a render scale is one, within a hundredth.
bool scale_is_one(float scale) noexcept {
    const float delta = scale - 1.0F;
    return delta < 0.01F && delta > -0.01F;
}

/// Copies RGBA rows into a packed buffer. The pitch may be wider than the row.
void pack_rgba(
    std::vector<uint8_t>& packed,
    const uint8_t* bytes,
    uint32_t width,
    uint32_t height,
    uint32_t pitch
) {
    const std::size_t row = static_cast<std::size_t>(width) * 4U;
    packed.resize(row * height);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(
            packed.data() + static_cast<std::size_t>(y) * row,
            bytes + static_cast<std::size_t>(y) * pitch,
            row
        );
    }
}

} // namespace

int Runtime::photo_look_scale(int size_factor, uint32_t photo_width, int32_t field_width) noexcept {
    const int factor = size_factor < 1 ? 1 : size_factor;
    if (field_width <= 0 || photo_width == 0)
        return factor;
    const auto field = static_cast<uint32_t>(field_width);
    const uint64_t covered = static_cast<uint64_t>(field) * static_cast<uint64_t>(factor);
    if (static_cast<uint64_t>(photo_width) <= covered)
        return factor;
    const uint64_t scaled =
        (static_cast<uint64_t>(photo_width) + static_cast<uint64_t>(field) / 2U) / field;
    if (scaled <= static_cast<uint64_t>(factor))
        return factor;
    if (scaled > static_cast<uint64_t>(std::numeric_limits<int>::max()))
        return std::numeric_limits<int>::max();
    return static_cast<int>(scaled);
}

int32_t Runtime::photo_haze_step(
    int size_factor, float zoom, uint32_t photo_width, int32_t field_width
) noexcept {
    return picture_step(photo_look_scale(size_factor, photo_width, field_width), zoom);
}

bool Runtime::photo_look_active() const noexcept {
    if (!photo_mode() || photo_ == nullptr)
        return false;
    const bool haze = photo_->look_haze > 0 && !live_renderer_active();
    return photo_->look_preset != 0 || photo_->look_vignette > 0 || photo_->look_grain > 0 ||
           photo_->look_frame != 0 || photo_->look_tilt_radius > 0 || haze;
}

uint64_t Runtime::photo_look_key() const {
    uint64_t hash = 0x6C6F6F6BULL;
    if (photo_ == nullptr)
        return hash;
    const bool held = photo_->picture_held && photo_->camera_held;
    const uint32_t tick =
        held ? photo_->held_tick : (match_ != nullptr ? match_->state().game.tick : 0U);
    mix_key(hash, photo_->look_preset);
    mix_key(hash, static_cast<uint64_t>(photo_->look_vignette));
    mix_key(hash, static_cast<uint64_t>(photo_->look_roundness));
    mix_key(hash, static_cast<uint64_t>(photo_->look_grain));
    mix_key(hash, static_cast<uint64_t>(photo_->look_grain_size));
    mix_key(hash, photo_->look_frame);
    mix_key(hash, static_cast<uint64_t>(photo_->look_tilt_centre));
    mix_key(hash, static_cast<uint64_t>(photo_->look_tilt_width));
    mix_key(hash, static_cast<uint64_t>(photo_->look_tilt_radius));
    mix_key(hash, static_cast<uint64_t>(photo_->look_haze));
    mix_key(hash, static_cast<uint64_t>(photo_->look_depth_of_field));
    mix_key(hash, live_renderer_active() ? 1U : 0U);
    mix_key(hash, tick);
    mix_key(hash, static_cast<uint64_t>(photo_->shutter_eighth));
    mix_key(hash, photo_->show_selection ? 1U : 0U);
    mix_key(hash, photo_->show_health_bars ? 1U : 0U);
    mix_key(hash, held ? 1U : 0U);
    // A crop moves the look to the crop, so a new guide filters again.
    mix_key(hash, photo_->guide);
    const int32_t camera_x = held ? photo_->held_camera_x : match_camera_x_;
    const int32_t camera_z = held ? photo_->held_camera_z : match_camera_z_;
    const float zoom = held ? photo_->held_zoom : match_zoom_;
    mix_key(hash, static_cast<uint64_t>(static_cast<uint32_t>(camera_x)));
    mix_key(hash, static_cast<uint64_t>(static_cast<uint32_t>(camera_z)));
    mix_key(hash, float_bits(zoom));
    if (live_renderer_active() || held) {
        const float yaw = held ? photo_->held_live_yaw : live_camera_angles()[0];
        const float pitch = held ? photo_->held_live_pitch : live_camera_angles()[1];
        mix_key(hash, float_bits(yaw));
        mix_key(hash, float_bits(pitch));
    }
    const uint32_t width = held ? photo_->held_width : match_world_cpu_.width;
    const uint32_t height = held ? photo_->held_height : match_world_cpu_.height;
    mix_key(hash, width);
    mix_key(hash, height);
    if (photo_->look_frame != 0) {
        const photo::Look look = make_look(
            photo_->look_preset,
            photo_->look_vignette,
            photo_->look_roundness,
            photo_->look_grain,
            photo_->look_grain_size,
            photo_->look_frame,
            tick,
            selected_map_name_runtime_,
            tick,
            photo_->look_tilt_centre,
            photo_->look_tilt_width,
            photo_->look_tilt_radius,
            photo_->look_haze
        );
        for (const unsigned char byte : look.caption)
            mix_key(hash, byte);
    }
    return hash;
}

bool Runtime::photo_look_preview_due() const {
    if (!photo_look_active() || photo_ == nullptr)
        return false;
    if (!photo_look_key_set_ || photo_look_key_ != photo_look_key())
        return true;
    const bool still = photo_->picture_held || photo_->time == PhotoState::Time::frozen;
    return !still;
}

bool Runtime::photo_fog_cells(
    std::vector<uint8_t>& unmapped, int32_t& width, int32_t& height
) const {
    unmapped.clear();
    width = 0;
    height = 0;
    // The fog's own conditions (apply_match_fog): no black without mapping,
    // and no fog at all without the viewer's sight.
    if (match_ == nullptr || !match_mapping_on())
        return false;
    const uint8_t viewer = match_view_player();
    try {
        if (match_->player_coverage(viewer).empty())
            return false;
    } catch (const std::exception&) {
        return false;
    }
    const oa::sim::visibility_state::PlayerSightGrid& sight = match_->sight();
    if (sight.width <= 0 || sight.height <= 0)
        return false;
    // The viewer's mapped bit and its allies', as build_fog_grid reads them.
    const auto shown = static_cast<uint16_t>((1U << (viewer & 0x1fU)) | allied_mapped_bits());
    const std::size_t cells =
        static_cast<std::size_t>(sight.width) * static_cast<std::size_t>(sight.height);
    unmapped.assign(cells, 1);
    const std::size_t read = std::min(cells, sight.player_bits.size());
    for (std::size_t cell = 0; cell < read; ++cell)
        unmapped[cell] = static_cast<uint8_t>((sight.player_bits[cell] & shown) == 0 ? 1 : 0);
    width = sight.width;
    height = sight.height;
    return true;
}

void Runtime::apply_photo_look_rgb(
    std::vector<uint8_t>& rgb,
    uint32_t width,
    uint32_t height,
    int size_factor,
    int32_t crop_x,
    int32_t crop_y,
    std::optional<std::array<int32_t, 2>> corner,
    int grain_factor
) const {
    if (!photo_look_active() || photo_ == nullptr || width == 0 || height == 0)
        return;
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    if (rgb.size() < pixels * 3U)
        return;
    if (width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
        height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
        return;
    const bool held = photo_->picture_held;
    const bool camera_held = held && photo_->camera_held;
    const uint32_t tick =
        held ? photo_->held_tick : (match_ != nullptr ? match_->state().game.tick : 0U);
    // The caller passes the factor the picture was drawn at. take_photo
    // widens it when a Screen photo is the window's pixels and those are
    // finer than the battlefield. A caller that passes 1 on a larger
    // picture is asking for the panel's grain, and keeps it.
    const int factor = std::clamp(size_factor, 1, 4);
    // A poster names its own factor for grain and the tilt band: its pixels
    // over the preview's for the same ground, one over the window's zoom,
    // with no cap at 4. Haze stays on the size factor.
    const int grain_times = grain_factor > 0 ? std::max(grain_factor, 1) : factor;
    const bool classic = !live_renderer_active();
    const int32_t haze = classic ? std::clamp(photo_->look_haze, 0, 100) : 0;
    const int32_t band =
        photo::photo_tilt_width(std::clamp(photo_->look_tilt_width, 0, 160), grain_times);
    const int32_t radius =
        photo::photo_tilt_radius(std::clamp(photo_->look_tilt_radius, 0, 32), grain_times);
    const photo::Look look = make_look(
        photo_->look_preset,
        photo_->look_vignette,
        photo_->look_roundness,
        photo_->look_grain,
        photo::photo_grain_size(photo_->look_grain_size, grain_times),
        photo_->look_frame,
        tick,
        selected_map_name_runtime_,
        tick,
        photo_->look_tilt_centre,
        band,
        radius,
        haze
    );
    // A poster is drawn at zoom 1 from its own corner, which is not the
    // window's view: its haze follows the poster's ground.
    const int32_t camera_x =
        corner ? (*corner)[0] : (camera_held ? photo_->held_camera_x : match_camera_x_);
    const int32_t camera_z =
        corner ? (*corner)[1] : (camera_held ? photo_->held_camera_z : match_camera_z_);
    const float zoom = corner ? 1.0F : (camera_held ? photo_->held_zoom : match_zoom_);
    GroundColumn ground;
    photo::HazeField field;
    const oa::formats::tnt::Map* map = selected_tnt_ ? &*selected_tnt_ : nullptr;
    const bool hazing =
        prepare_haze(field, ground, map, camera_x, camera_z, zoom, factor, crop_x, crop_y, haze);
    // The fog the picture was drawn with: the held picture's when one is
    // held, else the view's now. A replay watched through one player's eyes
    // draws that player's fog, and so does this.
    std::vector<uint8_t> live_cells;
    if (hazing && held) {
        keep_fog(
            ground,
            field,
            photo_->held_unmapped,
            photo_->held_fog_width,
            photo_->held_fog_height,
            fog_tiles_
        );
    } else if (hazing) {
        int32_t fog_width = 0;
        int32_t fog_height = 0;
        if (photo_fog_cells(live_cells, fog_width, fog_height))
            keep_fog(ground, field, live_cells, fog_width, fog_height, fog_tiles_);
    }
    photo::Picture picture;
    picture.bytes = rgb.data();
    picture.width = static_cast<int32_t>(width);
    picture.height = static_cast<int32_t>(height);
    picture.channels = 3;
    photo::apply_look(picture, look, caption_fonts(), draw_pool_.get(), hazing ? &field : nullptr);
}

void Runtime::paint_photo_look_preview(
    uint8_t* bytes,
    uint32_t width,
    uint32_t height,
    uint32_t stride,
    uint32_t channels,
    int size_factor
) const {
    if (!photo_look_active() || photo_ == nullptr || bytes == nullptr || width == 0 || height == 0)
        return;
    constexpr auto most = static_cast<uint32_t>(std::numeric_limits<int32_t>::max());
    if (width > most || height > most || stride > most || (channels != 3U && channels != 4U))
        return;
    const bool held = photo_->picture_held;
    const bool camera_held = held && photo_->camera_held;
    const uint32_t tick =
        held ? photo_->held_tick : (match_ != nullptr ? match_->state().game.tick : 0U);
    const bool classic = !live_renderer_active();
    const int32_t haze = classic ? std::clamp(photo_->look_haze, 0, 100) : 0;
    // A preview of the window's pixels at a density above 1 is painted at
    // the factor take_photo gives a Screen photo of those pixels.
    const int factor = std::clamp(size_factor, 1, 4);
    const photo::Look look = make_look(
        photo_->look_preset,
        photo_->look_vignette,
        photo_->look_roundness,
        photo_->look_grain,
        photo::photo_grain_size(photo_->look_grain_size, factor),
        photo_->look_frame,
        tick,
        selected_map_name_runtime_,
        tick,
        photo_->look_tilt_centre,
        photo::photo_tilt_width(std::clamp(photo_->look_tilt_width, 0, 160), factor),
        photo::photo_tilt_radius(std::clamp(photo_->look_tilt_radius, 0, 32), factor),
        haze
    );
    photo::Picture picture;
    picture.bytes = bytes;
    picture.width = static_cast<int32_t>(width);
    picture.height = static_cast<int32_t>(height);
    picture.stride = static_cast<int32_t>(stride);
    picture.channels = static_cast<int32_t>(channels);
    // The photo is cropped before its look is painted, so the look goes on
    // the crop's own rectangle here too. The crop's origin is where the
    // haze reads the ground.
    const auto guide = static_cast<oa::ui::photo_panel::Guide>(photo_->guide);
    const oa::ui::kit::Rect crop =
        oa::ui::photo_panel::crop_bounds(picture.width, picture.height, guide);
    int32_t crop_x = 0;
    int32_t crop_y = 0;
    if (crop.width > 0 && crop.height > 0) {
        crop_x = crop.x;
        crop_y = crop.y;
        picture.bytes = bytes + static_cast<std::size_t>(crop.y) * stride +
                        static_cast<std::size_t>(crop.x) * channels;
        picture.width = crop.width;
        picture.height = crop.height;
    }
    const int32_t camera_x = camera_held ? photo_->held_camera_x : match_camera_x_;
    const int32_t camera_z = camera_held ? photo_->held_camera_z : match_camera_z_;
    const float zoom = camera_held ? photo_->held_zoom : match_zoom_;
    GroundColumn ground;
    photo::HazeField field;
    const oa::formats::tnt::Map* map = selected_tnt_ ? &*selected_tnt_ : nullptr;
    const bool hazing =
        prepare_haze(field, ground, map, camera_x, camera_z, zoom, factor, crop_x, crop_y, haze);
    // The fog the picture was drawn with: the held picture's when one is
    // held, else the view's now. A replay watched through one player's eyes
    // draws that player's fog, and so does this.
    std::vector<uint8_t> live_cells;
    if (hazing && held) {
        keep_fog(
            ground,
            field,
            photo_->held_unmapped,
            photo_->held_fog_width,
            photo_->held_fog_height,
            fog_tiles_
        );
    } else if (hazing) {
        int32_t fog_width = 0;
        int32_t fog_height = 0;
        if (photo_fog_cells(live_cells, fog_width, fog_height))
            keep_fog(ground, field, live_cells, fog_width, fog_height, fog_tiles_);
    }
    photo::apply_look(picture, look, caption_fonts(), draw_pool_.get(), hazing ? &field : nullptr);
}

bool Runtime::refresh_photo_look_cpu() {
    if (!photo_look_active() || photo_ == nullptr)
        return false;
    const uint64_t key = photo_look_key();
    if (photo_look_cpu_valid_ && photo_look_key_set_ && photo_look_key_ == key &&
        !photo_look_preview_due())
        return !photo_look_cpu_.rgb.empty();

    std::vector<uint8_t> rgb;
    uint32_t width = 0;
    uint32_t height = 0;
    if (photo_picture_held()) {
        width = photo_->held_width;
        height = photo_->held_height;
        const std::size_t pixels = static_cast<std::size_t>(width) * height;
        if (width == 0 || height == 0 || photo_->held_rgb.size() != pixels * 3U)
            return false;
        rgb = photo_->held_rgb;
        apply_gamma_rgb(rgb.data(), pixels, 3);
    } else if (!copy_world_with_player_gamma(rgb, width, height)) {
        return false;
    }
    if (rgb.size() < static_cast<std::size_t>(width) * height * 3U)
        return false;
    paint_photo_look_preview(
        rgb.data(),
        width,
        height,
        width * 3U,
        3U,
        photo_look_scale(1, width, match_layout_.battlefield_width())
    );
    photo_look_cpu_.width = width;
    photo_look_cpu_.height = height;
    photo_look_cpu_.rgb = std::move(rgb);
    photo_look_key_ = key;
    photo_look_key_set_ = true;
    photo_look_cpu_valid_ = true;
    return !photo_look_cpu_.rgb.empty();
}

void Runtime::present_photo_look_full(
    int x, int y, int width, int height, bool moved, SDL_Texture* target
) {
    if (!photo_look_active() || sdl_.renderer == nullptr || width <= 0 || height <= 0)
        return;
    float scale_x = 1.0F;
    float scale_y = 1.0F;
    int logical_width = 0;
    int logical_height = 0;
    SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
    const bool scales =
        SDL_GetRenderScale(sdl_.renderer, &scale_x, &scale_y) &&
        SDL_GetRenderLogicalPresentation(sdl_.renderer, &logical_width, &logical_height, &mode);
    const bool one_to_one = !moved && scales && scale_is_one(scale_x) && scale_is_one(scale_y) &&
                            mode == SDL_LOGICAL_PRESENTATION_DISABLED;
    const uint64_t key = photo_look_key();
    const bool cached = one_to_one && photo_look_rgba_valid_ && photo_look_key_set_ &&
                        photo_look_key_ == key && !photo_look_preview_due() &&
                        photo_look_rgba_width_ == static_cast<uint32_t>(width) &&
                        photo_look_rgba_height_ == static_cast<uint32_t>(height) &&
                        photo_look_rgba_.size() ==
                            static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    if (cached) {
        SDL_Texture* texture = SDL_CreateTexture(
            sdl_.renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height
        );
        if (texture != nullptr) {
            const bool drawn =
                SDL_UpdateTexture(texture, nullptr, photo_look_rgba_.data(), width * 4) &&
                SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_NONE) &&
                SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_NEAREST);
            if (drawn) {
                const SDL_FRect destination{
                    static_cast<float>(x),
                    static_cast<float>(y),
                    static_cast<float>(width),
                    static_cast<float>(height)
                };
                if (SDL_RenderTexture(sdl_.renderer, texture, nullptr, &destination)) {
                    SDL_DestroyTexture(texture);
                    return;
                }
            }
            SDL_DestroyTexture(texture);
        }
    }

    const auto process = [this, width, height, one_to_one](const full_effects::Pixels& image) {
        if (image.bytes.empty() || image.width == 0 || image.height == 0)
            return;
        if (image.width > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) ||
            image.height > static_cast<uint32_t>(std::numeric_limits<int32_t>::max()))
            return;
        // Read in the window's pixels: at a density above 1 those are finer
        // than the battlefield's points, as the Screen photo is.
        paint_photo_look_preview(
            image.bytes.data(),
            image.width,
            image.height,
            image.pitch,
            4U,
            photo_look_scale(1, image.width, width)
        );
        if (!one_to_one || image.width != static_cast<uint32_t>(width) ||
            image.height != static_cast<uint32_t>(height)) {
            photo_look_rgba_valid_ = false;
            return;
        }
        pack_rgba(photo_look_rgba_, image.bytes.data(), image.width, image.height, image.pitch);
        photo_look_rgba_width_ = image.width;
        photo_look_rgba_height_ = image.height;
        photo_look_rgba_valid_ = true;
    };
    full_effects::Upload upload;
    const std::string failure = full_effects::compose(
        sdl_.renderer,
        upload,
        SDL_Rect{x, y, width, height},
        nullptr,
        1U,
        process,
        nullptr,
        [this](uint64_t bytes) { return accelerated_buffer_fits(bytes); },
        target,
        full_effects::Pass::world
    );
    if (failure.empty()) {
        photo_look_key_ = key;
        photo_look_key_set_ = true;
    } else {
        photo_look_rgba_valid_ = false;
    }
}

void Runtime::clear_photo_look_cache() noexcept {
    photo_look_cpu_valid_ = false;
    photo_look_key_set_ = false;
    photo_look_rgba_valid_ = false;
    photo_look_rgba_width_ = 0;
    photo_look_rgba_height_ = 0;
    photo_look_cpu_.rgb.clear();
    photo_look_cpu_.width = 0;
    photo_look_cpu_.height = 0;
    photo_look_rgba_.clear();
}

} // namespace oa::app

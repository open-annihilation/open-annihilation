// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Textures whose size follows the window, made as one texture within the
// renderer's texture limit and as tiles beyond it, each tile carrying a
// one-texel gutter copied from its neighbour; the error a failed SDL call
// of the standard tier's presenting throws; and the accelerated tier's
// drawing on the graphics card: the battlefield's scene magnified into the
// window, and the interface and the 640x480 screens scaled by the
// pixel-art or sharp-bilinear filter, through prescale targets made once
// at their largest and redrawn only when their source changes. Every
// failure of a call only the accelerated tier makes is an
// AccelerationError, thrown after the render target is set back to the
// final one, so that the caller can drop to the standard tier and present
// the frame as it does.
#pragma once

#include "oa/app/frame_coordinates.hpp"
#include "oa/app/render_policy.hpp"
#include "oa/platform/job_pool.hpp"
#include "oa/present/world_renderer/scene_filter.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace oa::app {

/// A failed SDL call while presenting a frame of the standard tier: the
/// renderer is rebuilt for the rest of the run (Runtime::render).
class PresentError : public std::runtime_error {
  public:

    using std::runtime_error::runtime_error;
};

/// Throws PresentError with SDL's error after what failed.
///
/// @param what the call or texture that failed
[[noreturn]] void throw_present_error(const std::string& what);

/// Whose fault an AccelerationError is.
enum class AccelerationFault : uint8_t {
    /// A call to the graphics driver failed, or a new render target read
    /// back another colour than it was cleared to: kept against the driver.
    driver,
    /// The game asked for something it cannot do, with no driver call
    /// failing: a texture that cannot be split into tiles under the limit, a
    /// prescale target smaller than what it is to hold, or no letterbox to
    /// scale the screen into. Never kept against the driver.
    engine,
};

/// A call that only the accelerated tier makes failed: the tier is dropped
/// for the rest of the run and the frame presented as the standard tier
/// presents it.
class AccelerationError : public std::runtime_error {
  public:

    /// Makes the error.
    ///
    /// @param what what failed, with SDL's error where a call failed
    /// @param fault whose fault it is
    explicit AccelerationError(
        const std::string& what, AccelerationFault fault = AccelerationFault::driver
    )
        : std::runtime_error(what), fault_(fault) {}

    /// Returns whose fault the error is.
    ///
    /// @return the driver's, or the game's own
    [[nodiscard]] AccelerationFault fault() const noexcept { return fault_; }

  private:

    AccelerationFault fault_{AccelerationFault::driver};
};

/// What the accelerated tier made and drew on the card, counted for the checks.
struct ScaledWorldCounts {
    uint64_t textures_created{};   ///< tile textures and prescale targets made
    uint64_t textures_destroyed{}; ///< tile textures and prescale targets destroyed
    uint64_t prescale_draws{};     ///< sources drawn into a prescale target
};

/// A streaming texture as one texture within the renderer's limit, or as
/// tiles beyond it (render_policy::plan_tiles).
///
/// Within the limit it makes and uses exactly one texture, and passes the
/// caller's rectangles to SDL unchanged, so a renderer with no limit, such
/// as SDL's software renderer, draws as before. Beyond it, each tile is
/// uploaded from the part of the picture it holds, its gutters included,
/// and drawn without its gutters, each part landing where it lands in the
/// whole, so that tiles drawn 1:1 at whole-pixel places give the same
/// pixels as one texture would and tiles drawn scaled meet with no seam.
///
/// The standard tier's layers make it with ensure, and every SDL failure
/// then throws PresentError. The accelerated tier's scene and overlay make
/// it with create, ARGB8888 and at the largest size they are to hold, its
/// tiles counted, and every SDL failure then throws AccelerationError.
/// Either way each new tile is cleared (clear_streaming_texture) before it
/// is drawn.
class TiledTexture {
  public:

    TiledTexture() = default;
    TiledTexture(const TiledTexture&) = delete;
    TiledTexture& operator=(const TiledTexture&) = delete;

    /// Destroys the texture or its tiles.
    ~TiledTexture() { reset(); }

    /// Makes the texture or its tiles for the standard tier unless they
    /// already match: a format, size, limit or blend mode that differs makes
    /// them again. Each is a streaming texture scaled NEAREST with the given
    /// blend mode, cleared; an alpha format with SDL_BLENDMODE_NONE has its
    /// blending turned off, which SDL turns on for alpha formats.
    ///
    /// Throws PresentError when SDL cannot make one.
    ///
    /// @param renderer the renderer
    /// @param format the pixel format
    /// @param width the picture's width in pixels, above 0
    /// @param height the picture's height in pixels, above 0
    /// @param limit the renderer's texture limit in texels; 0 for none
    /// @param blend how the texture is drawn over what is under it
    /// @return true when the texture or its tiles were made anew
    bool ensure(
        SDL_Renderer* renderer,
        SDL_PixelFormat format,
        int width,
        int height,
        uint32_t limit,
        SDL_BlendMode blend
    );

    /// Makes the texture or its tiles for the accelerated tier, ARGB8888,
    /// replacing any made before; each is streaming, scaled NEAREST, with
    /// the blend mode given, and cleared.
    ///
    /// Throws AccelerationError when a tile cannot be made; the tiles made
    /// by then are destroyed.
    ///
    /// @param renderer the renderer
    /// @param width texels across, above 0
    /// @param height texels down, above 0
    /// @param limit the renderer's texture limit in texels; 0 for none
    /// @param blend SDL_BLENDMODE_NONE for an opaque picture, SDL_BLENDMODE_BLEND for an overlay
    /// @param[in,out] counts the tiles made are counted here, and destroyed ones later
    void create(
        SDL_Renderer* renderer,
        uint32_t width,
        uint32_t height,
        uint32_t limit,
        SDL_BlendMode blend,
        ScaledWorldCounts& counts
    );

    /// Fills the texture through its tiles: each is locked and handed to
    /// fill with the part of the picture it holds, gutters included.
    ///
    /// Throws when a tile cannot be locked (fail).
    ///
    /// @param fill called as fill(const SDL_Rect& source, uint8_t* pixels,
    ///     int pitch) with the part of the picture in its pixels, the tile's
    ///     locked pixels and their pitch in bytes
    template <typename Fill>
    void upload(Fill&& fill) {
        for (const auto& piece : tiles_) {
            void* pixels = nullptr;
            int pitch = 0;
            if (!SDL_LockTexture(piece.texture, nullptr, &pixels, &pitch))
                fail("SDL_LockTexture");
            fill(source_rect(piece), static_cast<uint8_t*>(pixels), pitch);
            SDL_UnlockTexture(piece.texture);
        }
    }

    /// Copies rows of a picture into the texture's tiles with
    /// SDL_UpdateTexture, across the texture's whole width; by default
    /// every row.
    ///
    /// Throws when SDL refuses a tile (fail).
    ///
    /// @param pixels the picture's first row, as wide as the texture, in its format
    /// @param pitch bytes from one row of the picture to the next
    /// @param bytes_per_pixel bytes of one pixel of the format
    /// @param first_row the first row copied
    /// @param end_row the row past the last copied; beyond the texture's height, its height
    void update(
        const uint8_t* pixels,
        int pitch,
        int bytes_per_pixel,
        uint32_t first_row = 0,
        uint32_t end_row = UINT32_MAX
    );

    /// Copies a rectangle of a picture into the texture's tiles with
    /// SDL_UpdateTexture, each tile's gutters with the rest of it; the
    /// texels outside the rectangle keep what they hold.
    ///
    /// Throws when SDL refuses a tile (fail).
    ///
    /// @param pixels the rectangle's top left pixel, in the texture's format
    /// @param pitch bytes from one row of the rectangle to the next
    /// @param bytes_per_pixel bytes of one pixel of the format
    /// @param rect the rectangle, in the picture's pixels, within the texture
    void update_rect(
        const uint8_t* pixels, int pitch, int bytes_per_pixel, const render_policy::TexelRect& rect
    );

    /// Uploads the top-left corner of an ARGB8888 texture from an RGB24
    /// picture through the gamma table, each texel opaque, and the column
    /// and row past it where the texture holds them: a LINEAR draw of the
    /// corner reads them at its edge, and they take the corner's last
    /// column and row, the corner's last texel where they meet, as a
    /// texture of the corner's own size gives back past its edge.
    ///
    /// Only the tiles, and the parts of them, that the corner and the column
    /// and row past it cover are written, gutters included. Throws when a
    /// tile cannot be locked (fail).
    ///
    /// @param rgb the picture's first row, 3 bytes a pixel
    /// @param rgb_pitch bytes from one row of the picture to the next
    /// @param width columns of the corner, at most the texture's
    /// @param height rows of the corner, at most the texture's
    /// @param gamma the display gamma's table; null when the gamma is 1
    /// @param pool threads to convert the rows on; null converts them on the calling thread
    void upload_rgb24(
        const uint8_t* rgb,
        std::size_t rgb_pitch,
        uint32_t width,
        uint32_t height,
        const std::array<uint8_t, 256>* gamma,
        platform::job_pool::Pool* pool
    );

    /// Sets the scale mode every tile is drawn with.
    ///
    /// @param mode the scale mode
    /// @return false when a tile refused it
    bool set_scale_mode(SDL_ScaleMode mode) noexcept;

    /// Draws a rectangle of the texture onto one of the render target.
    ///
    /// With one texture it is one SDL_RenderTexture with the rectangles
    /// unchanged, null included. With tiles, each tile's part of `source`,
    /// its gutters left out, is drawn onto its part of `destination`.
    ///
    /// Throws when SDL refuses a draw (fail).
    ///
    /// @param renderer the renderer the texture was made on
    /// @param source the part of the picture to draw, in its pixels; null for all of it
    /// @param destination where it goes on the render target; null for the
    ///     whole target with one texture, and the picture's own rectangle
    ///     with tiles
    void draw(SDL_Renderer* renderer, const SDL_FRect* source, const SDL_FRect* destination) const;

    /// Destroys the texture or its tiles, counting them where create made
    /// them; the next ensure or create makes them again.
    void reset() noexcept;

    /// Returns how many textures hold the picture.
    ///
    /// @return 0 before it is made, 1 within the limit, more beyond it
    [[nodiscard]] std::size_t tile_count() const noexcept { return tiles_.size(); }

    /// Returns whether the texture holds tiles.
    ///
    /// @return true once made, until reset
    [[nodiscard]] bool made() const noexcept { return !tiles_.empty(); }

    /// Says whether the texture was made at a size and limit.
    ///
    /// @param width texels across
    /// @param height texels down
    /// @param limit the renderer's texture limit in texels; 0 for none
    /// @return true when it holds tiles made for exactly these
    [[nodiscard]] bool made_for(uint32_t width, uint32_t height, uint32_t limit) const noexcept {
        return made() && grid_.width == width && grid_.height == height && limit_ == limit;
    }

    /// Returns the one texture that holds the whole picture.
    ///
    /// @return the texture; null when there is none or the picture is tiled
    [[nodiscard]] SDL_Texture* single() const noexcept {
        return tiles_.size() == 1 ? tiles_.front().texture : nullptr;
    }

    /// Returns one tile's texture.
    ///
    /// @param column the tile's column
    /// @param row the tile's row
    /// @return the texture; null outside the grid
    [[nodiscard]] SDL_Texture* tile_texture(uint32_t column, uint32_t row) const noexcept {
        if (column >= grid_.columns || row >= grid_.rows)
            return nullptr;
        return tiles_[static_cast<std::size_t>(row) * grid_.columns + column].texture;
    }

    /// Returns how the texture is split into tiles.
    ///
    /// @return the grid it was made with; empty before it is made
    [[nodiscard]] const render_policy::TileGrid& grid() const noexcept { return grid_; }

    /// Returns the picture's width.
    ///
    /// @return pixels; 0 before it is made
    [[nodiscard]] int width() const noexcept { return static_cast<int>(grid_.width); }

    /// Returns the picture's height.
    ///
    /// @return pixels; 0 before it is made
    [[nodiscard]] int height() const noexcept { return static_cast<int>(grid_.height); }

    /// Returns the textures' pixel format.
    ///
    /// @return the format; SDL_PIXELFORMAT_UNKNOWN before it is made
    [[nodiscard]] SDL_PixelFormat format() const noexcept { return format_; }

  private:

    /// One texture of the picture and the part it holds.
    struct Piece {
        SDL_Texture* texture{};
        render_policy::Tile tile{};
    };

    /// Makes the tiles of a grid, row after row, in a format; a tile that
    /// cannot be made destroys those made by then and throws (fail).
    ///
    /// @param renderer the renderer
    /// @param format the pixel format
    /// @param grid the tiles, already planned; at least one
    /// @param limit the texture limit the grid was planned for; 0 for none
    /// @param blend how the tiles are drawn over what is under them
    void make(
        SDL_Renderer* renderer,
        SDL_PixelFormat format,
        const render_policy::TileGrid& grid,
        uint32_t limit,
        SDL_BlendMode blend
    );

    /// Throws the error of the tier that made the texture, with SDL's error
    /// after what failed: AccelerationError where create made it, else
    /// PresentError.
    ///
    /// @param what the call that failed
    [[noreturn]] void fail(const std::string& what) const;

    /// Returns the part of the picture a tile holds, gutters included.
    ///
    /// @param piece the tile
    /// @return the rectangle in the picture's pixels
    [[nodiscard]] static SDL_Rect source_rect(const Piece& piece) noexcept {
        return {
            static_cast<int>(piece.tile.texture.x),
            static_cast<int>(piece.tile.texture.y),
            static_cast<int>(piece.tile.texture.width),
            static_cast<int>(piece.tile.texture.height)
        };
    }

    std::vector<Piece> tiles_; ///< row after row of the grid
    render_policy::TileGrid grid_{};
    SDL_PixelFormat format_{SDL_PIXELFORMAT_UNKNOWN};
    SDL_BlendMode blend_{SDL_BLENDMODE_NONE};
    uint32_t limit_{}; ///< the texture limit the tiles were planned for; 0 for none
    /// Where create counts the tiles; null for a texture ensure made, whose
    /// failures are the standard tier's.
    ScaledWorldCounts* counts_{};
};

/// A render target a source is drawn into NEAREST at a whole-number
/// factor, for the sharp-bilinear filter to reduce LINEAR; ARGB8888, with
/// no blending. It is made once at the largest size it is to hold, and the
/// source drawn into it again only when it changed. Beyond the renderer's
/// texture limit it is split into tiles (render_policy's plan_tiles), each
/// with a gutter that holds its neighbour's texels, as TiledTexture is.
/// Each draw into it also fills the column and row past the enlarged
/// source from the source's last column and row, so that LINEAR reads at
/// the source's edge what a target of the source's own size gives back
/// there, never the colour a new target is cleared to or a larger source
/// drawn into it before.
class PrescaleTarget {
  public:

    PrescaleTarget() = default;
    PrescaleTarget(const PrescaleTarget&) = delete;
    PrescaleTarget& operator=(const PrescaleTarget&) = delete;

    /// Destroys the target.
    ~PrescaleTarget() { destroy(); }

    /// Makes the target at a size, unless it was made at that size and
    /// limit; each tile of a new target is cleared and its first pixel read
    /// back, which must be the colour cleared.
    ///
    /// Throws AccelerationError, after setting the render target back to
    /// `final_target`, when a tile cannot be made or reads back wrong; the
    /// tiles made by then are destroyed.
    ///
    /// @param renderer the renderer
    /// @param final_target the target frames are drawn into; null for the window
    /// @param width texels across, above 0
    /// @param height texels down, above 0
    /// @param limit the renderer's texture limit in texels; 0 for none
    /// @param[in,out] counts the tiles made are counted here, and destroyed ones later
    void ensure(
        SDL_Renderer* renderer,
        SDL_Texture* final_target,
        uint32_t width,
        uint32_t height,
        uint32_t limit,
        ScaledWorldCounts& counts
    );

    /// Destroys the target.
    void destroy() noexcept;

    /// Returns whether the target holds tiles.
    ///
    /// @return true once made, until destroyed
    [[nodiscard]] bool made() const noexcept { return !tiles_.empty(); }

    /// Returns how the target is split into tiles.
    ///
    /// @return the grid it was made with; empty before it is made
    [[nodiscard]] const render_policy::TileGrid& grid() const noexcept { return grid_; }

    /// Returns one tile's texture.
    ///
    /// @param column the tile's column
    /// @param row the tile's row
    /// @return the texture; null outside the grid
    [[nodiscard]] SDL_Texture* tile_texture(uint32_t column, uint32_t row) const noexcept;

    /// Returns the target's width.
    ///
    /// @return texels across; 0 before it is made
    [[nodiscard]] uint32_t width() const noexcept { return grid_.width; }

    /// Returns the target's height.
    ///
    /// @return texels down; 0 before it is made
    [[nodiscard]] uint32_t height() const noexcept { return grid_.height; }

    /// Returns its pixels, charged to the prescale budget.
    ///
    /// @return width times height
    [[nodiscard]] uint64_t pixels() const noexcept { return uint64_t{grid_.width} * grid_.height; }

    /// The source revision drawn into it last; a new target has none.
    uint64_t drawn_revision{~uint64_t{0}};
    /// The factor it was drawn at last; 0 for none.
    uint32_t drawn_factor{};

  private:

    render_policy::TileGrid grid_{};
    uint32_t limit_{};                ///< the texture limit the grid was planned for; 0 for none
    std::vector<SDL_Texture*> tiles_; ///< row after row of the grid
    ScaledWorldCounts* counts_{};     ///< where destroyed tiles are counted
};

/// Clears a streaming texture: every byte of each of its rows set to 0, which
/// is black in an opaque format and clear in a format with alpha. A new
/// texture's pixels are left undefined by the graphics card until something
/// writes them, and a LINEAR draw at the edge of what was uploaded, or a
/// draw before the first upload, would show them.
///
/// @param texture a streaming texture
/// @return false when SDL cannot lock it
[[nodiscard]] bool clear_streaming_texture(SDL_Texture* texture) noexcept;

/// How the card scales a picture: the filter, and the prescale target's
/// factor where the filter is sharp-bilinear.
struct CardScale {
    render_policy::ScaleFilter filter{render_policy::ScaleFilter::nearest};
    uint32_t factor{1}; ///< prescale target pixels per source pixel; 1 for none
};

/// Returns the scale mode the card draws a filter with straight from its
/// source, with no prescale target: NEAREST; PIXELART, or NEAREST in a
/// build of SDL older than 3.4, which lacks it, as SDL's software renderer
/// draws PIXELART; and plain LINEAR for sharp-bilinear and LINEAR.
///
/// @param filter the filter
/// @return the scale mode
[[nodiscard]] SDL_ScaleMode direct_scale_mode(render_policy::ScaleFilter filter) noexcept;

/// Draws a layer laid out 1:1 in layout pixels, whose scale mode is NEAREST,
/// with a scale mode, and sets it back to NEAREST after, as the standard
/// tier draws it; at NEAREST it is the one draw the standard tier makes.
/// On a window at native density the renderer scales the layer by the
/// density with that mode. An SDL older than 3.2.10 draws a queued texture
/// with the scale mode it has when the queued draws run, so on it the queue
/// is run before the mode goes back.
///
/// Throws PresentError when SDL refuses the draw, and AccelerationError when
/// it refuses a scale mode other than NEAREST or running the queue.
///
/// @param renderer the renderer
/// @param texture the layer
/// @param destination where it goes, in layout pixels; null for the whole target
/// @param mode the scale mode
void draw_one_to_one(
    SDL_Renderer* renderer, SDL_Texture* texture, const SDL_FRect* destination, SDL_ScaleMode mode
);

/// Draws a layer of tiles laid out 1:1 in layout pixels (TiledTexture::draw)
/// with a scale mode, and sets it back to NEAREST after, as the standard tier
/// draws it; at NEAREST it is the one draw the standard tier makes. On an
/// SDL older than 3.2.10 the queue is run before the mode goes back, as the
/// other draw_one_to_one does.
///
/// Throws as TiledTexture::draw throws, and AccelerationError when SDL
/// refuses a scale mode other than NEAREST or running the queue.
///
/// @param renderer the renderer the layer was made on
/// @param layer the layer, whose scale mode is NEAREST
/// @param source the part of the picture to draw; null for all of it
/// @param destination where it goes, in layout pixels; null as TiledTexture::draw takes it
/// @param mode the scale mode
void draw_one_to_one(
    SDL_Renderer* renderer,
    TiledTexture& layer,
    const SDL_FRect* source,
    const SDL_FRect* destination,
    SDL_ScaleMode mode
);

/// Sets the renderer's logical presentation for a screen drawn as one
/// frame: the menus, the loading screen and the other screens of 640x480,
/// and a dialog over a frame of its own size. The window holds the frame as
/// render_policy::frame_fit decides for the renderer's output size:
/// letterboxed, or in whole steps (SDL_LOGICAL_PRESENTATION_INTEGER_SCALE),
/// which only a window that holds the frame at least once is given, since
/// SDL would show one step of a larger frame cut by the window's edges.
///
/// @param renderer the renderer
/// @param scaling the Menu scaling setting
/// @param width the frame's width, in its own pixels
/// @param height the frame's height, in its own pixels
/// @return false when SDL refused
[[nodiscard]] bool set_frame_presentation(
    SDL_Renderer* renderer, render_policy::MenuScaling scaling, int width, int height
);

/// Draws a frame's texture over the logical presentation in the standard
/// tier with a scale mode for this draw alone, and sets it back to NEAREST
/// after; at NEAREST it is the one draw the standard tier makes. A mode the
/// renderer refuses draws the frame NEAREST. An SDL older than 3.2.10 runs
/// the queued draws before the mode goes back, as draw_one_to_one does.
///
/// Throws PresentError when SDL refuses the draw or running the queue.
///
/// @param renderer the renderer
/// @param texture the frame's texture, whose scale mode is NEAREST
/// @param mode the scale mode
void draw_frame(SDL_Renderer* renderer, SDL_Texture* texture, SDL_ScaleMode mode);

/// Draws a frame held in tiles (TiledTexture::draw) over the logical
/// presentation in the standard tier with a scale mode for this draw alone,
/// as the other draw_frame does.
///
/// Throws as TiledTexture::draw throws, and PresentError when SDL refuses
/// running the queue.
///
/// @param renderer the renderer the frame was made on
/// @param frame the frame, whose scale mode is NEAREST
/// @param mode the scale mode
/// @param destination where the frame is drawn, in window pixels; null draws it
///     through the logical presentation, over the whole output
void draw_frame(
    SDL_Renderer* renderer,
    TiledTexture& frame,
    SDL_ScaleMode mode,
    const SDL_FRect* destination = nullptr
);

/// Probes whether the renderer's pixel-art scale mode works: a 2x1 black
/// and white texture drawn at 2.5 times into a 5x1 target must read back
/// pure black in its second pixel and a level between black and white in
/// its third, which LINEAR and NEAREST do not give.
///
/// Its textures and target are destroyed when it ends, and the render
/// target set back to `final_target`. A build of SDL without the mode
/// answers false.
///
/// @param renderer the renderer
/// @param final_target the target frames are drawn into; null for the window
/// @return true when the mode scales as the pixel-art filter does
[[nodiscard]] bool probe_pixelart(SDL_Renderer* renderer, SDL_Texture* final_target);

/// Draws the corner of a scene the battlefield shows into the final
/// target, magnified, clipped to the battlefield.
///
/// NEAREST at a whole-number scale; PIXELART; plain LINEAR; or
/// sharp-bilinear: the corner, with one more column and row where the scene
/// has them and the prescale target holds them, drawn NEAREST into the
/// prescale target at its factor, each of the scene's tiles into each of the
/// target's, then that LINEAR into the destination, each of the target's
/// tiles with its gutters left out. The scene's tiles keep the scale mode
/// the draw sets.
///
/// A destination that starts between pixels draws a view between map
/// pixels: the card places the corner there, and SDL's software renderer
/// at the whole pixel its own rounding gives; before SDL 3.4 that renderer
/// also moves a draw the clip cuts by up to a texel.
///
/// Throws AccelerationError, after setting the render target back to
/// `final_target`, when a call fails or the prescale target cannot hold the
/// corner at the factor.
///
/// @param renderer the renderer
/// @param final_target the target frames are drawn into; null for the window
/// @param scene the scene's texture
/// @param scene_width columns of the scene uploaded, at least `width`
/// @param scene_height rows of the scene uploaded, at least `height`
/// @param width columns of the corner drawn
/// @param height rows of the corner drawn
/// @param destination where the corner lands, in pixels of the final target
/// @param clip the battlefield, in whole pixels of the final target
/// @param scale the filter and the prescale factor
/// @param[in,out] prescale the prescale target, made at least as large as the factor needs
/// @param[in,out] counts the prescale draw is counted here
void draw_scaled_world(
    SDL_Renderer* renderer,
    SDL_Texture* final_target,
    TiledTexture& scene,
    uint32_t scene_width,
    uint32_t scene_height,
    uint32_t width,
    uint32_t height,
    const SDL_FRect& destination,
    const SDL_Rect& clip,
    const CardScale& scale,
    PrescaleTarget& prescale,
    ScaledWorldCounts& counts
);

/// One part of a source texture and where it lands in the final target.
struct SharpPart {
    SDL_FRect source{};      ///< the part, in the source's texels
    SDL_FRect destination{}; ///< where it lands, in the final target's coordinates
};

/// Draws parts of a source texture into the final target scaled by the
/// card: NEAREST, PIXELART or plain LINEAR straight from the source, which
/// is left at NEAREST after, the queue run first on an SDL older than
/// 3.2.10 as draw_one_to_one runs it; or sharp-bilinear, the whole source
/// drawn NEAREST into the prescale target, into each of its tiles, only
/// when its revision or the factor changed, then each part LINEAR from it,
/// from each tile the part covers with the tile's gutters left out.
///
/// Throws AccelerationError, after setting the render target back to
/// `final_target`, when a call fails.
///
/// @param renderer the renderer
/// @param final_target the target frames are drawn into; null for the window
/// @param source the source texture, whose scale mode is NEAREST
/// @param source_width texels across the source
/// @param source_height texels down the source
/// @param parts the parts drawn
/// @param scale the filter and the prescale factor
/// @param revision the source's revision: a new one draws it into the prescale target again
/// @param[in,out] prescale the prescale target, made at least as large as the factor needs
/// @param[in,out] counts the prescale draw is counted here
void sharp_draw(
    SDL_Renderer* renderer,
    SDL_Texture* final_target,
    SDL_Texture* source,
    uint32_t source_width,
    uint32_t source_height,
    std::span<const SharpPart> parts,
    const CardScale& scale,
    uint64_t revision,
    PrescaleTarget& prescale,
    ScaledWorldCounts& counts
);

/// Draws a picture into a rectangle of a target as SDL's software renderer
/// draws a texture LINEAR, so that a check can hold that renderer's
/// read-back to it exactly: along each axis the picture is stepped through
/// in 16.16 fixed point, a step being its texels over the rectangle's
/// pixels, truncated, and the first sample half a step, rounded, less half
/// a texel in; each pixel weighs the two texels around its sample in
/// 128ths of a texel toward the second, rows first and then columns, and
/// the sum is truncated once; and a sample before the first texel's
/// centre, or beyond the last pair, takes the edge texel. SDL draws so on
/// processors with SSE2 or NEON; a build of SDL without either truncates
/// after each pass, which can give one level less. With a factor above 1
/// the picture is first enlarged that many times by nearest replication,
/// as the NEAREST pass of sharp-bilinear into its prescale target does.
/// Pixels of the rectangle outside the target are left out.
///
/// @param source the picture, at least one pixel
/// @param factor texels of the enlargement per picture texel, at least 1
/// @param rectangle where the picture lands, in the target's whole pixels
/// @param[out] target the rectangle's pixels within it written
void software_linear_rgb24(
    const oa::present::world_renderer::RgbSource& source,
    uint32_t factor,
    const SDL_Rect& rectangle,
    const oa::present::world_renderer::RgbTarget& target
);

} // namespace oa::app

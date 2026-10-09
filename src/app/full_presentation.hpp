// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the Full tier's presentation holds between frames (runtime_full.cpp,
// runtime_full_overlays.cpp): the card's executor on the game's renderer,
// the match's terrain atlas, built as the match loads, and the pages it and
// its greyed variant were uploaded as, the sprite pages and the card's
// pages that hold them, the model stage, the target a zoom between whole
// numbers is drawn through, the world target the battlefield is
// anti-aliased through, the fog grid the last planned frame built, the
// overlay canvas's key colour and the overlay of what the painters painted,
// the quads the painters asked the card to darken the world with, and what
// the last Full frame drew, which the render tiers check reads.
#pragma once

#include "oa/app/runtime.hpp"

#include "full_supersampling.hpp"
#include "full_terrain.hpp"
#include "oa/app/card/executor.hpp"
#include "oa/present/gpu_world/sprite_pages.hpp"
#include "oa/present/world_renderer/world_fog.hpp"
#include "runtime_full.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace oa::app {

/// A call of the card's own that failed, or the Full function test that
/// did, which drops Full to Basic for the rest of the run (Runtime::drop_full):
/// the error the stages name (full::CardError), under the presentation's
/// own name.
using FullCardError = full::CardError;

/// A frame the card refused before drawing anything of it, which drops Full
/// to Basic for the run with nothing struck against the driver
/// (full::FrameRefusedError).
using FullFrameRefusedError = full::FrameRefusedError;

/// The atlas levels the Full tier uploads and draws from: the tile levels
/// 0 to 2, the last of which the zoom floor of one sixth draws
/// (full_terrain::plan_terrain_draw).
inline constexpr uint8_t full_terrain_levels = 3;

/// The target a zoom above 1 that is not whole is drawn through, where the
/// renderer lacks the pixel-art sampling mode, is sized to a multiple of
/// this, so that it holds a whole number of map pixels at every
/// whole-number zoom up to the largest (2, 3 and 4 divide it).
inline constexpr uint32_t full_target_grain = 12;

/// Side of an ordinary sprite page, in texels, and of the largest page a
/// frame too big for one may have of its own.
inline constexpr uint32_t full_sprite_page_side = 1024;
inline constexpr uint32_t full_largest_sprite_page_side = 2048;
/// Texel memory the sprite pages start a match with: sixteen ordinary pages.
inline constexpr std::size_t full_sprite_page_memory = std::size_t{64} * 1024 * 1024;
/// The most the sprite pages, and each of the model stage's page sets, grow
/// to where one frame's sprites fill them, as far as the memory guard allows
/// (FullPresentation::allow_page_growth).
inline constexpr std::size_t full_largest_page_memory = std::size_t{512} * 1024 * 1024;

/// The vertices of a Full frame, by what made them.
struct FullFrameVertices {
    std::size_t total{};   ///< every vertex the frame held
    std::size_t shadows{}; ///< the model stage's shadows
    std::size_t models{};  ///< the units, 3D features, projectiles, debris and fragments
    std::size_t sprites{}; ///< sprites, particle squares and lines
    float zoom{};          ///< the zoom the frame was drawn at
};

/// The card's page that holds a page of the sprite pages.
struct FullCardPage {
    card::PageHandle handle{}; ///< none for a page not made
    uint32_t size{};           ///< texels a side it was made with
    /// The sprite page's revision whose texels it holds; 0 for none
    /// uploaded yet, which uploads the whole page.
    uint64_t revision{};
};

/// A quad a painter after the fog asked the card to draw over the world
/// under it, in place of reading and shading the world itself: the kill
/// board's shade and light levels, the +stats panel's fills and the
/// shadow, outline and partly covered letters of game text in the modern
/// fonts (Runtime::paint_world_level, Runtime::paint_world_blend,
/// Runtime::paint_world_minimum).
struct FullWorldQuad {
    int32_t x{}; ///< battlefield pixels, from the battlefield's top-left corner
    int32_t y{};
    int32_t width{};
    int32_t height{};
    card::Colour colour{}; ///< through the display gamma, at the quad's alpha
    card::Blend blend{card::Blend::alpha};
};

/// Draws the quads a Full frame's painters asked the card for over a
/// picture of the frame on the processor, in paint order, each by its
/// blend computed exactly, as the checks hold the card's picture to it.
///
/// @param quads the quads, in pixels of the battlefield
/// @param minimum_composed the renderer takes the minimum blend; where it
///     does not, a minimum quad blends as alpha (card::Capabilities)
/// @param[in,out] picture the picture's pixels, red, green and blue, top row first
/// @param width the picture's columns
/// @param height its rows
/// @param origin_x the column of the battlefield's left edge in the picture
/// @param origin_y the row of its top edge
/// @return for each pixel of the picture, how many quads lie over it, at
///         most 255
[[nodiscard]] std::vector<uint8_t> replay_world_quads(
    std::span<const FullWorldQuad> quads,
    bool minimum_composed,
    std::vector<uint8_t>& picture,
    uint32_t width,
    uint32_t height,
    int32_t origin_x,
    int32_t origin_y
);

struct Runtime::FullPresentation {
    /// What a terrain atlas was built from: the map by the storage and size
    /// of its tile grid and its tile pixels, and the grid's extent. One map
    /// record holds every map of the run in turn, so the record's address
    /// tells one map from the next no better than the storage its vectors
    /// hold, which each new map takes afresh; every member zero names no
    /// map.
    struct AtlasSource {
        const uint16_t* tile_indices{};
        std::size_t tile_index_count{};
        const uint8_t* tile_pixels{};
        std::size_t tile_pixel_count{};
        uint32_t tile_width{};
        uint32_t tile_height{};

        friend bool operator==(const AtlasSource&, const AtlasSource&) = default;

        /// Returns what identifies a map's terrain.
        ///
        /// @param map the map
        /// @return the identity
        [[nodiscard]] static AtlasSource of(const oa::formats::tnt::Map& map) noexcept {
            return {
                map.tile_indices.data(),
                map.tile_indices.size(),
                map.tile_palette_indices.data(),
                map.tile_palette_indices.size(),
                map.tile_width,
                map.tile_height
            };
        }
    };

    bool on{};                 ///< the tier decided for the frame is Full (set_full_presentation)
    card::Executor executor;   ///< open on the renderer while Full holds pages
    bool function_tested{};    ///< the Full function test ran on this executor and passed
    bool overflow_logged{};    ///< the sprite pages overflowing a frame has been logged
    bool stage_error_logged{}; ///< a model stage error has been logged

    // The match's terrain atlas and what it was built from. Once the pages
    // are filled the atlas keeps its grid, its slots and its pages' sizes
    // and levels, which the builder reads, and not their texels, which the
    // card holds: read it with tile_rect, never with read_terrain_view.
    oa::present::gpu_world::TerrainAtlas atlas;
    AtlasSource atlas_source{}; ///< the map it was built from; every member zero for none
    oa::PaletteBytes atlas_palette{};
    bool atlas_gamma{}; ///< built through the display gamma table
    std::array<uint8_t, 256> atlas_gamma_table{};
    uint32_t atlas_page_edge{};
    std::vector<card::PageHandle> pages; ///< the executor's page of each atlas page, in order
    /// The greyed terrain: the atlas built again with a palette of the gray
    /// table's entries, which the fog's greyed pass reads; it shares the
    /// colour atlas's slots and grid, so only its pages are kept.
    std::vector<card::PageHandle> greyed_pages;
    uint64_t page_bytes{};     ///< texel bytes uploaded to the pages, greyed ones included
    uint64_t atlas_build_ns{}; ///< the time the atlases took to build
    uint64_t page_upload_ns{}; ///< the time the pages took to make and fill
    /// The pages were made as the match loaded (make_full_match_pages),
    /// before its first frame, not at a frame.
    bool pages_from_load{};

    // The target a zoom between whole numbers is drawn through.
    card::TargetHandle target{};
    uint32_t target_width{};  ///< pixels across; 0 for none
    uint32_t target_height{}; ///< pixels down
    /// The target could not be made on this renderer, so such a zoom draws
    /// the terrain LINEAR straight to the window.
    bool target_refused{};

    // The target a frame below zoom 1 draws the battlefield into, on the
    // screen pixels laid from the map's corner, with a pixel of room on
    // every side, before the card moves it by the rest of a screen pixel
    // onto the view's exact place (Runtime::view_shift); made at the
    // battlefield's size and twice the display's density, or at the
    // display's density alone where the memory or the renderer refuses
    // that (Runtime::ensure_full_moved_target).
    card::TargetHandle moved_target{};
    /// The moved target's picture moved by the rest of a screen pixel at its
    /// own texels, LINEAR, which is then reduced onto the battlefield, each
    /// display pixel the mean of the two by two texels over it, so that the
    /// picture keeps one sharpness wherever between pixels it lands; at the
    /// moved target's size and factor. None at the display's density alone,
    /// where one LINEAR draw moves and lands the moved target.
    card::TargetHandle shifted_target{};
    uint32_t moved_target_width{};  ///< layout pixels across, the room included; 0 for none
    uint32_t moved_target_height{}; ///< layout pixels down
    uint32_t moved_target_factor{}; ///< texture pixels a layout pixel; 0 for none
    /// The size and factor at which the two targets at twice the display's
    /// density were refused, by the memory or the renderer, and are not
    /// asked for again; 0 for none.
    uint32_t refused_moved_width{};
    uint32_t refused_moved_height{};
    uint32_t refused_moved_factor{};
    /// The frame that moves the target onto the battlefield, run after the
    /// painters' overlay is drawn into the target: the resolve into the
    /// shifted target by the shift and the resolve of that onto the
    /// battlefield, or with no shifted target the one resolve that does
    /// both.
    card::CardFrame moved_frame;

    /// Destroys the moved and shifted targets and forgets their size and
    /// factor; a refusal remembered is left for ensure_full_moved_target.
    void destroy_moved_targets() noexcept;

    // The world target the battlefield is drawn into for anti-aliasing
    // (full_supersampling.hpp): at the supersample factor the Enhanced
    // anti-aliasing row asks for, fitted to the budget S and the texture
    // limit at the battlefield's size (Runtime::ensure_full_world_target);
    // none at a factor of 1.
    uint64_t supersample_budget{}; ///< S for this machine, in pixels; 0 until read
    uint32_t supersample_asked{1}; ///< the factor the row asks for
    uint32_t supersample{1};       ///< the factor in use: the target's, or 1 without one
    card::TargetHandle world_target{};
    uint32_t world_target_width{};  ///< the target's size across, in window pixels; 0 for none
    uint32_t world_target_height{}; ///< the target's size down
    uint32_t world_target_factor{}; ///< the factor the target was made at; 0 for none
    uint64_t world_target_bytes{};  ///< texture bytes the target and its half hold
    /// The renderer refused the target of this size at this factor, so the
    /// tier draws straight until the size or the factor asked changes.
    uint32_t refused_world_width{};
    uint32_t refused_world_height{};
    uint32_t refused_world_factor{};
    /// How the last Full frame was drawn through the world target
    /// (full_supersampling::plan_world_target): a factor of 1 for a frame
    /// drawn straight to the window.
    full_supersampling::WorldTargetPlan drawn_plan{};

    /// Destroys the world target, with its half, and forgets its size and
    /// factor; the factor in use and a refusal remembered are left as they
    /// are, for ensure_full_world_target to decide.
    void destroy_world_target() noexcept;

    card::CardFrame frame;              ///< the frame being built, its memory kept between frames
    FullFrameVertices frame_vertices{}; ///< the vertices of the frame being built, or the last
    /// The match's frame of the most vertices, which the log names as the
    /// match ends.
    FullFrameVertices busiest_frame{};

    // The sprite stage: the pages, with the match's palette and gray table,
    // and the card's pages that hold them.
    oa::present::gpu_world::SpritePages sprite_pages{oa::present::gpu_world::Limits{
        full_sprite_page_side,
        full_largest_sprite_page_side,
        full_sprite_page_memory,
        full_largest_page_memory
    }};
    uint64_t gray_generation{}; ///< the pages' palette generation the gray table was built for
    std::vector<FullCardPage> card_pages; ///< the card's pages, by the sprite pages' index
    full::SpriteStageResult sprites{};    ///< what the sprite stage did on the last frame built

    /// Sets the sprite pages' palette and gray table to the match's.
    ///
    /// @param palette_bytes the match's palette, 4 bytes a colour
    /// @param gamma the display gamma
    void ensure_sprite_palette(const oa::PaletteBytes& palette_bytes, float gamma);

    /// Returns the card's page for a sprite page, making it when the page
    /// is new or has a new size; the page at its old size is destroyed once
    /// the frame has run.
    ///
    /// Throws FullCardError when the card cannot make it.
    ///
    /// @param page index into the sprite pages
    /// @return the handle; none for a page the sprite pages do not hold
    [[nodiscard]] card::PageHandle card_page(uint32_t page);

    /// Returns the card's page for a sprite page (card_page), as the sprite
    /// stage asks for it (full::SpritePageHooks::card_page).
    ///
    /// @param context the presentation
    /// @param page index into the sprite pages
    /// @return the handle
    [[nodiscard]] static card::PageHandle card_page_hook(void* context, uint32_t page);

    /// Uploads what the sprite pages changed since the card's pages last
    /// took their texels: a page made this frame whole, another its dirty
    /// rectangle; a page released is destroyed once the frame has run
    /// (card::Executor::retire_page).
    ///
    /// Throws FullCardError when a page cannot be filled.
    void upload_sprite_pages();

    /// Destroys the card's pages of the sprite pages.
    void destroy_sprite_card_pages() noexcept;

    /// Says whether the sprite or model pages may grow by some bytes of
    /// texels (gpu_world::GrowthHooks::allow): whether the memory guard lets
    /// them (Runtime::accelerated_buffer_fits).
    ///
    /// @param context the runtime
    /// @param bytes the texel bytes the pages would grow by
    /// @return true when the memory allows them
    [[nodiscard]] static bool allow_page_growth(void* context, std::size_t bytes);

    /// Logs the sprite pages and the model stage's pages growing, and, once
    /// a match, their leaving out what a frame needs past what they may
    /// hold (page_memory_full).
    void note_page_memory();

    /// The memory limits of the sprite pages and of the model stage's pages
    /// together, as the log last named them in this match: what they
    /// started with, until they grow; 0 before the match's first frame.
    std::size_t logged_page_memory{};
    /// The pages left sprites or textures out of a frame in this match,
    /// holding as much as they may, which the "+stats" renderer row names
    /// and the log says once.
    bool page_memory_full{};
    /// The pages' held_refusals counts together when last looked at.
    uint64_t held_refusals_seen{};

    // The model stage, and what it was given.
    full::ModelStage models;
    oa::PaletteBytes models_palette{}; ///< the palette the stage was last set to
    float models_gamma{};              ///< and the gamma; 0 before the first
    /// FX.GAF "shadow" frame 0 as the stage reads it, from the match's sprite.
    oa::formats::gaf::RenderedFrame projectile_shadow;
    const void* projectile_shadow_source{}; ///< the sprite's pixels it was made from

    /// The fog the last planned frame built (note_full_fog_grid), which the
    /// card's fog passes draw from.
    struct Fog {
        oa::present::world_renderer::FogGrid grid; ///< empty when the frame draws no fog
        int32_t camera_x{};                        ///< the map pixel the grid was built for
        int32_t camera_z{};
        bool dithered{};         ///< the DitheredFog option
        card::Colour unmapped{}; ///< UI colour 0 through the display gamma
        card::Colour dither{};   ///< palette index 0 through the display gamma
    } fog;

    // The overlay canvas: the key colour the world layer is cleared to
    // before the painters paint it, and the overlay of what they painted.
    std::array<uint8_t, 3> overlay_key{};
    oa::PaletteBytes overlay_key_palette{}; ///< the palette the key was found for
    bool overlay_key_found{};
    /// The last match frame's world layer is the overlay canvas, not a
    /// picture, until the world is drawn whole again (full_frame_drawn).
    bool canvas_drawn{};
    /// The camera and the zoom the canvas frame was planned at, which the
    /// card's frame draws from (note_full_canvas); the camera below 0 left
    /// of or above the map.
    int32_t frame_camera_x{};
    int32_t frame_camera_y{};
    float frame_zoom{};
    /// The canvas frame's pixels, which the painters after the fog reach
    /// through whichever surface holds them while they paint
    /// (paints_full_canvas); null before the canvas is cleared.
    const uint8_t* canvas_pixels{};
    TiledTexture overlay_texture;
    std::vector<uint8_t> overlay; ///< ARGB8888 words, the battlefield's size
    std::vector<uint8_t> opaque_bands;
    std::vector<uint8_t> uploaded_bands;
    bool overlay_uploaded{};
    ScaledWorldCounts counts; ///< what the overlay's texture was made of
    /// The quads the painters after the fog asked for this frame, in paint
    /// order (paint_world_level, paint_world_blend, paint_world_minimum).
    std::vector<FullWorldQuad> world_quads;

    // What the last Full match frame drew, for the checks and the statistics.
    bool drawn{}; ///< the last match frame presented was drawn by the card
    float drawn_zoom{};
    full_terrain::TerrainDrawPlan plan{};
    uint32_t drawn_quads{};       ///< terrain quads
    uint32_t drawn_fog_quads{};   ///< the fog passes' quads
    uint32_t drawn_world_quads{}; ///< the painters' quads
    uint32_t drawn_batches{};
    bool drawn_through_target{};
    uint64_t build_ns{};   ///< the time the terrain, the fog and the painters' quads took to build
    uint64_t stage_ns{};   ///< the time the sprite and model stages took to build and upload
    uint64_t execute_ns{}; ///< the time the card's call took
    uint64_t overlay_ns{}; ///< the time the overlay took to convert and upload
    uint64_t frames{};     ///< match frames the Full branch presented
};

} // namespace oa::app

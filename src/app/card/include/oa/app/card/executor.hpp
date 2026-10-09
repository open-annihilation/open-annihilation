// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The executor of card command lists on SDL's renderer: it makes, updates
// and destroys texture pages and render targets, and runs a CardFrame
// (card.hpp) with SDL_RenderGeometryRaw and the renderer's blend modes,
// one call for each run of batches that share their state. Every failure
// is reported through error(); a frame that is malformed, or names a page
// or target that does not exist, is refused whole and nothing of it is
// drawn. Only the Full tier reaches it.
#pragma once

#include "oa/app/card.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace oa::app::card {

/// Most pages alive at once.
inline constexpr uint32_t most_pages = 4096;
/// Most render targets alive at once.
inline constexpr uint32_t most_targets = 64;

/// What a page is made with. Level n holds level 0 halved n times, rounded
/// down and never below 1 (level_edge); the engine fills each level itself,
/// reduced by its own filter, so that what a reduced draw shows is the
/// same on every renderer. How a draw reads the texels is the draw's own
/// (Batch::sampling).
struct PageDescription {
    uint32_t width{};       ///< texels across level 0, 1 to largest_page_edge
    uint32_t height{};      ///< texels down level 0, 1 to largest_page_edge
    uint8_t level_count{1}; ///< levels, 1 to most_page_levels
};

/// What the executor could arrange on its renderer.
struct Capabilities {
    /// Darken runs as a blend mode composed for the renderer, which reads
    /// only the source alpha; where the renderer refuses it, as the
    /// renderer's multiply mode with the vertex colours taken as black,
    /// which gives the same pixels from a copy of the batch's vertices.
    bool darken_composed{};
    /// Minimum runs as a blend mode composed for the renderer, each channel
    /// the lesser of the source's and the destination's; where the renderer
    /// refuses it, as SDL's software renderer does, a minimum draw blends
    /// as alpha, its colour over what is under it at its alpha.
    bool minimum_composed{};
    /// The renderer was told to clamp texture coordinates at the page's
    /// edge, which keeps it from reading every vertex of a draw to decide.
    bool clamped_addressing{};
    /// Sampling::pixel_art is set on the renderer's textures as its own
    /// mode; where false, on SDL before 3.4 or a renderer that refuses the
    /// mode, a pixel_art draw reads the nearest texel.
    bool pixel_art_sampling{};
    uint32_t texture_limit{}; ///< the largest texture edge it makes; 0 for no limit
};

/// What the executor did, for checks and the frame statistics.
struct Counts {
    uint64_t frames{};           ///< frames run to the end
    uint64_t frames_refused{};   ///< frames refused before anything was drawn
    uint64_t draw_calls{};       ///< geometry calls made
    uint64_t triangles{};        ///< triangles those calls drew
    uint64_t batches_merged{};   ///< draw batches run in the call of the batch before them
    uint64_t target_switches{};  ///< render targets bound
    uint64_t clip_changes{};     ///< scissors set or cleared
    uint64_t blend_changes{};    ///< blend modes set on a texture or the renderer
    uint64_t sampling_changes{}; ///< sampling modes set on a page's level
    uint64_t clears{};           ///< clear batches run
    uint64_t resolves{};         ///< resolve batches run
    uint64_t blend_reductions{}; ///< blend_reduce batches run
    uint64_t halvings{};         ///< render targets halved into their half
    uint64_t pages_alive{};
    uint64_t targets_alive{};
    uint64_t texture_bytes{}; ///< bytes of every page level and render target alive
};

/// Runs card command lists on one SDL renderer.
class Executor {
  public:

    Executor() = default;
    Executor(const Executor&) = delete;
    Executor& operator=(const Executor&) = delete;

    /// Closes the executor, destroying every page and target it made.
    ~Executor() { close(); }

    /// Binds the executor to a renderer, closing it first if it was open:
    /// composes the darken blend mode and finds whether the renderer takes
    /// it and the pixel-art sampling mode, and tells the renderer to clamp
    /// texture coordinates where SDL offers the call.
    ///
    /// @param renderer the renderer
    /// @param texture_limit the largest texture edge the renderer makes, in texels; 0 for no limit
    /// @return false, with error() set, when the renderer is null or a call failed
    [[nodiscard]] bool open(SDL_Renderer* renderer, uint32_t texture_limit);

    /// Destroys every page and target and lets go of the renderer.
    void close() noexcept;

    /// Says whether the executor is bound to a renderer.
    ///
    /// @return true between open and close
    [[nodiscard]] bool is_open() const noexcept { return renderer_ != nullptr; }

    /// Returns why the last call that failed did.
    ///
    /// @return what failed, with SDL's error after a failed call
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

    /// Returns what the renderer could arrange.
    ///
    /// @return the capabilities found at open
    [[nodiscard]] const Capabilities& capabilities() const noexcept { return capabilities_; }

    /// Returns what the executor has done.
    ///
    /// @return the counts since open
    [[nodiscard]] const Counts& counts() const noexcept { return counts_; }

    /// Makes a page: one texture for each level, holding Texels. Refuses a
    /// description beyond the limits or the renderer's texture limit with
    /// an error naming both.
    ///
    /// @param description the page's size and levels
    /// @return the page's handle; none, with error() set, when it was not made
    [[nodiscard]] PageHandle create_page(const PageDescription& description);

    /// Writes texels into a level of a page, the whole level or a part of it.
    ///
    /// @param page the page
    /// @param level the level written
    /// @param part the texels written, within the level; null for the whole level
    /// @param texels the first byte of the part's first row: Texels, texel_bytes each, in memory order
    /// @param pitch bytes from one row of `texels` to the next, at least the part's width times texel_bytes
    /// @return false, with error() set, when the page, level, part or pitch is wrong or SDL refused
    [[nodiscard]] bool update_page(
        PageHandle page, uint8_t level, const Rect* part, const uint8_t* texels, uint32_t pitch
    );

    /// Destroys a page; a handle that names no page is ignored. A frame
    /// that still names the page is refused.
    ///
    /// @param page the page
    void destroy_page(PageHandle page) noexcept;

    /// Destroys a page once the next frame has run, refused or not, or at
    /// close: a frame built while the page was replaced may still name it,
    /// and draws from it as it was. A handle that names no page is ignored.
    ///
    /// @param page the page
    void retire_page(PageHandle page);

    /// Says whether a handle names a page that is alive.
    ///
    /// @param page the handle
    /// @return true when it does
    [[nodiscard]] bool page_alive(PageHandle page) const noexcept;

    /// Returns the texture of a page's level, for checks that read it back.
    ///
    /// @param page the page
    /// @param level the level
    /// @return the texture; null when the page or level does not exist
    [[nodiscard]] SDL_Texture* page_texture(PageHandle page, uint8_t level) const noexcept;

    /// Makes a render target of a size at a supersampling factor: a texture
    /// of the size times the factor, which a resolve reduces to the size by
    /// halving it once for each doubling, every sample averaged. Draws into
    /// it give their vertices in pixels of the size. The texture, and the
    /// halves a factor above 2 reduces through, one fewer than its
    /// doublings, each half the one before, are cleared to a check colour
    /// and one pixel is read back, since a texture a driver failed to make
    /// draws black and reports no error; a pixel of another colour refuses
    /// the target. They are then cleared transparent. With `keep_half` the
    /// target keeps its first half at any factor, for the two-level
    /// reduction (Operation::blend_reduce), which is refused on a target
    /// without one; the halves' pixels count with the target's. Refuses a
    /// factor that is not a power of two from 1 to
    /// largest_supersampling_factor, and a texture beyond the limits or the
    /// renderer's texture limit, with an error naming both.
    ///
    /// @param width pixels across, above 0
    /// @param height pixels down, above 0
    /// @param factor the supersampling factor, a power of two from 1 to 16
    /// @param keep_half whether the target keeps a half for the two-level reduction
    /// @return the target's handle; none, with error() set, when it was not made
    [[nodiscard]] TargetHandle
    create_target(uint32_t width, uint32_t height, uint32_t factor, bool keep_half = false);

    /// Destroys a render target; a handle that names no target is ignored.
    ///
    /// @param target the target
    void destroy_target(TargetHandle target) noexcept;

    /// Says whether a handle names a render target that is alive.
    ///
    /// @param target the handle
    /// @return true when it does
    [[nodiscard]] bool target_alive(TargetHandle target) const noexcept;

    /// Returns a render target's texture, for checks that read it back.
    ///
    /// @param target the target
    /// @return the texture, of the size times the factor; null when the target does not exist
    [[nodiscard]] SDL_Texture* target_texture(TargetHandle target) const noexcept;

    /// Runs a frame. The frame is checked whole first (check_frame, and
    /// every page, level and target it names must exist); a frame found
    /// wanting is refused and nothing of it is drawn. Then each batch runs
    /// in order, draw batches that share their target, page, level, blend,
    /// sampling and scissor and follow one another in the indices in one
    /// geometry call. The render target is `final_target` when it returns,
    /// with the scissor, draw colour and draw blend mode it found there.
    /// The pages retired before it are destroyed after it (retire_page).
    ///
    /// @param frame the frame
    /// @param final_target the target batches of no render target draw into; null for the window.
    ///     Never one of the executor's own render targets, which a resolve draws instead
    /// @return false, with error() set, when the frame was refused or a call failed part way;
    ///     frame_refused() tells the two apart
    [[nodiscard]] bool execute(const CardFrame& frame, SDL_Texture* final_target);

    /// Says whether the last frame run was refused before anything of it
    /// was drawn: a frame built wrong, which no driver caused, as against a
    /// call to SDL that failed.
    ///
    /// @return true when the last execute refused its frame
    [[nodiscard]] bool frame_refused() const noexcept { return frame_refused_; }

  private:

    /// One level of a page.
    struct Level {
        SDL_Texture* texture{};
        uint32_t width{};
        uint32_t height{};
        SDL_BlendMode blend{SDL_BLENDMODE_NONE};    ///< the mode the texture is set to
        SDL_ScaleMode scale{SDL_SCALEMODE_NEAREST}; ///< the sampling the texture is set to
    };

    /// One page slot.
    struct Page {
        bool alive{};
        uint32_t generation{}; ///< raised when the slot is freed, so old handles miss it
        uint8_t level_count{};
        std::array<Level, most_page_levels> levels{};
    };

    /// The scissor set on a render target's own view.
    struct ClipState {
        bool known{};   ///< whether `clipped` and `clip` describe the view
        bool clipped{}; ///< a scissor is set
        Rect clip{};    ///< the scissor set
    };

    /// The most halvings a render target keeps: those a factor of
    /// largest_supersampling_factor resolves through before the one LINEAR
    /// draw that lands it.
    static constexpr uint32_t most_halvings = 3;

    /// One render target slot.
    struct Target {
        bool alive{};
        uint32_t generation{};
        SDL_Texture* texture{}; ///< the size times the factor
        /// The texture halved once, twice and three times, each half the
        /// one before: the halvings a factor above 2 resolves through into
        /// the frame's final target, one fewer than the factor's doublings
        /// (into a target at a factor above 1, fewer), and the first for a target
        /// made with keep_half, which the two-level reduction reads it from;
        /// null beyond those made.
        std::array<SDL_Texture*, most_halvings> halves{};
        uint32_t width{};
        uint32_t height{};
        uint32_t factor{};
        SDL_BlendMode blend{SDL_BLENDMODE_NONE}; ///< the mode `texture` is set to
        /// The modes the halves are set to.
        std::array<SDL_BlendMode, most_halvings> half_blends{};
        /// The run the halves were last filled in, with `texture` not drawn
        /// into or cleared since, and how many of them were; 0 for never.
        uint64_t halved_in{};
        uint32_t halved_count{};
        ClipState clip{};
    };

    /// What execute keeps while it runs.
    struct Run {
        SDL_Texture* final_target{};
        SDL_Texture* bound{};   ///< the render target bound now
        bool final_bound{};     ///< whether `bound` is the final target
        Target* bound_target{}; ///< the render target bound now; null for the final or a half
        ClipState final_clip{}; ///< the scissor on the final target: found, then set
        bool final_clip_changed{};
        bool saved_clipped{};      ///< the final target's own scissor, to put back
        SDL_Rect saved_clip{};     ///< the final target's own scissor
        SDL_FColor saved_colour{}; ///< the draw colour found, to put back
        bool colour_changed{};
        SDL_BlendMode saved_draw_blend{SDL_BLENDMODE_NONE}; ///< the draw blend mode found
        SDL_BlendMode draw_blend{SDL_BLENDMODE_NONE};       ///< the draw blend mode set now
        bool draw_blend_changed{};
    };

    /// Records a failed call and its SDL error.
    ///
    /// @param what the call that failed, and what it was for
    /// @return false, for the caller to return
    bool fail(const std::string& what);

    /// Records a refusal, with no SDL error.
    ///
    /// @param why what was refused
    /// @return false, for the caller to return
    bool refuse(const std::string& why);

    /// Runs a frame for execute, which then destroys the retired pages.
    ///
    /// @param frame the frame
    /// @param final_target the frame's final target
    /// @return false, with error() set, when the frame was refused or a call failed part way
    bool run_frame(const CardFrame& frame, SDL_Texture* final_target);

    /// Returns a page slot a handle names.
    ///
    /// @param page the handle
    /// @return the slot; null when the handle names none alive
    [[nodiscard]] const Page* find_page(PageHandle page) const noexcept;

    /// Returns a page slot a handle names, to change it.
    ///
    /// @param page the handle
    /// @return the slot; null when the handle names none alive
    [[nodiscard]] Page* find_page(PageHandle page) noexcept;

    /// Returns a target slot a handle names.
    ///
    /// @param target the handle
    /// @return the slot; null when the handle names none alive
    [[nodiscard]] const Target* find_target(TargetHandle target) const noexcept;

    /// Returns a target slot a handle names, to change it.
    ///
    /// @param target the handle
    /// @return the slot; null when the handle names none alive
    [[nodiscard]] Target* find_target(TargetHandle target) noexcept;

    /// Readies a new render target's texture: binds it, scales draws into
    /// it by the factor, clears it to the check colour and reads one pixel
    /// back, then clears it transparent.
    ///
    /// @param texture the texture
    /// @param factor the supersampling factor draws into it are scaled by
    /// @param need what the texture is, for the error
    /// @return false, with error() set, when a call failed or the pixel read back another colour
    bool prepare_target(SDL_Texture* texture, uint32_t factor, const std::string& need);

    /// Checks that every page, level and target a frame names exists, and
    /// that the final target is none of the executor's own.
    ///
    /// @param frame the frame, already well formed
    /// @param final_target the frame's final target
    /// @return what is missing; empty when nothing is
    [[nodiscard]] std::string
    check_handles(const CardFrame& frame, const SDL_Texture* final_target) const;

    /// Binds a render target unless it is bound.
    ///
    /// @param[in,out] run the run
    /// @param target the target; null for the final target
    /// @return false when SDL refused
    bool bind(Run& run, Target* target);

    /// Binds a texture that is no target of the frame's: a target's half.
    ///
    /// @param[in,out] run the run
    /// @param texture the texture
    /// @return false when SDL refused
    bool bind_texture(Run& run, SDL_Texture* texture);

    /// Sets the bound target's scissor unless it is set so.
    ///
    /// @param[in,out] run the run
    /// @param scissored whether a scissor applies
    /// @param scissor the scissor, in pixels of the target
    /// @return false when SDL refused
    bool set_clip(Run& run, bool scissored, const Rect& scissor);

    /// Sets a texture's blend mode unless it is set so.
    ///
    /// @param texture the texture
    /// @param[in,out] current the mode the texture is set to, updated
    /// @param mode the mode
    /// @return false when SDL refused
    bool set_texture_blend(SDL_Texture* texture, SDL_BlendMode& current, SDL_BlendMode mode);

    /// Sets a texture's sampling mode unless it is set so.
    ///
    /// @param texture the texture
    /// @param[in,out] current the mode the texture is set to, updated
    /// @param mode the mode
    /// @return false when SDL refused
    bool set_texture_sampling(SDL_Texture* texture, SDL_ScaleMode& current, SDL_ScaleMode mode);

    /// Sets the renderer's draw blend mode unless it is set so.
    ///
    /// @param[in,out] run the run
    /// @param mode the mode
    /// @return false when SDL refused
    bool set_draw_blend(Run& run, SDL_BlendMode mode);

    /// Returns the renderer's mode for a blend.
    ///
    /// @param blend the blend
    /// @return the mode
    [[nodiscard]] SDL_BlendMode blend_mode(Blend blend) const noexcept;

    /// Returns the renderer's mode for a sampling.
    ///
    /// @param sampling the sampling
    /// @return the mode; nearest for pixel_art where the renderer lacks it
    [[nodiscard]] SDL_ScaleMode scale_mode(Sampling sampling) const noexcept;

    /// Runs a draw batch, or a run of merged ones, as one geometry call.
    ///
    /// @param[in,out] run the run
    /// @param frame the frame
    /// @param batch the first batch of the run, whose state every one shares
    /// @param first_index the first index drawn
    /// @param index_count the indices drawn
    /// @return false when SDL refused
    bool run_draw(
        Run& run,
        const CardFrame& frame,
        const Batch& batch,
        Index first_index,
        uint32_t index_count
    );

    /// Runs a clear batch.
    ///
    /// @param[in,out] run the run
    /// @param batch the batch
    /// @return false when SDL refused
    bool run_clear(Run& run, const Batch& batch);

    /// Halves a render target's texture into its halves, each from the one
    /// before, up to a count, unless done this run with the texture not
    /// written since.
    ///
    /// @param[in,out] run the run
    /// @param[in,out] source the target, which has that many halves
    /// @param count the halves to fill, 1 to most_halvings
    /// @return false when SDL refused
    bool halve(Run& run, Target& source, uint32_t count);

    /// Runs a resolve batch: halves a source above factor 2 through its
    /// halves, one fewer than its doublings, unless done this run with the
    /// source not written since, then draws the last reduced by one more
    /// halving into the destination.
    ///
    /// @param[in,out] run the run
    /// @param batch the batch
    /// @return false when SDL refused
    bool run_resolve(Run& run, const Batch& batch);

    /// Runs a blend_reduce batch: halves the source unless done this run
    /// with the source not written since, draws the part's half LINEAR
    /// into the destination, then the part LINEAR over it at alpha 1 - t,
    /// t = log2(1 / scale) by the scale across, left out at a scale of
    /// one half, where it would add nothing.
    ///
    /// @param[in,out] run the run
    /// @param batch the batch
    /// @return false when SDL refused
    bool run_blend_reduce(Run& run, const Batch& batch);

    /// Puts back what the run found: the final target bound, with its own
    /// scissor, draw colour and draw blend mode.
    ///
    /// @param[in,out] run the run
    /// @return false when SDL refused
    bool restore(Run& run);

    /// Destroys a page slot's textures and frees the slot.
    ///
    /// @param slot the slot's index
    void free_page(std::size_t slot) noexcept;

    /// Destroys a target slot's textures and frees the slot.
    ///
    /// @param slot the slot's index
    void free_target(std::size_t slot) noexcept;

    SDL_Renderer* renderer_{};
    Capabilities capabilities_{};
    Counts counts_{};
    std::string error_{};
    SDL_BlendMode darken_mode_{SDL_BLENDMODE_NONE};  ///< the composed darken mode
    SDL_BlendMode minimum_mode_{SDL_BLENDMODE_NONE}; ///< the composed minimum mode
    /// How pages are made: static, or streaming on SDL's software renderer,
    /// which run-length encodes a static texture, so that each copy of a
    /// part of it would scan the whole.
    SDL_TextureAccess page_access_{SDL_TEXTUREACCESS_STATIC};
    uint64_t run_serial_{}; ///< runs of execute begun, refused ones left out
    bool frame_refused_{};  ///< the last execute refused its frame
    std::vector<Page> pages_{};
    std::vector<std::size_t> free_pages_{};   ///< page slots freed, to use again
    std::vector<PageHandle> retired_pages_{}; ///< pages destroyed after the next frame
    std::vector<Target> targets_{};
    std::vector<std::size_t> free_targets_{};
    std::vector<Vertex> scratch_{}; ///< the darken fallback's copy of a batch's vertices
};

} // namespace oa::app::card

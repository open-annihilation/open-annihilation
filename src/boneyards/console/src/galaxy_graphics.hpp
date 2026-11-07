// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// War-map graphics of the Boneyards system object: the static map, the
// planet auras of each side, the selection and passover rings, the capitol
// markers, the planet sprites and name labels, and the war-map drawer.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "oa/boneyards/console/images.hpp"
#include "oa/boneyards/screens/galaxy.hpp"
#include "oa/boneyards/screens/lobby_widgets.hpp"
#include "oa/boneyards/service.hpp"
#include "oa/boneyards/toolkit.hpp"
#include "oa/formats/gaf.hpp"
#include "oa/present/surface.h"
#include "oa/present/surface.hpp"

namespace oa::boneyards::console {

inline constexpr std::size_t k_aura_sides = 3;
inline constexpr std::size_t k_aura_sizes = 12; // 25_L .. 100_S
inline constexpr std::size_t k_ring_sizes = 4;  // 25, 50, 75, 100
inline constexpr std::size_t k_ring_frames = 3; // frames 2, 1 and 0 of each ring
inline constexpr std::size_t k_capitol_frames = 12;
inline constexpr std::size_t k_planet_sprite_sizes = 4; // 25, 50, 75, 100

// The Recon image's terrain scan: the clip its draw sets and the corner a
// frame at the strip's scroll position lands on.
inline constexpr Rect32 k_recon_scan_clip = {0xD, 0x10, 0x1E3, 0x89};
inline constexpr int32_t k_recon_scan_x = 0xD;
inline constexpr int32_t k_recon_scan_y = 0x10;

// Aura archives by side letter: 'I' (independent), 'A' Arm, 'C' Core.
enum class AuraSide : uint8_t { independent, arm, core };

// A frame decoded for drawing.
struct DecodedFrame {
    const formats::gaf::Frame* frame{};
    formats::gaf::RenderedFrame image{};
};

// A planet's pan frames laid side by side, as the Recon open lays them.
struct PanoramaStrip {
    int32_t frame_count{};
    int32_t total{};                                            // width of every frame side by side
    std::array<int32_t, screens::kMaxReconPanFrames> offsets{}; // each frame's left edge
};

// The war-map graphics the system object keeps (StatusWidget.galaxy_graphics).
struct GalaxyGraphics {
    const formats::gaf::Sequence* map_static = nullptr;
    int32_t map_static_frames = 0;
    std::optional<formats::gaf::Archive> map_archive; // anims\Map_Static.gaf
    std::array<std::optional<formats::gaf::Archive>, k_aura_sides> auras;
    std::array<std::array<const formats::gaf::Sequence*, k_aura_sizes>, k_aura_sides>
        aura_sequences{};
    std::optional<formats::gaf::Archive> selected; // anims\Selected.gaf
    std::array<std::array<const formats::gaf::Frame*, k_ring_frames>, k_ring_sizes>
        selection_rings{};
    std::optional<formats::gaf::Archive> passover; // anims\Passover.gaf
    std::array<std::array<const formats::gaf::Frame*, k_ring_frames>, k_ring_sizes>
        passover_rings{};
    std::array<const formats::gaf::Frame*, k_capitol_frames> capitol_core{};
    std::array<const formats::gaf::Frame*, k_capitol_frames> capitol_arm{};
    std::optional<formats::gaf::Archive> planets; // anims\Planets.gaf
    std::vector<DecodedFrame> decoded{};          // frames drawn so far
    // Each loaded planet's Planets.gaf sequences by size,
    // aligned with GalaxyView::planets.
    std::array<
        std::array<const formats::gaf::Sequence*, k_planet_sprite_sizes>,
        screens::kMaxGalaxyPlanets>
        planet_sequences{};
    // The system object's font table and the toolkit holding its fonts. The
    // load renders the planet name labels in the table's first font
    // (small_light: BYHatFontSmall87 tinted 0x54, set as the Main Frame first opens);
    // without them no label is rendered.
    Toolkit* label_toolkit{};
    const screens::HatFonts* system_fonts{};
    // Each loaded planet's "PlanetName" surface, aligned
    // with GalaxyView::planets.
    std::array<present::SurfaceBuffer, screens::kMaxGalaxyPlanets> labels{};
    // Each planet's pan archive and pan sequence once
    // the Recon has loaded them, and the strip of its frames; aligned with
    // GalaxyView::planets and released when a war map loads.
    std::array<std::optional<formats::gaf::Archive>, screens::kMaxGalaxyPlanets> pans;
    std::array<const formats::gaf::Sequence*, screens::kMaxGalaxyPlanets> pan_sequences{};
    std::array<PanoramaStrip, screens::kMaxGalaxyPlanets> pan_strips{};
    // Palette the colour-vision mode matches. Null keeps today's link colours and aura pixels.
    const oa::Palette* palette = nullptr;
    // Arm then Core aura pixels saved before a colour-vision mode rewrites them, in walk order.
    // Empty while those archives are unchanged.
    std::array<std::vector<std::vector<uint8_t>>, 2> aura_originals{};
    // Colour-vision mode last applied to the auras. Off means the archives are unchanged.
    uint8_t aura_mode = 0;
};

/// Finds the first sequence of an archive with a name, ignoring case.
///
/// @param archive GAF archive
/// @param name sequence name
/// @return the sequence, or null when none has the name
const formats::gaf::Sequence*
galaxy_find_sequence(const formats::gaf::Archive& archive, const char* name);

/// Loads one side's aura archive and resolves its twelve size sequences.
///
/// @param[in,out] graphics war-map graphics
/// @param side side letter, either case: I (independent), A (Arm) or C (Core)
/// @param files game resource reader
/// @return false for another letter, a missing archive or any missing sequence (which drops the
///     archive)
bool galaxy_load_auras(GalaxyGraphics& graphics, char side, const FileReader& files);

/// Loads the selection and passover ring archives with their four sizes, and the Arm and Core
/// capitol frames.
///
/// @param[in,out] graphics war-map graphics
/// @param capitols archive holding Capitol_Arm and Capitol_Core (the independent aura archive);
///     null fails the load
/// @param files game resource reader
/// @return false when an archive or ring sequence is missing
bool galaxy_load_rings(
    GalaxyGraphics& graphics, const formats::gaf::Archive* capitols, const FileReader& files
);

/// Loads the static map, the independent, Core and Arm auras, the rings and the planet sprites.
///
/// @param[out] graphics war-map graphics
/// @param files game resource reader
/// @return false when any of them is missing
bool galaxy_load_graphics(GalaxyGraphics& graphics, const FileReader& files);

/// Loads a war map into the galaxy view.
///
/// Asks for the planets' allegiances and every planet's census, clears the
/// view (its planets' panoramas with it) and opens the war-map file through
/// the service's sector-map requests. When its map image covers the galaxy,
/// the planets and connectors are read into temporary records; the three
/// star layers are seeded from the sector id and each planet is built with
/// its sprite sequences, name label and hotspot and entered in id order,
/// then the connectors are linked. The temporary records are freed and the
/// file closed. The star layers come from galaxy_load after the same size
/// check, whose planet records are read first here (the layers draw only on
/// their own generator, so the order is not observable), and a planet
/// without its sprite sequences fails the load before any planet is
/// entered.
///
/// @param[in,out] view galaxy view
/// @param[in,out] graphics war-map graphics; its planet sequences and labels are set
/// @param service client service reading the war map
/// @param path war-map file (Boneyards\<area name>_map.gwm)
/// @return false when the file is missing or invalid, the map image is smaller than the galaxy, a
///     star layer cannot be built, a sprite sequence is missing or a planet id repeats
bool galaxy_view_load(
    screens::GalaxyView* view,
    GalaxyGraphics& graphics,
    const BoneyardsService& service,
    const char* path
);

/// Draws a GAF frame at a point less its origin, its transparency index left out.
///
/// @param[in,out] graphics war-map graphics caching the decoded frame
/// @param[in,out] target surface drawn into
/// @param frame frame drawn; null draws nothing
/// @param x where the frame's origin lands, in target pixels
/// @param y where the frame's origin lands, in target pixels
void galaxy_draw_frame(
    GalaxyGraphics& graphics,
    Surface* target,
    const formats::gaf::Frame* frame,
    int32_t x,
    int32_t y
);

/// Loads a planet's pan archive and lays its pan sequence's frames side by side into the planet's
/// strip.
///
/// Loads anims\<pan_gaf> and finds the pan sequence, ignoring case, unless
/// the sequence is already found; then lays at most
/// screens::kMaxReconPanFrames frames side by side as the Recon open does.
/// The frames keep their origins, which the Recon open clears and the
/// panorama draw leaves out.
///
/// @param[in,out] graphics war-map graphics
/// @param index planet index in the galaxy view
/// @param planet the planet, naming its pan archive and sequence
/// @param files game resource reader
/// @return false without the archive or the sequence
bool galaxy_load_panorama(
    GalaxyGraphics& graphics,
    std::size_t index,
    const screens::GalaxyPlanet& planet,
    const FileReader& files
);

/// Releases every planet's pan archive, sequence and strip.
///
/// @param[in,out] graphics war-map graphics
void galaxy_release_panoramas(GalaxyGraphics& graphics);

/// Paints the Recon image with a panorama.
///
/// Draws the strip frame under `scroll` (the last whose left edge is at most
/// `scroll`) with its top-left corner at (left edge - scroll + 0xD, 0x10)
/// and, when the frame under `end` is another one, that frame straight after
/// it; both are clipped to `clip` within `target`, whose own clip is
/// restored afterwards. The wind field refresh and child draws are left to
/// the Recon console.
///
/// @param[in,out] graphics war-map graphics
/// @param index planet index in the galaxy view
/// @param[in,out] target the Recon console's image
/// @param scroll left edge of the shown strip (ReconState::pan_scroll)
/// @param end right edge of the shown strip (ReconState::pan_end)
/// @param clip scan clip (k_recon_scan_clip)
void galaxy_draw_panorama(
    GalaxyGraphics& graphics,
    std::size_t index,
    Surface* target,
    int32_t scroll,
    int32_t end,
    const Rect32& clip
);

/// Paints the Recon image without a panorama: a Map_Static frame at (0, 0) less its origin.
///
/// @param[in,out] graphics war-map graphics
/// @param frame Map_Static frame index
/// @param[in,out] target the Recon console's image; its own clip is restored afterwards
/// @param clip scan clip (k_recon_scan_clip)
void galaxy_draw_map_static(
    GalaxyGraphics& graphics, int32_t frame, Surface* target, const Rect32& clip
);

/// Renders a planet's name label in the system object's first font (the label half of the planet
/// constructor).
///
/// Measures the name, makes a surface that wide and three rows taller than
/// the font, clears it to 9 and draws the name from its corner. The label
/// stays empty without the font table.
///
/// @param[in,out] graphics war-map graphics holding the label surfaces
/// @param index planet index in the galaxy view
/// @param name planet name
void galaxy_render_label(GalaxyGraphics& graphics, std::size_t index, const char* name);

/// Plots a line of one colour towards (x1, y1), one pixel per step of the longer axis, the last
/// step left out.
///
/// Walks the longer axis from the lower end, adding the slope to the other
/// coordinate in floating point and truncating it per pixel; pixels off the
/// surface are skipped.
///
/// @param[in,out] target surface drawn into
/// @param x0 first end, in target pixels
/// @param y0 first end, in target pixels
/// @param x1 second end, in target pixels
/// @param y1 second end, in target pixels
/// @param color palette index
void galaxy_draw_line(
    Surface* target, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color
);

/// Draws a visible star in the grey ramp.
///
/// The core shade is the star's dim plus the layer's and the halo four steps
/// dimmer, both held at the ramp's end. A point lights its pixel; a pair its
/// pixel and the one right (variants 0, 1) or below (2, 3), the halo first
/// in 0 and 2; a square its 2 x 2 block with the halo on the main diagonal
/// in variant 0 and the other in 1; a cross the core at (x + 1, y + 1) and
/// the halo on its four sides.
///
/// @param[in,out] target surface drawn into
/// @param star the star; an invisible one draws nothing
/// @param layer_dim grey-ramp steps its layer dims every star by
void galaxy_draw_star(Surface* target, const screens::GalaxyStar& star, int32_t layer_dim);

/// Draws every star of a layer.
///
/// @param[in,out] target surface drawn into
/// @param layer star-field layer
void galaxy_draw_star_layer(Surface* target, const screens::GalaxyStarLayer& layer);

/// Draws the war map into the map panel's viewer.
///
/// Once galaxy_shows_war_map says so (map data is present, and while it
/// loads only with GalaxyView.show_map_while_loading set): the
/// three star layers, each visible connector in its colour (the active
/// colour-vision mode's side colour when one is on), then each
/// visible planet in id order: the aura of its current side at its activity
/// frame, the capitol marker of its initial side, the selection and
/// passover rings when selected or hovered, its sprite at its frame and,
/// zoomed in or selected, its name label. Otherwise, while the data loads,
/// the current Map_Static frame. Auras, rings and capitol markers take the
/// variant scale * 3 + size class. An activity past the aura's frames
/// draws no aura.
///
/// @param[in,out] graphics war-map graphics
/// @param view galaxy view
/// @param[in,out] viewer the map panel's viewer surface
void galaxy_draw(GalaxyGraphics& graphics, const screens::GalaxyView& view, Surface* viewer);

/// Counts the visible planets whose anchor pixel in the viewer is the non-key anchor pixel of their
/// current sprite frame.
///
/// This is what a check reads of a drawn map.
///
/// @param[in,out] graphics war-map graphics caching the decoded frames
/// @param view galaxy view
/// @param viewer the map panel's viewer surface as last drawn
/// @return number of planets found drawn
std::size_t galaxy_count_drawn_planets(
    GalaxyGraphics& graphics, const screens::GalaxyView& view, const Surface& viewer
);

} // namespace oa::boneyards::console

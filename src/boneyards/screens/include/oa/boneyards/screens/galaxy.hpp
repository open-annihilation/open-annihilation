// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Galactic War map behind the Main Frame: the planets and connectors of the
// downloaded war map, the scrolled and scaled view with its planet hotspots,
// the census and allegiance feeds, the territory summary and the planet
// information rows shown by the Recon screen.
#pragma once

#include "oa/boneyards/screens/widgets.hpp"
#include "oa/boneyards/service.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace oa::boneyards::screens {

// The static map image and its four zoom levels: level `scale` shows
// (scale + 1) / kMapUnitsPerPixelAtLevel0 pixel per map unit, so the whole
// map fits the 498 x 328 panel at level 0.
inline constexpr int32_t kGalaxyMapWidth = 0x7c8;
inline constexpr int32_t kGalaxyMapHeight = 0x520;
inline constexpr int32_t kGalaxyScaleCount = 4;
inline constexpr int32_t kMapUnitsPerPixelAtLevel0 = 4;
static_assert(
    kMapUnitsPerPixelAtLevel0 == kGalaxyScaleCount, "the last level shows one pixel per unit"
);
// The map panel's viewer: the galaxy at level 0.
inline constexpr int32_t kGalaxyViewerWidth = kGalaxyMapWidth / kMapUnitsPerPixelAtLevel0;
inline constexpr int32_t kGalaxyViewerHeight = kGalaxyMapHeight / kMapUnitsPerPixelAtLevel0;
static_assert(kGalaxyViewerWidth == 0x1f2 && kGalaxyViewerHeight == 0x148, "498 x 328 viewer");

inline constexpr std::size_t kMaxGalaxyPlanets = 128;
inline constexpr std::size_t kMaxGalaxyConnectors = 256;
inline constexpr std::size_t kPlanetNameCapacity = 0x10;
inline constexpr std::size_t kPlanetDescriptionCapacity = 0x400;
inline constexpr std::size_t kPlanetMapNameCapacity = 0x3f;
inline constexpr std::size_t kPlanetMapTypeCapacity = 0x10;
inline constexpr std::size_t kPlanetPanelGafCapacity = 0x29;
inline constexpr std::size_t kPlanetPanelSequenceCapacity = 0x20;
inline constexpr std::size_t kPlanetSettingCount = 9;
inline constexpr int32_t kNoPlanet = -1;

// Origin of a planet's Galactic War map (its ta_mapsource key), which decides
// the notice shown when the map is not installed.
enum class MapSource : int32_t {
    ta_disk = 0,              // "TA"
    core_contingency = 1,     // "CC"
    battle_tactics = 2,       // "BT"
    web_ta = 3,               // "WTA": downloaded map for the base game
    web_core_contingency = 4, // "WCC"
    web_battle_tactics = 5,   // "WBT"
};

// One planet of the war map as the comms layer stores it (SectorPlanetRecord,
// whose strings are inline).
struct PlanetRecord {
    int32_t id;
    const char* name; // at most 15 characters
    int32_t x;        // sprite anchor in map units
    int32_t y;
    const char* sequence;     // sprite sequence base name
    int32_t variant_group;    // 0..2, selects the sprite name suffix set
    int32_t frame_interval{}; // animation steps per sprite frame, 0 = every step
    char side;                // 'A', 'C' or 'I'
    bool capitol;
    const char* description;
    const char* ta_map;
    const char* ta_mapsource; // TA, CC, BT, WTA, WCC or WBT
    const char* ta_maptype;
    const char* pan_gaf;
    const char* pan_seq;
    int32_t settings[kPlanetSettingCount]; // see GalaxyPlanet
};

using oa::boneyards::ConnectorRecord;

// One frame of the planet sprite: size and anchor offset.
struct SpriteFrame {
    int32_t width;
    int32_t height;
    int32_t x_offset;
    int32_t y_offset;
};

// GlobalHeader values of the planet's map TDF.
struct PlanetEnvironment {
    bool loaded;
    int32_t gravity;
    int32_t max_wind;
    int32_t min_wind;
    int32_t tidal_strength;
    bool lavaworld;
};

// Reader for the planet map's TDF: open selects the map by name (false when
// it is not installed), get_int reads a GlobalHeader value. note_source, when
// set, is told the planet's map source before open, so a base-disc map that
// failed to load can be read again after the archives are scanned.
struct MapTdfReader {
    void* user;
    bool (*open)(void* user, const char* map_name);
    int32_t (*get_int)(void* user, const char* key, int32_t fallback);
    void (*note_source)(void* user, MapSource source) = nullptr;
};

using oa::boneyards::CensusReport;

// Planet display record.
struct GalaxyPlanet {
    bool hotspot; // a PlanetHotSpot widget exists for the planet
    int32_t id;
    char name[kPlanetNameCapacity];
    // The record's variant group: 0 large, 1 medium, 2 small; picks
    // the Planets.gaf suffix, the radius and the aura, ring and capitol
    // variant within a zoom level
    int32_t size_class;
    int32_t frame_interval{};
    int32_t x;
    int32_t y;
    char side; // current affiliation, replaced by the allegiance feed
    char initial_side;
    bool capitol;
    char description[kPlanetDescriptionCapacity];
    char ta_map[kPlanetMapNameCapacity];
    MapSource map_source;
    char map_type[kPlanetMapTypeCapacity];
    int32_t player_limit;
    int32_t commander_death;
    int32_t extra_settings[kPlanetSettingCount - 2];
    PlanetEnvironment environment;
    int32_t frame_count{}; // frames of the planet sprite
    int32_t frame{};       // sprite frame shown
    // By size class: 20, 16, 12, half the 100-size sprite's width; the
    // name label hangs radius + 6 map units below the anchor
    int32_t radius;
    char pan_gaf[kPlanetPanelGafCapacity]; // "anims\" + pan_gaf
    char pan_seq[kPlanetPanelSequenceCapacity];
    int32_t population;
    int32_t activity;
    int32_t in_progress; // shown as "Battles:"
    int32_t forming;     // shown as "Forming:"
    int32_t arm_wins;    // shown beside ArmCoreLogos frame 0 while contested
    int32_t core_wins;   // shown beside ArmCoreLogos frame 1 while contested
    int32_t left;        // sprite rectangle in map units
    int32_t top;
    int32_t right;
    int32_t bottom;
    bool visible;   // inside the scrolled view
    int32_t view_x; // anchor in view pixels
    int32_t view_y;
    int32_t view_left; // hotspot bounds in view pixels
    int32_t view_top;
    int32_t view_width;
    int32_t view_height;
};

// Connector between two planets, with its endpoints in the map and in the view.
struct GalaxyConnector {
    int32_t a; // planet index
    int32_t b;
    int32_t ax; // endpoints in map units
    int32_t ay;
    int32_t bx;
    int32_t by;
    bool visible;
    int32_t view_ax; // endpoints in view pixels
    int32_t view_ay;
    int32_t view_bx;
    int32_t view_by;
    int32_t color; // palette index
};

// Connector palette indices.
namespace connector_color {
inline constexpr int32_t mixed = 0x42; // Arm-Core or Contested-Contested
inline constexpr int32_t core = 0xd6;
inline constexpr int32_t arm = 0xe5;
} // namespace connector_color

// Star fields behind the planets: three layers, each with one star per
// square cell of the map image, shown through a window that follows a
// share of the map's scroll range.
inline constexpr std::size_t kGalaxyStarLayers = 3;
// Cells one layer holds. 3.1c takes a war map of any height; cells 5% of
// the image wide make 20 or 21 columns, so this bound admits up to 48 rows
// and a taller map fails its load here.
inline constexpr std::size_t kMaxGalaxyStars = 0x400;

// Star shapes (GalaxyStar.kind).
enum class StarKind : int32_t {
    none = 0, // never placed; draws nothing
    point = 1,
    pair = 2,   // two pixels across (variants 0, 1) or down (2, 3)
    square = 3, // 2 x 2
    cross = 4,  // five pixels around (x + 1, y + 1)
};

// One star.
struct GalaxyStar {
    int32_t x; // map units
    int32_t y;
    StarKind kind;
    int32_t variant; // orientation and which pixels take the dimmer shade
    int32_t dim;     // grey-ramp steps below the brightest, added to the layer's
    int32_t left;    // lit rectangle in map units
    int32_t top;
    int32_t right;
    int32_t bottom;
    bool visible;   // inside the layer's window
    int32_t view_x; // view pixels
    int32_t view_y;
};

// A star-field layer.
struct GalaxyStarLayer {
    int32_t width; // map image size
    int32_t height;
    float scroll_share; // share of the map's scroll range the window travels
    int32_t dim;
    GalaxyStar stars[kMaxGalaxyStars]; // cells row by row
    int32_t star_count;                // cells; 0 while the layer is not built
};

inline constexpr std::size_t kTerritoryNameCapacity = kPlanetNameCapacity;

// Territory summary under the map, shown by the battle summary panel.
struct TerritoryPanel {
    int32_t planet; // planet index, kNoPlanet when none
    bool shown;     // the six first-row labels are visible
    char name[kTerritoryNameCapacity];
    const char* owner; // "Arm", "Core" or "Contested"
    CountLabel in_progress;
    CountLabel forming;
    bool contested_shown; // the four second-row labels are visible
    CountLabel arm_wins;
    CountLabel core_wins;
};

struct GalaxyView {
    bool loading; // clear once the allegiance or activity feed arrived
    bool has_data;
    bool
        show_map_while_loading{}; // draws the map instead of the static while loading; only ever cleared
    int32_t static_frame{};       // Map_Static frame shown while loading
    int32_t sector_id{};          // from the war-map header
    char sector_name[kSectorNameCapacity]{};
    int32_t map_width; // static map image size
    int32_t map_height;
    int32_t scale;      // zoom level 0..3
    int32_t view_width; // map units shown
    int32_t view_height;
    int32_t max_scroll_x;
    int32_t max_scroll_y;
    int32_t scroll_step[kGalaxyScaleCount]; // map units per scroller step
    int32_t scroll_x;
    int32_t scroll_y;
    int32_t animation_steps{};                      // steps taken since the map data arrived
    GalaxyStarLayer star_layers[kGalaxyStarLayers]; // nearest first
    GalaxyPlanet planets[kMaxGalaxyPlanets];        // in id order
    std::size_t planet_count;
    GalaxyConnector connectors[kMaxGalaxyConnectors];
    std::size_t connector_count;
    int32_t selected; // planet index, kNoPlanet when none
    int32_t hovered;  // planet under the pointer, kNoPlanet when none
    bool locked;      // map input ignored during a screen transition
    TerritoryPanel territory;
    bool territory_attached; // the territory panel exists
};

/// Resets the view to its defaults: zoom level 0 showing the whole map, loading and locked.
///
/// The scroller steps are 20, 14, 10 and 3 map units at the four zoom levels.
///
/// @param[out] view galaxy view to reset
void galaxy_init(GalaxyView* view);

/// Drops the planets, connectors, star layers, scroll position and selection; the map returns to
/// loading.
///
/// @param[in,out] view galaxy view
void galaxy_clear(GalaxyView* view);

/// Runs one Main Frame cycle of the war-map animation.
///
/// While loading, the static cycles through its frames; once map data is
/// present each planet sprite steps on every frame_interval-th step.
///
/// @param[in,out] view galaxy view
/// @param static_frames frames of the loading static; 0 or less holds it still
void galaxy_step_animation(GalaxyView* view, int32_t static_frames);

/// Maps a ta_mapsource key to a MapSource: TA, BT, CC, WTA, WCC or WBT, compared
/// case-insensitively.
///
/// @param key the planet's ta_mapsource text
/// @return the source; unknown keys give ta_disk
MapSource map_source_from_key(const char* key);

/// Fills a display record from a war-map planet record; the sprite rectangle starts as the anchor
/// point.
///
/// @param[out] planet display record, cleared first
/// @param record war-map planet record
void galaxy_build_planet(GalaxyPlanet* planet, const PlanetRecord* record);

/// Copies the description and sizes the planet's sprite from its frames.
///
/// Keeps the frame count and the variant group's radius, grows the sprite
/// rectangle over every frame placed at the anchor minus its offset, then
/// stores the panel art names.
///
/// @param[in,out] planet display record
/// @param record war-map planet record
/// @param frames the planet's sprite frames
/// @param frame_count number of frames
/// @return false for an unknown variant group
bool galaxy_set_planet_sprite(
    GalaxyPlanet* planet,
    const PlanetRecord* record,
    const SpriteFrame* frames,
    std::size_t frame_count
);

// Sprite frames per planet, in record order.
struct PlanetSprites {
    const SpriteFrame* frames;
    const std::size_t* frame_counts; // per planet
};

/// Builds the war map from its records.
///
/// Checks the map image covers the galaxy, builds the star-field layers
/// seeded from the view's sector id, then each planet (in id order) with its
/// sprite and hotspot, then links the connectors.
///
/// @param[in,out] view galaxy view, cleared first
/// @param map_width static map image width
/// @param map_height static map image height
/// @param planets war-map planet records
/// @param planet_count number of planet records
/// @param connectors planet-to-planet links
/// @param connector_count number of links
/// @param sprites sprite frames per planet, in record order; may be null
/// @return false when the image is smaller than the galaxy, a star layer cannot be built, a planet
///     has an unknown variant group, a planet id repeats or a connector names an unknown planet
bool galaxy_load(
    GalaxyView* view,
    int32_t map_width,
    int32_t map_height,
    const PlanetRecord* planets,
    std::size_t planet_count,
    const ConnectorRecord* connectors,
    std::size_t connector_count,
    const PlanetSprites* sprites
);

/// Finds a planet by id.
///
/// @param view galaxy view
/// @param id planet (area) id
/// @return the planet's index, or kNoPlanet when unknown
int32_t galaxy_find_planet(const GalaxyView* view, int32_t id);

/// Returns the capitol founded by the player's side.
///
/// The founding side is the one the planet was given, which a later allegiance
/// does not replace. A side that is not Arm or Core has no capitol.
///
/// @param view the war map
/// @param allegiance the player's side, 'A', 'C' or 'U', in either case
/// @return the planet's index, or kNoPlanet when that side has no capitol
int32_t galaxy_player_capitol(const GalaxyView* view, char allegiance);

/// Recomputes a planet's visibility and view pixels, creating, moving or dropping its hotspot.
///
/// @param view galaxy view
/// @param[in,out] planet the planet
void galaxy_place_hotspot(GalaxyView* view, GalaxyPlanet* planet);

/// Recomputes a connector's visibility and view pixels.
///
/// A connector is invisible only when both endpoints lie beyond the same
/// edge of the view.
///
/// @param view galaxy view
/// @param[in,out] connector the connector
void galaxy_project_connector(const GalaxyView* view, GalaxyConnector* connector);

/// Colours a connector from its planets' affiliations.
///
/// A side's colour when both ends are that side or one is contested; the
/// mixed colour for Arm-Core and contested-contested links.
///
/// @param view galaxy view
/// @param[in,out] connector the connector
void galaxy_color_connector(const GalaxyView* view, GalaxyConnector* connector);

// Star kinds the layer builder chooses by percent chance; the rest are
// crosses.
inline constexpr std::size_t kStarKindChances = 3;

/// Builds a star-field layer over a map image, drawing from a rand15 generator seeded with `seed`.
///
/// Each cell, row by row, gets a star at a random spot inside it (none when
/// the spot falls off the image): a point, pair or square by
/// `kind_percents`, and a cross otherwise. Every empty cell is zero and draws
/// nothing.
///
/// @param[out] layer layer to build
/// @param width map image width
/// @param height map image height
/// @param seed generator seed
/// @param scroll_share share of the map's scroll range the layer's window travels
/// @param cell_size side of a square cell in map units
/// @param dim grey-ramp steps every star of the layer is dimmed by
/// @param kind_percents percent chances of a point, a pair and a square
/// @return false when the layer would need more than kMaxGalaxyStars cells or a cell is narrower
///     than two units
bool galaxy_build_star_layer(
    GalaxyStarLayer* layer,
    int32_t width,
    int32_t height,
    uint32_t seed,
    float scroll_share,
    int32_t cell_size,
    int32_t dim,
    const int32_t kind_percents[kStarKindChances]
);

/// Places a star, drawing its variant and dim from a rand15 generator state.
///
/// A point dims 0..10, a pair picks one of four orientations and grows one
/// unit across or down, a square picks one of two diagonals and grows one
/// unit both ways, a cross grows two; the shapes dim 0..6. An unknown kind
/// becomes a point dimmed 0..5.
///
/// @param[out] star star to place
/// @param x position in map units
/// @param y position in map units
/// @param kind star shape
/// @param[in,out] holdrand generator state, stepped with rand15_step
void galaxy_place_star(GalaxyStar* star, int32_t x, int32_t y, StarKind kind, uint32_t* holdrand);

/// Recomputes a star's visibility in a layer window and its view pixels.
///
/// A star is visible when its rectangle meets the window; its view pixel
/// scales the offset from the window's corner onto the 498 x 328 viewer.
///
/// @param[in,out] star the star
/// @param left window's left edge in map units
/// @param top window's top edge in map units
/// @param width window width in map units
/// @param height window height in map units
void galaxy_project_star(
    GalaxyStar* star, int32_t left, int32_t top, int32_t width, int32_t height
);

/// Moves a layer's window for the view's scroll position and extent and re-projects its stars.
///
/// The window travels scroll_share of the scroll range (map less view plus
/// one, truncated) and is that much narrower than the map; it sits at the
/// same fraction of its travel as the view does of the range. A layer that
/// travels nothing shows the whole map from its corner.
///
/// @param[in,out] layer star-field layer
/// @param scroll_x view's scroll position in map units
/// @param scroll_y view's scroll position in map units
/// @param view_width map units the view shows across
/// @param view_height map units the view shows down
void galaxy_scale_star_layer(
    GalaxyStarLayer* layer,
    int32_t scroll_x,
    int32_t scroll_y,
    int32_t view_width,
    int32_t view_height
);

/// Scrolls the view, clamped to the map, and re-places the connectors, hotspots and star-layer
/// windows.
///
/// Nothing happens while the map loads or has no data. A relative scroll
/// that does not move re-places nothing.
///
/// @param[in,out] view galaxy view
/// @param dx horizontal distance, in scroller steps or with `absolute` in map units
/// @param dy vertical distance, in scroller steps or with `absolute` in map units
/// @param absolute whether the distances are map units
void galaxy_scroll(GalaxyView* view, int32_t dx, int32_t dy, bool absolute);

/// Changes the zoom level, keeping the selected planet at its view position.
///
/// Without a selection everything is re-placed at the same scroll offset.
///
/// @param[in,out] view galaxy view
/// @param scale zoom level 0..3; other values, the current level, a loading map or no data change
///     nothing
void galaxy_set_scale(GalaxyView* view, int32_t scale);

/// Applies a census to its planet and refreshes the territory panel when the planet is the current
/// area.
///
/// @param[in,out] view galaxy view
/// @param report census of one area
/// @param current_area the session's current area
/// @return false for an unknown planet; otherwise the caller stops the activity light
bool galaxy_apply_census(GalaxyView* view, const CensusReport* report, uint32_t current_area);

/// Applies the allegiance feed, recolours the connectors and marks the map usable.
///
/// @param[in,out] view galaxy view
/// @param allegiances one affiliation character per planet in id order; may be null
void galaxy_apply_allegiances(GalaxyView* view, const char* allegiances);

/// Applies the activity feed and marks the map usable.
///
/// @param[in,out] view galaxy view
/// @param digits one decimal digit per planet in id order; may be null
void galaxy_apply_activity(GalaxyView* view, const char* digits);

/// Refreshes the territory panel labels from its planet.
///
/// Shows the planet's name, owner and battle counts, and the win counts only
/// while the planet is contested. Without a planet every label is hidden.
///
/// @param[in,out] panel territory panel
/// @param view galaxy view holding the planet
void territory_panel_update(TerritoryPanel* panel, const GalaxyView* view);

/// Selects a planet from its hotspot: it becomes the current area, the selection and the territory
/// panel's planet.
///
/// @param[in,out] view galaxy view
/// @param planet planet index
/// @param[out] area_id receives the planet's id as the current area; may be null
/// @return false while the map is locked or for an unknown planet
bool galaxy_select_planet(GalaxyView* view, int32_t planet, uint32_t* area_id);

/// Tracks the planet under the pointer.
///
/// @param[in,out] view galaxy view
/// @param planet planet index, kNoPlanet when the pointer left
/// @return false while the map is locked
bool galaxy_hover_planet(GalaxyView* view, int32_t planet);

/// Whether the war map is drawn instead of the loading picture.
///
/// Map data must be present. While that data is still loading, the war map
/// is drawn only when show_map_while_loading is set; otherwise the loading
/// picture stays up.
///
/// @param view galaxy view
/// @return whether the war map is drawn
bool galaxy_shows_war_map(const GalaxyView* view);

/// Reads gravity, wind limits, tidal strength and lavaworld from the planet map's GlobalHeader
/// once. The reader is told the planet's map source first, when it asked to be.
///
/// @param[in,out] planet the planet; its environment is filled
/// @param reader map TDF reader; may be null
/// @return whether the map is installed
bool galaxy_load_environment(GalaxyPlanet* planet, const MapTdfReader* reader);

// Rows of the planet information panel.
namespace planet_info_row {
inline constexpr std::size_t planet = 0;
inline constexpr std::size_t gw_map = 1;
inline constexpr std::size_t type = 2;
inline constexpr std::size_t gravity = 3;
inline constexpr std::size_t wind_speed = 4;
inline constexpr std::size_t tide_strength = 5;
inline constexpr std::size_t affiliation = 6;
inline constexpr std::size_t population = 7;
inline constexpr std::size_t count = 8;
} // namespace planet_info_row

inline constexpr std::size_t kPlanetInfoRowCount = planet_info_row::count;
inline constexpr std::size_t kPlanetInfoValueCapacity = 0x40;
inline constexpr std::size_t kPlanetInfoLineCount = 6;

// Text rows of the planet information panel.
struct PlanetInfo {
    const char* labels[kPlanetInfoRowCount]; // "Planet:" .. "Population:"
    char values[kPlanetInfoRowCount][kPlanetInfoValueCapacity];
    bool map_installed;
    // Recon pulses until the wind next shifts, drawn before the speed
    int32_t wind_countdown;
    int32_t wind_speed;                       // sampled between the map's wind limits
    const char* notice[kPlanetInfoLineCount]; // Galactic War participation notice
    std::size_t notice_count;
    const char* description; // wrapped by the view
};

// Random source for the panel's samples (the shared rand15 stream).
using RandomSource = int32_t (*)(void* user);

/// Fills the rows of the planet information panel for a planet.
///
/// Fills Planet, GW Map, Type, Gravity, Wind Speed, Tide Strength,
/// Affiliation and Population. The environment rows read "?" when the map is
/// missing, tide reads "n/a" on a lavaworld, and the wind speed is sampled
/// between the map's limits after the wind countdown. A missing map adds the
/// participation notice for the planet's map source.
///
/// @param[out] info panel rows, cleared first
/// @param[in,out] planet the planet; its map TDF is read on first use
/// @param reader map TDF reader; may be null
/// @param random random source (the shared rand15 stream); may be null
/// @param random_user context handed to `random`
void planet_info_fill(
    PlanetInfo* info,
    GalaxyPlanet* planet,
    const MapTdfReader* reader,
    RandomSource random,
    void* random_user
);

// Width of the panorama strip the Recon screen shows.
inline constexpr int32_t kReconPanWidth = 0x1d6;
// Frames of a planet's pan sequence the Recon screen lays side by side (3.1c
// takes every frame of the sequence; the installed pans have four).
inline constexpr std::size_t kMaxReconPanFrames = 0x20;
// Largest step either way the wind takes when its countdown runs out.
inline constexpr int32_t kReconWindJitter = 2;

// Animation state of the Recon screen.
struct ReconState {
    int32_t planet{kNoPlanet}; // index of the selected planet
    bool map_installed{};
    bool has_pan{};                            // the planet's pan sequence is loaded
    int32_t pan_scroll{};                      // left edge of the shown strip
    int32_t pan_end{kReconPanWidth};           // right edge, wrapped into the strip
    int32_t pan_total{};                       // width of every pan frame side by side
    int32_t pan_offsets[kMaxReconPanFrames]{}; // each frame's left edge in the strip
    int32_t pan_frame_count{};
    int32_t wind_countdown{};
    int32_t wind{}; // wind speed shown
    int32_t wind_min{};
    int32_t wind_max{};
    int32_t wind_range{};   // wind_max - wind_min; the wind holds still at 0
    bool wind_dirty{};      // the wind field needs its new value
    int32_t static_frame{}; // Map_Static frame shown without a panorama
};

/// Starts the Recon animation for a planet from its filled information rows.
///
/// Takes the wind limits, countdown and speed of an installed map, and lays
/// the pan frames side by side from x 0 with the strip showing its first
/// kReconPanWidth pixels; without a panorama the static starts at frame 0.
/// An empty pan sequence counts as none.
///
/// @param[out] state Recon animation state, reset first
/// @param planet planet index
/// @param record the planet
/// @param info the planet's filled information rows
/// @param pan_frame_widths widths of the pan frames; null when the sequence is not loaded
/// @param pan_frame_count number of pan frames; at most kMaxReconPanFrames are used
void recon_begin(
    ReconState* state,
    int32_t planet,
    const GalaxyPlanet* record,
    const PlanetInfo* info,
    const int32_t* pan_frame_widths,
    std::size_t pan_frame_count
);

/// Runs one Recon pulse (the caller skips it while the screen is hidden).
///
/// With the map installed and a panorama, the wind shifts by up to
/// kReconWindJitter within its limits whenever its countdown runs out, and
/// the strip scrolls a pixel, wrapping at pan_total; otherwise the static
/// steps through its frames. Without a random source the countdown still
/// runs but the wind holds still.
///
/// @param[in,out] state Recon animation state
/// @param random random source (the shared rand15 stream); may be null
/// @param random_user context handed to `random`
/// @param static_frames frames of the static; 0 or less holds it still
void recon_step(ReconState* state, RandomSource random, void* random_user, int32_t static_frames);

/// Chat role colours of the small hat font, and the dim tint of a muzzled name, with no
/// colour-vision mode. A mode matches its own colours to the palette in use.
namespace chat_tint {
/// Status lines.
inline constexpr int32_t system = 0xe9;
/// News lines.
inline constexpr int32_t newsbot = 0xc7;
/// Arm names.
inline constexpr int32_t arm = 0xe2;
/// Core names.
inline constexpr int32_t core = 0xd4;
/// A muzzled name.
inline constexpr int32_t muzzle = 0x5a;
} // namespace chat_tint

/// Body index of each side's logo in ArmCoreLogos. The greys around the body stay.
namespace side_icon_body {
/// Arm logo body.
inline constexpr int32_t arm = 229;
/// Core logo body.
inline constexpr int32_t core = 214;
} // namespace side_icon_body

/// Records the palette the module is drawing with.
///
/// A null pointer or fewer than 256 colours clears it. With none recorded, a colour-vision mode
/// keeps today's indices.
///
/// @param entries palette entries, 4 bytes each; null clears the recorded palette
/// @param bytes byte length of `entries`
void boneyards_use_palette(const uint8_t* entries, std::size_t bytes);

/// The palette recorded for the module.
///
/// @return 256 entries of 4 bytes, or an empty span when none is recorded
[[nodiscard]] std::span<const uint8_t> boneyards_palette();

/// Font for a muzzled name while a colour-vision mode is on.
///
/// A negative handle keeps the dim hat font. Off ignores the handle and uses the dim tint.
///
/// @param font tinted hat font, or a negative handle
void boneyards_muzzle_font(int32_t font);

/// Palette index a war-map link draws for a stored side colour.
///
/// Off, a short palette, or a colour other than the three side colours leaves `stored` unchanged.
/// Protanopia and deuteranopia share one set of side colours.
///
/// @param stored palette index stored on the connector
/// @param palette palette in use, 4 bytes a colour
/// @return the index to draw
[[nodiscard]] int32_t galaxy_link_index(int32_t stored, std::span<const uint8_t> palette);

/// Chat role colours matched to the palette in use.
struct GalaxyChatColours {
    int32_t system{};  ///< status lines
    int32_t newsbot{}; ///< news lines
    int32_t arm{};     ///< Arm names
    int32_t core{};    ///< Core names
};

/// Chat role colours for the active colour-vision mode.
///
/// Off, or a short palette, is the hat font's own tints.
///
/// @param palette palette in use, 4 bytes a colour
/// @return the four tints
[[nodiscard]] GalaxyChatColours galaxy_chat_colours(std::span<const uint8_t> palette);

/// Palette index of a muzzled name for the active colour-vision mode.
///
/// Off, or a short palette, is the dim tint.
///
/// @param palette palette in use, 4 bytes a colour
/// @return the index
[[nodiscard]] int32_t muzzle_name_index(std::span<const uint8_t> palette);

/// Body colours of the planet summary's two side icons.
struct SideIconColours {
    int32_t arm{};  ///< Arm logo body
    int32_t core{}; ///< Core logo body
};

/// Body colours of the side icons for the active colour-vision mode.
///
/// Off, or a short palette, is the logo's own body index. The greys stay.
///
/// @param palette palette in use, 4 bytes a colour
/// @return the two body indices
[[nodiscard]] SideIconColours side_icon_colours(std::span<const uint8_t> palette);

/// Index table for one side's planet aura.
///
/// Off, or a short palette, is the identity, and so is every index outside that side's shades.
/// The transparency index stays. In protanopia, deuteranopia and tritanopia each shade moves to
/// the shade of the mode's colour nearest in lightness, and the shade nearest that colour becomes
/// it. In achromatopsia the Arm shades stay as they are, and each Core shade moves to the entry of
/// Core's colour nearest its lightness plus 12. The lower index wins a tie, and achromatopsia
/// forces no shade onto the colour itself.
///
/// @param arm true for the Arm aura, false for the Core aura
/// @param palette palette in use, 4 bytes a colour
/// @return source index to drawn index
[[nodiscard]] std::array<uint8_t, 256> galaxy_aura_map(bool arm, std::span<const uint8_t> palette);

/// Index table for a latency picture.
///
/// Off, or a short palette, is the identity. Each grade keeps its shading. Its middle shade
/// becomes the mode's good, middle or bad health colour, and every other shade of the grade,
/// the cross among the bad ones, moves by the same lightness. The lower index wins a tie. The
/// grey outline and the transparency index stay.
///
/// @param palette palette in use, 4 bytes a colour
/// @return source index to drawn index
[[nodiscard]] std::array<uint8_t, 256> lobby_latency_map(std::span<const uint8_t> palette);

} // namespace oa::boneyards::screens

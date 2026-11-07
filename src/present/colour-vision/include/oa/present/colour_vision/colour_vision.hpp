// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Player colours chosen for a colour-vision deficiency, and the semantic
// colours that go with them. A mode remaps the ten team ramps and the
// minimap blips; it never tints the whole picture, and it never changes a
// saved game, a recording or a byte another machine receives. Another
// player's machine uses that player's own preference.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace oa::formats::gaf {
struct Archive;
}

namespace oa::present::colour_vision {

/// How this machine draws player colours. `off` is the game's own set.
enum class Mode : uint8_t {
    off = 0,
    protanopia,
    deuteranopia,
    tritanopia,
    achromatopsia,
};

/// Players a match can colour, and the indices of one palette.
inline constexpr int kPlayerCount = 10;
inline constexpr int kPaletteColours = 256;

/// Preference that selects `Mode`. Any text other than the four mode names reads as `off`.
inline constexpr std::string_view kColourVisionKey = "open-annihilation.access.colour-vision";

/// Preference that asks for a shape with each player colour. A whole number above 0 turns it on.
inline constexpr std::string_view kShapeCuesKey = "open-annihilation.access.shape-cues";

/// One sRGB colour, 0 to 255 a channel.
struct Rgb {
    uint8_t red{};
    uint8_t green{};
    uint8_t blue{};
};

/// A colour in CIELAB (D65).
struct Lab {
    double l{};
    double a{};
    double b{};
};

/// Where one indexed frame's palette indices lie. A null `indices` skips the frame.
struct RawFrame {
    const uint8_t* indices{};
    std::size_t count{};
    int key{-1}; ///< transparency index left unmapped; below 0 maps every index
};

/// Ten frames of one team sequence, frame 0 the first player colour.
struct RawSequence {
    std::array<RawFrame, kPlayerCount> frames{};
};

/// Palette index written in place of each source index. Identity leaves a picture unchanged.
using IndexMap = std::array<uint8_t, kPaletteColours>;

/// One map for each of the ten player colours.
using TeamMaps = std::array<IndexMap, kPlayerCount>;

/// A colour's minimap blip bodies, the unit logo and the weapon logo.
/// A value below 0 is unused. `blip_maps` sends a body to the anchor. The team
/// maps do not. The two logos of a blip can name the same index.
using BlipPins = std::array<std::array<int, 2>, kPlayerCount>;

/// The colours a mode uses for health, the war map, resources, chat and the kit.
/// Protanopia and deuteranopia share one red-green set. `off` is the game's own.
struct SemanticColours {
    Rgb health_good{};
    Rgb health_mid{};
    Rgb health_bad{};
    Rgb health_trough{}; ///< the trough under the health bar
    Rgb war_arm{};
    Rgb war_core{};
    Rgb war_mixed{};
    Rgb war_space{}; ///< empty ground on the war map
    Rgb resource_produced{};
    Rgb resource_consumed{};
    Rgb resource_text{};
    Rgb chat_system{};
    Rgb chat_newsbot{};
    Rgb chat_arm{};
    Rgb chat_core{};
    Rgb chat_plain{};
    Rgb kit_ok{};
    Rgb kit_caution{};
    Rgb kit_danger{};
    Rgb kit_information{};
    Rgb kit_panel{};
};

/// Reads a colour-vision preference. Any text other than the four mode names is `off`.
///
/// @param text stored preference text; empty is `off`
/// @return the mode
[[nodiscard]] Mode parse_mode(std::string_view text) noexcept;

/// The preference text of a mode, including `off`.
///
/// @param mode mode to name
/// @return `off`, `protanopia`, `deuteranopia`, `tritanopia` or `achromatopsia`
[[nodiscard]] std::string_view mode_name(Mode mode) noexcept;

/// Reports whether a shape-cue preference is on.
///
/// A whole number above 0 is on. Any other text, including a word, is off,
/// which is the default.
///
/// @param text stored preference text
/// @return true when the text is a whole number above 0
[[nodiscard]] bool shape_cues_preference(std::string_view text) noexcept;

/// Reports whether shape cues show for a mode and a preference.
///
/// They show when the preference is on, and always for achromatopsia, on the
/// menus and in a match, together with the colour-vision mode.
///
/// @param mode colour-vision mode
/// @param preference_on true when the shape-cue preference is a whole number above 0
/// @return true when shapes are in effect
[[nodiscard]] bool shape_cues_in_effect(Mode mode, bool preference_on) noexcept;

/// The colour-vision mode this machine is drawing with. The default is `off`.
///
/// @return the process-wide mode
[[nodiscard]] Mode active_mode() noexcept;

/// Whether shape cues are in effect for the process-wide mode and preference.
///
/// @return true when shapes show
[[nodiscard]] bool shape_cues_active() noexcept;

/// Sets the process-wide mode and shape-cue preference. The default is off and no shapes.
///
/// Does not by itself repaint a picture. Dot colours for a mode start as the
/// stock palette's anchor indices until `match_player_dots` matches a palette.
///
/// @param mode colour-vision mode
/// @param shape_cues_on true when the shape-cue preference is a whole number above 0
void set_active(Mode mode, bool shape_cues_on) noexcept;

/// sRGB of one player's anchor in a mode. `off` has no anchor and returns black.
///
/// @param mode colour-vision mode
/// @param player player colour, 0 to 9; any other player returns black
/// @return the anchor
[[nodiscard]] Rgb anchor_srgb(Mode mode, int player) noexcept;

/// Anchor index in the stock palette. Achromatopsia repeats four levels across the ten players.
///
/// @param mode colour-vision mode
/// @param player player colour, 0 to 9; any other player returns 0
/// @return the palette index; 0 for `off` or a player out of range
[[nodiscard]] int stock_anchor_index(Mode mode, int player) noexcept;

/// The semantic colours of a mode, including the background each group is drawn on.
///
/// @param mode colour-vision mode; `off` is the game's own set
/// @return the groups
[[nodiscard]] SemanticColours semantic_colours(Mode mode) noexcept;

/// Palette index nearest an sRGB colour: the smallest sum of absolute channel
/// differences, the first index on a tie, byte 3 of each entry ignored.
///
/// @param palette 256 entries of 4 bytes; a shorter span matches nothing and returns 0
/// @param colour colour to match
/// @return the palette index
[[nodiscard]] int match_index(std::span<const uint8_t> palette, Rgb colour) noexcept;

/// Writes a mode's good, mid and bad palette indices into the UI colour slots.
///
/// Slot 10 takes good (health, a legal build site, the selection box). Slot 14
/// takes mid (health between, a site over the player's own units, the radar's
/// marks). Slots 12 and 4 take bad (low health, a refused site, the weapon
/// range's blink). Produced and consumed resource figures are not these slots:
/// `match_resource_figures` keeps their own indices. `off`, a palette shorter
/// than 256 entries, or fewer than 15 slots writes nothing, so the table stays
/// as the remap built it.
///
/// @param slots UI colour table, one palette index a slot
/// @param palette palette in use, 4 bytes an entry
/// @param mode colour-vision mode
void paint_mark_slots(
    std::span<uint8_t> slots, std::span<const uint8_t> palette, Mode mode
) noexcept;

/// Matches the active mode's produced and consumed figures to a palette.
///
/// The indices stay here, not in the game, a save or the simulation. `off`,
/// or a palette shorter than 256 entries, leaves the stored indices. The HUD
/// asks for them only while a mode is on; with the mode off the figures stay
/// on UI colour slots 10 and 12.
///
/// @param palette palette in use, 4 bytes an entry
void match_resource_figures(std::span<const uint8_t> palette) noexcept;

/// Palette index of the produced-resource figures for the active mode.
///
/// Valid after `match_resource_figures` for the mode now active.
///
/// @return the palette index
[[nodiscard]] uint8_t resource_produced_index() noexcept;

/// Palette index of the consumed-resource figures for the active mode.
///
/// Valid after `match_resource_figures` for the mode now active.
///
/// @return the palette index
[[nodiscard]] uint8_t resource_consumed_index() noexcept;

/// Matches the active mode's anchors to a palette and keeps those indices as the player dots.
///
/// `off`, or a palette shorter than 256 entries, leaves the stock indices.
///
/// @param palette palette in use, 4 bytes an entry
void match_player_dots(std::span<const uint8_t> palette) noexcept;

/// Palette index of a player dot for the active mode.
///
/// With `off` this is not used: the game's own dot table stays in place.
/// A colour outside 0 to 9 uses the first player's dot.
///
/// @param colour player colour number
/// @return the palette index
[[nodiscard]] uint8_t player_dot(int colour) noexcept;

/// CIELAB (D65) of an sRGB colour, through the sRGB linearisation.
///
/// @param colour sRGB colour
/// @return L*, a*, b*
[[nodiscard]] Lab lab_from_srgb(Rgb colour) noexcept;

/// The colour as the mode's eye sees it, in CIELAB (D65).
///
/// The three dichromacies use Machado, Oliveira and Fernandes (2009) at
/// severity 1.0 on linear sRGB, each channel then held to 0..1.
/// Achromatopsia is CIE relative luminance Y, then the grey of that Y.
/// `off` is unchanged sRGB.
///
/// @param mode the eye
/// @param colour sRGB colour
/// @return the simulated colour in CIELAB
[[nodiscard]] Lab simulate(Mode mode, Rgb colour) noexcept;

/// The colour as Viénot, Brettel and Mollon (1999) see it, in CIELAB (D65).
///
/// Protanopia and deuteranopia use that paper's matrices on linear sRGB, each
/// channel then held to 0..1. Any other mode is `simulate`.
///
/// @param mode the eye
/// @param colour sRGB colour
/// @return the simulated colour in CIELAB
[[nodiscard]] Lab simulate_vienot(Mode mode, Rgb colour) noexcept;

/// The colour as Brettel, Viénot and Mollon (1997) see tritanopia, in CIELAB.
///
/// The plane is the one the separation normal picks on linear sRGB, and each
/// channel is then held to 0..1. Any mode other than tritanopia is `simulate`.
///
/// @param mode the eye
/// @param colour sRGB colour
/// @return the simulated colour in CIELAB
[[nodiscard]] Lab simulate_brettel(Mode mode, Rgb colour) noexcept;

/// CIEDE2000 distance between two CIELAB colours.
///
/// @param left first colour
/// @param right second colour
/// @return the distance
[[nodiscard]] double ciede2000(Lab left, Lab right) noexcept;

/// Indices of each player colour's team ramp, gathered from ten-frame sequences.
///
/// An index that appears in three or more frames of one sequence is shared
/// metal and is left out of that sequence. Transparency indices are left out.
/// The result is sorted and holds each index once.
///
/// @param sequences team sequences whose frames are original indices
/// @return the ten shade lists
[[nodiscard]] std::array<std::vector<uint8_t>, kPlayerCount>
team_shades(std::span<const RawSequence> sequences);

/// Index maps that centre each army's new ramp on its anchor.
///
/// The new ramp is the run of the anchor's palette row whose neighbouring
/// colours stay near in hue, or are both near grey. Each colour's old dot
/// index (227, 212, 80, 235, 108, 219, 208, 93, 130 and 67) is sent to the
/// anchor. Every other old shade is sent to the ramp entry nearest in L* to
/// that shade's L* plus the step from the old dot's L* to the anchor's, both
/// taken from the palette in use. Equal distance keeps the lower index.
/// `off`, or a short palette, yields identity maps.
///
/// @param mode colour-vision mode
/// @param palette palette in use, 4 bytes an entry
/// @param sequences original team frames the shades are read from
/// @return the ten maps
[[nodiscard]] TeamMaps
team_maps(Mode mode, std::span<const uint8_t> palette, std::span<const RawSequence> sequences);

/// Index maps that send each minimap blip's body to that colour's anchor.
///
/// Every other index, including the outline the frames share, stays as it is.
/// A body below 0 leaves that colour's map as the identity. `off`, or a short
/// palette, yields identity maps.
///
/// @param mode colour-vision mode
/// @param palette palette in use, 4 bytes an entry
/// @param bodies one body index per player colour, or below 0
/// @return the ten maps
[[nodiscard]] TeamMaps
blip_maps(Mode mode, std::span<const uint8_t> palette, const std::array<int, kPlayerCount>& bodies);

/// Writes each index through a map, leaving a frame's transparency index as it is.
///
/// @param indices palette indices rewritten in place
/// @param map source index to destination index
/// @param key transparency index left unchanged; below 0 changes every index
void remap_indices(std::span<uint8_t> indices, const IndexMap& map, int key) noexcept;

/// Byte length of a row-compressed frame, or 0 when the stream does not cover it.
///
/// @param stream bytes from the first row's length word
/// @param width frame width
/// @param height frame height
/// @return the bytes the rows use, or 0 when they run short
[[nodiscard]] std::size_t
row_rle_size(std::span<const uint8_t> stream, int width, int height) noexcept;

/// Decodes a row-compressed frame into width times height indices.
///
/// Pixels a row skips are `key` when that is in 0..255, and 0 otherwise.
///
/// @param stream bytes from the first row's length word
/// @param width frame width
/// @param height frame height
/// @param key index written for a skipped pixel
/// @param[out] indices replaced with the decoded frame on success
/// @return false when the stream does not cover the frame
[[nodiscard]] bool decode_row_rle(
    std::span<const uint8_t> stream, int width, int height, int key, std::vector<uint8_t>& indices
);

/// Remaps the colour bytes of a row-compressed frame, leaving skips and lengths alone.
///
/// @param stream bytes from the first row's length word, rewritten in place
/// @param width frame width
/// @param height frame height
/// @param map source index to destination index
/// @param key transparency index left unchanged; below 0 changes every colour byte
/// @return false when the stream does not cover the frame
[[nodiscard]] bool remap_row_rle(
    std::span<uint8_t> stream, int width, int height, const IndexMap& map, int key
) noexcept;

/// The blip body of each frame: the index farthest from the index that occurs
/// in the most frames. A frame that holds only that index contributes -1.
///
/// @param frames ten decoded frames; a null frame contributes -1
/// @param palette palette the distance is measured in
/// @return a body index per frame, or -1
[[nodiscard]] std::array<int, kPlayerCount>
blip_bodies(const std::array<RawFrame, kPlayerCount>& frames, std::span<const uint8_t> palette);

/// Blip body indices last stored. Unset pins are below 0.
///
/// The team maps do not read these. `blip_maps` sends each body to the anchor.
///
/// @return the pins last stored; all unused until `remember_blip_pins`
[[nodiscard]] const BlipPins& blip_pins() noexcept;

/// Stores the blip body indices of the radar frames in use.
///
/// @param pins pins to keep; values below 0 are unused
void remember_blip_pins(const BlipPins& pins) noexcept;

/// The palette indices the stock maps' terrain draws most often.
///
/// Thirty-two indices. The table in the source records how they were counted.
///
/// @return the indices, in the order of the count
[[nodiscard]] std::span<const int> terrain_indices() noexcept;

/// The team maps last built for the pictures in use. Identity until a paint builds them.
///
/// @return the maps
[[nodiscard]] const TeamMaps& team_map_tables() noexcept;

/// Keeps the maps a paint built, so another picture can be remapped with the same tables.
///
/// @param maps the maps to keep
void remember_team_maps(const TeamMaps& maps) noexcept;

/// Reports whether any frame's original indices are still held, waiting to be restored.
///
/// @return true when a paint has kept originals
[[nodiscard]] bool holds_originals() noexcept;

/// What `prepare_original` did with a frame's bytes.
enum class FramePaint : uint8_t {
    leave,    ///< nothing was written
    restored, ///< the original indices were written back and forgotten
    ready,    ///< the bytes hold the original indices and may be remapped
};

/// Keeps a frame's original indices and puts them back in `pixels`.
///
/// With `paint` false and no original held, nothing is written. With `paint`
/// false and an original held, it is written back and forgotten. With `paint`
/// true, the original is kept and written into `pixels` so a map can be applied
/// to the original rather than to indices already remapped.
///
/// @param role which picture the frame belongs to, such as the menus or the radar
/// @param sequence sequence name
/// @param frame frame index
/// @param pixels the frame's indices, raw or row-compressed
/// @param paint true to prepare for a remap, false to restore only
/// @return what was done
FramePaint prepare_original(
    std::string_view role,
    std::string_view sequence,
    int frame,
    std::span<uint8_t> pixels,
    bool paint
);

/// Remaps every ten-frame sequence of a logo archive, or restores it when the mode is off.
///
/// Original indices are held until the mode is off again. A short palette
/// restores and does not remap. Sequences that are not ten frames are left
/// alone, and so is a frame the archive could not expand.
///
/// @param archive logo archive rewritten in place
/// @param palette palette in use
/// @param role name the originals are kept under, distinct for each archive
void paint_team_archive(
    oa::formats::gaf::Archive& archive, std::span<const uint8_t> palette, std::string_view role
);

} // namespace oa::present::colour_vision

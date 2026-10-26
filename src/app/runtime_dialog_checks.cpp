// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Bounded checks of the stacked frontend dialogs: headless over a frontend
// screen, and over a live match through the SDL presenter.
#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/base/text/line_break.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/languages/unit_texts.hpp"
#include "oa/ui/decoded.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/gui_input.hpp"
#include "oa/ui/gui_layout.hpp"
#include "oa/ui/hud/resource_bar.hpp"
#include "oa/ui/frontend/ingame_menu.hpp"
#include "oa/ui/frontend/options.hpp"
#include "oa/ui/frontend/savegame_dialogs.hpp"
#include "oa/ui/frontend_renderer/gadget_draw.hpp"
#include "oa/ui/frontend_renderer/scroll_bars.hpp"
#include "oa/audio/unit_announcements.hpp"
#include "oa/data/persist/hapibank.hpp"
#include "oa/data/persist/save_sections.hpp"
#include "oa/present/model/mesh_raster.hpp"
#include "oa/present/surface.hpp"
#include "oa/sim/messages.hpp"
#include "oa/sim/speed.hpp"
#include "match_fault.hpp"
#include "panel_first_draw.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace oa::app {

namespace {

using oa::ui::display_layout::Rect;

// Distance from the pointer that covers every cursor frame.
constexpr int32_t kCursorReach = 64;
// Readout steps that close any store gap: an eighth of it each step, then
// one unit at a time.
constexpr int kReadoutSettleSteps = 256;
// The face of a panel whose GUI file names no picture of its own: the common
// GUI art's BackTile (kBackTile), three rows (top, middle, bottom) of three
// frames (left, middle, right).
constexpr std::size_t kBackTileFrames = 9;
constexpr std::size_t kBackTileRowFrames = 3;
constexpr std::size_t kBackTileMiddle = 1;
constexpr std::size_t kBackTileLast = 2;
constexpr std::string_view kExitMenuLayout = "EXITMENU.GUI";
constexpr std::string_view kConfirmLayout = "YESORNO.GUI";
constexpr std::string_view kRestartLayout = "RESTART.GUI";
// PlayerSetupInfo.role bit of the player hosting the game.
constexpr uint8_t kHostRole = 0x01;
// Frames a press on a button is held for before it is let go.
constexpr int kHeldPressFrames = 3;
// A control's face (a switch, a slider, a button) is this bright. The
// turned-over menu and the map under it are darker, so a blank page has none.
constexpr uint8_t kControlChannel = 140;
constexpr std::size_t kOptionsControlPixels = 200;

/// The BackTile frame and the pixel of it a panel's face shows.
struct FacePixel {
    std::size_t frame = 0; ///< row * kBackTileRowFrames + column
    int32_t x = 0;
    int32_t y = 0;
};

/// Returns the tile along one axis of a face at least one tile long, and the
/// offset into it.
///
/// The first tile starts at the near edge and the others follow on from it,
/// except that with `last_flush` the last one ends flush with the far edge,
/// over the tiles before it.
///
/// @param at pixel along the axis, from the near edge
/// @param length face length along the axis, in pixels
/// @param tile tile length, in pixels
/// @param last_flush true when the last tile ends flush with the far edge
/// @return the tile (0 first, kBackTileMiddle, kBackTileLast) and the offset into it
std::pair<std::size_t, int32_t>
face_tile(int32_t at, int32_t length, int32_t tile, bool last_flush) {
    if (last_flush && at >= length - tile)
        return {kBackTileLast, at - (length - tile)};
    if (at < tile)
        return {0, at};
    return {kBackTileMiddle, at % tile};
}

/// Returns the BackTile pixel a face shows at a point.
///
/// The right column ends flush with the right edge, and the bottom row with
/// the bottom edge unless the face is a whole number of tiles high, when its
/// last row is a middle one.
///
/// @param x face column
/// @param y face row
/// @param width face width, at least one tile
/// @param height face height, at least one tile
/// @param tile side of the square BackTile frames, in pixels
/// @return the frame and its pixel
FacePixel back_tile_pixel(int32_t x, int32_t y, int32_t width, int32_t height, int32_t tile) {
    const auto [column, tile_x] = face_tile(x, width, tile, true);
    const auto [row, tile_y] = face_tile(y, height, tile, height % tile != 0);
    return {row * kBackTileRowFrames + column, tile_x, tile_y};
}

bool inside(const Rect& rect, int32_t x, int32_t y) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.width && y < rect.y + rect.height;
}

uint8_t* pixel(renderer::Surface& frame, int32_t x, int32_t y) {
    return &frame.rgb
                [(static_cast<std::size_t>(y) * frame.width + static_cast<std::size_t>(x)) * 3U];
}

bool same_pixel(renderer::Surface& a, renderer::Surface& b, int32_t x, int32_t y) {
    return std::equal(pixel(a, x, y), pixel(a, x, y) + 3, pixel(b, x, y));
}

// Pixels of `rect` bright enough to be a control's face.
std::size_t bright_control_pixels(const renderer::Surface& frame, const Rect& rect) {
    std::size_t count = 0;
    if (frame.rgb.empty() || frame.width == 0)
        return 0;
    const auto width = static_cast<int32_t>(frame.width);
    const auto height = static_cast<int32_t>(frame.height);
    for (int32_t y = rect.y; y < rect.y + rect.height; ++y) {
        if (y < 0 || y >= height)
            continue;
        for (int32_t x = rect.x; x < rect.x + rect.width; ++x) {
            if (x < 0 || x >= width)
                continue;
            const auto* sample =
                frame.rgb.data() +
                (static_cast<std::size_t>(y) * frame.width + static_cast<std::size_t>(x)) * 3U;
            if (std::max({sample[0], sample[1], sample[2]}) >= kControlChannel)
                ++count;
        }
    }
    return count;
}

// Pixels of `rect` that differ between the frames.
std::size_t differing_pixels(renderer::Surface& a, renderer::Surface& b, const Rect& rect) {
    std::size_t count = 0;
    for (int32_t y = rect.y; y < rect.y + rect.height; ++y)
        for (int32_t x = rect.x; x < rect.x + rect.width; ++x)
            if (!same_pixel(a, b, x, y))
                ++count;
    return count;
}

// Pixels outside every excluded rectangle that differ between the frames.
std::size_t differing_pixels_outside(
    renderer::Surface& a, renderer::Surface& b, std::initializer_list<Rect> excluded
) {
    std::size_t count = 0;
    for (int32_t y = 0; y < static_cast<int32_t>(a.height); ++y)
        for (int32_t x = 0; x < static_cast<int32_t>(a.width); ++x)
            if (std::none_of(
                    excluded.begin(),
                    excluded.end(),
                    [x, y](const Rect& rect) { return inside(rect, x, y); }
                ) &&
                !same_pixel(a, b, x, y))
                ++count;
    return count;
}

uint64_t brightness(renderer::Surface& frame, const Rect& rect) {
    uint64_t sum = 0;
    for (int32_t y = rect.y; y < rect.y + rect.height; ++y)
        for (int32_t x = rect.x; x < rect.x + rect.width; ++x) {
            const auto* rgb = pixel(frame, x, y);
            sum += static_cast<uint64_t>(rgb[0]) + rgb[1] + rgb[2];
        }
    return sum;
}

// Brightness of the pixels of `rect` outside `excluded`.
uint64_t brightness_outside(renderer::Surface& frame, const Rect& rect, const Rect& excluded) {
    uint64_t sum = 0;
    for (int32_t y = rect.y; y < rect.y + rect.height; ++y)
        for (int32_t x = rect.x; x < rect.x + rect.width; ++x)
            if (!inside(excluded, x, y)) {
                const auto* rgb = pixel(frame, x, y);
                sum += static_cast<uint64_t>(rgb[0]) + rgb[1] + rgb[2];
            }
    return sum;
}

Rect cursor_reach(float x, float y) {
    return {
        static_cast<int32_t>(x) - kCursorReach,
        static_cast<int32_t>(y) - kCursorReach,
        2 * kCursorReach,
        2 * kCursorReach
    };
}

// The palette's 32-row shade and light tables, 256 entries a row.
constexpr std::string_view kShadeTable = "palettes/palette.shd";
constexpr std::string_view kLightTable = "palettes/palette.lht";
constexpr std::size_t kTableRows = 32;
constexpr std::size_t kTableRowEntries = 256;
// The shade table row a panel opened with shade_below darkens the panel
// under it through (level -0x18); a pixel of entry 0x80 or above reads the
// row before.
constexpr std::size_t kShadeBelowRow = 8;
constexpr uint8_t kFirstRowBeforeEntry = 0x80;
// The focus marker's outline: six rings round the focused record, the first
// a pixel outside it, lit through these light table rows, innermost first;
// a ring's corners are lit twice.
constexpr std::array<std::size_t, 6> kFocusRingLevels{31, 28, 24, 19, 13, 6};
// The GUI palette entry a caption's quick key is underlined in.
constexpr std::size_t kUnderlineEntry = 2;
// The first byte of a caption past ASCII.
constexpr unsigned char kFirstByteAboveAscii = 0x80;

/// Returns the lowest palette entry holding a colour.
///
/// @param palette palette searched, 4 bytes an entry
/// @param rgb the colour
/// @return the entry, or nothing when no entry holds it
std::optional<std::size_t> palette_entry(const oa::PaletteBytes& palette, const uint8_t* rgb) {
    for (std::size_t entry = 0; entry < oa::palette_color_count; ++entry)
        if (std::equal(rgb, rgb + 3, &palette[entry * oa::palette_entry_bytes]))
            return entry;
    return std::nullopt;
}

/// Returns a colour darkened as the panel under a shade_below panel is.
///
/// @param palette the frame's palette
/// @param shade the shade table
/// @param rgb the colour
/// @return the darkened colour, or nothing for a colour off the palette
std::optional<std::array<uint8_t, 3>> darkened_colour(
    const oa::PaletteBytes& palette, const std::vector<uint8_t>& shade, const uint8_t* rgb
) {
    const auto entry = palette_entry(palette, rgb);
    if (!entry)
        return std::nullopt;
    const auto row = *entry >= kFirstRowBeforeEntry ? kShadeBelowRow - 1 : kShadeBelowRow;
    const auto* colour = &palette[shade[row * kTableRowEntries + *entry] * oa::palette_entry_bytes];
    return std::array<uint8_t, 3>{colour[0], colour[1], colour[2]};
}

/// Counts the pixels of a rectangle of `shown` that are not those of `before`,
/// darkened as the panel under a shade_below panel is when `darkened` says so.
///
/// @param shown frame with the panel over it
/// @param before frame before the panel opened
/// @param rect rectangle compared
/// @param darkened true when the rectangle is darkened
/// @param palette the frames' palette
/// @param shade the shade table
/// @return the pixels that differ
std::size_t differing_under(
    renderer::Surface& shown,
    renderer::Surface& before,
    const Rect& rect,
    bool darkened,
    const oa::PaletteBytes& palette,
    const std::vector<uint8_t>& shade
) {
    std::size_t count = 0;
    for (int32_t y = rect.y; y < rect.y + rect.height; ++y)
        for (int32_t x = rect.x; x < rect.x + rect.width; ++x) {
            if (!darkened) {
                count += same_pixel(shown, before, x, y) ? 0 : 1;
                continue;
            }
            const auto expected = darkened_colour(palette, shade, pixel(before, x, y));
            count += expected && std::equal(expected->begin(), expected->end(), pixel(shown, x, y))
                         ? 0
                         : 1;
        }
    return count;
}

/// Counts the pixels of an RGB image that do not show the focus marker's
/// outline round a record over the image drawn without it.
///
/// Every ring pixel inside `root` is read as its palette entry (the nearest
/// one for a colour off the palette) and lit through its ring's light table
/// row, twice at a corner; every other pixel is the same in both images.
///
/// @param lit the image drawn with the focus
/// @param unlit the image drawn without it
/// @param record the focused record, in the images' pixels
/// @param root the panel's root, which clips the rings
/// @param palette the images' palette
/// @param light the light table
/// @param[out] ring_pixels the ring pixels inside the root
/// @return the pixels that differ from the expected image
std::size_t differing_focus(
    renderer::Surface& lit,
    renderer::Surface& unlit,
    const Rect& record,
    const Rect& root,
    const oa::PaletteBytes& palette,
    const std::vector<uint8_t>& light,
    std::size_t& ring_pixels
) {
    ring_pixels = 0;
    std::size_t count = 0;
    for (int32_t y = 0; y < static_cast<int32_t>(lit.height); ++y)
        for (int32_t x = 0; x < static_cast<int32_t>(lit.width); ++x) {
            std::array<uint8_t, 3> expected{};
            std::copy_n(pixel(unlit, x, y), 3, expected.begin());
            for (std::size_t ring = 0; ring < kFocusRingLevels.size() && inside(root, x, y);
                 ++ring) {
                const auto out = static_cast<int32_t>(ring) + 1;
                const int32_t left = record.x - out;
                const int32_t top = record.y - out;
                const int32_t right = record.x + record.width - 1 + out;
                const int32_t bottom = record.y + record.height - 1 + out;
                if (x < left || x > right || y < top || y > bottom)
                    continue;
                const int edges =
                    (y == top || y == bottom ? 1 : 0) + (x == left || x == right ? 1 : 0);
                if (edges == 0)
                    continue;
                ++ring_pixels;
                auto entry = renderer::read_indices(unlit, palette, x, y, 1, 1).pixels.front();
                for (int time = 0; time < edges; ++time)
                    entry = light[kFocusRingLevels[ring] * kTableRowEntries + entry];
                std::copy_n(&palette[entry * oa::palette_entry_bytes], 3, expected.begin());
                break;
            }
            if (!std::equal(expected.begin(), expected.end(), pixel(lit, x, y)))
                ++count;
        }
    return count;
}

// Windows the placed dialogs are checked on besides 640x480 and the default
// one, where the side column's scale is a fraction (1.25 and 1.6).
constexpr int kSmallWindowWidth = 800;
constexpr int kSmallWindowHeight = 600;
constexpr int kMediumWindowWidth = 1024;
constexpr int kMediumWindowHeight = 768;
// A 4:3 window at twice 640x480, where the side column is drawn at twice its
// size.
constexpr int kLargeWindowWidth = 1280;
constexpr int kLargeWindowHeight = 960;
// The name the save dialog's name field holds while the check presses it.
constexpr const char* kCheckPressedName = "LSPRESS";
constexpr const char* kLoadBitmap = "bitmaps/dloadgame2.pcx";
constexpr const char* kSaveBitmap = "bitmaps/dsavegame2.pcx";
constexpr const char* kCheckSaveName = "LSCHECK1";
constexpr const char* kCheckSecondSaveName = "LSCHECK2";
// A save written as the game writes one, its Summary holding a radar image
// of kCheckRadarWidth by kCheckRadarHeight and the Core side.
constexpr const char* kCheckRadarSaveFile = "LSRADAR.SAV";
constexpr const char* kCheckRadarSaveDescription = "Radar picture check";
constexpr int32_t kCheckRadarWidth = 94;
constexpr int32_t kCheckRadarHeight = 70;
constexpr int32_t kCheckRadarSide = 1;
constexpr const char* kBriefingGui = "briefing.gui";
constexpr const char* kBriefingBitmap = "bitmaps/igmbrief.pcx";
// The ARM campaign's third mission, whose briefing runs to a second page.
constexpr std::size_t kBriefingCheckMission = 2;
// The preferences a match opens, in the in-game menu's place.
constexpr std::string_view kPreferencesLayout = "PREFS.GUI";
constexpr std::string_view kInGameMenuLayout = "ARMOPT.GUI";
// Frames the OPTIONS lightbar check draws: the sweep's thirteen steps and
// three held frames.
constexpr int kSweepFrames = 16;
// The pointer's picture ENDMSN's MAIN MENU selects in a match without a
// return label.
constexpr uint8_t kLeavingPanelCursor = 0x14;
// Ticks a skirmish swept of its opponents is given to end in victory.
constexpr uint32_t kVictoryTicks = 600;

/// Returns the side name the load and save dialogs show for a saved side index.
///
/// @param side the Summary's "Side"
/// @return "Arm" for 0, "Core" for 1, else an empty name
std::string_view saved_side_name(int32_t side) {
    return side == 0 ? "Arm" : side == 1 ? "Core" : "";
}

/// Returns the radar picture of the check save: palette indices in diagonal bands.
///
/// @return the picture
oa::present::SurfaceBuffer check_radar_picture() {
    auto picture = oa::present::create_surface(kCheckRadarWidth, kCheckRadarHeight);
    for (int32_t y = 0; y < kCheckRadarHeight; ++y)
        for (int32_t x = 0; x < kCheckRadarWidth; ++x)
            picture.pixels[static_cast<std::size_t>(y * kCheckRadarWidth + x)] =
                static_cast<uint8_t>(16 + (x / 3 + y / 2) % 200);
    return picture;
}

/// Writes the check save into the saves folder: a Summary account of a
/// two-player skirmish on the Core side with `picture` in its "Radar Image"
/// blob.
///
/// @param saves the folder the saved games are written to (Runtime::saves_folder)
/// @param picture the radar image
void write_radar_check_save(const fs::path& saves, const oa::present::SurfaceBuffer& picture) {
    namespace persist = oa::data::persist;
    namespace save_key = persist::save_key;
    std::error_code error;
    fs::create_directories(saves, error);
    persist::Bank bank{};
    persist::bank_init(&bank);
    persist::bank_reset(&bank);
    persist::bank_open_account(&bank, save_key::summary);
    persist::bank_set_text(&bank, save_key::description, kCheckRadarSaveDescription);
    persist::bank_set_int(&bank, save_key::game_type, persist::game_type_skirmish);
    persist::bank_set_int(&bank, save_key::players, 2);
    persist::bank_set_text(&bank, save_key::map, "Radar Check");
    persist::bank_set_text(&bank, save_key::mission, "Radar Check");
    persist::bank_set_int(&bank, save_key::side, kCheckRadarSide);
    persist::bank_set_int(&bank, save_key::difficulty, 1);
    persist::bank_set_int(&bank, save_key::game_time, 30 * 65);
    persist::bank_open_blob_name(&bank, save_key::radar_image);
    const persist::ImageRows rows{
        static_cast<uint32_t>(picture.surface.width),
        static_cast<uint32_t>(picture.surface.height),
        static_cast<uint32_t>(picture.surface.pitch),
        picture.pixels.data()
    };
    persist::save_write_image_rows(&rows, &bank);
    const auto sink = persist::stdio_file_sink();
    const auto path = path_to_utf8(saves / kCheckRadarSaveFile);
    const bool written = persist::bank_write_file(
        &bank, path.c_str(), persist::savegame_description, true, false, &sink
    );
    persist::bank_destroy(&bank);
    if (!written)
        throw std::runtime_error("load/save check: cannot write " + path);
}

/// Counts the pixels of a RADAR record that do not show `picture` as a hot
/// surface's image is drawn.
///
/// The picture is stretched by a quad over the record's corners that samples
/// it from one texel inside its edges; the record's last row and column are
/// not compared, and neither are pixels the cursor may cover.
///
/// @param frame frame the dialog is drawn on
/// @param radar the RADAR record on the frame
/// @param picture the radar image, as palette indices
/// @param palette palette the frame's colours come from
/// @param cursor area the software cursor may cover
/// @return the number of pixels that differ
std::size_t radar_mismatches(
    renderer::Surface& frame,
    const Rect& radar,
    const oa::present::SurfaceBuffer& picture,
    const oa::PaletteBytes& palette,
    const Rect& cursor
) {
    const int32_t right = radar.width - 1;
    const int32_t bottom = radar.height - 1;
    auto stretched = oa::present::create_surface(radar.width, radar.height);
    oa::Sprite texture{};
    texture.width = static_cast<uint16_t>(picture.surface.width);
    texture.height = static_cast<uint16_t>(picture.surface.height);
    texture.encoding = OA_SPRITE_RAW;
    texture.data = const_cast<uint8_t*>(picture.pixels.data());
    const oa::present::PolygonVertex quad[4] = {{0, 0}, {right, 0}, {right, bottom}, {0, bottom}};
    const int32_t u = texture.width - 1;
    const int32_t v = texture.height - 1;
    const oa::present::model::TexturePoint uv[4] = {{1, 1}, {u, 1}, {u, v}, {1, v}};
    oa::present::model::texture_quad(&stretched.surface, &texture, quad, uv);
    std::size_t differing = 0;
    for (int32_t y = 0; y < bottom; ++y)
        for (int32_t x = 0; x < right; ++x) {
            if (inside(cursor, radar.x + x, radar.y + y))
                continue;
            const auto index = stretched.pixels[static_cast<std::size_t>(y * radar.width + x)];
            const auto* shown = pixel(frame, radar.x + x, radar.y + y);
            if (!std::equal(
                    shown,
                    shown + 3,
                    &palette[static_cast<std::size_t>(index) * oa::palette_entry_bytes]
                ))
                ++differing;
        }
    return differing;
}

/// Leaves no saved game for the dialogs to list while it lives: the saves
/// folder, and the earlier ones the dialogs list too, are moved aside, and
/// put back when it goes.
class HeldSaves {
  public:

    /// Moves the folders the dialogs list aside, each that is there.
    ///
    /// @param roots where the saved games lie (Runtime::save_roots)
    explicit HeldSaves(const oa::ui::frontend::SaveRoots& roots) {
        hold(roots.saves);
        for (const auto& earlier : roots.earlier)
            hold(earlier);
    }

    HeldSaves(const HeldSaves&) = delete;
    HeldSaves& operator=(const HeldSaves&) = delete;

    /// Puts the saves back, in place of any folder written meanwhile.
    ~HeldSaves() {
        for (auto folder = held_.rbegin(); folder != held_.rend(); ++folder) {
            std::error_code error;
            fs::remove_all(folder->first, error);
            fs::rename(folder->second, folder->first, error);
        }
    }

  private:

    /// Moves a folder aside, beside itself, if it is there.
    ///
    /// @param folder the folder
    void hold(const fs::path& folder) {
        const fs::path aside = folder.parent_path() / (path_to_utf8(folder.filename()) + ".held");
        std::error_code error;
        fs::remove_all(aside, error);
        if (!fs::exists(folder, error))
            return;
        fs::rename(folder, aside, error);
        if (error)
            throw std::runtime_error("cannot move the saves aside: " + error.message());
        held_.emplace_back(folder, aside);
    }

    /// Each folder moved aside, and where it went.
    std::vector<std::pair<fs::path, fs::path>> held_;
};

/// Returns the label the return label probe answers (Extension::return_label).
///
/// @return the label, kept for the whole run
const char*& probe_return_label() {
    static const char* label = nullptr;
    return label;
}

/// Checks a panel drawn over `frame` with a bitmap as its backdrop.
///
/// The root is at `expected`; each panel pixel outside the records and the
/// cursor is the bitmap's index in `palette`, and every panel pixel outside
/// the cursor a colour of that palette.
///
/// @param frame frame the panel is drawn on
/// @param layout the panel's records, the root placed on the frame
/// @param bitmap the backdrop, whose top-left corner is the panel's
/// @param palette palette the frame's colours come from
/// @param expected where the root belongs on the frame
/// @param cursor area the software cursor may cover
/// @param what the check and the panel, which start each failure message
/// @return the panel's rectangle on the frame
Rect check_backdrop_panel(
    renderer::Surface& frame,
    const oa::ui::gui_layout::Layout& layout,
    const oa::Image& bitmap,
    const oa::PaletteBytes& palette,
    oa::ui::display_layout::Point expected,
    const Rect& cursor,
    const std::string& what
) {
    const auto& root = layout.gadgets.front().common;
    if (root.x != expected.x || root.y != expected.y)
        throw std::runtime_error(
            what + " panel is at " + std::to_string(root.x) + ',' + std::to_string(root.y) +
            ", not " + std::to_string(expected.x) + ',' + std::to_string(expected.y)
        );
    const Rect panel{root.x, root.y, root.width, root.height};
    if (panel.x < 0 || panel.y < 0 || panel.x + panel.width > static_cast<int32_t>(frame.width) ||
        panel.y + panel.height > static_cast<int32_t>(frame.height) ||
        panel.width > static_cast<int32_t>(bitmap.width) ||
        panel.height > static_cast<int32_t>(bitmap.height) ||
        bitmap.indices.size() != static_cast<std::size_t>(bitmap.width) * bitmap.height)
        throw std::runtime_error(what + " panel does not fit the frame or its bitmap");
    std::vector<Rect> records;
    for (std::size_t index = 1; index < layout.gadgets.size(); ++index)
        if (const auto rect = oa::ui::gui_input::gadget_geometry(layout.gadgets, index))
            records.push_back(
                {panel.x + rect->left,
                 panel.y + rect->top,
                 rect->right - rect->left + 1,
                 rect->bottom - rect->top + 1}
            );
    std::vector<bool> colours(1U << 24);
    for (std::size_t entry = 0; entry < oa::palette_color_count; ++entry) {
        const auto* rgb = &palette[entry * oa::palette_entry_bytes];
        colours
            [static_cast<std::size_t>(rgb[0]) << 16 | static_cast<std::size_t>(rgb[1]) << 8 |
             rgb[2]] = true;
    }
    std::size_t compared = 0;
    std::size_t differing = 0;
    std::size_t foreign = 0;
    for (int32_t y = panel.y; y < panel.y + panel.height; ++y)
        for (int32_t x = panel.x; x < panel.x + panel.width; ++x) {
            if (inside(cursor, x, y))
                continue;
            const auto* shown = pixel(frame, x, y);
            if (!colours
                    [static_cast<std::size_t>(shown[0]) << 16 |
                     static_cast<std::size_t>(shown[1]) << 8 | shown[2]])
                ++foreign;
            if (std::any_of(records.begin(), records.end(), [x, y](const Rect& rect) {
                    return inside(rect, x, y);
                }))
                continue;
            const auto index = bitmap.indices
                                   [static_cast<std::size_t>(y - panel.y) * bitmap.width +
                                    static_cast<std::size_t>(x - panel.x)];
            ++compared;
            if (!std::equal(
                    shown,
                    shown + 3,
                    &palette[static_cast<std::size_t>(index) * oa::palette_entry_bytes]
                ))
                ++differing;
        }
    const auto area =
        static_cast<std::size_t>(panel.width) * static_cast<std::size_t>(panel.height);
    if (compared < area / 2 || differing != 0)
        throw std::runtime_error(
            what + " backdrop differs from its bitmap in the palette below at " +
            std::to_string(differing) + " of " + std::to_string(compared) + " pixels"
        );
    if (foreign != 0)
        throw std::runtime_error(
            what + " panel shows " + std::to_string(foreign) + " pixels outside the palette below"
        );
    return panel;
}

} // namespace

void Runtime::check_dialogs(const fs::path& report_directory) {
    namespace dialogs = oa::ui::frontend_dialogs;
    load(Screen::single_player);
    check_frontend_keyboard();
    show_frontend_message(
        entry::message_text(entry::Message::multiplayer_disc),
        entry::disc_message_width,
        entry::message_show_ok,
        entry::message_fit_width
    );
    if (dialogs::dialog_kind() != dialogs::DialogKind::message_box)
        throw std::runtime_error("navigation check did not open MSGBOX.GUI");
    if (frontend_has_keyboard())
        throw std::runtime_error("Single Player kept the keyboard under MSGBOX.GUI");
    rebuild_surface();
    write_ppm(report_directory / "native-msgbox.ppm", surface_);
    auto context = screen_context();
    if (!dialogs::dialog_click(&context, "OK") ||
        dialogs::dialog_kind() != dialogs::DialogKind::none)
        throw std::runtime_error("MSGBOX.GUI OK did not release the message box");
    show_cd_check();
    if (dialogs::dialog_kind() != dialogs::DialogKind::cd_check)
        throw std::runtime_error("navigation check did not open CDCHECK.GUI");
    rebuild_surface();
    write_ppm(report_directory / "native-cdcheck.ppm", surface_);
    dialogs::close_dialog();
    open_help();
    const auto* help = dialogs::dialog_resources();
    if (dialogs::dialog_kind() != dialogs::DialogKind::help || help == nullptr ||
        help->layout.gadgets.size() != 4 + 17 * 2)
        throw std::runtime_error("navigation check did not fill HELP.GUI page 1");
    rebuild_surface();
    write_ppm(report_directory / "native-help.ppm", surface_);
    if (!dialogs::dialog_click(&context, "Page") || dialogs::help_page() != 1)
        throw std::runtime_error("HELP.GUI Page did not advance to page 2");
    rebuild_surface();
    write_ppm(report_directory / "native-help-page2.ppm", surface_);
    if (!dialogs::dialog_click(&context, "OK") ||
        dialogs::dialog_kind() != dialogs::DialogKind::none)
        throw std::runtime_error("HELP.GUI OK did not release the help panel");
    std::cout << "dialog check: MSGBOX.GUI, CDCHECK.GUI and HELP.GUI pages\n";
}

void Runtime::check_frontend_keyboard() {
    const auto require = [](bool ok, const std::string& what) {
        if (!ok)
            throw std::runtime_error("frontend keyboard check: " + what);
    };
    const auto light = assets_.read(kLightTable).bytes;
    const auto& gadgets = resources_.layout.gadgets;
    require(!gadgets.empty(), "Single Player has no panel");
    const auto record_index = [&](std::string_view name) {
        for (std::size_t index = 1; index < gadgets.size(); ++index)
            if (gadgets[index].common.name == name)
                return static_cast<int32_t>(index);
        throw std::runtime_error("frontend keyboard check: no control " + std::string(name));
    };
    const auto& palette =
        resources_.background.palette ? *resources_.background.palette : resources_.gui_palette;
    // The focus marker's outline rings the focused record inside the panel's
    // root, over the screen drawn with no record focused.
    const auto check_focus = [&](const std::string& what, std::string_view name) {
        const auto record = record_index(name);
        require(
            frontend_has_keyboard() && frontend_focus() == record,
            what + " does not give " + std::string(name) + " the focus"
        );
        const auto focus = frontend_focus_;
        frontend_focus_ = -1;
        rebuild_surface();
        auto unlit = surface_;
        frontend_focus_ = focus;
        rebuild_surface();
        const auto& common = gadgets[static_cast<std::size_t>(record)].common;
        const auto& root = gadgets.front().common;
        std::size_t ring_pixels = 0;
        const auto differing = differing_focus(
            surface_,
            unlit,
            {common.x, common.y, common.width, common.height},
            {root.x, root.y, root.width, root.height},
            palette,
            light,
            ring_pixels
        );
        require(
            ring_pixels > 0 && differing == 0,
            what + " rings " + std::string(name) + " wrongly at " + std::to_string(differing) +
                " pixels"
        );
    };
    // Each button underlines its quick key's glyph in its centred caption,
    // in the GUI palette's entry 2, on the row under the text. The caption
    // starts (width - 1 - text width) / 2 + 1 across and
    // (height - 1 - text height) / 2 down the button.
    const auto check_quick_key = [&](std::string_view name, char key) {
        const auto& gadget = gadgets[static_cast<std::size_t>(record_index(name))];
        const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
        require(
            button != nullptr && button->quick_key == key,
            std::string(name) + " does not have the quick key " + key
        );
        const std::string_view text = button->text;
        const auto at = text.find(key);
        require(at != std::string_view::npos, std::string(name) + "'s caption lacks its quick key");
        const auto& font = resources_.font;
        const auto remap = oa::remap_palette(resources_.gui_palette, palette);
        const auto* colour = &palette[remap[kUnderlineEntry] * oa::palette_entry_bytes];
        const auto text_width = static_cast<int32_t>(oa::formats::fnt::measure_text(font, text));
        const auto text_height = static_cast<int32_t>(oa::formats::fnt::line_height(font));
        const auto& record = gadget.common;
        const int32_t left =
            record.x + (record.width - 1 - text_width) / 2 + 1 +
            static_cast<int32_t>(oa::formats::fnt::measure_text(font, text.substr(0, at)));
        const int32_t right =
            left + static_cast<int32_t>(oa::formats::fnt::measure_text(font, text.substr(at, 1))) -
            1;
        const int32_t row = record.y + (record.height - 1 - text_height) / 2 + text_height - 1;
        rebuild_surface();
        std::size_t wrong = 0;
        for (int32_t x = left; x <= right; ++x)
            if (!std::equal(colour, colour + 3, pixel(surface_, x, row)))
                ++wrong;
        require(
            right >= left && wrong == 0,
            std::string(name) + " does not underline its quick key " + key
        );
    };
    bool running = true;
    // A keyboard sends a key's place with its code; the key bindings read it.
    const auto key = [&](SDL_Keycode code, SDL_Keymod modifiers) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = code;
        event.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
        event.key.mod = modifiers;
        event.key.down = true;
        handle_sdl_event(event, running);
    };
    // Single Player has the GUI keyboard: the loader focuses NewCamp, the
    // root's default focus, and Tab and Shift+Tab move the focus on and back.
    check_focus("Single Player", "NewCamp");
    check_quick_key("NewCamp", 'N');
    check_quick_key("Skirmish", 'S');
    key(SDLK_TAB, SDL_KMOD_NONE);
    require(
        frontend_focus() != record_index("NewCamp") && frontend_focus() > 0,
        "Tab did not move the focus"
    );
    check_focus("Tab", gadgets[static_cast<std::size_t>(frontend_focus())].common.name);
    key(SDLK_TAB, SDL_KMOD_LSHIFT);
    check_focus("Shift+Tab", "NewCamp");
    // Return and Space press the focused button as a click released on it
    // does (a gamepad's A and Menu send Return), through the event dispatch
    // a gamepad's keys take, and a held key's repeat presses nothing: NewCamp
    // leaves Single Player, or says over it why it cannot, and Single Player
    // then comes back as it was. With no record focused Return rings the
    // first and presses nothing.
    namespace dialogs = oa::ui::frontend_dialogs;
    const auto entry_state = state_;
    const auto press = [&](SDL_Keycode code, bool repeat = false) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
        event.key.key = code;
        event.key.down = true;
        event.key.repeat = repeat;
        dispatch_event(event, running);
    };
    const auto on_single_player = [&] {
        return screen_ == Screen::single_player && dialogs::dialog_count() == 0;
    };
    const auto pressed_new_camp = [&](const std::string& what) {
        const bool told = dialogs::dialog_count() != 0;
        require(screen_ != Screen::single_player || told, what + " did not press NewCamp");
        if (told) {
            auto context = screen_context();
            require(dialogs::dialog_click(&context, "OK"), "NewCamp's notice has no OK");
        }
        state_ = entry_state;
        load(Screen::single_player);
        require(on_single_player(), "Single Player did not come back after " + what);
    };
    frontend_focus_ = -1;
    rebuild_surface();
    press(SDLK_RETURN);
    require(
        on_single_player() && frontend_focus() > 0,
        "Return with no record focused did not focus one"
    );
    check_focus("Return", gadgets[static_cast<std::size_t>(frontend_focus())].common.name);
    frontend_focus_ = record_index("NewCamp");
    press(SDLK_RETURN, true);
    require(on_single_player(), "a held Return's repeat pressed NewCamp");
    press(SDLK_RETURN);
    pressed_new_camp("Return");
    check_focus("Single Player again", "NewCamp");
    press(SDLK_SPACE);
    pressed_new_camp("Space");
    std::cout << "frontend keyboard check: Single Player rings its focus, which Tab moves and "
                 "Return gives, underlines its quick keys, and Return and Space press NewCamp\n";
}

void Runtime::check_match_dialogs() {
    namespace dialogs = oa::ui::frontend_dialogs;
    // A text the game sets, as the language shown gives it.
    const auto game_text = [this](const char* english) {
        const char* translated = game_translation(english);
        return std::string(translated != nullptr ? translated : english);
    };
    const fs::path report_directory = "local/reports";
    fs::create_directories(report_directory);
    start_benchmark_skirmish();
    show_match_pause_menu();
    // The HUD metrics panel eases the shown stores a step each frame; with them settled
    // the paused and help frames differ only where the dialog draws.
    if (const auto viewer = match_view_player(); viewer < OA_PLAYER_COUNT) {
        auto& game = match_->state().game;
        for (int step = 0; step < kReadoutSettleSteps; ++step)
            oa::ui::hud::update_resource_readout(
                game.resource_readout, game.players[viewer], game.tick
            );
    }
    renderer::Surface paused;
    capture_frame_ = &paused;
    render();
    capture_frame_ = nullptr;
    write_ppm(report_directory / "native-match-paused.ppm", paused);
    activate_pause_gadget("HELP");
    if (dialogs::dialog_kind() != dialogs::DialogKind::help)
        throw std::runtime_error("match dialog check: HELP did not open HELP.GUI");
    renderer::Surface presented;
    capture_frame_ = &presented;
    render();
    capture_frame_ = nullptr;
    write_ppm(report_directory / "native-match-help.ppm", presented);
    const auto canvas_width = static_cast<uint32_t>(match_layout_.width);
    const auto canvas_height = static_cast<uint32_t>(match_layout_.height);
    if (!match_use_layers_ || presented.width != canvas_width ||
        presented.height != canvas_height || paused.width != canvas_width ||
        paused.height != canvas_height)
        throw std::runtime_error("match dialog check: HELP.GUI left the layered match presenter");
    const auto& gadgets = dialogs::dialog_resources()->layout.gadgets;
    const auto& root = gadgets.front().common;
    const Rect panel{root.x, root.y, root.width, root.height};
    const auto strip = match_layout_.left;
    if (panel.x != (match_layout_.width - strip - panel.width) / 2 + strip ||
        panel.y != (match_layout_.height - panel.height) / 2)
        throw std::runtime_error(
            "match dialog check: HELP.GUI is not centred right of the side column"
        );
    const auto area =
        static_cast<std::size_t>(panel.width) * static_cast<std::size_t>(panel.height);
    if (differing_pixels(paused, presented, panel) < area / 2)
        throw std::runtime_error("match dialog check: the presented frame does not show HELP.GUI");
    const auto& options = match_hud_->layout.gadgets.front().common;
    const auto shaded = oa::ui::display_layout::source_rect_to_canvas(
        match_layout_, options.x, options.y, options.width, options.height
    );
    if (brightness(presented, shaded) * 4 > brightness(paused, shaded) * 3)
        throw std::runtime_error("match dialog check: HELP.GUI did not darken the options panel");
    // The software cursor animates between the two frames.
    const auto pointer_x = static_cast<int32_t>(pointer_x_);
    const auto pointer_y = static_cast<int32_t>(pointer_y_);
    const Rect cursor{
        pointer_x - kCursorReach, pointer_y - kCursorReach, 2 * kCursorReach, 2 * kCursorReach
    };
    if (differing_pixels_outside(paused, presented, {panel, shaded, cursor}) != 0)
        throw std::runtime_error(
            "match dialog check: HELP.GUI changed the frame outside its panels"
        );
    const auto ok = std::find_if(gadgets.begin(), gadgets.end(), [](const auto& gadget) {
        return gadget.common.name == "OK";
    });
    if (ok == gadgets.end())
        throw std::runtime_error("match dialog check: HELP.GUI has no OK button");
    float window_x = 0;
    float window_y = 0;
    if (!frame_to_window(
            sdl_.renderer,
            static_cast<float>(panel.x + ok->common.x + ok->common.width / 2),
            static_cast<float>(panel.y + ok->common.y + ok->common.height / 2),
            &window_x,
            &window_y
        ))
        throw std::runtime_error(std::string("SDL_RenderCoordinatesToWindow: ") + SDL_GetError());
    for (const auto type : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
        SDL_Event click{};
        click.button.type = type;
        click.button.windowID = SDL_GetWindowID(sdl_.window);
        click.button.button = SDL_BUTTON_LEFT;
        click.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        click.button.clicks = 1;
        click.button.x = window_x;
        click.button.y = window_y;
        dispatch_screen_input(click);
    }
    if (dialogs::dialog_count() != 0)
        throw std::runtime_error(
            "match dialog check: OK at its presented position did not close HELP.GUI"
        );
    std::cout << "match dialog check: HELP.GUI at " << panel.x << ',' << panel.y << " on the "
              << match_layout_.width << 'x' << match_layout_.height << " match canvas\n";
    check_in_game_briefing(report_directory);
    check_placed_dialogs(report_directory);

    // Over a new skirmish: the window's close request, a held Escape, held
    // keys in the chat line, the OPTIONS lightbar and a unit's speech with
    // its order's caption.
    load(Screen::main_menu);
    start_benchmark_skirmish();
    const auto require = [](bool ok, const std::string& failure) {
        if (!ok)
            throw std::runtime_error("match dialog check: " + failure);
    };
    require(screen_ == Screen::match && match_ && !match_finished_, "no skirmish to close");
    if (match_paused_)
        resume_match_pause();
    const auto send = [this](SDL_Event event) {
        bool running = true;
        dispatch_event(event, running);
        return running;
    };
    const auto close_request = [this](SDL_EventType type) {
        SDL_Event event{};
        event.type = type;
        if (type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            event.window.windowID = SDL_GetWindowID(sdl_.window);
        return event;
    };
    const auto escape = [](bool repeat) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = SDLK_ESCAPE;
        event.key.scancode = SDL_SCANCODE_ESCAPE;
        event.key.down = true;
        event.key.repeat = repeat;
        return event;
    };
    const auto hud_label = [this](std::string_view name) {
        for (const auto& gadget : match_hud_->layout.gadgets)
            if (gadget.common.name == name) {
                if (const auto* label =
                        std::get_if<oa::ui::gui_layout::LabelFields>(&gadget.fields))
                    return label->text;
            }
        return std::string();
    };
    const auto confirm = oa::data::defs::gui_path("YESORNO.GUI");
    const auto confirming = [&] {
        return match_paused_ && match_hud_ && match_hud_panel_ == confirm &&
               hud_label("TITLE") == game_text("Surrender this battle and exit to the system?");
    };
    // A held Escape's repeats open no menu.
    require(send(escape(true)) && !match_paused_, "a held Escape opened the pause menu");
    // Each repeat of a held Backspace deletes a character from the chat
    // line. Repeats press no hotkey: held Enter opens the line once, and its
    // repeats neither send it nor close it.
    const auto chat_key = [](SDL_Keycode code, SDL_Scancode scancode, bool repeat) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = code;
        event.key.scancode = scancode;
        event.key.down = true;
        event.key.repeat = repeat;
        return event;
    };
    require(
        send(chat_key(SDLK_RETURN, SDL_SCANCODE_RETURN, false)) && chat_composing_,
        "Enter did not open the chat line"
    );
    require(
        send(chat_key(SDLK_RETURN, SDL_SCANCODE_RETURN, true)) &&
            send(chat_key(SDLK_RETURN, SDL_SCANCODE_RETURN, true)) && chat_composing_ &&
            chat_buffer_.empty(),
        "a held Enter's repeats sent or closed the chat line"
    );
    SDL_Event typed{};
    typed.type = SDL_EVENT_TEXT_INPUT;
    typed.text.text = "abcd";
    require(send(typed) && chat_buffer_ == "abcd", "the chat line did not take typed text");
    require(
        send(chat_key(SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE, false)) &&
            send(chat_key(SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE, true)) &&
            send(chat_key(SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE, true)) && chat_buffer_ == "a",
        "a held Backspace did not take a character from the chat line with each repeat"
    );
#if defined(SDL_PLATFORM_MACOS) && SDL_VERSION_ATLEAST(3, 4, 0)
    // On macOS a held letter repeats too, rather than opening the accents menu.
    require(
        !SDL_GetHintBoolean(SDL_HINT_MAC_PRESS_AND_HOLD, true),
        "a held key opens macOS's accents menu"
    );
#endif
    close_chat_line();
    // The keypad's Enter is Return: it opens the chat line, sends the line
    // typed into it, and answers HELP.GUI over the in-game menu with the
    // dialog's Enter default.
    const auto keypad_enter = chat_key(SDLK_KP_ENTER, SDL_SCANCODE_KP_ENTER, false);
    require(send(keypad_enter) && chat_composing_, "keypad Enter did not open the chat line");
    SDL_Event line{};
    line.type = SDL_EVENT_TEXT_INPUT;
    line.text.text = "sent from the keypad";
    require(
        send(line) && send(keypad_enter) && !chat_composing_ && status_ == line.text.text,
        "keypad Enter did not send the chat line"
    );
    show_match_pause_menu();
    activate_pause_gadget("HELP");
    require(dialogs::dialog_kind() == dialogs::DialogKind::help, "HELP did not open HELP.GUI");
    require(
        send(keypad_enter) && dialogs::dialog_count() == 0 && match_paused_,
        "keypad Enter did not answer HELP.GUI"
    );
    resume_match_pause();
    require(
        send(close_request(SDL_EVENT_WINDOW_CLOSE_REQUESTED)) && !exit_requested_,
        "closing the window ended a running match"
    );
    require(confirming(), "closing the window did not ask to surrender (YESORNO.GUI)");
    require(
        send(close_request(SDL_EVENT_WINDOW_CLOSE_REQUESTED)) && confirming(),
        "a second close request did not leave the confirmation as it was"
    );
    activate_pause_gadget("CHOICE2");
    require(
        !match_paused_ && match_hud_panel_ != confirm && !exit_requested_,
        "CHOICE2 did not return to the running match"
    );
    // Escape answers as CHOICE2 does.
    require(send(close_request(SDL_EVENT_WINDOW_CLOSE_REQUESTED)) && confirming(), "no second ask");
    require(send(escape(false)) && !match_paused_, "Escape did not close the confirmation");
    // Asked over the in-game menu, CHOICE2 goes back to the menu.
    show_match_pause_menu();
    require(send(close_request(SDL_EVENT_WINDOW_CLOSE_REQUESTED)) && confirming(), "no menu ask");
    activate_pause_gadget("CHOICE2");
    require(
        match_paused_ && match_hud_panel_ == oa::data::defs::gui_path("ARMOPT.GUI"),
        "CHOICE2 did not return to the in-game menu it was asked over"
    );
    // Escape answers as CHOICE2 does over the menu too.
    require(send(close_request(SDL_EVENT_WINDOW_CLOSE_REQUESTED)) && confirming(), "no menu ask");
    require(
        send(escape(false)) && match_paused_ &&
            match_hud_panel_ == oa::data::defs::gui_path("ARMOPT.GUI"),
        "Escape did not return to the in-game menu the confirmation was asked over"
    );
    // Enter answers No as well: CHOICE2 is the confirmation's Enter default.
    const auto enter = [] {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = SDLK_RETURN;
        event.key.scancode = SDL_SCANCODE_RETURN;
        event.key.down = true;
        return event;
    };
    require(send(close_request(SDL_EVENT_WINDOW_CLOSE_REQUESTED)) && confirming(), "no menu ask");
    require(
        send(enter()) && !exit_requested_ && match_paused_ &&
            match_hud_panel_ == oa::data::defs::gui_path("ARMOPT.GUI"),
        "Enter did not return to the in-game menu the confirmation was asked over"
    );
    // The confirmation EXITGAME asks answers Enter and Escape as No too,
    // going back to the in-game menu.
    const auto ask_from_menu = [&] {
        activate_pause_gadget("EXIT");
        activate_pause_gadget("EXITGAME");
        return confirming();
    };
    require(ask_from_menu(), "EXITGAME did not ask to surrender");
    require(
        send(enter()) && !exit_requested_ && match_paused_ &&
            match_hud_panel_ == oa::data::defs::gui_path("ARMOPT.GUI"),
        "Enter did not answer EXITGAME's confirmation as No"
    );
    require(ask_from_menu(), "EXITGAME did not ask to surrender again");
    require(
        send(escape(false)) && !exit_requested_ && match_paused_ &&
            match_hud_panel_ == oa::data::defs::gui_path("ARMOPT.GUI"),
        "Escape did not answer EXITGAME's confirmation as No"
    );
    // A panel that pauses the match after an answered confirmation, as the
    // team panels do, still gets asked.
    resume_match_pause();
    require(load_team_panel("SHARE.GUI", true) && match_paused_, "SHARE.GUI did not open");
    require(
        send(close_request(SDL_EVENT_WINDOW_CLOSE_REQUESTED)) && confirming(),
        "closing the window over a panel after an answered confirmation did not ask"
    );
    activate_pause_gadget("CHOICE2");
    resume_match_pause();
    // A page over the match (here the save page) goes back to the match to
    // ask there.
    show_match_pause_menu();
    activate_pause_gadget("SAVEGAME");
    require(screen_ == Screen::load_game, "SAVEGAME did not open the save page");
    require(
        send(close_request(SDL_EVENT_WINDOW_CLOSE_REQUESTED)) && !exit_requested_ &&
            screen_ == Screen::match && confirming(),
        "closing the window over the save page did not ask in the match"
    );
    activate_pause_gadget("CHOICE2");
    require(
        match_paused_ && match_hud_panel_ == oa::data::defs::gui_path("ARMOPT.GUI"),
        "CHOICE2 after the save page did not return to the in-game menu"
    );

    // PREFS opens the preferences over the match, in the side column in the
    // in-game menu's place: PREFS.GUI, while the lightbar sweeps the menu's
    // picture away. It folds onto column 127, turns over and opens out over
    // the battlefield to column 277, playing "Options" once as it ends.
    const auto hud_gadget = [this](std::string_view name) -> const oa::ui::gui_layout::Gadget* {
        if (!match_hud_)
            return nullptr;
        for (const auto& gadget : match_hud_->layout.gadgets)
            if (gadget.common.name == name)
                return &gadget;
        return nullptr;
    };
    const auto showing = [this](std::string_view layout) {
        return screen_ == Screen::match && match_paused_ && match_hud_ &&
               match_hud_panel_ == layout;
    };
    // The match frame as the layers compose it, without the pointer.
    const auto composed = [this] {
        render();
        renderer::Surface frame;
        compose_match_layers(frame);
        return frame;
    };
    // The canvas rectangle over 640x480 source pixels, as the chrome places them.
    const auto canvas_rect = [this](int32_t x, int32_t y, int32_t width, int32_t height) {
        const auto from = oa::ui::display_layout::source_to_canvas(match_layout_, x, y);
        const auto to =
            oa::ui::display_layout::source_to_canvas(match_layout_, x + width, y + height);
        return Rect{from.x, from.y, to.x - from.x, to.y - from.y};
    };
    // Clicks a control of the panel loaded as the match HUD where the pointer
    // would: `percent` of its width in, halfway down.
    const auto click_control = [&](std::string_view name, int32_t percent) {
        const auto* gadget = hud_gadget(name);
        require(
            gadget != nullptr && gadget->common.active != 0,
            std::string(name) + " does not show on " + match_hud_panel_
        );
        const auto at = oa::ui::display_layout::source_to_canvas(
            match_layout_,
            gadget->common.x + gadget->common.width * percent / 100,
            gadget->common.y + gadget->common.height / 2
        );
        update_pointer(static_cast<float>(at.x), static_cast<float>(at.y));
        require(
            hovered_ && match_hud_->layout.gadgets[*hovered_].common.name == name,
            "the pointer over " + std::string(name) + " is not over it"
        );
        activate_match_hud(*hovered_);
    };
    const auto key = [](SDL_Keycode code, SDL_Scancode scancode) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = code;
        event.key.scancode = scancode;
        event.key.down = true;
        return event;
    };
    require(
        showing(oa::data::defs::gui_path(kInGameMenuLayout)),
        "the in-game menu is not open for PREFS"
    );
    if (const auto viewer = match_view_player(); viewer < OA_PLAYER_COUNT) {
        auto& game = match_->state().game;
        for (int step = 0; step < kReadoutSettleSteps; ++step)
            oa::ui::hud::update_resource_readout(
                game.resource_readout, game.players[viewer], game.tick
            );
    }
    update_pointer(
        static_cast<float>(match_layout_.width - 1), static_cast<float>(match_layout_.height - 1)
    );
    const auto menu = match_hud_->layout.gadgets.front().common;
    auto in_game_menu = composed();
    const auto sounds = options_lightbar_sounds_;
    // Each frame of the lightbar marks it drawn in the game.
    match_->state().game.options_lightbar_drawn = 0;
    activate_pause_gadget("PREFS");
    require(
        showing(oa::data::defs::gui_path(kPreferencesLayout)),
        "PREFS did not open PREFS.GUI over the match"
    );
    const auto prefs = match_hud_->layout.gadgets.front().common;
    require(
        prefs.x == 0 && prefs.y == 126 && prefs.width == 128 && prefs.height == 354,
        "PREFS.GUI is not the side column's 128x354 panel at 0,126"
    );
    const auto* picture = hud_gadget("IGOPT");
    require(
        picture != nullptr && picture->common.x == 0 && picture->common.y == 128,
        "PREFS.GUI's IGOPT picture is not at 0,128"
    );
    require(
        options_flip_.width == static_cast<uint32_t>(menu.width) &&
            options_flip_.height == static_cast<uint32_t>(menu.height),
        "the lightbar did not take the in-game menu's picture"
    );
    // The fold's lift reaches six steps above the menu's top.
    const auto lift = 6 * oa::ui::frontend::kLightbarVelocityStep;
    const auto swept = canvas_rect(
        0, menu.y - lift, oa::ui::frontend::kLightbarScrollEnd + 1, kCanvasHeight - menu.y + lift
    );
    // The frame the preferences opened on took the first step.
    std::vector<renderer::Surface> frames{composed()};
    for (int frame = 1; frame < kSweepFrames; ++frame) {
        tick_screen_packages();
        frames.push_back(composed());
    }
    std::size_t moving = 0;
    for (int frame = 1; frame < kSweepFrames; ++frame) {
        require(
            differing_pixels_outside(frames[frame - 1], frames[frame], {swept}) == 0,
            "frame " + std::to_string(frame) + " of the lightbar changed outside columns 0 to 277"
        );
        if (differing_pixels(frames[frame - 1], frames[frame], swept) != 0)
            ++moving;
    }
    // Twelve more steps move the sweep, which then holds at its end.
    require(moving == 12, "the lightbar moved on " + std::to_string(moving) + " frames, not 12");
    require(
        differing_pixels(frames[kSweepFrames - 2], frames[kSweepFrames - 1], swept) == 0,
        "the lightbar still moved after it reached its end"
    );
    require(
        options_lightbar_sounds_ == sounds + 1,
        "the lightbar played \"Options\" " + std::to_string(options_lightbar_sounds_ - sounds) +
            " times"
    );
    require(
        match_->state().game.options_lightbar_drawn == 1,
        "the lightbar's frames did not mark it drawn in the game"
    );
    // Held at its end, the sound page covers the battlefield beside the side
    // column; the rest of the match shows as the menu left it. The sweep used
    // to stop on the turned-over picture, which has no controls.
    const auto beside = canvas_rect(kBattlefieldLeft + 2, 150, 146, 280);
    const auto beside_area =
        static_cast<std::size_t>(beside.width) * static_cast<std::size_t>(beside.height);
    require(
        differing_pixels(in_game_menu, frames.back(), beside) > beside_area / 2,
        "the lightbar's picture does not show over the battlefield beside the side column"
    );
    require(
        bright_control_pixels(frames.back(), beside) >= kOptionsControlPixels,
        "the options menu opened on a blank panel (" +
            std::to_string(bright_control_pixels(frames.back(), beside)) +
            " control pixels beside the tabs)"
    );
    require(
        differing_pixels_outside(in_game_menu, frames.back(), {swept}) == 0,
        "the preferences changed the match right of column 277"
    );
    write_ppm(report_directory / "native-match-options-lightbar.ppm", frames[6]);
    write_ppm(report_directory / "native-match-options-lightbar-end.ppm", frames.back());

    // A tab frees the lightbar. SOUND widens PREFS.GUI to 278 pixels and
    // merges SOUNDSRT.GUI into its PANEL filler, beside the side column over
    // the battlefield; right of it the match shows as before.
    click_control("SOUND", 50);
    require(
        showing(oa::data::defs::gui_path(kPreferencesLayout)) && options_flip_.rgb.empty(),
        "SOUND did not free the lightbar"
    );
    require(
        match_hud_->layout.gadgets.front().common.width == 278,
        "SOUND did not widen PREFS.GUI to 278 pixels"
    );
    const auto* filler = hud_gadget("PANEL");
    require(filler != nullptr && filler->common.active == 0, "PREFS.GUI has no hidden PANEL");
    // FXVOL, at 140,195, lies between its 9-pixel arrows once bound.
    const auto* volume = hud_gadget("FXVOL");
    require(
        volume != nullptr && volume->common.x == 140 + 9 && volume->common.y == 126 + 69 &&
            volume->common.width == 122 - 2 * 9,
        "SOUNDSRT.GUI's FXVOL is not bound between its arrows at 140,195"
    );
    const auto* sound_picture = hud_gadget("SOUNDSRT");
    require(
        sound_picture != nullptr && sound_picture->common.x == 128 &&
            sound_picture->common.y == 127,
        "SOUNDSRT.GUI's picture is not at 128,127"
    );
    auto sound = composed();
    tick_screen_packages();
    auto after_tab = composed();
    require(
        differing_pixels_outside(sound, after_tab, {}) == 0, "the lightbar still moved after a tab"
    );
    require(
        differing_pixels(in_game_menu, sound, beside) > beside_area / 2,
        "SOUNDSRT.GUI does not show beside the side column"
    );
    require(
        differing_pixels_outside(in_game_menu, sound, {swept}) == 0,
        "SOUNDSRT.GUI changed the match right of column 277"
    );
    write_ppm(report_directory / "native-match-preferences-sound.ppm", sound);
    require(
        bright_control_pixels(sound, beside) >= kOptionsControlPixels,
        "SOUND's page is blank beside the tabs"
    );

    // A slider's knob, dragged or stepped by its arrows, applies the value
    // it stands for at once; Cancel puts back what the preferences opened with.
    const auto entry_volume = preferences_.fx_volume;
    if ((preferences_.sound_flags & init::preference_flags::sound_mode) == 0)
        click_control("MODE", 50);
    drag_check_knob("FXVOL", -volume->common.width);
    const auto quiet = preferences_.fx_volume;
    for (int step = 0; step < 20; ++step)
        click_check_arrow("FXVOL", true);
    const auto loud = preferences_.fx_volume;
    require(
        quiet == 0 && loud > quiet,
        "FXVOL's knob did not set the effects volume (" + std::to_string(quiet) + ", then " +
            std::to_string(loud) + ")"
    );
    click_control("CANCEL", 50);
    require(
        showing(oa::data::defs::gui_path(kInGameMenuLayout)),
        "Cancel did not return to the in-game menu"
    );
    require(
        preferences_.fx_volume == entry_volume,
        "Cancel did not put the effects volume back to " + std::to_string(entry_volume)
    );

    // INTERFACE's GAME slider sets the running game's speed; Enter answers as
    // OK, which keeps it.
    auto& game = match_->state().game;
    const auto speed = game.requested_speed;
    activate_pause_gadget("PREFS");
    click_control("SPEEDS", 50);
    require(hud_gadget("GAME") != nullptr, "INTERFACE did not merge SPEEDSRT.GUI");
    {
        const auto interface_page = composed();
        write_ppm(report_directory / "native-match-preferences-interface.ppm", interface_page);
        require(
            bright_control_pixels(interface_page, beside) >= kOptionsControlPixels,
            "INTERFACE's page is blank beside the tabs"
        );
    }
    // Position 85 of GAME's 88 stands for speed 20: 85 / 87 * 21, truncated.
    drag_check_knob("GAME", 85 - check_scroll_bar("GAME").bar.knob);
    require(
        game.requested_speed == oa::sim::speed::fastest &&
            match_timing_.requested_rate == oa::sim::speed::fastest,
        "GAME did not set the running game's speed to " + std::to_string(oa::sim::speed::fastest)
    );
    require(send(key(SDLK_RETURN, SDL_SCANCODE_RETURN)), "Enter ended the run");
    require(
        showing(oa::data::defs::gui_path(kInGameMenuLayout)) &&
            game.requested_speed == oa::sim::speed::fastest &&
            preferences_.game_speed == oa::sim::speed::fastest,
        "Enter did not leave the preferences as OK does"
    );
    // The speed set is read back from the Game block below.
    std::ignore = oa::sim::speed::set_speed(match_->state(), speed, message_hooks());
    match_timing_.requested_rate = game.requested_speed;
    match_timing_.actual_rate = game.current_speed;
    preferences_.game_speed = game.requested_speed;
    preferences_.current_game_speed = game.current_speed;

    // VISUALS merges VISUALRT.GUI, and MUSIC MUSICRT.GUI. 3.1c's VISUALRT.GUI
    // offers no display mode; OA adds the screen-size slider, the same list
    // as Settings'.
    activate_pause_gadget("PREFS");
    click_control("VISUALS", 50);
    require(
        hud_gadget("GAMMA") != nullptr && hud_gadget("VISUALSRT") != nullptr &&
            hud_gadget("VIDSLDR") != nullptr,
        "VISUALS did not merge VISUALRT.GUI with the screen-size slider"
    );
    {
        const auto visuals = composed();
        write_ppm(report_directory / "native-match-preferences-visuals.ppm", visuals);
        require(
            bright_control_pixels(visuals, beside) >= kOptionsControlPixels,
            "VISUALS's page is blank beside the tabs"
        );
    }
    click_control("MUSIC", 50);
    require(
        hud_gadget("CDPLAY") != nullptr && hud_gadget("MUSICRT") != nullptr &&
            hud_gadget("GAMMA") == nullptr && match_music_panel_open(),
        "MUSIC did not merge MUSICRT.GUI"
    );
    {
        const auto music = composed();
        write_ppm(report_directory / "native-match-preferences-music.ppm", music);
        require(
            bright_control_pixels(music, beside) >= kOptionsControlPixels,
            "MUSIC's page is blank beside the tabs"
        );
    }
    // The panel brings its own GAF, so its transport buttons draw
    // MUSICRT.GAF's frames rather than the shared fallback frame.
    require(
        std::any_of(
            match_hud_->sprites.sequences.begin(),
            match_hud_->sprites.sequences.end(),
            [](const oa::formats::gaf::Sequence& sequence) { return sequence.name == "CDPLAY"; }
        ),
        "MUSIC did not merge MUSICRT.GAF's transport art"
    );
    // Its transport buttons take the size of the frames they draw, so each is
    // hit where it is drawn (MUSICRT.GUI authors them 16x16).
    {
        const auto* play = hud_gadget("CDPLAY");
        const auto art = std::find_if(
            match_hud_->sprites.sequences.begin(),
            match_hud_->sprites.sequences.end(),
            [](const oa::formats::gaf::Sequence& sequence) { return sequence.name == "CDPLAY"; }
        );
        require(
            play != nullptr && art != match_hud_->sprites.sequences.end() && !art->frames.empty() &&
                play->common.width == art->frames.front().width &&
                play->common.height == art->frames.front().height,
            "MUSIC's CDPLAY is not hit where its frame is drawn"
        );
    }
    // The buttons' quick keys press them: 's' is SOUND, 'c' Cancel. Leaving
    // the MUSIC tab leaves the music's panel.
    require(send(key(SDLK_S, SDL_SCANCODE_S)), "'s' ended the run");
    require(hud_gadget("FXVOL") != nullptr, "'s' did not open SOUND");
    require(!match_music_panel_open(), "SOUND left the MUSIC tab counted as the music's panel");
    require(send(key(SDLK_C, SDL_SCANCODE_C)), "'c' ended the run");
    require(
        showing(oa::data::defs::gui_path(kInGameMenuLayout)), "'c' did not cancel the preferences"
    );
    // Escape answers as OK.
    activate_pause_gadget("PREFS");
    require(send(escape(false)), "Escape ended the run");
    require(
        showing(oa::data::defs::gui_path(kInGameMenuLayout)) && options_flip_.rgb.empty(),
        "Escape did not leave the preferences for the in-game menu"
    );
    // F2 closes the menus over the match, the preferences among them.
    activate_pause_gadget("PREFS");
    require(send(key(SDLK_F2, SDL_SCANCODE_F2)), "F2 ended the run");
    require(
        screen_ == Screen::match && !match_paused_ &&
            match_hud_panel_ != oa::data::defs::gui_path(kPreferencesLayout),
        "F2 did not close the preferences with the in-game menu"
    );
    show_match_pause_menu();
    // Closing the window over the preferences asks over the in-game menu,
    // and CHOICE2 returns to that menu.
    activate_pause_gadget("PREFS");
    require(
        send(close_request(SDL_EVENT_WINDOW_CLOSE_REQUESTED)) && !exit_requested_ && confirming(),
        "closing the window over the preferences did not ask to surrender"
    );
    activate_pause_gadget("CHOICE2");
    require(
        showing(oa::data::defs::gui_path(kInGameMenuLayout)),
        "CHOICE2 over the preferences did not return to the in-game menu"
    );
    resume_match_pause();
    std::cout << "match preferences check: PREFS.GUI in the side column, the lightbar over "
              << moving << " frames, SOUNDSRT, SPEEDSRT, VISUALRT and MUSICRT beside it\n";

    // An order's own caption reaches its owner's message log: the commander
    // told to capture the other commander says it cannot.
    uint16_t commander = 0;
    uint16_t enemy = 0;
    for (const auto& slot : match_->world().slots) {
        if (slot.unit == nullptr || slot.record.type_index == 0 ||
            (slot.record.flags & OA_UNIT_FLAG_LIVE) == 0)
            continue;
        if (slot.record.owner_index == match_local_player_ && commander == 0)
            commander = slot.unit_index;
        else if (slot.record.owner_index != match_local_player_ && enemy == 0)
            enemy = slot.unit_index;
    }
    require(commander != 0 && enemy != 0, "the skirmish has no two commanders");
    match_->issue_capture(commander, enemy, false);
    ++match_timing_.tick;
    match_->simulation().tick = match_timing_.tick;
    tick_or_raise(*match_);
    // The line names the unit and gives the caption in the language shown,
    // and keeps what of them fits the line in whole characters.
    std::string name;
    if (const auto* def =
            oa::world_unit_def_of(&match_->state(), &match_->world().slots[commander].record))
        name = std::string(oa::data::languages::unit_display_name(*def));
    if (const auto* definition = definition_for(commander); name.empty() && definition != nullptr)
        name = definition->display_name;
    const std::string spoken = name + ": " + translate_ui("That unit cannot be captured");
    const std::string caption = spoken.substr(
        0, oa::base::text::whole_character_bytes(spoken, oa::sim::messages::text_bytes - 1)
    );
    bool captioned = false;
    for (std::size_t pump = 0;
         pump < oa::audio::game_audio::AnnouncementQueue::capacity && !captioned;
         ++pump) {
        present_unit_announcements();
        const auto lines = match_message_lines();
        captioned = std::find(lines.begin(), lines.end(), caption) != lines.end();
    }
    require(captioned, "\"" + caption + "\" did not reach the message log");
    // Another player's unit speaks to its own owner only.
    const auto before = match_message_lines().size();
    offline_services_.command_speech(match_->world().slots[enemy], 7, "Transport mission failed");
    for (std::size_t pump = 0; pump < oa::audio::game_audio::AnnouncementQueue::capacity; ++pump)
        present_unit_announcements();
    const auto after = match_message_lines();
    require(
        std::none_of(
            after.begin() + static_cast<std::ptrdiff_t>(std::min(before, after.size())),
            after.end(),
            [](const std::string& line) {
                return line.find("Transport mission failed") != std::string::npos;
            }
        ),
        "another player's unit spoke in the viewer's log"
    );

    // A match with a return label names it: the exit confirmation returns
    // to it, and so does the end-of-game screen's MAIN MENU, which leaves
    // the pointer's picture as it is.
    const auto kept_extension = extension_;
    try {
        probe_return_label() = "Portal";
        extension_.return_label = [](void*) { return probe_return_label(); };
        load(Screen::main_menu);
        start_benchmark_skirmish();
        require(
            screen_ == Screen::match && match_ &&
                std::string_view(return_label_.data()) == "Portal",
            "the skirmish did not keep the return label"
        );
        const auto ask_main_menu = [&] {
            show_match_pause_menu();
            activate_pause_gadget("EXIT");
            require(
                showing(oa::data::defs::gui_path("EXITMENU.GUI")), "EXIT did not open EXITMENU.GUI"
            );
            activate_pause_gadget("MAINMENU");
            require(showing(confirm), "MAINMENU did not ask to surrender");
            const auto title = hud_label("TITLE");
            activate_pause_gadget("CHOICE2");
            return title;
        };
        require(
            ask_main_menu() == game_text("Surrender this battle and return to Portal?"),
            "the confirmation does not return to the label"
        );
        // A label longer than nine characters leaves the wording as it is.
        probe_return_label() = "PortalGate";
        take_return_label();
        require(
            ask_main_menu() == game_text("Surrender this battle and return to main menu?"),
            "a ten-character label changed the confirmation"
        );
        probe_return_label() = "Portal";
        take_return_label();
        resume_match_pause();
        for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player)
            if (player != match_local_player_ &&
                match_->state().game.players[player].unit_count != 0)
                match_->destroy_player_units(player);
        for (uint32_t step = 0; step < kVictoryTicks && !match_finished_; ++step) {
            ++match_timing_.tick;
            match_->simulation().tick = match_timing_.tick;
            tick_or_raise(*match_);
            present_match_outcome();
        }
        require(match_finished_, "the skirmish swept of its opponents did not end");
        finish_match_outcome();
        require(
            screen_ == Screen::campaign_end && step_end_screen_to_panel().panel,
            "the skirmish did not end on ENDMSN.GUI"
        );
        const auto* main_menu = widget("MainMenu");
        const auto* main_menu_button =
            main_menu != nullptr ? std::get_if<oa::ui::gui_layout::ButtonFields>(&main_menu->fields)
                                 : nullptr;
        require(
            main_menu_button != nullptr && main_menu_button->text == "Portal",
            "ENDMSN.GUI's MAIN MENU does not read the return label"
        );
        const auto cursor_before_click = cursor_index_;
        click_end_panel(static_cast<std::size_t>(main_menu - resources_.layout.gadgets.data()));
        require(
            screen_ == Screen::main_menu, "ENDMSN.GUI's MAIN MENU did not leave for the main menu"
        );
        require(
            cursor_index_ == cursor_before_click && cursor_index_ != kLeavingPanelCursor,
            "ENDMSN.GUI's MAIN MENU changed the pointer's picture with a return label"
        );
    } catch (...) {
        extension_ = kept_extension;
        throw;
    }
    extension_ = kept_extension;
    std::cout << "return label check: the exit confirmation and ENDMSN.GUI name the label\n";
    start_benchmark_skirmish();
    if (match_paused_)
        resume_match_pause();

    // The system's quit asks too; CHOICE1 surrenders and ends the run.
    require(send(close_request(SDL_EVENT_QUIT)) && confirming(), "the system's quit did not ask");
    activate_pause_gadget("CHOICE1");
    require(exit_requested_ && !match_, "CHOICE1 did not leave the match and end the run");
    // Outside a match a close request ends the run at once.
    exit_requested_ = false;
    load(Screen::main_menu);
    require(
        !send(close_request(SDL_EVENT_WINDOW_CLOSE_REQUESTED)),
        "closing the window on the main menu did not end the run"
    );
    // The same four pages, drawn, at the two sizes the blank panel was reported
    // across: the sweep ends on sound, and each button shows its own controls.
    exit_requested_ = false;
    for (const auto& [width, height] :
         {std::pair{kCanvasWidth, kCanvasHeight}, std::pair{1920, 1200}}) {
        const auto size = std::to_string(width) + 'x' + std::to_string(height);
        if (match_)
            leave_match();
        if (!SDL_SetWindowSize(sdl_.window, width, height) || !SDL_SyncWindow(sdl_.window))
            throw std::runtime_error(std::string("SDL_SetWindowSize: ") + SDL_GetError());
        load(Screen::main_menu);
        start_benchmark_skirmish();
        if (match_layout_.width != width || match_layout_.height != height)
            throw std::runtime_error("options pages: could not size the match to " + size);
        show_match_pause_menu();
        update_pointer(static_cast<float>(width - 1), static_cast<float>(height - 1));
        activate_pause_gadget("PREFS");
        // Each frame of the sweep is drawn, as the first check draws them:
        // the step runs as the frame is rendered, and the last frame is the
        // sound page.
        auto opening = composed();
        for (int frame = 1; frame < kSweepFrames; ++frame) {
            tick_screen_packages();
            opening = composed();
        }
        const auto page = canvas_rect(kBattlefieldLeft + 2, 150, 146, 280);
        write_ppm(
            report_directory / ("native-match-preferences-opening-" + size + ".ppm"), opening
        );
        require(
            bright_control_pixels(opening, page) >= kOptionsControlPixels,
            "the options menu opened on a blank panel at " + size
        );
        const auto shown = [&](const char* button, const char* gadget, const std::string& picture) {
            click_control(button, 50);
            require(
                hud_gadget(gadget) != nullptr,
                std::string(button) + " did not show its page at " + size
            );
            const auto frame = composed();
            write_ppm(report_directory / picture, frame);
            require(
                bright_control_pixels(frame, page) >= kOptionsControlPixels,
                std::string(button) + "'s page is blank at " + size
            );
        };
        shown("SOUND", "FXVOL", "native-match-preferences-sound-" + size + ".ppm");
        shown("MUSIC", "MUSICVOL", "native-match-preferences-music-" + size + ".ppm");
        shown("SPEEDS", "GAME", "native-match-preferences-interface-" + size + ".ppm");
        shown("VISUALS", "SHADING", "native-match-preferences-visuals-" + size + ".ppm");
        click_control("CANCEL", 50);
        std::cout << "options pages: sound, music, interface and visuals drawn at " << size << '\n';
    }
    if (match_)
        leave_match();
    std::cout << "match close check: YESORNO.GUI on close, OPTIONS lightbar over " << moving
              << " frames, captioned speech, the return label\n";
}

void Runtime::check_in_game_briefing(const fs::path& report_directory) {
    const std::string what = "match briefing check: BRIEFING.GUI";
    const auto bitmap = oa::ui::decoded::require(
        oa::decode_pcx(assets_.read(kBriefingBitmap).bytes), kBriefingBitmap
    );
    const auto require = [&what](bool ok, const std::string& failure) {
        if (!ok)
            throw std::runtime_error(what + ' ' + failure);
    };
    const auto authored_layout =
        oa::ui::gui_layout::parse(assets_.read(oa::data::defs::gui_path(kBriefingGui)).bytes);
    require(authored_layout.ok() && !authored_layout.layout->gadgets.empty(), "does not parse");
    const auto authored = authored_layout.layout->gadgets.front().common;
    // The authored position centres the panel on a 640x480 screen.
    require(
        authored.x == (kCanvasWidth - authored.width) / 2 &&
            authored.y == (kCanvasHeight - authored.height) / 2,
        "is not authored centred on a 640x480 screen"
    );
    const auto click = [&](const oa::ui::gui_layout::CommonFields& root, std::string_view name) {
        const auto* gadget = widget(name);
        require(gadget != nullptr, "has no " + std::string(name));
        float window_x = 0;
        float window_y = 0;
        if (!frame_to_window(
                sdl_.renderer,
                static_cast<float>(root.x + gadget->common.x + gadget->common.width / 2),
                static_cast<float>(root.y + gadget->common.y + gadget->common.height / 2),
                &window_x,
                &window_y
            ))
            throw std::runtime_error(
                std::string("SDL_RenderCoordinatesToWindow: ") + SDL_GetError()
            );
        bool running = true;
        for (const auto type : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event event{};
            event.button.type = type;
            event.button.windowID = SDL_GetWindowID(sdl_.window);
            event.button.button = SDL_BUTTON_LEFT;
            event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.clicks = 1;
            event.button.x = window_x;
            event.button.y = window_y;
            dispatch_event(event, running);
        }
    };
    for (const auto& [width, height] :
         {std::pair{kCanvasWidth, kCanvasHeight},
          std::pair{kDefaultWindowWidth, kDefaultWindowHeight}}) {
        const auto size = std::to_string(width) + 'x' + std::to_string(height);
        if (match_)
            leave_match();
        if (!SDL_SetWindowSize(sdl_.window, width, height) || !SDL_SyncWindow(sdl_.window))
            throw std::runtime_error(std::string("SDL_SetWindowSize: ") + SDL_GetError());
        preferences_.side = 0;
        discover_campaigns();
        require(
            campaign_mission_files_.size() > kBriefingCheckMission,
            "has no campaign with mission " + std::to_string(kBriefingCheckMission + 1)
        );
        selected_mission_index_ = kBriefingCheckMission;
        show_mission_briefing();
        require(screen_ == Screen::briefing, "check could not open the mission's briefing");
        // The frontend briefing's first line, as the mission's briefing file has it.
        const auto heading = briefing_text_.substr(0, briefing_text_.find_first_of("\r\n"));
        start_campaign_mission();
        require(
            screen_ == Screen::match && match_ && campaign_mission_,
            "check could not start the mission: " + status_
        );
        require(
            match_layout_.width == width && match_layout_.height == height,
            "check could not size the match canvas to " + size
        );
        show_match_pause_menu();
        // The pointer rests in the window's corner, clear of the panel.
        update_pointer(static_cast<float>(width - 1), static_cast<float>(height - 1));
        // Settled readouts leave the paused frame as the briefing finds it.
        if (const auto viewer = match_view_player(); viewer < OA_PLAYER_COUNT) {
            auto& game = match_->state().game;
            for (int step = 0; step < kReadoutSettleSteps; ++step)
                oa::ui::hud::update_resource_readout(
                    game.resource_readout, game.players[viewer], game.tick
                );
        }
        render();
        renderer::Surface paused;
        compose_match_layers(paused);
        const auto palette = match_palette_;
        activate_pause_gadget("MISSION");
        require(screen_ == Screen::briefing, "did not open from the pause menu's MISSION");
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        write_ppm(report_directory / ("native-match-briefing-" + size + ".ppm"), presented);
        require(
            presented.width == paused.width && presented.height == paused.height &&
                surface_.width == paused.width && surface_.height == paused.height,
            "is not drawn over the " + size + " match frame"
        );
        const auto root = resources_.layout.gadgets.front().common;
        const auto reach = cursor_reach(pointer_x_, pointer_y_);
        const auto panel = check_backdrop_panel(
            surface_,
            resources_.layout,
            bitmap,
            palette,
            {authored.x, authored.y},
            reach,
            what + " on the " + size + " window"
        );
        require(
            differing_pixels_outside(paused, surface_, {panel, reach}) == 0,
            "changed the paused match outside its panel on the " + size + " window"
        );
        require(
            briefing_row(0) == heading,
            "reads \"" + briefing_row(0) + "\" on its first row, not \"" + heading + '"'
        );
        click(root, "MOREBAR");
        require(
            screen_ == Screen::briefing && briefing_row(0) != heading,
            "MOREBAR did not turn the page"
        );
        click(root, "OK");
        require(
            screen_ == Screen::match && match_paused_ && match_hud_ &&
                std::any_of(
                    match_hud_->layout.gadgets.begin(),
                    match_hud_->layout.gadgets.end(),
                    [](const auto& gadget) { return gadget.common.name == "MISSION"; }
                ),
            "OK did not return to the pause menu"
        );
        std::cout << what << " at " << panel.x << ',' << panel.y << " on the " << size
                  << " window\n";
    }
    leave_match();
    if (!SDL_SetWindowSize(sdl_.window, kDefaultWindowWidth, kDefaultWindowHeight) ||
        !SDL_SyncWindow(sdl_.window))
        throw std::runtime_error(std::string("SDL_SetWindowSize: ") + SDL_GetError());
}

void Runtime::check_placed_dialogs(const fs::path& report_directory) {
    // A text the game sets, as the language shown gives it.
    const auto game_text = [this](const char* english) {
        const char* translated = game_translation(english);
        return std::string(translated != nullptr ? translated : english);
    };
    namespace panel_flag = oa::ui::gui_input::panel_flag;
    const auto require = [](bool ok, const std::string& failure) {
        if (!ok)
            throw std::runtime_error("placed dialog check: " + failure);
    };
    oa::formats::gaf::Archive common;
    append_gaf_file(common, "anims/commongui.gaf");
    const auto* tile = gaf_sequence(common, kBackTile);
    require(
        tile != nullptr && tile->frames.size() >= kBackTileFrames,
        "the common GUI art has no BackTile"
    );
    std::vector<oa::formats::gaf::RenderedFrame> tiles;
    for (std::size_t index = 0; index < kBackTileFrames; ++index) {
        auto rendered = oa::formats::gaf::render_normal(tile->frames[index]);
        require(rendered.ok(), "a BackTile frame does not render");
        tiles.push_back(std::move(*rendered.frame));
        require(
            tiles.back().width == tiles.front().width &&
                tiles.back().height == tiles.front().height &&
                tiles.back().width == tiles.back().height,
            "the BackTile frames are not square tiles of one size"
        );
    }
    const auto tile_size = static_cast<int32_t>(tiles.front().width);
    const auto shade = assets_.read(kShadeTable).bytes;
    const auto light = assets_.read(kLightTable).bytes;
    require(
        shade.size() == kTableRows * kTableRowEntries &&
            light.size() == kTableRows * kTableRowEntries,
        "the palette's shade and light tables are not 32 rows of 256"
    );
    const auto composed = [this] {
        render();
        renderer::Surface frame;
        compose_match_layers(frame);
        return frame;
    };
    const auto point_at = [this](float x, float y) {
        update_pointer(x, y);
        return hovered_ ? std::string_view(match_hud_->layout.gadgets[*hovered_].common.name)
                        : std::string_view();
    };
    const auto click_at = [this](float x, float y) {
        float window_x = 0;
        float window_y = 0;
        if (!frame_to_window(sdl_.renderer, x, y, &window_x, &window_y))
            throw std::runtime_error(
                std::string("SDL_RenderCoordinatesToWindow: ") + SDL_GetError()
            );
        bool running = true;
        for (const auto type : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event event{};
            event.button.type = type;
            event.button.windowID = SDL_GetWindowID(sdl_.window);
            event.button.button = SDL_BUTTON_LEFT;
            event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.clicks = 1;
            event.button.x = window_x;
            event.button.y = window_y;
            dispatch_event(event, running);
        }
    };
    const auto hud_label = [this](std::string_view name) {
        for (const auto& gadget : match_hud_->layout.gadgets)
            if (gadget.common.name == name)
                if (const auto* label =
                        std::get_if<oa::ui::gui_layout::LabelFields>(&gadget.fields))
                    return label->text;
        return std::string();
    };
    const auto record_index = [this](std::string_view name) -> std::size_t {
        for (std::size_t index = 1; index < match_hud_->layout.gadgets.size(); ++index)
            if (match_hud_->layout.gadgets[index].common.name == name)
                return index;
        throw std::runtime_error("placed dialog check: no control " + std::string(name));
    };
    const auto control = [&](std::string_view name) {
        return match_hud_->layout.gadgets[record_index(name)].common;
    };
    // Each button underlines its quick key's glyph in its centred caption,
    // in the GUI palette's entry 2, on the HUD layer's row under the text:
    // the caption's top, (height - 1 - text height) / 2 into the button,
    // plus the text height (the 'I' glyph's and 2), less one. The caption
    // starts (width - 1 - text width) / 2 + 1 into the button. In English
    // each button takes the letter given. In another language its key is
    // the one its translated caption gives, a whole one-byte character of
    // it, or none, or the one its GUI file names, which the caption may
    // lack and which nothing then marks. A caption past ASCII is drawn
    // whole, its key underlined only where the font draws all of it, so
    // only a key in a caption of ASCII is measured here.
    const bool english = game_language() == nullptr;
    const auto check_quick_keys = [&](
                                      const std::string& what,
                                      std::initializer_list<std::pair<std::string_view, char>> keys
                                  ) {
        render();
        const auto& font = match_hud_->font;
        const auto remap = oa::remap_palette(match_hud_->gui_palette, match_palette_);
        const auto* colour = &match_palette_[remap[kUnderlineEntry] * oa::palette_entry_bytes];
        for (const auto& [name, english_key] : keys) {
            const auto& gadget = match_hud_->layout.gadgets[record_index(name)];
            const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
            require(button != nullptr, what + "'s " + std::string(name) + " is not a button");
            const char key = english ? english_key : static_cast<char>(button->quick_key);
            require(
                button->quick_key == key,
                what + "'s " + std::string(name) + " does not have the quick key " + key
            );
            if (key == '\0')
                continue;
            const std::string_view text = button->text;
            const auto at = oa::ui::frontend_renderer::quick_key_offset(text, key);
            require(
                at != std::string_view::npos || !english,
                what + "'s " + std::string(name) + " caption lacks its quick key"
            );
            if (at == std::string_view::npos ||
                std::any_of(text.begin(), text.end(), [](char byte) {
                    return static_cast<unsigned char>(byte) >= kFirstByteAboveAscii;
                }))
                continue;
            const auto text_width =
                static_cast<int32_t>(oa::formats::fnt::measure_text(font, text));
            const auto text_height = static_cast<int32_t>(oa::formats::fnt::line_height(font));
            const auto& record = gadget.common;
            const int32_t left =
                record.x + (record.width - 1 - text_width) / 2 + 1 +
                static_cast<int32_t>(oa::formats::fnt::measure_text(font, text.substr(0, at)));
            const int32_t right =
                left +
                static_cast<int32_t>(oa::formats::fnt::measure_text(font, text.substr(at, 1))) - 1;
            const int32_t row = record.y + (record.height - 1 - text_height) / 2 + text_height - 1;
            std::size_t wrong = 0;
            for (int32_t x = left; x <= right; ++x)
                if (!std::equal(colour, colour + 3, pixel(match_hud_cpu_, x, row)))
                    ++wrong;
            require(
                right >= left && wrong == 0,
                what + "'s " + std::string(name) + " does not underline its quick key " + key
            );
        }
    };
    // The focus marker's outline rings the focused record inside the panel's
    // root, over the HUD layer drawn while the panels take no keyboard.
    const auto check_focus = [&](const std::string& what, std::string_view name) {
        require(
            match_panels_keyboard_ && match_hud_focus_ == static_cast<int32_t>(record_index(name)),
            what + " does not give " + std::string(name) + " the focus"
        );
        match_panels_keyboard_ = false;
        render();
        auto unlit = match_hud_cpu_;
        match_panels_keyboard_ = true;
        render();
        const auto& record = control(name);
        const auto& root = match_hud_->layout.gadgets.front().common;
        std::size_t ring_pixels = 0;
        const auto differing = differing_focus(
            match_hud_cpu_,
            unlit,
            {record.x, record.y, record.width, record.height},
            {root.x, root.y, root.width, root.height},
            match_palette_,
            light,
            ring_pixels
        );
        require(
            ring_pixels > 0 && differing == 0,
            what + " rings " + std::string(name) + " wrongly at " + std::to_string(differing) +
                " pixels"
        );
    };
    for (const auto& [width, height] :
         {std::pair{kCanvasWidth, kCanvasHeight},
          std::pair{kSmallWindowWidth, kSmallWindowHeight},
          std::pair{kMediumWindowWidth, kMediumWindowHeight},
          std::pair{kDefaultWindowWidth, kDefaultWindowHeight}}) {
        const auto size = std::to_string(width) + 'x' + std::to_string(height);
        const auto on = " on the " + size + " window";
        if (match_)
            leave_match();
        if (!SDL_SetWindowSize(sdl_.window, width, height) || !SDL_SyncWindow(sdl_.window))
            throw std::runtime_error(std::string("SDL_SetWindowSize: ") + SDL_GetError());
        load(Screen::main_menu);
        start_benchmark_skirmish();
        require(
            match_layout_.width == width && match_layout_.height == height,
            "could not size the match canvas to " + size
        );
        show_match_pause_menu();
        // The pointer rests in the window's corner, clear of the panels.
        update_pointer(static_cast<float>(width - 1), static_cast<float>(height - 1));
        if (const auto viewer = match_view_player(); viewer < OA_PLAYER_COUNT) {
            auto& game = match_->state().game;
            for (int step = 0; step < kReadoutSettleSteps; ++step)
                oa::ui::hud::update_resource_readout(
                    game.resource_readout, game.players[viewer], game.tick
                );
        }
        const auto battlefield = Rect{
            match_layout_.left,
            match_layout_.top,
            match_layout_.battlefield_width(),
            match_layout_.battlefield_height()
        };
        const auto scaled = [this](int32_t value) {
            return static_cast<int32_t>(
                std::lround(static_cast<double>(value) * match_layout_.scale)
            );
        };
        // The in-game menu underlines its buttons' quick keys and, as it
        // gives the panels the keyboard, rings Resume, its default focus.
        check_quick_keys(
            "ARMOPT.GUI" + on,
            {{"LOADGAME", 'L'}, {"SAVEGAME", 'S'}, {"PREFS", 'O'}, {"EXIT", 'E'}, {"OK", 'R'}}
        );
        check_focus("ARMOPT.GUI" + on, "OK");
        // The paused battlefield with nothing over it but the paused title,
        // and the in-game menu in the side column.
        auto paused = composed();
        // A copy: the panels opened below replace the layout it comes from.
        const auto menu_root = match_hud_->layout.gadgets.front().common;
        const auto menu = oa::ui::display_layout::source_rect_to_canvas(
            match_layout_, menu_root.x, menu_root.y, menu_root.width, menu_root.height
        );
        // The loaded dialog's canvas rectangle: its root placed on the 640x480
        // screen as `placement` says, shown at the side column's scale placed
        // the same way on the canvas; over the battlefield only it differs
        // from `before`. With `face`, every pixel of it off its records and
        // the focus marker's rings is the BackTile face in the match palette.
        const auto placed = [&](renderer::Surface& before,
                                renderer::Surface& frame,
                                std::string_view layout,
                                uint32_t placement,
                                bool face) {
            const auto name = std::string(layout) + on;
            require(
                screen_ == Screen::match && match_paused_ && match_hud_ &&
                    match_hud_panel_ == layout,
                name + " is not open"
            );
            const auto& gadgets = match_hud_->layout.gadgets;
            const auto& root = gadgets.front().common;
            int16_t source_x = 0;
            int16_t source_y = 0;
            oa::ui::gui_input::place_root(
                source_x,
                source_y,
                root.width,
                root.height,
                placement | panel_flag::first_draw,
                kCanvasWidth,
                kCanvasHeight,
                kBattlefieldLeft
            );
            require(
                root.x == source_x && root.y == source_y,
                name + " is at " + std::to_string(root.x) + ',' + std::to_string(root.y) +
                    ", not " + std::to_string(source_x) + ',' + std::to_string(source_y)
            );
            int16_t canvas_x = 0;
            int16_t canvas_y = 0;
            oa::ui::gui_input::place_root(
                canvas_x,
                canvas_y,
                scaled(root.width),
                scaled(root.height),
                placement | panel_flag::first_draw,
                width,
                height,
                match_layout_.left
            );
            const Rect panel{canvas_x, canvas_y, scaled(root.width), scaled(root.height)};
            std::size_t outside = 0;
            for (int32_t y = battlefield.y; y < battlefield.y + battlefield.height; ++y)
                for (int32_t x = battlefield.x; x < battlefield.x + battlefield.width; ++x)
                    if (!inside(panel, x, y) && !same_pixel(before, frame, x, y))
                        ++outside;
            require(
                outside == 0,
                name + " changed " + std::to_string(outside) +
                    " battlefield pixels outside the panel"
            );
            const auto area =
                static_cast<std::size_t>(panel.width) * static_cast<std::size_t>(panel.height);
            require(
                differing_pixels(before, frame, panel) > area / 2,
                name + " does not show where it is placed"
            );
            if (!face)
                return panel;
            require(
                root.width >= tile_size && root.height >= tile_size,
                name + " is smaller than a BackTile frame"
            );
            const auto* focused = match_panels_keyboard_ && match_hud_focus_ > 0
                                      ? &gadgets[static_cast<std::size_t>(match_hud_focus_)].common
                                      : nullptr;
            const auto rings = static_cast<int32_t>(kFocusRingLevels.size());
            std::size_t compared = 0;
            std::size_t differing = 0;
            for (int32_t y = panel.y; y < panel.y + panel.height; ++y)
                for (int32_t x = panel.x; x < panel.x + panel.width; ++x) {
                    // The source pixel the panel's draw shows here.
                    const auto face_x = (x - panel.x) * root.width / panel.width;
                    const auto face_y = (y - panel.y) * root.height / panel.height;
                    const auto source = std::pair{root.x + face_x, root.y + face_y};
                    const auto on_record = [&](const auto& gadget) {
                        const auto& record = gadget.common;
                        return &gadget != &gadgets.front() && record.active != 0 &&
                               source.first >= record.x && source.first < record.x + record.width &&
                               source.second >= record.y &&
                               source.second < record.y + record.height;
                    };
                    if (std::any_of(gadgets.begin(), gadgets.end(), on_record))
                        continue;
                    if (focused != nullptr && source.first >= focused->x - rings &&
                        source.first < focused->x + focused->width + rings &&
                        source.second >= focused->y - rings &&
                        source.second < focused->y + focused->height + rings)
                        continue;
                    const auto tile_pixel =
                        back_tile_pixel(face_x, face_y, root.width, root.height, tile_size);
                    const auto& art = tiles[tile_pixel.frame];
                    const auto at = static_cast<std::size_t>(tile_pixel.y) * art.width +
                                    static_cast<std::size_t>(tile_pixel.x);
                    if (at >= art.coverage.size() || art.coverage[at] == 0)
                        continue;
                    const auto* colour =
                        &match_palette_[static_cast<std::size_t>(art.pixels[at]) * 4U];
                    ++compared;
                    if (!std::equal(colour, colour + 3, pixel(frame, x, y)))
                        ++differing;
                }
            require(
                compared > static_cast<std::size_t>(panel.width) * panel.height / 2,
                name + " has too few face pixels to compare"
            );
            require(
                differing == 0,
                name + " differs from its BackTile face at " + std::to_string(differing) + " of " +
                    std::to_string(compared) + " pixels"
            );
            return panel;
        };
        // The side column still shows the in-game menu as the paused frame
        // does, darkened when the dialog shades the panel below.
        const auto menu_under =
            [&](renderer::Surface& frame, std::string_view what, bool darkened) {
                const auto differing =
                    differing_under(frame, paused, menu, darkened, match_palette_, shade);
                require(
                    differing == 0,
                    std::string(what) + on + " shows the in-game menu " +
                        (darkened ? "darkened " : "") + "wrongly at " + std::to_string(differing) +
                        " of " + std::to_string(menu.width * menu.height) + " pixels"
                );
            };

        activate_pause_gadget("EXIT");
        auto exit_menu = composed();
        write_ppm(report_directory / ("native-match-exit-menu-" + size + ".ppm"), exit_menu);
        placed(
            paused,
            exit_menu,
            oa::data::defs::gui_path(kExitMenuLayout),
            panel_flag::beside_hud,
            true
        );
        menu_under(exit_menu, oa::data::defs::gui_path(kExitMenuLayout), true);
        // Restart takes its first letter as its caption is set; the focus
        // goes to the first control, Exit to Menu.
        check_quick_keys(
            std::string(oa::data::defs::gui_path(kExitMenuLayout)) + on, {{"RESTART", 'R'}}
        );
        check_focus(std::string(oa::data::defs::gui_path(kExitMenuLayout)) + on, "MAINMENU");

        activate_pause_gadget("EXITGAME");
        auto confirm = composed();
        write_ppm(report_directory / ("native-match-surrender-" + size + ".ppm"), confirm);
        const auto panel = placed(
            paused, confirm, oa::data::defs::gui_path(kConfirmLayout), panel_flag::beside_hud, true
        );
        menu_under(confirm, oa::data::defs::gui_path(kConfirmLayout), false);
        require(
            hud_label("TITLE") == game_text("Surrender this battle and exit to the system?"),
            "EXITGAME's confirmation reads \"" + hud_label("TITLE") + '"'
        );
        check_quick_keys(
            std::string(oa::data::defs::gui_path(kConfirmLayout)) + on,
            {{"CHOICE1", 'Y'}, {"CHOICE2", 'N'}}
        );
        check_focus(
            std::string(oa::data::defs::gui_path(kConfirmLayout)) + on,
            oa::ui::frontend::kExitConfirmDefault
        );
        // The pointer finds the choices where they show, and nothing beside them.
        const auto root = match_hud_->layout.gadgets.front().common;
        const auto shown_at = [&](const Rect& shown,
                                  const oa::ui::gui_layout::CommonFields& record) {
            return std::pair{
                static_cast<float>(
                    shown.x + (record.x - root.x) * shown.width / root.width +
                    record.width * shown.width / root.width / 2
                ),
                static_cast<float>(
                    shown.y + (record.y - root.y) * shown.height / root.height +
                    record.height * shown.height / root.height / 2
                )
            };
        };
        const auto [yes_x, yes_y] = shown_at(panel, control("CHOICE1"));
        const auto [no_x, no_y] = shown_at(panel, control("CHOICE2"));
        require(point_at(yes_x, yes_y) == "CHOICE1", "the pointer over Yes is not over it");
        require(
            point_at(static_cast<float>(panel.x - 1), yes_y).empty(),
            "the pointer left of the confirmation is over one of its controls"
        );
        require(point_at(no_x, no_y) == "CHOICE2", "the pointer over No is not over it");
        click_at(no_x, no_y);
        require(
            !exit_requested_ && match_paused_ &&
                match_hud_panel_ == oa::data::defs::gui_path(kInGameMenuLayout),
            "a click on No where it shows did not return to the in-game menu"
        );
        update_pointer(static_cast<float>(width - 1), static_cast<float>(height - 1));

        activate_pause_gadget("EXIT");
        activate_pause_gadget("MAINMENU");
        auto main_menu = composed();
        placed(
            paused,
            main_menu,
            oa::data::defs::gui_path(kConfirmLayout),
            panel_flag::beside_hud,
            true
        );
        menu_under(main_menu, "MAINMENU's confirmation", false);
        require(
            hud_label("TITLE") == game_text("Surrender this battle and return to main menu?"),
            "MAINMENU's confirmation reads \"" + hud_label("TITLE") + '"'
        );
        activate_pause_gadget("CHOICE2");

        activate_pause_gadget("EXIT");
        activate_pause_gadget("RESTART");
        auto restart = composed();
        placed(
            paused, restart, oa::data::defs::gui_path(kRestartLayout), panel_flag::beside_hud, false
        );
        menu_under(restart, oa::data::defs::gui_path(kRestartLayout), false);
        check_focus(std::string(oa::data::defs::gui_path(kRestartLayout)) + on, "Difficulty");
        activate_pause_gadget("CANCEL");

        // Asked by closing the window over the running match, the
        // confirmation opens over the side column as it was; the running
        // match gives its panels no keyboard, so nothing is ringed.
        resume_match_pause();
        auto running = composed();
        request_match_close();
        require(!match_panels_keyboard_, "the close request gave the panels the keyboard");
        auto closing = composed();
        placed(
            running, closing, oa::data::defs::gui_path(kConfirmLayout), panel_flag::beside_hud, true
        );
        const auto side = oa::ui::display_layout::source_rect_to_canvas(
            match_layout_, 0, menu_root.y, kBattlefieldLeft, kCanvasHeight - menu_root.y
        );
        const auto side_differing =
            differing_under(closing, running, side, false, match_palette_, shade);
        require(
            side_differing == 0,
            "the close request's confirmation" + on + " changed " + std::to_string(side_differing) +
                " pixels of the side column"
        );
        activate_pause_gadget("CHOICE2");
        require(!match_paused_, "No to the close request did not return to the match");

        // The removal question CONTROL.GUI asks in a multiplayer match hosted
        // here, over the tab menu: centred on the whole screen over its
        // BackTile face, CONTROL.GUI left as it is under it and shown again
        // once the question is answered.
        const Extension saved_extension = extension_;
        auto& world = match_->state();
        auto* my_info =
            oa::world_player_info(&world, &world.game.players[world.game.local_player_index]);
        require(my_info != nullptr, "the local player has no setup block");
        const auto saved_role = my_info->role;
        const auto restore = [&] {
            extension_ = saved_extension;
            my_info->role = saved_role;
        };
        try {
            extension_.state = [](void*, const Runtime&) -> uint32_t {
                return extension_state::multiplayer | extension_state::shared_match;
            };
            my_info->role = static_cast<uint8_t>(my_info->role | kHostRole);
            uint8_t other = OA_PLAYER_COUNT;
            for (uint8_t index = 0; index < OA_PLAYER_COUNT; ++index)
                if (index != world.game.local_player_index &&
                    world.game.players[index].in_use != 0) {
                    other = index;
                    break;
                }
            require(other != OA_PLAYER_COUNT, "the skirmish has no other player");
            auto before_menu = composed();
            toggle_team_menu();
            // The tab menu lies on the bottom bar over its BackTile face, as
            // the game places it from the screen's bottom edge: once, whole
            // at the bars' scale, its top row just above the bar, and the
            // battlefield above that as it was.
            {
                const auto name = "TABMENU.GUI" + on;
                require(
                    team_panel_open() &&
                        match_hud_panel_ == oa::data::defs::gui_path("TABMENU.GUI"),
                    "Tab did not open TABMENU.GUI" + on
                );
                const auto& gadgets = match_hud_->layout.gadgets;
                const auto& tab_root = gadgets.front().common;
                require(
                    tab_root.y + tab_root.height == kCanvasHeight,
                    name + " does not end on the screen's bottom edge"
                );
                auto menu_frame = composed();
                const auto shown = oa::ui::display_layout::source_panel_to_canvas(
                    match_layout_, tab_root.x, tab_root.y, tab_root.width, tab_root.height
                );
                require(
                    shown.y < match_layout_.bottom_bar_y() && shown.y + shown.height == height,
                    name + " does not lie on the bottom bar"
                );
                std::size_t stray = 0;
                for (int32_t y = battlefield.y; y < battlefield.y + battlefield.height; ++y)
                    for (int32_t x = battlefield.x; x < battlefield.x + battlefield.width; ++x)
                        if (!inside(shown, x, y) && !same_pixel(before_menu, menu_frame, x, y))
                            ++stray;
                require(
                    stray == 0,
                    name + " changed " + std::to_string(stray) +
                        " battlefield pixels above the bottom bar"
                );
                const auto* focused =
                    match_panels_keyboard_ && match_hud_focus_ > 0
                        ? &gadgets[static_cast<std::size_t>(match_hud_focus_)].common
                        : nullptr;
                const auto rings = static_cast<int32_t>(kFocusRingLevels.size());
                // The face the BackTile frames give each pixel of the root,
                // tiled as the panel's draw tiles them, a later tile over an
                // earlier one; a root lower than a frame shows the lower rows
                // of the top frames.
                std::vector<const uint8_t*> face(
                    static_cast<std::size_t>(tab_root.width) *
                        static_cast<std::size_t>(tab_root.height),
                    nullptr
                );
                for (const auto& placed_tile : oa::ui::gui_layout::skin_tiles(
                         tab_root.width, tab_root.height, tile_size, tile_size
                     )) {
                    const auto& art = tiles[placed_tile.frame];
                    for (int32_t ty = 0; ty < static_cast<int32_t>(art.height); ++ty)
                        for (int32_t tx = 0; tx < static_cast<int32_t>(art.width); ++tx) {
                            const auto fx = placed_tile.x + tx;
                            const auto fy = placed_tile.y + ty;
                            const auto at = static_cast<std::size_t>(ty) * art.width +
                                            static_cast<std::size_t>(tx);
                            if (fx < 0 || fy < 0 || fx >= tab_root.width || fy >= tab_root.height ||
                                at >= art.coverage.size() || art.coverage[at] == 0)
                                continue;
                            face
                                [static_cast<std::size_t>(fy) * tab_root.width +
                                 static_cast<std::size_t>(fx)] =
                                    &match_palette_[static_cast<std::size_t>(art.pixels[at]) * 4U];
                        }
                }
                std::size_t compared = 0;
                std::size_t differing = 0;
                for (int32_t y = tab_root.y; y < tab_root.y + tab_root.height; ++y)
                    for (int32_t x = tab_root.x; x < tab_root.x + tab_root.width; ++x) {
                        const auto on_record = [&](const auto& gadget) {
                            const auto& record = gadget.common;
                            return &gadget != &gadgets.front() && record.active != 0 &&
                                   x >= record.x && x < record.x + record.width && y >= record.y &&
                                   y < record.y + record.height;
                        };
                        if (std::any_of(gadgets.begin(), gadgets.end(), on_record))
                            continue;
                        if (focused != nullptr && x >= focused->x - rings &&
                            x < focused->x + focused->width + rings && y >= focused->y - rings &&
                            y < focused->y + focused->height + rings)
                            continue;
                        const auto* colour = face
                            [static_cast<std::size_t>(y - tab_root.y) * tab_root.width +
                             static_cast<std::size_t>(x - tab_root.x)];
                        if (colour == nullptr)
                            continue;
                        ++compared;
                        if (!std::equal(colour, colour + 3, pixel(match_hud_cpu_, x, y)))
                            ++differing;
                    }
                require(
                    compared > static_cast<std::size_t>(tab_root.width) * tab_root.height / 4,
                    name + " has too few face pixels to compare"
                );
                require(
                    differing == 0,
                    name + " differs from its BackTile face at " + std::to_string(differing) +
                        " of " + std::to_string(compared) + " pixels"
                );
                write_ppm(report_directory / ("native-match-tabmenu-" + size + ".ppm"), menu_frame);
            }
            click_team_panel("CONTROL");
            require(
                team_panel_open() && match_hud_panel_ == oa::data::defs::gui_path("CONTROL.GUI"),
                "the tab menu's CONTROL did not open CONTROL.GUI" + on
            );
            check_focus("CONTROL.GUI" + on, "OK");
            auto control_frame = composed();
            open_removal_question(other);
            auto question = composed();
            write_ppm(report_directory / ("native-match-removal-" + size + ".ppm"), question);
            const auto asked = placed(
                control_frame,
                question,
                oa::data::defs::gui_path(kConfirmLayout),
                panel_flag::centre,
                true
            );
            check_quick_keys("the removal question" + on, {{"CHOICE1", 'Y'}, {"CHOICE2", 'N'}});
            check_focus("the removal question" + on, "CHOICE1");
            // Off the question, the side column is as CONTROL.GUI left it:
            // on a 640x480 window the question lies over its right edge.
            const Rect column{0, 0, match_layout_.left, height};
            std::size_t column_differing = 0;
            for (int32_t y = column.y; y < column.y + column.height; ++y)
                for (int32_t x = column.x; x < column.x + column.width; ++x)
                    if (!inside(asked, x, y) && !same_pixel(question, control_frame, x, y))
                        ++column_differing;
            require(
                column_differing == 0,
                "the removal question" + on + " changed " + std::to_string(column_differing) +
                    " pixels of the side column off the question"
            );
            const auto question_root = match_hud_->layout.gadgets.front().common;
            const auto no = control("CHOICE2");
            const auto asked_no_x = static_cast<float>(
                asked.x + (no.x - question_root.x) * asked.width / question_root.width +
                no.width * asked.width / question_root.width / 2
            );
            const auto asked_no_y = static_cast<float>(
                asked.y + (no.y - question_root.y) * asked.height / question_root.height +
                no.height * asked.height / question_root.height / 2
            );
            require(
                point_at(asked_no_x, asked_no_y) == "CHOICE2",
                "the pointer over the question's No is not over it"
            );
            click_at(asked_no_x, asked_no_y);
            require(
                team_panel_open() && match_hud_panel_ == oa::data::defs::gui_path("CONTROL.GUI") &&
                    match_panels_keyboard_,
                "No did not return to CONTROL.GUI" + on
            );
            check_focus("CONTROL.GUI after the removal question" + on, "OK");
            click_team_panel("OK");
            require(!team_panel_open(), "OK did not close CONTROL.GUI" + on);
            std::cout << "placed dialog check: the removal question at " << asked.x << ','
                      << asked.y << " over its BackTile face and CONTROL.GUI" << on << '\n';
        } catch (...) {
            restore();
            throw;
        }
        restore();
        std::cout << "placed dialog check: EXITMENU.GUI and YESORNO.GUI at " << panel.x << ','
                  << panel.y << " over their BackTile faces and the in-game menu" << on << '\n';
    }
    leave_match();
    if (!SDL_SetWindowSize(sdl_.window, kDefaultWindowWidth, kDefaultWindowHeight) ||
        !SDL_SyncWindow(sdl_.window))
        throw std::runtime_error(std::string("SDL_SetWindowSize: ") + SDL_GetError());
}

void Runtime::check_load_save() {
    namespace dialogs = oa::ui::frontend_dialogs;
    if (!offers_saved_games()) {
        check_saved_games_unavailable();
        return;
    }
    const fs::path report_directory = "local/reports";
    fs::create_directories(report_directory);
    const auto load_bitmap =
        oa::ui::decoded::require(oa::decode_pcx(assets_.read(kLoadBitmap).bytes), kLoadBitmap);
    const auto save_bitmap =
        oa::ui::decoded::require(oa::decode_pcx(assets_.read(kSaveBitmap).bytes), kSaveBitmap);
    const auto authored_layout =
        oa::ui::gui_layout::parse(assets_.read(oa::data::defs::gui_path("loadgame.gui")).bytes);
    if (!authored_layout.ok() || authored_layout.layout->gadgets.empty())
        throw std::runtime_error("load/save check: LOADGAME.GUI does not parse");
    const auto header = authored_layout.layout->gadgets.front().common;
    bool running = true;
    const auto click = [&](int32_t x, int32_t y) {
        float window_x = 0;
        float window_y = 0;
        if (!frame_to_window(
                sdl_.renderer, static_cast<float>(x), static_cast<float>(y), &window_x, &window_y
            ))
            throw std::runtime_error(
                std::string("SDL_RenderCoordinatesToWindow: ") + SDL_GetError()
            );
        for (const auto type : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event event{};
            event.button.type = type;
            event.button.windowID = SDL_GetWindowID(sdl_.window);
            event.button.button = SDL_BUTTON_LEFT;
            event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.clicks = 1;
            event.button.x = window_x;
            event.button.y = window_y;
            dispatch_event(event, running);
            // A player's press and release come at least a frame apart.
            if (type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                tick_screen_packages();
        }
    };
    const auto press_key = [&](SDL_Keycode code) {
        SDL_Event key{};
        key.type = SDL_EVENT_KEY_DOWN;
        key.key.key = code;
        dispatch_event(key, running);
    };
    const auto record = [&](std::string_view name) {
        const auto* gadget = widget(name);
        if (gadget == nullptr)
            throw std::runtime_error("load/save check: LOADGAME.GUI has no " + std::string(name));
        const auto origin = panel_origin();
        return Rect{
            origin.x + gadget->common.x,
            origin.y + gadget->common.y,
            gadget->common.width,
            gadget->common.height
        };
    };
    const auto click_record = [&](std::string_view name) {
        const auto rect = record(name);
        click(rect.x + rect.width / 2, rect.y + rect.height / 2);
    };
    // GAMES row `index` on the frame, the list's first row showing.
    const auto games_row = [&](int32_t index) {
        const auto games = record("GAMES");
        const auto* fields =
            std::get_if<oa::ui::gui_layout::ListBoxFields>(&widget("GAMES")->fields);
        const int32_t step = fields != nullptr && fields->item_height != 0
                                 ? fields->item_height
                                 : oa::formats::fnt::line_height(resources_.font) + 1;
        return Rect{games.x + 2, games.y + 2 + index * step, games.width - 3, step};
    };
    const auto label_text = [&](std::string_view name) {
        const auto* gadget = widget(name);
        const auto* label = gadget != nullptr
                                ? std::get_if<oa::ui::gui_layout::LabelFields>(&gadget->fields)
                                : nullptr;
        return label != nullptr ? label->text : std::string{};
    };
    // The saves as the dialogs list them, newest first, read back through
    // the save directory and each Summary.
    const oa::ui::frontend::SaveRoots check_roots = save_roots();
    const auto listed_saves = [&] {
        oa::ui::frontend::SaveDialogContext saves;
        saves.files = oa::ui::frontend::savegame_host_files(&check_roots);
        saves.reader = oa::ui::frontend::savegame_persist_reader(&check_roots);
        // The list is read from the context; its length is not needed.
        std::ignore = oa::ui::frontend::savegame_build_list(saves);
        return saves;
    };

    // The saved side and radar image of GAMES row `index`.
    struct SavedPreview {
        int32_t side = -1;
        oa::present::SurfaceBuffer radar;
    };

    const auto saved_preview = [&](std::size_t index) {
        auto saves = listed_saves();
        if (index >= saves.list.entries.size())
            throw std::runtime_error("load/save check: GAMES has no row " + std::to_string(index));
        char path[oa::ui::frontend::kSaveFileBytes * 2];
        oa::ui::frontend::savegame_entry_path(saves, index, path, sizeof path);
        const auto& reader = saves.reader;
        void* bank = reader.open(reader.context, path);
        if (bank == nullptr)
            throw std::runtime_error(std::string("load/save check: cannot read ") + path);
        SavedPreview preview;
        preview.side =
            reader.get_int(reader.context, bank, oa::data::persist::save_key::side, preview.side);
        if (!reader.load_radar(reader.context, bank, preview.radar))
            preview.radar = {};
        reader.close(reader.context, bank);
        return preview;
    };
    // The previewed save's side name beside Side, and its radar image over
    // RADAR in the palette the dialog is drawn in.
    const auto check_preview =
        [&](const SavedPreview& saved, const oa::PaletteBytes& palette, const std::string& what) {
            const auto side = label_text("SIDE");
            if (side != saved_side_name(saved.side))
                throw std::runtime_error(
                    "load/save check: " + what + " shows side \"" + side + "\" for saved side " +
                    std::to_string(saved.side)
                );
            if (saved.radar.pixels.empty())
                throw std::runtime_error("load/save check: " + what + " save holds no radar image");
            const auto* radar = widget("RADAR");
            if (radar == nullptr || radar->common.active == 0)
                throw std::runtime_error("load/save check: " + what + " hides RADAR");
            const auto reach = cursor_reach(pointer_x_, pointer_y_);
            if (const auto differing =
                    radar_mismatches(surface_, record("RADAR"), saved.radar, palette, reach);
                differing != 0)
                throw std::runtime_error(
                    "load/save check: " + what + " RADAR does not show the saved radar image (" +
                    std::to_string(differing) + " pixels differ)"
                );
        };
    // The files the dialogs list: the saves folder's, and those of the
    // folders that held saved games before while they are there.
    std::size_t save_files = 0;
    std::vector<fs::path> listed_folders{check_roots.saves};
    listed_folders.insert(
        listed_folders.end(), check_roots.earlier.begin(), check_roots.earlier.end()
    );
    for (const fs::path& listed_folder : listed_folders) {
        std::error_code listing;
        for (const auto& entry : fs::directory_iterator(listed_folder, listing))
            if (entry.is_regular_file())
                ++save_files;
    }

    // With no save listed, MSGBOX.GUI says so over the load dialog just
    // opened; its OK at the drawn position closes it and leaves the dialog up.
    const auto close_no_saves_message = [&](const std::string& where) {
        const auto* message = dialogs::dialog_kind() == dialogs::DialogKind::message_box
                                  ? dialogs::dialog_resources()
                                  : nullptr;
        if (message == nullptr || message->layout.gadgets.empty())
            throw std::runtime_error(
                "load/save check: no message says there is no save to load " + where
            );
        const auto& gadgets = message->layout.gadgets;
        const auto ok = std::find_if(gadgets.begin(), gadgets.end(), [](const auto& gadget) {
            return gadget.common.name == "OK";
        });
        if (ok == gadgets.end())
            throw std::runtime_error("load/save check: the no-saves message has no OK");
        const auto& root = gadgets.front().common;
        click(
            root.x + ok->common.x + ok->common.width / 2,
            root.y + ok->common.y + ok->common.height / 2
        );
        if (dialogs::dialog_count() != 0 || screen_ != Screen::load_game)
            throw std::runtime_error(
                "load/save check: OK did not close the no-saves message over the dialog " + where
            );
    };
    // CANCEL at its drawn position, then Escape, each leave the load dialog
    // `open` opens for the screen `returned` finds, with saves listed or,
    // when `empty`, with none; the dialog does not come back on the next
    // frame. Before CANCEL is clicked, a press on it, and with saves one on
    // LOAD, do nothing while held or once released away from the button.
    const auto check_load_leaves = [&](
                                       auto&& open, auto&& returned, bool empty, std::string where
                                   ) {
        std::optional<HeldSaves> held;
        if (empty)
            held.emplace(check_roots);
        where += empty ? " with no save" : " with saves";
        const auto in_dialog = [&] {
            return screen_ == Screen::load_game && !save_dialog_open() &&
                   dialogs::dialog_count() == 0;
        };
        for (const bool by_escape : {false, true}) {
            open();
            if (screen_ != Screen::load_game || save_dialog_open())
                throw std::runtime_error("load/save check: the load dialog did not open " + where);
            if (empty)
                close_no_saves_message(where);
            if (by_escape) {
                press_key(SDLK_ESCAPE);
            } else {
                check_press_released_away("CANCEL", in_dialog, "in the load dialog " + where);
                if (!empty)
                    check_press_released_away("LOAD", in_dialog, "in the load dialog " + where);
                click_record("CANCEL");
            }
            tick_screen_packages();
            if (screen_ == Screen::load_game || dialogs::dialog_count() != 0 || !returned())
                throw std::runtime_error(
                    std::string("load/save check: ") + (by_escape ? "Escape" : "CANCEL") +
                    " did not leave the load dialog " + where
                );
        }
    };

    // Single Player's LOAD GAME: centred on the 640x480 frame over SINGLE.GUI.
    exercise_click(menu::resource_name(menu::Button::single_player));
    if (screen_ != Screen::single_player)
        throw std::runtime_error("load/save check did not reach SINGLE.GUI");
    const auto single_palette = screen_palette();
    auto single = frame_without_cursor();
    exercise_click(entry::resource_name(entry::Button::load_game));
    if (screen_ != Screen::load_game || save_dialog_open())
        throw std::runtime_error("LOAD GAME did not open the load dialog");
    tick_screen_packages();
    if (dialogs::dialog_kind() == dialogs::DialogKind::message_box) {
        if (save_files != 0)
            throw std::runtime_error("load/save check: the load dialog found no save to list");
        rebuild_surface();
        write_ppm(report_directory / "native-loadsave-load-empty.ppm", surface_);
        auto context = screen_context();
        if (!dialogs::dialog_click(&context, "OK") || dialogs::dialog_count() != 0)
            throw std::runtime_error("load/save check: OK did not close the no-saves message");
    } else if (save_files == 0) {
        throw std::runtime_error("load/save check: no message says there is no save to load");
    }
    rebuild_surface();
    write_ppm(report_directory / "native-loadsave-load-frontend.ppm", surface_);
    const oa::ui::display_layout::Point centred{
        (kCanvasWidth - header.width) / 2, (kCanvasHeight - header.height) / 2
    };
    const auto panel = check_backdrop_panel(
        surface_,
        resources_.layout,
        load_bitmap,
        single_palette,
        centred,
        cursor_reach(pointer_x_, pointer_y_),
        "load/save check: the Single Player load"
    );
    if (surface_.width != single.width || surface_.height != single.height)
        throw std::runtime_error(
            "load/save check: the load dialog left the Single Player frame size"
        );
    const Rect whole{0, 0, static_cast<int32_t>(single.width), static_cast<int32_t>(single.height)};
    if (brightness_outside(surface_, whole, panel) * 4 >
        brightness_outside(single, whole, panel) * 3)
        throw std::runtime_error("load/save check: the load dialog did not darken SINGLE.GUI");
    click_record("CANCEL");
    if (screen_ != Screen::single_player)
        throw std::runtime_error(
            "load/save check: CANCEL at its drawn position did not leave the load dialog"
        );
    std::cout << "load/save check: load dialog at " << panel.x << ',' << panel.y
              << " over Single Player\n";
    const auto open_single_load = [&] {
        exercise_click(entry::resource_name(entry::Button::load_game));
        tick_screen_packages();
    };
    const auto on_single = [&] { return screen_ == Screen::single_player; };
    check_load_leaves(open_single_load, on_single, true, "over Single Player");

    // A save as the game writes one, with a radar image in its Summary: LOAD
    // GAME lists it and, when its row is chosen, shows that image stretched
    // over RADAR and "Core" beside Side.
    const auto radar_picture = check_radar_picture();
    write_radar_check_save(check_roots.saves, radar_picture);
    std::optional<std::size_t> radar_row;
    {
        const auto saves = listed_saves();
        for (std::size_t index = 0; index < saves.list.entries.size(); ++index)
            if (std::string_view(saves.list.entries[index].file.data()) == kCheckRadarSaveFile)
                radar_row = index;
    }
    if (!radar_row)
        throw std::runtime_error("load/save check: the radar check save is not listed");
    exercise_click(entry::resource_name(entry::Button::load_game));
    if (screen_ != Screen::load_game || save_dialog_open())
        throw std::runtime_error("load/save check: LOAD GAME did not list the radar check save");
    tick_screen_packages();
    {
        const auto row = games_row(static_cast<int32_t>(*radar_row));
        click(row.x + 8, row.y + row.height / 2);
    }
    tick_screen_packages();
    rebuild_surface();
    write_ppm(report_directory / "native-loadsave-load-radar.ppm", surface_);
    SavedPreview radar_save;
    radar_save.side = kCheckRadarSide;
    radar_save.radar = check_radar_picture();
    check_preview(radar_save, single_palette, "LOAD GAME over Single Player");
    if (saved_preview(*radar_row).radar.pixels != radar_picture.pixels)
        throw std::runtime_error("load/save check: the radar check save reads back changed");
    click_record("CANCEL");
    if (screen_ != Screen::single_player)
        throw std::runtime_error("load/save check: CANCEL did not leave the second load dialog");
    check_load_leaves(open_single_load, on_single, false, "over Single Player");

    // A paused skirmish: the save and load dialogs centred on the match frame.
    exercise_click(entry::resource_name(entry::Button::skirmish));
    state_.player_count = 2;
    if (map_player_capacity() < 2)
        throw std::runtime_error("load/save check map lacks two start positions");
    exercise_click(skirmish::resource_name(skirmish::Button::start));
    if (screen_ != Screen::match || !match_)
        throw std::runtime_error("load/save check Start did not enter a match");
    show_match_pause_menu();

    struct Paused {
        renderer::Surface frame;
        Rect options;
        oa::PaletteBytes palette{};
    };

    const auto open_over_match = [&](std::string_view gadget, bool save) {
        // The HUD metrics panel eases the shown stores a step each frame; settled, the
        // frames before and under the dialog differ only where it draws.
        auto& game = match_->state().game;
        if (const auto viewer = match_view_player(); viewer < OA_PLAYER_COUNT)
            for (int step = 0; step < kReadoutSettleSteps; ++step)
                oa::ui::hud::update_resource_readout(
                    game.resource_readout, game.players[viewer], game.tick
                );
        render();
        Paused paused;
        compose_match_layers(paused.frame);
        const auto& options = match_hud_->layout.gadgets.front().common;
        const auto shaded = oa::ui::display_layout::source_rect_to_canvas(
            match_layout_, options.x, options.y, options.width, options.height
        );
        paused.options = {shaded.x, shaded.y, shaded.width, shaded.height};
        paused.palette = match_palette_;
        activate_pause_gadget(gadget);
        if (screen_ != Screen::load_game || save_dialog_open() != save)
            throw std::runtime_error(
                "load/save check: " + std::string(gadget) + " did not open its dialog"
            );
        tick_screen_packages();
        return paused;
    };
    const auto present = [&](const Paused& paused, const char* role) {
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        if (presented.width != paused.frame.width || presented.height != paused.frame.height ||
            surface_.width != paused.frame.width || surface_.height != paused.frame.height)
            throw std::runtime_error(
                std::string("load/save check: the ") + role + " dialog left the match frame size"
            );
    };
    const auto check_over_match = [&](Paused& paused,
                                      const oa::Image& bitmap,
                                      oa::ui::display_layout::Point expected,
                                      const char* role) {
        const auto reach = cursor_reach(pointer_x_, pointer_y_);
        const auto shown = check_backdrop_panel(
            surface_,
            resources_.layout,
            bitmap,
            paused.palette,
            expected,
            reach,
            std::string("load/save check: the ") + role
        );
        if (differing_pixels_outside(paused.frame, surface_, {shown, paused.options, reach}) != 0)
            throw std::runtime_error(
                std::string("load/save check: the ") + role +
                " dialog changed the match outside its panels"
            );
        if (brightness_outside(surface_, paused.options, shown) * 4 >
            brightness_outside(paused.frame, paused.options, shown) * 3)
            throw std::runtime_error(
                std::string("load/save check: the ") + role +
                " dialog did not darken the options panel"
            );
        return shown;
    };
    // Where LOADGAME.GUI's root is centred on a frame of the window's size.
    const auto centred_on = [&](const Paused& paused) {
        return oa::ui::display_layout::Point{
            (static_cast<int32_t>(paused.frame.width) - header.width) / 2,
            (static_cast<int32_t>(paused.frame.height) - header.height) / 2
        };
    };
    const auto count_save_files = [&] {
        std::size_t count = 0;
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(check_roots.saves, error))
            if (entry.is_regular_file())
                ++count;
        return count;
    };
    const auto type_name = [&](const char* name) {
        for (int erase = 0; erase < 32; ++erase)
            press_key(SDLK_BACKSPACE);
        SDL_Event text{};
        text.type = SDL_EVENT_TEXT_INPUT;
        text.text.text = name;
        dispatch_event(text, running);
    };
    // A click on the name field only gives it the keys: it neither saves
    // under the name it holds nor closes the dialog. A press on OK, CANCEL or
    // DELETE does nothing while held or once released away from the button.
    // CANCEL then leaves without a save.
    {
        const auto files = count_save_files();
        std::ignore = open_over_match("SAVEGAME", true);
        type_name(kCheckPressedName);
        click_record("GAMENAME");
        const auto in_save_dialog = [&] {
            return screen_ == Screen::load_game && save_dialog_open() &&
                   count_save_files() == files && dialogs::dialog_count() == 0;
        };
        if (!in_save_dialog())
            throw std::runtime_error(
                "load/save check: a click on the save dialog's name field saved or closed it"
            );
        // The saves written above are listed, so DELETE shows.
        for (const auto* record_name : {"LOAD", "CANCEL", "DELETE"})
            check_press_released_away(
                record_name, in_save_dialog, "in the save dialog over the match"
            );
        click_record("CANCEL");
        if (screen_ != Screen::match || save_dialog_open())
            throw std::runtime_error(
                "load/save check: CANCEL at its drawn position did not close the save dialog"
            );
        if (count_save_files() != files)
            throw std::runtime_error("load/save check: CANCEL wrote a save");
    }
    Rect save_panel{};
    // Each save holds the radar image the match shows as it is saved, and
    // the local player's side.
    const auto check_saved_radar = [&](const char* name, const std::vector<uint8_t>& shown) {
        const auto reader = oa::ui::frontend::savegame_persist_reader(&check_roots);
        const auto path = std::string(oa::ui::frontend::kSaveDirectory) + '\\' + name + '.' +
                          std::string(oa::ui::frontend::kSaveExtension);
        void* bank = reader.open(reader.context, path.c_str());
        if (bank == nullptr)
            throw std::runtime_error("load/save check: cannot read " + path);
        oa::present::SurfaceBuffer saved;
        const bool radar = reader.load_radar(reader.context, bank, saved);
        const auto side =
            reader.get_int(reader.context, bank, oa::data::persist::save_key::side, -1);
        reader.close(reader.context, bank);
        const auto& game = match_->state().game;
        if (!radar || saved.surface.width != game.radar_width ||
            saved.surface.height != game.radar_height || saved.pixels != shown)
            throw std::runtime_error(
                "load/save check: " + path + " does not hold the radar image the match shows"
            );
        const auto* info = oa::world_player_info(
            &match_->state(), oa::world_player(&match_->state(), match_local_player_)
        );
        if (info == nullptr || side != info->side)
            throw std::runtime_error(
                "load/save check: " + path + " does not hold the local player's side"
            );
    };
    // The first save is written by Return at the end of its name, the second
    // by OK at its drawn position.
    for (const auto* name : {kCheckSaveName, kCheckSecondSaveName}) {
        auto paused = open_over_match("SAVEGAME", true);
        type_name(name);
        present(paused, "save");
        if (name == kCheckSaveName) {
            write_ppm(report_directory / "native-loadsave-save-match.ppm", surface_);
            save_panel = check_over_match(paused, save_bitmap, centred_on(paused), "save");
        }
        const auto* final_image = radar_state_.surfaces.final_image;
        if (radar_state_.built_for != &match_->state() || final_image == nullptr)
            throw std::runtime_error("load/save check: the match has no radar image to save");
        std::vector<uint8_t> shown;
        for (int32_t y = 0; y < final_image->height; ++y) {
            const auto* line =
                final_image->pixels + static_cast<std::ptrdiff_t>(y) * final_image->pitch;
            shown.insert(shown.end(), line, line + final_image->width);
        }
        const bool by_return = name == kCheckSaveName;
        if (by_return)
            press_key(SDLK_RETURN);
        else
            click_record("LOAD");
        if (screen_ != Screen::match || save_dialog_open())
            throw std::runtime_error(
                std::string("load/save check: ") + (by_return ? "Return" : "OK") +
                " did not save and close the save dialog"
            );
        check_saved_radar(name, shown);
    }

    // Opened again, the save dialog previews its first save: the radar image
    // saved with it and the name of the side it was saved on.
    {
        auto reopened = open_over_match("SAVEGAME", true);
        present(reopened, "save");
        write_ppm(report_directory / "native-loadsave-save-preview.ppm", surface_);
        check_preview(saved_preview(0), reopened.palette, "the save dialog");
        click_record("CANCEL");
        if (screen_ != Screen::match || save_dialog_open())
            throw std::runtime_error("load/save check: CANCEL did not close the save dialog");
    }

    // The load dialog centred on the match frame; a click on the second
    // GAMES row moves the lit row there.
    auto paused = open_over_match("LOADGAME", false);
    const auto match_centred = centred_on(paused);
    present(paused, "match load");
    auto listed = surface_;
    const auto row = games_row;
    click(row(1).x + 8, row(1).y + row(1).height / 2);
    tick_screen_packages();
    present(paused, "match load");
    if (brightness(surface_, row(1)) <= brightness(listed, row(1)) ||
        brightness(surface_, row(0)) >= brightness(listed, row(0)))
        throw std::runtime_error(
            "load/save check: a click on the second GAMES row did not light it"
        );
    write_ppm(report_directory / "native-loadsave-load-match.ppm", surface_);
    check_preview(saved_preview(1), paused.palette, "the load dialog over the match");
    const auto load_panel = check_over_match(paused, load_bitmap, match_centred, "match load");
    click_record("CANCEL");
    if (screen_ != Screen::match || !match_paused_)
        throw std::runtime_error("load/save check: CANCEL did not return to the paused match");
    std::cout << "load/save check: save dialog at " << save_panel.x << ',' << save_panel.y
              << ", load dialog at " << load_panel.x << ',' << load_panel.y << " on the "
              << paused.frame.width << 'x' << paused.frame.height << " match frame\n";

    // Escape leaves the load dialog as CANCEL does, for the paused match.
    std::ignore = open_over_match("LOADGAME", false);
    press_key(SDLK_ESCAPE);
    if (screen_ != Screen::match || !match_paused_ || !match_hud_ ||
        match_hud_panel_ != oa::data::defs::gui_path(kInGameMenuLayout))
        throw std::runtime_error(
            "load/save check: Escape did not return the load dialog to the in-game menu"
        );
    {
        const auto open_match_load = [&] { std::ignore = open_over_match("LOADGAME", false); };
        const auto on_menu = [&] {
            return screen_ == Screen::match && match_paused_ && match_hud_ &&
                   match_hud_panel_ == oa::data::defs::gui_path(kInGameMenuLayout);
        };
        for (const bool empty : {false, true})
            check_load_leaves(open_match_load, on_menu, empty, "over the in-game menu");
    }
    // Return starts the selected save, as OK does.
    {
        std::optional<std::size_t> second_row;
        const auto saves = listed_saves();
        for (std::size_t index = 0; index < saves.list.entries.size(); ++index)
            if (std::string_view(saves.list.entries[index].file.data()) ==
                std::string(kCheckSecondSaveName) + '.' +
                    std::string(oa::ui::frontend::kSaveExtension))
                second_row = index;
        if (!second_row)
            throw std::runtime_error("load/save check: the save written by OK is not listed");
        std::ignore = open_over_match("LOADGAME", false);
        const auto saved_row = games_row(static_cast<int32_t>(*second_row));
        click(saved_row.x + 8, saved_row.y + saved_row.height / 2);
        press_key(SDLK_RETURN);
        const auto loaded = std::string("Loaded ") + kCheckSecondSaveName + '.';
        if (screen_ != Screen::match || status_.rfind(loaded, 0) != 0)
            throw std::runtime_error(
                "load/save check: Return did not start the selected save (" + status_ + ")"
            );
    }

    // At every window size, from 640x480 to 1920x1080, the side column drawn
    // once or twice its size, both dialogs open centred on the screen over the
    // paused match.
    for (const auto& [width, height] :
         {std::pair{kCanvasWidth, kCanvasHeight},
          std::pair{kLargeWindowWidth, kLargeWindowHeight},
          std::pair{kDefaultWindowWidth, kDefaultWindowHeight}}) {
        const auto size = std::to_string(width) + 'x' + std::to_string(height);
        if (match_)
            leave_match();
        if (!SDL_SetWindowSize(sdl_.window, width, height) || !SDL_SyncWindow(sdl_.window))
            throw std::runtime_error(std::string("SDL_SetWindowSize: ") + SDL_GetError());
        load(Screen::main_menu);
        start_benchmark_skirmish();
        if (match_layout_.width != width || match_layout_.height != height)
            throw std::runtime_error("load/save check: could not size the match to " + size);
        show_match_pause_menu();
        // The pointer rests in the window's corner, clear of the panels.
        update_pointer(static_cast<float>(width - 1), static_cast<float>(height - 1));
        for (const auto& [gadget, save] :
             {std::pair{"SAVEGAME", true}, std::pair{"LOADGAME", false}}) {
            const auto role = std::string(save ? "save" : "load") + " dialog on " + size;
            auto at_size = open_over_match(gadget, save);
            present(at_size, role.c_str());
            // Centred on the window itself, whatever the side column's scale.
            const oa::ui::display_layout::Point expected{
                (width - header.width) / 2, (height - header.height) / 2
            };
            const auto origin = panel_origin();
            if (origin.x != expected.x || origin.y != expected.y)
                throw std::runtime_error(
                    "load/save check: the " + role + " is at " + std::to_string(origin.x) + ',' +
                    std::to_string(origin.y) + ", not centred at " + std::to_string(expected.x) +
                    ',' + std::to_string(expected.y)
                );
            std::ignore =
                check_over_match(at_size, save ? save_bitmap : load_bitmap, expected, role.c_str());
            press_key(SDLK_ESCAPE);
            if (screen_ != Screen::match || !match_paused_ || save_dialog_open())
                throw std::runtime_error("load/save check: Escape did not close the " + role);
        }
        std::cout << "load/save check: both dialogs centred at " << (width - header.width) / 2
                  << ',' << (height - header.height) / 2 << " on the " << size << " window\n";
    }
}

void Runtime::check_end_panel_load_cancel() {
    namespace dialogs = oa::ui::frontend_dialogs;
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("end panel load check: " + what);
    };
    if (screen_ != Screen::campaign_end || endgame_world() == nullptr)
        fail("the check does not start on ENDMSN.GUI with a finished game");
    const auto* load_game = widget("LoadGame");
    if (load_game == nullptr || load_game->common.active == 0)
        fail("ENDMSN.GUI does not offer Load Game");
    const auto mission = selected_mission_index_;
    bool running = true;
    // A press and a release at a point on the screen, a frame apart.
    const auto click = [&](int32_t x, int32_t y) {
        float window_x = static_cast<float>(x);
        float window_y = static_cast<float>(y);
        // Headless, the events carry the screen's own coordinates.
        if (sdl_.renderer != nullptr &&
            !frame_to_window(
                sdl_.renderer, static_cast<float>(x), static_cast<float>(y), &window_x, &window_y
            ))
            fail(std::string("SDL_RenderCoordinatesToWindow: ") + SDL_GetError());
        for (const auto type : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event event{};
            event.button.type = type;
            event.button.windowID = sdl_.window != nullptr ? SDL_GetWindowID(sdl_.window) : 0;
            event.button.button = SDL_BUTTON_LEFT;
            event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.clicks = 1;
            event.button.x = window_x;
            event.button.y = window_y;
            dispatch_event(event, running);
            if (type == SDL_EVENT_MOUSE_BUTTON_DOWN)
                tick_screen_packages();
        }
    };
    const auto click_gadget = [&](std::string_view name) {
        const auto* gadget = widget(name);
        if (gadget == nullptr)
            fail("the screen has no " + std::string(name));
        const auto origin = panel_origin();
        click(
            origin.x + gadget->common.x + gadget->common.width / 2,
            origin.y + gadget->common.y + gadget->common.height / 2
        );
    };
    for (const bool empty : {false, true}) {
        std::optional<HeldSaves> held;
        if (empty)
            held.emplace(save_roots());
        const std::string saves = empty ? " with no save" : " with saves";
        for (const bool by_escape : {false, true}) {
            const std::string way = by_escape ? "Escape" : "CANCEL";
            // The panel takes the pointer once its stat bars have run.
            if (!step_end_screen_to_panel().panel)
                fail("ENDMSN.GUI did not show its panel" + saves);
            click_gadget("LoadGame");
            tick_screen_packages();
            if (screen_ != Screen::load_game || save_dialog_open())
                fail("Load Game did not open the load dialog" + saves);
            if (empty) {
                const auto* message = dialogs::dialog_kind() == dialogs::DialogKind::message_box
                                          ? dialogs::dialog_resources()
                                          : nullptr;
                if (message == nullptr)
                    fail("no message says there is no save to load");
                auto context = screen_context();
                if (!dialogs::dialog_click(&context, "OK") || dialogs::dialog_count() != 0)
                    fail("OK did not close the no-saves message");
            } else if (dialogs::dialog_count() != 0) {
                fail("a message opened over the load dialog" + saves);
            }
            if (by_escape) {
                SDL_Event key{};
                key.type = SDL_EVENT_KEY_DOWN;
                key.key.key = SDLK_ESCAPE;
                dispatch_event(key, running);
            } else {
                // A press on CANCEL does nothing while held or once released
                // away from it; the click follows.
                check_press_released_away(
                    "CANCEL",
                    [&] {
                        return screen_ == Screen::load_game && !save_dialog_open() &&
                               dialogs::dialog_count() == 0;
                    },
                    "in the load dialog over ENDMSN.GUI" + saves
                );
                click_gadget("CANCEL");
            }
            tick_screen_packages();
            if (!step_end_screen_to_panel().panel)
                fail(way + " did not return the load dialog to ENDMSN.GUI's panel" + saves);
            load_game = widget("LoadGame");
            if (screen_ != Screen::campaign_end || endgame_world() == nullptr ||
                load_game == nullptr || load_game->common.active == 0 ||
                dialogs::dialog_count() != 0)
                fail(way + " did not return the load dialog to ENDMSN.GUI" + saves);
            if (selected_mission_index_ != mission)
                fail(way + " changed the mission ENDMSN.GUI had chosen" + saves);
        }
    }
    std::cout << "end panel load check: CANCEL and Escape return the load dialog to ENDMSN.GUI, "
                 "with saves and with none\n";
}

void Runtime::check_press_released_away(
    std::string_view name, const std::function<bool()>& unchanged, const std::string& where
) {
    const auto fail = [&](const std::string& what) {
        throw std::runtime_error(std::string(name) + ' ' + what + ' ' + where);
    };
    // A frame drawn shows the records the screen's setup left shown.
    rebuild_surface();
    const auto* gadget = widget(name);
    if (gadget == nullptr || gadget->common.active == 0)
        fail("is not shown");
    const auto index = static_cast<std::size_t>(gadget - resources_.layout.gadgets.data());
    const auto origin = panel_origin();
    const Rect button{
        origin.x + gadget->common.x,
        origin.y + gadget->common.y,
        gadget->common.width,
        gadget->common.height
    };
    const oa::ui::display_layout::Point centre{
        button.x + button.width / 2, button.y + button.height / 2
    };
    constexpr oa::ui::display_layout::Point corner{0, 0};
    send_check_pointer(SDL_EVENT_MOUSE_MOTION, centre, 0);
    auto before = frame_without_cursor();
    send_check_pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, centre, SDL_BUTTON_LEFT);
    for (int frame = 0; frame < kHeldPressFrames; ++frame)
        tick_screen_packages();
    if (!unchanged())
        fail("acted on its press");
    auto held = frame_without_cursor();
    if (differing_pixels(before, held, button) == 0)
        fail("is not drawn pressed while held");
    send_check_pointer(SDL_EVENT_MOUSE_MOTION, corner, 0);
    if (hovered_ == index)
        fail("is under the screen's corner");
    auto away = frame_without_cursor();
    if (differing_pixels(before, away, button) != 0)
        fail("stays drawn pressed once the pointer leaves it");
    send_check_pointer(SDL_EVENT_MOUSE_BUTTON_UP, corner, SDL_BUTTON_LEFT);
    tick_screen_packages();
    if (!unchanged())
        fail("acted on a release away from it");
}

} // namespace oa::app

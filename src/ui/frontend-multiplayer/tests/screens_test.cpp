// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Navigation through the multiplayer screens with the installed game's GUI
// data and the loopback network: SELPROV -> TCP -> SELGAME -> NEWMULTI ->
// BATTLEROOM, the battleroom dialogs, and joining the hosted game as a client.
// The battle room's rows, option lamps, doors and sliders are checked pixel
// by pixel against the installed game's art, START against a real pointer
// click, and the sliders and RESTRICT2's count sliders against real pointer
// presses, drags and holds. The lists' scroll bars on SELPROV, SELMAP,
// SELGAME and RESTRICT2 are checked against their art and against the
// pointer, the keys and the wheel. The map pictures of SELMAP and VIEWMAP are
// checked pixel by pixel against the installed maps' minimaps.
#include "oa/data/defs/asset_files.hpp"
#include "oa/data/defs/locale.hpp"
#include "oa/data/defs/unit_header.hpp"
#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/ota.hpp"
#include "oa/formats/tnt.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/netgame/records.hpp"
#include "oa/present/palette_tables.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_multiplayer/presence_card.hpp"
#include "oa/ui/frontend_multiplayer/map_list.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/data/unit_definitions.hpp"
#include "oa/test/game_assets.hpp"
#include "oa/platform/system.hpp"
#include "oa/platform/files.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mp = oa::ui::frontend_multiplayer;
namespace renderer = oa::ui::frontend_renderer;
namespace gaf = oa::formats::gaf;
using oa::app::ScreenId;

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        ++failures;
    }
}

struct Host {
    std::optional<ScreenId> requested;
    std::map<std::string, std::string> strings;
    std::string status;
    std::vector<std::string> sounds;
    int frontend_state = -1;
    int frontend_signal = -1;
    int frontend_passes = 0;
};

void request_screen(void* host, ScreenId id) {
    static_cast<Host*>(host)->requested = id;
}

void play_sound(void* host, const char* name) {
    static_cast<Host*>(host)->sounds.emplace_back(name);
}

int read_number(void*, const char*, const char*, uint32_t*) {
    return 0;
}

void write_number(void*, const char*, const char*, uint32_t) {
}

int read_string(void* host, const char* section, const char* key, char* out, std::size_t capacity) {
    const auto& strings = static_cast<Host*>(host)->strings;
    const auto found = strings.find(std::string(section) + "|" + key);
    if (found == strings.end() || found->second.size() >= capacity)
        return 0;
    std::memcpy(out, found->second.c_str(), found->second.size() + 1);
    return 1;
}

void write_string(void* host, const char* section, const char* key, const char* value) {
    static_cast<Host*>(host)->strings[std::string(section) + "|" + key] = value;
}

void set_status(void* host, const char* text) {
    static_cast<Host*>(host)->status = text;
}

void set_frontend_signal(void* host, uint8_t signal) {
    static_cast<Host*>(host)->frontend_signal = signal;
}

void set_frontend_state(void* host, uint8_t state) {
    static_cast<Host*>(host)->frontend_state = state;
}

void run_frontend(void* host) {
    ++static_cast<Host*>(host)->frontend_passes;
}

constexpr oa::app::ScreenServices kServices{
    request_screen,
    play_sound,
    read_number,
    write_number,
    read_string,
    write_string,
    set_status,
    set_frontend_signal,
    set_frontend_state,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    run_frontend
};

struct Driver {
    oa::app::ScreenRegistry registry{};
    Host host;
    oa::AssetStore assets;
    oa::ui::frontend_renderer::Surface surface;
    oa::app::ScreenContext ctx{};
    ScreenId current = 0;

    explicit Driver(const std::filesystem::path& data) : assets(oa::test::open_game_assets(data)) {
        oa::app::register_multiplayer_screens(&registry);
        ctx = {&host, &kServices, &assets, &surface, nullptr, nullptr, 0};
    }

    const oa::app::ScreenDesc* desc(ScreenId id) { return oa::app::screen_find(&registry, id); }

    void go(ScreenId id) {
        if (const auto* previous = desc(current); previous != nullptr && previous->leave != nullptr)
            previous->leave(&ctx, previous->state);
        current = id;
        ctx.screen = id;
        host.requested.reset();
        if (const auto* next = desc(id); next != nullptr && next->enter != nullptr)
            next->enter(&ctx, next->state);
    }

    bool follow(ScreenId expected, const char* what) {
        const bool ok = host.requested.has_value() && *host.requested == expected;
        expect(ok, what);
        if (ok)
            go(expected);
        return ok;
    }

    void frame() {
        const auto* screen = desc(current);
        screen->tick(&ctx, screen->state);
        screen->draw(&ctx, screen->state);
    }

    bool click(const char* name) { return mp::multiplayer_click(&ctx, name); }

    /// Sends one pointer event to the current screen at a screen position.
    void pointer(oa::app::ScreenInputKind kind, int32_t x, int32_t y) {
        oa::app::ScreenInput input{};
        input.kind = kind;
        input.button = 1;
        input.clicks = 1;
        input.x = static_cast<float>(x);
        input.y = static_cast<float>(y);
        ctx.input = &input;
        const auto* screen = desc(current);
        (void)screen->event(&ctx, screen->state);
        ctx.input = nullptr;
    }

    /// Presses a key on the current screen, with the modifier keys held.
    void key(uint32_t code, uint16_t modifiers = 0) {
        oa::app::ScreenInput input{};
        input.kind = oa::app::ScreenInputKind::key_down;
        input.key = code;
        input.modifiers = modifiers;
        ctx.input = &input;
        const auto* screen = desc(current);
        (void)screen->event(&ctx, screen->state);
        ctx.input = nullptr;
    }

    /// Sends typed text to the current screen, as the system does after a key press types it.
    void text(const char* typed) {
        oa::app::ScreenInput input{};
        input.kind = oa::app::ScreenInputKind::text;
        input.text = typed;
        ctx.input = &input;
        const auto* screen = desc(current);
        (void)screen->event(&ctx, screen->state);
        ctx.input = nullptr;
    }

    /// Turns the mouse wheel over a screen position, by notches away from the player.
    void wheel(int32_t x, int32_t y, float notches) {
        oa::app::ScreenInput input{};
        input.kind = oa::app::ScreenInputKind::wheel;
        input.x = static_cast<float>(x);
        input.y = static_cast<float>(y);
        input.wheel_y = notches;
        ctx.input = &input;
        const auto* screen = desc(current);
        (void)screen->event(&ctx, screen->state);
        ctx.input = nullptr;
    }

    /// Moves the pointer to a screen position, presses and releases it there.
    void pointer_click(int32_t x, int32_t y) {
        pointer(oa::app::ScreenInputKind::pointer_move, x, y);
        pointer(oa::app::ScreenInputKind::pointer_down, x, y);
        pointer(oa::app::ScreenInputKind::pointer_up, x, y);
    }

    /// Tells whether the screens played a sound since the list was last cleared.
    bool played(std::string_view name) const {
        return std::find(host.sounds.begin(), host.sounds.end(), name) != host.sounds.end();
    }

    // Frames go to $OA_MULTIPLAYER_SNAPSHOTS/<name>.ppm when that is set.
    void snapshot(const char* name) const {
        const auto directory = oa::platform::environment_value("OA_MULTIPLAYER_SNAPSHOTS");
        if (!directory || surface.width == 0)
            return;
        const auto path = std::filesystem::path(*directory) / (std::string(name) + ".ppm");
        if (FILE* file = oa::platform::open_file(path, "wb")) {
            std::fprintf(file, "P6\n%u %u\n255\n", surface.width, surface.height);
            std::fwrite(surface.rgb.data(), 1, surface.rgb.size(), file);
            std::fclose(file);
        }
    }

    bool drawn() const {
        if (surface.width != 640 || surface.height != 480)
            return false;
        std::size_t lit = 0;
        for (std::size_t index = 0; index < surface.rgb.size(); index += 3)
            if (surface.rgb[index] != 0 || surface.rgb[index + 1] != 0 ||
                surface.rgb[index + 2] != 0)
                ++lit;
        return lit > surface.rgb.size() / 30;
    }
};

/// Counts the installed units without norestrict, from their unit headers.
///
/// @param assets the installed game's store
/// @return the count
int32_t restrictable_units(oa::AssetStore& assets) {
    const auto files = oa::data::defs::asset_store_files(&assets);
    oa::data::defs::WeaponTdfSet weapons{};
    oa::data::defs::weapon_tdf_set_init(&weapons);
    const oa::data::defs::UnitHeaderSources sources{"", &weapons, 3, 1, false, false};
    int32_t count = 0;
    for (const auto& path : assets.list_effective("units", ".fbi")) {
        auto header = std::make_unique<oa::UnitDef>();
        bool refused = false;
        if (oa::data::defs::load_unit_header(&files, path.c_str(), *header, sources, &refused) &&
            (header->abilities & OA_UNIT_DEF_ABILITY_NO_RESTRICT) == 0)
            ++count;
    }
    return count;
}

/// The battle room's art as the renderer loads it, and the palette it draws in.
struct Art {
    renderer::ScreenResources screen;
    oa::PaletteBytes palette{};
};

/// Loads a screen's art as the screen does: the battle room's unless told another.
Art load_art(
    oa::AssetStore& assets,
    const char* layout = "guis/lounge2.gui",
    const char* background = "bitmaps/battleroom.pcx",
    const char* sprites = "anims/lounge2.gaf"
) {
    Art art;
    art.screen = renderer::load_screen(
        assets, {layout, background, "palettes/guipal.pal", sprites, "anims/commongui.gaf"}
    );
    art.palette = art.screen.background.palette.value_or(art.screen.gui_palette);
    return art;
}

/// Finds a sequence by name, ignoring ASCII case; null when there is none.
const gaf::Sequence* sequence(const gaf::Archive& archive, std::string_view name) {
    for (const auto& candidate : archive.sequences)
        if (candidate.name.size() == name.size() &&
            std::equal(name.begin(), name.end(), candidate.name.begin(), [](char a, char b) {
                return std::toupper(static_cast<unsigned char>(a)) ==
                       std::toupper(static_cast<unsigned char>(b));
            }))
            return &candidate;
    return nullptr;
}

/// Finds a control of the front panel by name; null when there is none.
const mp::Control* control(const std::string& name) {
    return mp::panel_control(mp::multiplayer_panel(), name);
}

/// Screen pixels a comparison leaves out, one byte per pixel of the 640x480 screen.
using Mask = std::vector<uint8_t>;

/// What part of a frame a comparison looks at, and how the frame is lit.
struct Compare {
    int32_t first_column = 0;
    int32_t first_row = 0;
    int32_t end_row = std::numeric_limits<int32_t>::max();
    const Mask* skip = nullptr;
    std::span<const uint8_t> light; // one light-table row; empty draws the frame unlit
};

struct Comparison {
    std::size_t compared = 0;
    std::size_t differing = 0;
};

/// Tells whether a comparison looked at some pixels and found them all as expected.
bool shows(const Comparison& comparison) {
    return comparison.compared > 0 && comparison.differing == 0;
}

/// Compares the drawn pixels of a frame, its top-left corner at (x, y) and
/// stretched to width x height as the renderer stretches a picture, with the screen.
Comparison compare_frame(
    const renderer::Surface& surface,
    const oa::PaletteBytes& palette,
    const gaf::Frame& source,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    const Compare& part = {}
) {
    Comparison result;
    const auto rendered = gaf::render_normal(source);
    if (!rendered.ok() || width <= 0 || height <= 0) {
        result.differing = 1;
        return result;
    }
    const auto& frame = *rendered.frame;
    for (int32_t row = part.first_row; row < std::min(height, part.end_row); ++row)
        for (int32_t column = part.first_column; column < width; ++column) {
            const auto from = static_cast<std::size_t>(row * frame.height / height) * frame.width +
                              static_cast<std::size_t>(column * frame.width / width);
            if (frame.coverage[from] == 0)
                continue;
            const int32_t px = x + column;
            const int32_t py = y + row;
            if (px < 0 || py < 0 || px >= static_cast<int32_t>(surface.width) ||
                py >= static_cast<int32_t>(surface.height))
                continue;
            const auto at =
                static_cast<std::size_t>(py) * surface.width + static_cast<std::size_t>(px);
            if (part.skip != nullptr && (*part.skip)[at] != 0)
                continue;
            std::size_t index = frame.pixels[from];
            if (!part.light.empty())
                index = part.light[index];
            ++result.compared;
            if (std::memcmp(&surface.rgb[at * 3], &palette[index * 4], 3) != 0)
                ++result.differing;
        }
    return result;
}

/// Tells whether a frame is drawn at its own size with its top-left corner at a control's.
bool shows_frame(
    const Driver& d,
    const Art& art,
    const gaf::Sequence* sequence,
    std::size_t frame,
    const mp::Control* at,
    const Compare& part = {}
) {
    if (sequence == nullptr || frame >= sequence->frames.size() || at == nullptr)
        return false;
    const auto& shown = sequence->frames[frame];
    return shows(
        compare_frame(d.surface, art.palette, shown, at->x, at->y, shown.width, shown.height, part)
    );
}

/// Tells whether a frame is drawn at its own size with its top-left corner at (x, y).
bool shows_at(
    const Driver& d,
    const Art& art,
    const gaf::Sequence* sequence,
    std::size_t frame,
    int32_t x,
    int32_t y
) {
    if (sequence == nullptr || frame >= sequence->frames.size())
        return false;
    const auto& shown = sequence->frames[frame];
    return shows(compare_frame(d.surface, art.palette, shown, x, y, shown.width, shown.height));
}

/// Tells whether a frame is drawn at its own size from (x, y) grayed and
/// shaded, as a grayed control is: each of its pixels, lit when the
/// comparison says so, through the game palette's gray table, then through
/// the shade table's row for level -0x14, the row before it for entries from
/// 0x80.
bool shows_grayed(
    const Driver& d,
    const Art& art,
    const gaf::Frame& source,
    int32_t x,
    int32_t y,
    const Compare& part = {}
) {
    if (!art.screen.game_palette.has_value() || art.screen.shade_table.size() != 32 * 256)
        return false;
    oa::Palette game{};
    for (std::size_t index = 0; index < 256; ++index)
        game.entries[index] = {
            (*art.screen.game_palette)[index * 4],
            (*art.screen.game_palette)[index * 4 + 1],
            (*art.screen.game_palette)[index * 4 + 2],
            0
        };
    std::vector<uint8_t> gray(256);
    oa::present::DisplayContext display{};
    display.flags = oa::present::display_flag_gray_table;
    display.gray_table = gray.data();
    (void)oa::present::build_gray_table(display, game);
    const auto rendered = gaf::render_normal(source);
    if (!rendered.ok())
        return false;
    const auto& image = *rendered.frame;
    std::size_t compared = 0;
    for (int32_t row = part.first_row;
         row < std::min(static_cast<int32_t>(image.height), part.end_row);
         ++row)
        for (int32_t column = part.first_column; column < image.width; ++column) {
            const auto at = static_cast<std::size_t>(row) * image.width + column;
            const auto screen = static_cast<std::size_t>(y + row) * 640 + x + column;
            if (image.coverage[at] == 0 || (part.skip != nullptr && (*part.skip)[screen] != 0))
                continue;
            std::size_t index = image.pixels[at];
            if (!part.light.empty())
                index = part.light[index];
            const auto grayed = gray[index];
            const std::size_t shade_row = grayed >= 0x80 ? 11 : 12;
            const auto shaded = art.screen.shade_table[shade_row * 256 + grayed];
            ++compared;
            if (std::memcmp(&d.surface.rgb[screen * 3], &art.palette[shaded * 4U], 3) != 0)
                return false;
        }
    return compared > 0;
}

/// Tells whether a frame of a sequence is drawn grayed and shaded at its own size from (x, y).
bool shows_grayed_at(
    const Driver& d,
    const Art& art,
    const gaf::Sequence* sequence,
    std::size_t frame,
    int32_t x,
    int32_t y
) {
    return sequence != nullptr && frame < sequence->frames.size() &&
           shows_grayed(d, art, sequence->frames[frame], x, y);
}

/// Tells whether a line of text is drawn in the screen's font with its top-left corner at (x, y).
bool shows_text(const Driver& d, const Art& art, std::string_view text, int32_t x, int32_t y) {
    std::vector<uint8_t> pixels(640 * 480, 0);
    std::vector<uint8_t> coverage(640 * 480, 0);
    (void)oa::formats::fnt::raster_text(
        {640, 480, 640, pixels, coverage}, art.screen.font, text, x, y
    );
    std::size_t compared = 0;
    for (std::size_t at = 0; at < coverage.size(); ++at) {
        if (coverage[at] == 0)
            continue;
        ++compared;
        if (std::memcmp(&d.surface.rgb[at * 3], &art.palette[pixels[at] * 4U], 3) != 0)
            return false;
    }
    return compared > 0;
}

/// Tells whether a screen rectangle shows the screen's background image unchanged.
bool shows_background(
    const Driver& d, const Art& art, int32_t x, int32_t y, int32_t width, int32_t height
) {
    const auto& image = art.screen.background;
    if (image.width != d.surface.width || image.height != d.surface.height)
        return false;
    for (int32_t row = y; row < y + height; ++row)
        for (int32_t column = x; column < x + width; ++column) {
            const auto at =
                (static_cast<std::size_t>(row) * image.width + static_cast<std::size_t>(column)) *
                3U;
            if (std::memcmp(&d.surface.rgb[at], &image.rgb[at], 3) != 0)
                return false;
        }
    return true;
}

/// Marks the screen pixels a frame drawn at its own size from (x, y) covers.
Mask frame_mask(const gaf::Frame& source, int32_t x, int32_t y) {
    Mask mask(640 * 480, 0);
    const auto rendered = gaf::render_normal(source);
    if (!rendered.ok())
        return mask;
    const auto& frame = *rendered.frame;
    for (int32_t row = 0; row < frame.height; ++row)
        for (int32_t column = 0; column < frame.width; ++column)
            if (frame.coverage[static_cast<std::size_t>(row) * frame.width + column] != 0 &&
                x + column >= 0 && x + column < 640 && y + row >= 0 && y + row < 480)
                mask
                    [static_cast<std::size_t>(y + row) * 640 +
                     static_cast<std::size_t>(x + column)] = 1;
    return mask;
}

/// The version text a colour square carries: its glyph pixels, centred on the
/// square's rectangle, whose right and bottom edges are its last column and row.
struct VersionText {
    std::vector<uint8_t> pixels = std::vector<uint8_t>(640 * 480, 0);
    Mask coverage = Mask(640 * 480, 0);
};

/// Lays out a colour square's version text in the label font.
VersionText version_text(const Art& art, const mp::Control& logo, std::string_view text) {
    VersionText version;
    const auto& font = art.screen.label_font;
    const auto width = static_cast<int32_t>(oa::formats::fnt::measure_text(font, text));
    const auto height = static_cast<int32_t>(oa::formats::fnt::line_height(font));
    const int32_t x = (logo.x + logo.x + logo.width - 1 - width) / 2;
    const int32_t y = (logo.y + logo.y + logo.height - 1 - height) / 2;
    std::vector<uint8_t> pixels(640 * 480, 0);
    std::vector<uint8_t> coverage(640 * 480, 0);
    (void)oa::formats::fnt::raster_text({640, 480, 640, pixels, coverage}, font, text, x, y);
    for (int32_t row = std::max(0, y); row < std::min(480, y + height); ++row)
        for (int32_t column = std::max(0, x); column < std::min(640, x + width); ++column) {
            const auto at = static_cast<std::size_t>(row) * 640 + static_cast<std::size_t>(column);
            version.pixels[at] = pixels[at];
            version.coverage[at] = coverage[at];
        }
    return version;
}

/// Tells whether a colour square carries its player's version text.
bool shows_version(
    const Driver& d, const Art& art, const std::string& logo_name, std::string_view text
) {
    const auto* logo = control(logo_name);
    if (logo == nullptr)
        return false;
    const auto version = version_text(art, *logo, text);
    std::size_t compared = 0;
    for (std::size_t at = 0; at < version.coverage.size(); ++at) {
        if (version.coverage[at] == 0)
            continue;
        ++compared;
        if (std::memcmp(&d.surface.rgb[at * 3], &art.palette[version.pixels[at] * 4U], 3) != 0)
            return false;
    }
    return compared > 0;
}

/// Tells whether a colour square shows a colour, 32xlogos stretched over its
/// rectangle, around its version text.
bool shows_color(
    const Driver& d,
    const Art& art,
    const std::string& logo_name,
    uint8_t color,
    std::string_view text
) {
    const auto* logo = control(logo_name);
    const auto* colors = sequence(art.screen.global_sprites, "32xlogos");
    if (logo == nullptr || colors == nullptr || color >= colors->frames.size())
        return false;
    const auto version = version_text(art, *logo, text);
    Compare part;
    part.skip = &version.coverage;
    return shows(compare_frame(
        d.surface,
        art.palette,
        colors->frames[color],
        logo->x,
        logo->y,
        logo->width,
        logo->height,
        part
    ));
}

/// Each option button and the shared stage art LOUNGE2.GUI's attributes and
/// stage counts give it.
struct OptionArt {
    const char* button{};
    const char* art{};
};

constexpr OptionArt kOptionArt[] = {
    {"COMMANDER", "stagebuttn3"},
    {"FIXEDLOC", "stagebuttn2"},
    {"LOSTYPE", "stagebuttn3"},
    {"MAPPING", "stagebuttn1"},
    {"CHEATING", "stagebuttn1"},
    {"WATCHING", "stagebuttn1"},
    {"GAMEOPEN", "stagebuttn1"},
};

/// The lamps sit right of this column of the 120-pixel stage art, clear of every caption.
constexpr int32_t kLampColumn = 88;

/// Tells whether an option button shows a frame of its stage art right of its caption.
bool lamp_shows(const Driver& d, const Art& art, const OptionArt& option, std::size_t frame) {
    Compare part;
    part.first_column = kLampColumn;
    return shows_frame(
        d, art, sequence(art.screen.shared_sprites, option.art), frame, control(option.button), part
    );
}

/// Tells whether an option button's lamp shows its stage and no other.
bool lamp_follows_stage(const Driver& d, const Art& art, const OptionArt& option) {
    const auto* button = control(option.button);
    if (button == nullptr || button->stages < 2)
        return false;
    const auto other = static_cast<std::size_t>((button->stage + 1) % button->stages);
    return lamp_shows(d, art, option, button->stage) && !lamp_shows(d, art, option, other);
}

/// Tells whether a grayed option button shows the lamp of a stage, grayed and
/// shaded, right of its caption.
bool lamp_shows_grayed(
    const Driver& d, const Art& art, const OptionArt& option, std::size_t stage
) {
    const auto* button = control(option.button);
    const auto* stages = sequence(art.screen.shared_sprites, option.art);
    if (button == nullptr || stages == nullptr || stage >= stages->frames.size())
        return false;
    Compare part;
    part.first_column = kLampColumn;
    return shows_grayed(d, art, stages->frames[stage], button->x, button->y, part);
}

/// Hosts a game from the provider list as a player does and opens its battle room.
bool host_battleroom(Driver& d, const char* game, const char* nickname) {
    d.go(mp::kScreenProviders);
    auto* dplay = mp::panel_control(mp::multiplayer_panel(), "DPLAY");
    if (dplay == nullptr)
        return false;
    dplay->list_selection = 1;
    if (!d.click("SELECT") || !d.follow(mp::kScreenTcp, "the TCP/IP provider is chosen again"))
        return false;
    mp::panel_set_text(mp::multiplayer_panel(), "ADDRESS", "127.0.0.1");
    if (!d.click("OK") || !d.follow(mp::kScreenGameList, "the game list opens again"))
        return false;
    if (!d.click("STARTNEW") || !d.follow(mp::kScreenNewGame, "NEWMULTI opens again"))
        return false;
    mp::panel_set_text(mp::multiplayer_panel(), "GAMENAME", game);
    mp::panel_set_text(mp::multiplayer_panel(), "NICKNAME", nickname);
    return d.click("OK") && d.follow(mp::kScreenBattleroom, "hosting opens the battle room again");
}

/// Whether Developer Mode is on, as the engine's line the test binds says.
bool banner_developer_mode = false;

/// Whether the game may be remote-controlled, as the engine's line the test binds says.
bool banner_remote_controlled = false;

/// Returns the engine's line the test binds.
///
/// @return the line, at version v9.8.7
std::string banner_line(void*) {
    return mp::engine_banner_line("v9.8.7", banner_developer_mode, banner_remote_controlled);
}

/// Counts the battle room's chat lines that read as a text.
///
/// @param lobby the lobby
/// @param text the line
/// @return how many lines of the chat ring read so
int32_t chat_lines(mp::Lobby& lobby, std::string_view text) {
    int32_t count = 0;
    for (std::size_t index = 0; index < mp::kChatLines; ++index)
        if (std::string_view{mp::lobby_chat_line(*lobby.game, index)} == text)
            ++count;
    return count;
}

/// Counts the chat records the loopback keeps that read as a text.
///
/// @param text the record's text
/// @return how many kept records do
int32_t chat_records(std::string_view text) {
    const auto& loopback = mp::multiplayer_loopback();
    int32_t count = 0;
    for (int32_t index = 0; index < loopback.sent_count; ++index) {
        oa::netgame::ChatRecord record{};
        if (oa::netgame::decode_record(loopback.sent[index], loopback.sent_size[index], &record) !=
            oa::netgame::WireError::ok)
            continue;
        if (std::string_view{record.text, ::strnlen(record.text, sizeof record.text)} == text)
            ++count;
    }
    return count;
}

/// The battle room says the engine's line, as the local player's chat line,
/// once as it is entered and again whenever the line changes there.
void check_engine_banner(Driver& d) {
    expect(
        mp::engine_banner_line("v9.8.7", false, false) == "[Engine: OpenAnnihilation v9.8.7]",
        "the engine's line names the engine and its version"
    );
    expect(
        mp::engine_banner_line("v9.8.7", true, false) ==
            "[Engine: OpenAnnihilation v9.8.7 DEV MODE]",
        "the engine's line says when Developer Mode is on"
    );
    expect(
        mp::engine_banner_line("v9.8.7", false, true) ==
            "[Engine: OpenAnnihilation v9.8.7 REMOTED]",
        "the engine's line says when the game may be remote-controlled"
    );
    expect(
        mp::engine_banner_line("v9.8.7", true, true) ==
            "[Engine: OpenAnnihilation v9.8.7 DEV MODE REMOTED]",
        "the engine's line says both, Developer Mode first"
    );
    banner_developer_mode = false;
    banner_remote_controlled = false;
    mp::multiplayer_bind_engine_banner({nullptr, banner_line});
    if (!host_battleroom(d, "Banner", "Host")) {
        mp::multiplayer_bind_engine_banner({});
        return;
    }
    auto& lobby = mp::multiplayer_lobby();
    const std::string plain = "<Host> [Engine: OpenAnnihilation v9.8.7]";
    const std::string developer = "<Host> [Engine: OpenAnnihilation v9.8.7 DEV MODE]";
    d.frame();
    expect(chat_lines(lobby, plain) == 1, "the battle room says the engine's line as it opens");
    expect(chat_records(plain) == 1, "the engine's line goes out as a chat record");
    d.frame();
    d.frame();
    expect(
        chat_lines(lobby, plain) == 1 && chat_records(plain) == 1,
        "the engine's line is said once while it stays the same"
    );
    banner_developer_mode = true;
    d.frame();
    expect(
        chat_lines(lobby, developer) == 1 && chat_records(developer) == 1,
        "the engine's line is said again as Developer Mode turns on"
    );
    banner_developer_mode = false;
    d.frame();
    expect(
        chat_lines(lobby, plain) == 2 && chat_records(plain) == 2,
        "the engine's line is said again as Developer Mode turns off"
    );
    const std::string remoted = "<Host> [Engine: OpenAnnihilation v9.8.7 REMOTED]";
    banner_remote_controlled = true;
    d.frame();
    expect(
        chat_lines(lobby, remoted) == 1 && chat_records(remoted) == 1,
        "the battle room says the line of a game that may be remote-controlled"
    );
    banner_remote_controlled = false;
    d.frame();
    // A battle room entered again says it again, at the chat ring's head.
    if (host_battleroom(d, "Banner again", "Host")) {
        auto& again = mp::multiplayer_lobby();
        const uint16_t head = mp::lobby_chat_head(*again.game);
        d.frame();
        expect(
            static_cast<uint16_t>(mp::lobby_chat_head(*again.game)) ==
                    (head + 1U) % mp::kChatLines &&
                std::string_view{mp::lobby_chat_line(*again.game, head)} == plain,
            "a battle room entered again says the engine's line"
        );
    }
    mp::multiplayer_bind_engine_banner({});
}

int starts = 0;

/// Counts the games the battle room starts.
void count_start(void*, mp::Lobby&) {
    ++starts;
}

/// The 30 Hz tick the lobby's clock gives while the test steps it.
uint32_t stepped_tick = 0;

/// Returns the tick the test has stepped the lobby's clock to.
uint32_t stepped_clock(void*) {
    return stepped_tick;
}

/// SLIDERS frames of a horizontal bar in the shared art: its track's start,
/// middle and end, its knob, and its back and forward arrows, each followed
/// by its held face.
constexpr std::size_t kTrackStartFrame = 10;
constexpr std::size_t kTrackFrame = 11;
constexpr std::size_t kTrackEndFrame = 12;
constexpr std::size_t kKnobFrame = 13;
constexpr std::size_t kBackArrowFrame = 16;
constexpr std::size_t kForwardArrowFrame = 18;

/// Returns the column a horizontal bar's knob is drawn from: 3 into the bar
/// from its position, and at least its width and 2 before the bar's last column.
int32_t knob_column(const mp::Control& bar) {
    return std::min(bar.scroll.knob + bar.x + 3, bar.x + bar.width - 1 - 10 - 2);
}

/// SLIDERS frames of a vertical bar in the shared art: its knob's first
/// frame and its up and down arrows.
constexpr std::size_t kVerticalKnobFrame = 3;
constexpr std::size_t kUpArrowFrame = 6;
constexpr std::size_t kDownArrowFrame = 8;

/// Returns the first row a list's scroll bar shows: (count - visible) * knob / (range - 1), truncated.
int32_t knob_first_row(int32_t count, int32_t visible, const mp::Control& bar) {
    if (bar.scroll.range < 2)
        return 0;
    return static_cast<int32_t>(
        static_cast<double>(count - visible) * bar.scroll.knob / (bar.scroll.range - 1)
    );
}

/// Returns the knob step a list's first row puts its bar at: first * range / last page's first row, truncated.
int32_t first_row_knob(const mp::Control& list, const mp::Control& bar) {
    if (list.list_last_first == 0)
        return 0;
    return static_cast<int32_t>(
        static_cast<double>(list.list_first) * bar.scroll.range / list.list_last_first
    );
}

constexpr uint32_t kKeyTab = 0x09;
constexpr uint32_t kKeyRight = 0x4000004F;
constexpr uint32_t kKeyLeft = 0x40000050;
constexpr uint32_t kKeyDown = 0x40000051;
constexpr uint32_t kKeyUp = 0x40000052;
constexpr uint32_t kKeyReturn = 0x0d;
constexpr uint16_t kKeyShift = 0x0001;
constexpr uint32_t kKeyEscape = 0x1b;
constexpr uint32_t kKeyN = 'n';
constexpr uint32_t kKeyY = 'y';

// What the launch link heard during the launch checks.
struct LaunchRecord {
    int32_t app_mode = 0;
    std::vector<bool> leaves; // each leave, with the disconnect reason or not
};

LaunchRecord g_launch;

/// Checks, on the game list after a launch, the exit confirmation a
/// request to close the window opens, and the return to the main menu.
///
/// The confirmation opens over the screen at its next tick: CHOICE2, 'n',
/// Escape and Enter (CHOICE2 has the focus) close it; Ctrl+Y answers
/// nothing; 'y', as CHOICE1, leaves the game without the disconnect reason. PREVMENU returns to the main menu through
/// the frontend: application mode 1, then state 2 with the initialize signal
/// and one frontend pass.
///
/// @param d the driver, on SELGAME
void check_launch_exit(Driver& d) {
    g_launch = LaunchRecord{};
    mp::LaunchLink link{};
    link.launch_active = [](void*) { return true; };
    link.request_app_mode = [](void*, int32_t mode) { g_launch.app_mode = mode; };
    link.leave_game = [](void*, bool with_reason) { g_launch.leaves.push_back(with_reason); };
    mp::multiplayer_bind_launch_link(link);
    expect(mp::multiplayer_showing(), "a multiplayer screen shows");
    for (const uint32_t key : {0U, kKeyN, kKeyEscape, kKeyReturn}) {
        expect(mp::multiplayer_request_exit_confirm(), "the exit confirmation is asked for");
        expect(mp::multiplayer_modal_kind() == mp::ModalKind::none, "it waits for the next tick");
        d.frame();
        expect(
            mp::multiplayer_modal_kind() == mp::ModalKind::exit_confirm,
            "the tick opens YESORNO over the screen"
        );
        auto& panel = mp::multiplayer_panel();
        expect(
            mp::panel_text(panel, "TITLE") == "Surrender this battle and exit to the system?" &&
                mp::panel_text(panel, "CHOICE1") == "Yes" &&
                mp::panel_text(panel, "CHOICE2") == "No" &&
                panel.focus == mp::panel_find(panel, "CHOICE2"),
            "it asks to surrender and exit, No in focus"
        );
        d.host.sounds.clear();
        if (key == 0)
            expect(d.click("CHOICE2"), "CHOICE2 clicked");
        else
            d.key(key);
        expect(
            mp::multiplayer_modal_kind() == mp::ModalKind::none && d.played("Exit") &&
                g_launch.leaves.empty(),
            "No closes the confirmation"
        );
    }
    // During the wait for the host the confirmation takes the input: the
    // countdown gives way to it and the wait pauses beneath it.
    auto& connect = mp::multiplayer_connect();
    connect.host_waiting = true;
    connect.host_wait_next_poll_ms = 1000000000U;
    connect.host_wait_deadline_ms = 1000000000U;
    d.frame();
    expect(!mp::multiplayer_message().empty(), "the wait shows its countdown");
    expect(mp::multiplayer_request_exit_confirm(), "the confirmation is asked for during the wait");
    d.frame();
    expect(
        mp::multiplayer_modal_kind() == mp::ModalKind::exit_confirm &&
            mp::multiplayer_message().empty(),
        "the countdown gives way to the confirmation"
    );
    d.key(kKeyEscape);
    expect(
        mp::multiplayer_modal_kind() == mp::ModalKind::none && connect.host_waiting,
        "Escape reaches the confirmation, and the wait goes on"
    );
    connect.host_waiting = false;
    d.frame();
    d.host.requested.reset();
    expect(d.click("PREVMENU"), "PREVMENU clicked");
    expect(
        g_launch.app_mode == 1 && d.host.frontend_state == 2 && d.host.frontend_signal == 0 &&
            d.host.frontend_passes == 1 && !d.host.requested.has_value(),
        "PREVMENU after a launch returns to the main menu through a frontend pass in mode 1"
    );
    expect(mp::multiplayer_request_exit_confirm(), "the confirmation is asked for again");
    d.frame();
    const auto* yes = mp::panel_control(mp::multiplayer_panel(), "CHOICE1");
    const auto* no = mp::panel_control(mp::multiplayer_panel(), "CHOICE2");
    expect(
        yes != nullptr && no != nullptr && yes->quick_key == 'Y' && no->quick_key == 'N',
        "Yes and No take Y and N"
    );
    d.key(kKeyY, oa::app::kInputModifierCtrl);
    expect(
        mp::multiplayer_modal_kind() == mp::ModalKind::exit_confirm && g_launch.leaves.empty(),
        "Ctrl+Y answers nothing"
    );
    d.key(kKeyY);
    expect(
        g_launch.leaves == std::vector<bool>({false}) &&
            (mp::multiplayer_game().outcome_flags & mp::kOutcomeLeaving) != 0,
        "Yes leaves the game without the disconnect reason, the leaving bit raised"
    );
    d.go(0);
    expect(
        !mp::multiplayer_showing() && !mp::multiplayer_request_exit_confirm(),
        "no confirmation once the multiplayer screens are left"
    );
    mp::multiplayer_bind_launch_link(mp::LaunchLink{});
}

/// A map's minimap and its size in world pixels, as the installed game holds them.
struct Minimap {
    std::vector<uint8_t> pixels;
    int32_t width = 0;
    int32_t height = 0;
    int32_t world_width = 0;
    int32_t world_height = 0;
};

/// Reads a multiplayer map's minimap from the installed game.
///
/// @param assets the installed game's store
/// @param map the map's name
/// @return the minimap; empty when the map has none
Minimap read_minimap(oa::AssetStore& assets, const std::string& map) {
    struct File {
        oa::AssetStore* assets;
        std::string path;
    } file{&assets, "maps/" + map + ".tnt"};

    const oa::formats::tnt::MapFileReader reader{
        &file, [](void* context, uint32_t offset, void* out, uint32_t size) {
            auto& open = *static_cast<File*>(context);
            try {
                return open.assets->read_chunk(
                    open.path, offset, std::span(static_cast<uint8_t*>(out), size)
                );
            } catch (const std::exception&) {
                return false;
            }
        }
    };
    oa::formats::tnt::RadarPicture radar;
    int32_t width = 0;
    int32_t height = 0;
    Minimap minimap;
    if (!oa::formats::tnt::load_radar_picture(reader, radar, &width, &height))
        return minimap;
    minimap.pixels = std::move(radar.pixels);
    minimap.width = radar.width;
    minimap.height = radar.height;
    minimap.world_width = width * 16;
    minimap.world_height = height * 16;
    return minimap;
}

/// Tells whether a map's playable part (32 world pixels short on the right
/// and 128 at the bottom) is wider or taller than it is square.
bool letterboxed(const Minimap& map) {
    return map.world_width - 32 != map.world_height - 128;
}

/// Fills rows top..bottom-1 and columns left..right-1 of a target, short of
/// its last row and column, with texels stepped in 16.16 from (u0, v0) by
/// the span's length toward (u1, v1).
void stretch_model(
    std::vector<uint8_t>& target,
    int32_t target_width,
    int32_t target_height,
    const std::vector<uint8_t>& texels,
    int32_t texel_width,
    int32_t left,
    int32_t top,
    int32_t right,
    int32_t bottom,
    int32_t u0,
    int32_t v0,
    int32_t u1,
    int32_t v1
) {
    const int32_t columns = right - left;
    const int32_t rows = bottom - top;
    if (columns <= 0 || rows <= 0)
        return;
    const int32_t du = ((u1 - u0) << 16) / columns;
    const int32_t dv = ((v1 - v0) << 16) / rows;
    for (int32_t row = 0; row < rows; ++row) {
        const int32_t y = top + row;
        if (y < 0 || y > target_height - 2)
            continue;
        const int32_t v = (v0 << 16) + row * dv;
        for (int32_t column = 0; column < columns; ++column) {
            const int32_t x = left + column;
            if (x < 0 || x > target_width - 2)
                continue;
            const int32_t u = (u0 << 16) + column * du;
            target[static_cast<std::size_t>(y * target_width + x)] =
                texels[static_cast<std::size_t>((v >> 16) * texel_width + (u >> 16))];
        }
    }
}

/// Returns what a picture box shows of a map but its last column and row, as 3.1c draws it.
///
/// The map's playable part keeps its shape, its longer side filling the box
/// and centred on palette index 0; that picture is then stretched over the
/// box from one texel in.
///
/// @param map the map's minimap
/// @param width the box's width in pixels
/// @param height the box's height in rows
/// @return (width - 1) x (height - 1) palette indices; empty for a map without a minimap
std::vector<uint8_t> preview_model(const Minimap& map, int32_t width, int32_t height) {
    if (map.pixels.empty())
        return {};
    const int32_t playable_width = map.world_width - 32;
    const int32_t playable_height = map.world_height - 128;
    int32_t shown_width = width, shown_height = height;
    int32_t used_width = map.width, used_height = map.height;
    int32_t left = 0, top = 0;
    if (playable_width >= playable_height) {
        shown_height = playable_height * height / playable_width;
        used_height = map.height * playable_height / playable_width;
        top = (height - shown_height) / 2;
    } else {
        shown_width = playable_width * width / playable_height;
        used_width = map.width * playable_width / playable_height;
        left = (width - shown_width) / 2;
    }
    std::vector<uint8_t> fitted(static_cast<std::size_t>(width * height), 0);
    stretch_model(
        fitted,
        width,
        height,
        map.pixels,
        map.width,
        left,
        top,
        left + shown_width,
        top + shown_height,
        0,
        0,
        used_width - 1,
        used_height - 1
    );
    std::vector<uint8_t> face(static_cast<std::size_t>(width * height), 0);
    stretch_model(
        face, width, height, fitted, width, 0, 0, width - 1, height - 1, 1, 1, width - 1, height - 1
    );
    std::vector<uint8_t> shown;
    for (int32_t row = 0; row < height - 1; ++row)
        for (int32_t column = 0; column < width - 1; ++column)
            shown.push_back(face[static_cast<std::size_t>(row * width + column)]);
    return shown;
}

/// Tells whether a picture box shows a map's preview, its last column and row
/// showing the screen's art as it lies under the box.
///
/// @param d the driver whose screen is read
/// @param art the dialog's art, whose palette the preview is drawn in
/// @param map the map's minimap
/// @param box the picture box
/// @param x the box's left column on the screen
/// @param y the box's top row on the screen
/// @return true when every pixel matches
bool shows_preview(
    const Driver& d,
    const Art& art,
    const Minimap& map,
    const mp::Control& box,
    int32_t x,
    int32_t y
) {
    const auto face = preview_model(map, box.width, box.height);
    if (face.empty())
        return false;
    const auto& image = art.screen.background;
    const auto pixel = [&](int32_t column, int32_t row) {
        return &d.surface.rgb
                    [(static_cast<std::size_t>(y + row) * 640 +
                      static_cast<std::size_t>(x + column)) *
                     3];
    };
    const auto under = [&](int32_t column, int32_t row) {
        return &image.rgb
                    [(static_cast<std::size_t>(box.y + row) * image.width +
                      static_cast<std::size_t>(box.x + column)) *
                     3];
    };
    for (int32_t row = 0; row < box.height; ++row)
        for (int32_t column = 0; column < box.width; ++column) {
            const bool edge = row == box.height - 1 || column == box.width - 1;
            const uint8_t* expected =
                edge ? under(column, row)
                     : &art.palette
                            [face[static_cast<std::size_t>(row * (box.width - 1) + column)] * 4U];
            if (std::memcmp(pixel(column, row), expected, 3) != 0)
                return false;
        }
    return true;
}

/// Tells whether a picture box without a picture is filled with the GUI
/// palette's colour 7 as the dialog's palette matches it.
bool shows_blank_box(
    const Driver& d, const Art& art, const mp::Control& box, int32_t x, int32_t y
) {
    const auto blank = oa::remap_palette(art.screen.gui_palette, art.palette)[7];
    for (int32_t row = 0; row < box.height; ++row)
        for (int32_t column = 0; column < box.width; ++column)
            if (std::memcmp(
                    &d.surface.rgb
                         [(static_cast<std::size_t>(y + row) * 640 +
                           static_cast<std::size_t>(x + column)) *
                          3],
                    &art.palette[blank * 4U],
                    3
                ) != 0)
                return false;
    return true;
}

/// Returns a map's summary line as 3.1c writes it: the OTA's memory and player counts.
///
/// @param assets the installed game's store
/// @param map the map's name
/// @return "<memory>  Players: <player counts>"
std::string summary_size(oa::AssetStore& assets, const std::string& map) {
    const auto bytes = assets.read("maps/" + map + ".ota").bytes;
    const auto parsed = oa::formats::ota::parse(
        std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())
    );
    if (!parsed.metadata.has_value())
        return {};
    return parsed.metadata->memory_requirement +
           "  Players: " + parsed.metadata->permitted_player_counts;
}

/// Returns the screen offset of a dialog with its own art: its art centred on the screen.
///
/// @param art the dialog's art
/// @param root the dialog's panel record
/// @return the offset's column and row
std::pair<int32_t, int32_t> dialog_offset(const Art& art, const mp::Control& root) {
    return {
        (640 - std::min<int32_t>(static_cast<int32_t>(art.screen.background.width), root.width)) /
            2,
        (480 - std::min<int32_t>(static_cast<int32_t>(art.screen.background.height), root.height)) /
            2
    };
}

/// Counts the 0x1b records the loopback holds, or those naming one player.
///
/// @param player_id the player named, or 0 for any
/// @param reason the reject reason, checked when player_id is not 0
/// @return the count
int32_t rejects_sent(uint32_t player_id = 0, uint8_t reason = 0) {
    const auto& wire = mp::multiplayer_loopback();
    int32_t count = 0;
    for (int32_t index = 0; index < wire.sent_count; ++index) {
        oa::netgame::RejectRecord record{};
        if (wire.sent[index][0] != static_cast<uint8_t>(oa::netgame::RecordType::reject) ||
            oa::netgame::decode_record(wire.sent[index], wire.sent_size[index], &record) !=
                oa::netgame::WireError::ok)
            continue;
        if (player_id == 0 || (record.player_id == player_id && record.reason == reason))
            ++count;
    }
    return count;
}

/// Counts the 0x1b records the loopback holds that name a player, whatever the reason.
///
/// @param player_id the player named
/// @return the count
int32_t rejects_naming(uint32_t player_id) {
    const auto& wire = mp::multiplayer_loopback();
    int32_t count = 0;
    for (int32_t index = 0; index < wire.sent_count; ++index) {
        oa::netgame::RejectRecord record{};
        if (wire.sent[index][0] == static_cast<uint8_t>(oa::netgame::RecordType::reject) &&
            oa::netgame::decode_record(wire.sent[index], wire.sent_size[index], &record) ==
                oa::netgame::WireError::ok &&
            record.player_id == player_id)
            ++count;
    }
    return count;
}

/// Queues a one-byte probe (0x06) from a player for the battle room's next pump.
///
/// @param player_id the probing player
void hear_from(uint32_t player_id) {
    mp::LobbyEvent event{};
    event.kind = mp::LobbyEventKind::record;
    event.player_id = player_id;
    event.size = 1;
    event.data[0] = static_cast<uint8_t>(oa::netgame::RecordType::probe);
    expect(mp::loopback_inject(mp::multiplayer_loopback(), event), "a probe is queued");
}

/// Checks TIMEOUT.GUI over the host's battle room, on a clock the test steps.
///
/// A remote player silent past the 30 s timeout opens TIMEOUT.GUI naming
/// it, over a dialog already open, which is back in front once a record
/// from the player closes TIMEOUT.GUI again. Still silent at the timeout
/// plus 120 s, the player is rejected once with reason 6 and its slot
/// opens; REJECT does the same at once. The question to reject a player
/// takes 'n' and Escape as No and 'y' as Yes, and answers nothing with Ctrl
/// held; the character an answering key types after it never reaches the
/// chat line MESSAGE, focused again under the question. This machine's computer player, which it never hears from, is
/// never named or rejected.
///
/// @param d the driver
/// @return false when the battle room cannot be hosted
bool check_player_timeout(Driver& d) {
    constexpr uint32_t kTimeoutTicks = 30 * 30;
    constexpr uint32_t kDropTicks = (30 + 120) * 30;
    constexpr uint32_t kSilent = 0x2345;
    constexpr uint32_t kLate = 0x3456;
    constexpr uint32_t kAsked = 0x4567;
    mp::multiplayer_reset();
    if (!host_battleroom(d, "Silence", "Host"))
        return false;
    auto& lobby = mp::multiplayer_lobby();
    auto& wire = mp::multiplayer_loopback();
    lobby.services.tick = stepped_clock;
    stepped_tick = 100000;
    lobby.next_stats_tick = stepped_tick + 1'000'000; // no periodic block
    expect(lobby.game->player_timeout_seconds == 30, "the battle room's player timeout is 30 s");

    // A computer player of this machine, seated as its arrival is applied.
    mp::lobby_add_computer(lobby, 1);
    d.frame();
    const auto computer_id = mp::slot_player(lobby, 1).player_id;
    const auto computer_seated = [&] {
        const auto slot = mp::slot_for_player_id(lobby, computer_id);
        return slot >= 0 && mp::slot_player(lobby, slot).status == mp::kSlotComputer;
    };
    expect(computer_seated(), "a computer player is seated");
    // Its setup block reports the machine's memory, as the local player's
    // does, so other machines do not mark it short of the map's memory.
    {
        const auto* computer_info = mp::player_info(lobby, mp::slot_player(lobby, 1));
        expect(
            computer_info != nullptr && mp::local_info(lobby).memory_mb != 0 &&
                computer_info->memory_mb == mp::local_info(lobby).memory_mb,
            "a computer player's block reports the local player's memory"
        );
    }

    expect(mp::lobby_add_player(lobby, kSilent, "Silent"), "a remote player joins");
    auto heard = stepped_tick;
    stepped_tick = heard + kTimeoutTicks;
    d.frame();
    expect(mp::multiplayer_modal_kind() == mp::ModalKind::none, "30 s of silence opens nothing");
    stepped_tick = heard + kTimeoutTicks + 1;
    d.frame();
    auto* dialog = mp::multiplayer_modal();
    expect(
        mp::multiplayer_modal_kind() == mp::ModalKind::timeout && dialog != nullptr &&
            mp::panel_text(*dialog, "NAME") == "Silent",
        "past 30 s TIMEOUT.GUI opens naming the silent player"
    );
    d.frame();
    expect(
        dialog != nullptr &&
            mp::panel_text(*dialog, "TIMETEXT") == "will be rejected in 120 seconds",
        "its countdown runs to the timeout plus 120 s"
    );
    expect(d.drawn(), "TIMEOUT.GUI drawn");
    d.snapshot("battleroom-timeout");
    // The countdown is the game's own text in the language shown, the
    // seconds where its %d stands.
    mp::multiplayer_bind_translation({nullptr, [](void*, std::string_view text) {
                                          return std::string(
                                              text == "will be rejected in %d seconds"
                                                  ? "wirn in %d Sekunden ausgeschlossen"
                                                  : text
                                          );
                                      }});
    d.frame();
    expect(
        dialog != nullptr &&
            mp::panel_text(*dialog, "TIMETEXT") == "wirn in 120 Sekunden ausgeschlossen",
        "the countdown follows the language shown"
    );
    mp::multiplayer_bind_translation({});
    hear_from(kSilent);
    d.frame();
    expect(
        mp::multiplayer_modal_kind() == mp::ModalKind::none,
        "a record from the player closes TIMEOUT.GUI"
    );

    // Over another dialog, which is back once TIMEOUT.GUI closes.
    const auto row = std::to_string(mp::slot_for_player_id(lobby, kSilent));
    expect(d.click(("PLAYER" + row).c_str()), "the host clicks the remote player");
    expect(mp::multiplayer_modal_kind() == mp::ModalKind::confirm, "YESORNO asks to reject it");
    heard = stepped_tick;
    stepped_tick = heard + kTimeoutTicks + 1;
    d.frame();
    expect(
        mp::multiplayer_modal_kind() == mp::ModalKind::timeout, "TIMEOUT.GUI opens over YESORNO"
    );
    hear_from(kSilent);
    d.frame();
    dialog = mp::multiplayer_modal();
    expect(
        mp::multiplayer_modal_kind() == mp::ModalKind::confirm && dialog != nullptr &&
            mp::panel_text(*dialog, "TITLE") == "Reject Silent?",
        "YESORNO is back once the player is heard from"
    );
    const auto rejects_before = rejects_sent();
    d.key(kKeyN);
    expect(
        mp::multiplayer_modal_kind() == mp::ModalKind::none, "'n' answers No and closes YESORNO"
    );
    auto& room_panel = mp::multiplayer_panel();
    // The 'n' the key types after it reaches nothing, though the chat line
    // MESSAGE has the focus again; the next key's character reaches it.
    expect(
        room_panel.focus == mp::panel_find(room_panel, "MESSAGE"), "MESSAGE has the focus again"
    );
    d.text("n");
    expect(mp::panel_text(room_panel, "MESSAGE").empty(), "the answer's 'n' is not typed");
    d.key(kKeyN);
    d.text("n");
    expect(mp::panel_text(room_panel, "MESSAGE") == "n", "the next 'n' is typed");
    mp::panel_set_text(room_panel, "MESSAGE", "");
    expect(d.click(("PLAYER" + row).c_str()), "the host clicks the remote player again");
    d.key(kKeyY, oa::app::kInputModifierCtrl);
    expect(
        mp::multiplayer_modal_kind() == mp::ModalKind::confirm && rejects_sent() == rejects_before,
        "Ctrl+Y answers nothing"
    );
    d.key(kKeyEscape);
    expect(
        mp::multiplayer_modal_kind() == mp::ModalKind::none && rejects_sent() == rejects_before,
        "Escape answers No and closes YESORNO"
    );

    // Silent until the timeout plus 120 s: rejected once, with reason 6.
    heard = stepped_tick;
    stepped_tick = heard + kTimeoutTicks + 1;
    d.frame();
    expect(mp::multiplayer_modal_kind() == mp::ModalKind::timeout, "TIMEOUT.GUI opens again");
    wire.sent_count = 0;
    stepped_tick = heard + kDropTicks - 1;
    d.frame();
    dialog = mp::multiplayer_modal();
    expect(
        mp::multiplayer_modal_kind() == mp::ModalKind::timeout && dialog != nullptr &&
            mp::panel_text(*dialog, "TIMETEXT") == "will be rejected in 1 seconds" &&
            rejects_sent() == 0,
        "a second short of the timeout plus 120 s, no one is rejected"
    );
    stepped_tick = heard + kDropTicks;
    d.frame();
    expect(
        rejects_sent() == 1 && rejects_sent(kSilent, 6) == 1,
        "at the timeout plus 120 s one 0x1b {id, 6} goes out"
    );
    expect(
        mp::slot_for_player_id(lobby, kSilent) < 0 &&
            mp::multiplayer_modal_kind() == mp::ModalKind::none,
        "the silent player's slot opens and TIMEOUT.GUI closes"
    );
    stepped_tick += kDropTicks;
    d.frame();
    d.frame();
    expect(rejects_sent() == 1, "nothing more is sent");

    // REJECT drops the player at once.
    expect(mp::lobby_add_player(lobby, kLate, "Late"), "another remote player joins");
    heard = stepped_tick;
    stepped_tick = heard + kTimeoutTicks + 1;
    d.frame();
    dialog = mp::multiplayer_modal();
    expect(
        mp::multiplayer_modal_kind() == mp::ModalKind::timeout && dialog != nullptr &&
            mp::panel_text(*dialog, "NAME") == "Late",
        "TIMEOUT.GUI names the new silent player"
    );
    wire.sent_count = 0;
    expect(d.click("REJECT"), "REJECT clicked");
    expect(
        rejects_sent() == 1 && rejects_sent(kLate, 6) == 1 &&
            mp::slot_for_player_id(lobby, kLate) < 0 &&
            mp::multiplayer_modal_kind() == mp::ModalKind::none,
        "REJECT sends 0x1b {id, 6} at once, opens the slot and closes the dialog"
    );
    d.frame();
    expect(rejects_sent() == 1, "and sends nothing more");

    // 'y' answers the question to reject a player Yes, as CHOICE1 does.
    expect(mp::lobby_add_player(lobby, kAsked, "Asked"), "a third remote player joins");
    hear_from(kAsked);
    d.frame();
    const auto asked_row = std::to_string(mp::slot_for_player_id(lobby, kAsked));
    expect(d.click(("PLAYER" + asked_row).c_str()), "the host clicks the third player");
    expect(mp::multiplayer_modal_kind() == mp::ModalKind::confirm, "YESORNO asks to reject it");
    wire.sent_count = 0;
    d.key(kKeyY);
    expect(
        rejects_sent() == 1 && rejects_sent(kAsked, 1) == 1 &&
            mp::multiplayer_modal_kind() == mp::ModalKind::none,
        "'y' rejects the player with reason 1 and closes YESORNO"
    );
    d.text("Y");
    expect(mp::panel_text(room_panel, "MESSAGE").empty(), "the answer's 'Y' is not typed");

    // This machine's computer player, never heard from, stays.
    stepped_tick += 10 * kDropTicks;
    d.frame();
    d.frame();
    expect(
        computer_seated() && rejects_naming(computer_id) == 0 &&
            mp::multiplayer_modal_kind() == mp::ModalKind::none,
        "this machine's computer player is never named or rejected"
    );
    return true;
}

/// One installed pack map the battle room lists beside the base maps.
struct PackProbe {
    static constexpr const char* kName = "isle_of_ashes@archipelago";
    oa::AssetStore* assets = nullptr;
    std::filesystem::path folder;
    bool mount = false;
    bool refuse = false;
    const char* reason = "too many features";
    const char* index_title = nullptr;
    int prepares = 0;
    int releases = 0;

    mp::LobbyMapSource source() { return {this, count, at, prepare, release}; }

    static int32_t count(void*) { return 1; }

    static bool at(void* context, int32_t index, mp::LobbyPackMap* out) {
        if (index != 0 || out == nullptr)
            return false;
        const auto* self = static_cast<const PackProbe*>(context);
        *out = {kName, "A ring of cinders", "16x16", 16, self->index_title};
        return true;
    }

    static bool prepare(void* context, const char* name, char* reason, std::size_t capacity) {
        auto* self = static_cast<PackProbe*>(context);
        ++self->prepares;
        if (name == nullptr || std::strcmp(name, kName) != 0)
            return false;
        if (self->refuse) {
            if (reason != nullptr && capacity > 0) {
                const auto count = std::min(std::strlen(self->reason), capacity - 1);
                std::memcpy(reason, self->reason, count);
                reason[count] = '\0';
            }
            return false;
        }
        if (!self->mount || self->assets == nullptr)
            return true;
        if (self->assets->pack_layer_mounted())
            return true;
        oa::PackLayerSpec spec;
        spec.kind = oa::PackLayerKind::folder;
        spec.location = self->folder;
        spec.label = "archipelago";
        spec.files.push_back({"maps/isle_of_ashes@archipelago.ota", "maps/isle_of_ashes.ota"});
        spec.files.push_back({"maps/isle_of_ashes@archipelago.tnt", "maps/isle_of_ashes.tnt"});
        std::string error;
        if (!self->assets->mount_pack_layer(std::move(spec), &error)) {
            std::fprintf(stderr, "pack layer: %s\n", error.c_str());
            return false;
        }
        return true;
    }

    static void release(void* context) {
        auto* self = static_cast<PackProbe*>(context);
        ++self->releases;
        if (self->assets != nullptr && self->assets->pack_layer_mounted())
            (void)self->assets->unmount_pack_layer();
    }
};

/// Writes the pack map's files into a folder the test mounts.
bool write_pack_folder(const std::filesystem::path& folder) {
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    if (!std::filesystem::create_directories(folder / "maps", error))
        return false;
    const char* ota = "[GlobalHeader]\n"
                      "{\n"
                      "missionname=Isle of Ashes;\n"
                      "missiondescription=A ring of cinders;\n"
                      "[Schema 0]\n"
                      "    {\n"
                      "    Type=Network 1;\n"
                      "    [specials]\n"
                      "        {\n"
                      "        [special0]\n"
                      "            {\n"
                      "            specialwhat=StartPos1;\n"
                      "            }\n"
                      "        [special1]\n"
                      "            {\n"
                      "            specialwhat=StartPos2;\n"
                      "            }\n"
                      "        }\n"
                      "    }\n"
                      "}\n";
    {
        std::ofstream out(folder / "maps" / "isle_of_ashes.ota", std::ios::binary);
        out << ota;
        if (!out)
            return false;
    }
    std::vector<uint8_t> terrain(0x44, 0);
    const auto put = [&](std::size_t offset, uint32_t value) {
        terrain[offset] = static_cast<uint8_t>(value);
        terrain[offset + 1] = static_cast<uint8_t>(value >> 8);
        terrain[offset + 2] = static_cast<uint8_t>(value >> 16);
        terrain[offset + 3] = static_cast<uint8_t>(value >> 24);
    };
    put(0, 0x2000);
    put(4, 1);
    put(8, 1);
    put(0x10, 0x40);
    terrain[0x40] = 0x11;
    terrain[0x41] = 0x22;
    terrain[0x42] = 0x33;
    terrain[0x43] = 0x44;
    std::ofstream out(folder / "maps" / "isle_of_ashes.tnt", std::ios::binary);
    out.write(
        reinterpret_cast<const char*>(terrain.data()), static_cast<std::streamsize>(terrain.size())
    );
    return static_cast<bool>(out);
}

/// The name a map row chooses: its key when the list has one per row, else its text.
const std::string& map_row_key(const mp::Control& list, std::size_t row) {
    if (list.item_keys.size() == list.items.size())
        return list.item_keys[row];
    return list.items[row];
}

/// The words a map list shows, and choosing a row by those words or by its name.
void check_map_list_words() {
    const mp::MapListFact rows[] = {
        {"comet@sample-comet", "Comet Catcher", {}, "Cavedog: Comet Catcher", true},
        {"Comet Catcher", {}, {}, {}, false},
        {"ponds@sample-ponds", "A Plethora of Ponds", {}, "Sample Ponds", true},
        {"ashes@archipelago", {}, "Isle of Ashes", "Archipelago Pack", true},
        {"bare@archipelago", {}, {}, "Archipelago Pack", true},
        {"ridge@sample", "Ridge Pond", {}, "Ridge Pack", true, "for ridge"},
    };
    const std::vector<std::string> texts = mp::map_list_texts(rows);
    const std::vector<std::string> keys{
        "comet@sample-comet",
        "Comet Catcher",
        "ponds@sample-ponds",
        "ashes@archipelago",
        "bare@archipelago",
        "ridge@sample",
    };
    expect(texts.size() == keys.size(), "every row has words");
    if (texts.size() != keys.size())
        return;
    expect(texts[0] == "Comet Catcher (Cavedog: Comet Catcher)", "a shared title names the pack");
    expect(texts[1] == "Comet Catcher", "a base map keeps its file name");
    expect(texts[2] == "A Plethora of Ponds", "a pack map shows its index title");
    expect(texts[3] == "Isle of Ashes", "a pack map with no index name shows the map file's title");
    expect(texts[4] == "bare", "a pack map with no title shows its own name");
    expect(texts[5] == "Ridge Pond  for ridge", "a greyed row shows why");
    for (std::size_t index = 0; index < texts.size(); ++index) {
        if (!rows[index].pack)
            continue;
        expect(
            texts[index].find('@') == std::string::npos,
            ("a pack map's shown name has no '@': " + texts[index]).c_str()
        );
    }
    const int32_t by_words = mp::map_list_row(texts, keys, texts[0]);
    const int32_t by_name = mp::map_list_row(texts, keys, keys[0]);
    expect(
        by_words == 0 && by_name == 0,
        "the marked words and the pack map's name choose the same map"
    );
    expect(mp::map_list_row(texts, keys, "Comet Catcher") == 1, "the base map's words choose it");
    expect(mp::map_list_row(texts, keys, "") == -1, "an empty choice matches nothing");
    expect(mp::map_list_row(texts, keys, "Missing") == -1, "an unknown choice matches nothing");

    // A greyed row names an expansion or a game in the language shown; a
    // mod's name, which the catalogue does not hold, stays as it is.
    oa::data::languages::InterfaceText catalogue;
    expect(
        catalogue.add(
            "[needs {name}]\n{\nde=braucht {name};\n}\n"
            "[for {name}]\n{\nde=für {name};\n}\n"
            "[Cavedog units]\n{\nde=Cavedog-Einheiten;\n}\n"
        ),
        "the catalogue reads"
    );
    const oa::data::languages::Language* german = oa::data::languages::find_by_tag("de");
    expect(german != nullptr, "German is known");
    if (german == nullptr)
        return;
    oa::data::languages::set_interface_language(&catalogue, *german);
    const mp::MapListFact greyed[] = {
        {"moon@sample", "Moon Pond", {}, "Moon Pack", true, "needs Cavedog units"},
        {"ridge@sample", "Ridge Pond", {}, "Ridge Pack", true, "for ridge"},
    };
    const std::vector<std::string> german_texts = mp::map_list_texts(greyed);
    oa::data::languages::set_interface_language(nullptr, oa::data::languages::english());
    expect(german_texts.size() == 2, "every greyed row has words");
    if (german_texts.size() != 2)
        return;
    expect(
        german_texts[0] == "Moon Pond  braucht Cavedog-Einheiten",
        ("an expansion's name is translated: " + german_texts[0]).c_str()
    );
    expect(
        german_texts[1] == "Ridge Pond  für ridge",
        ("a mod's name stays as it is: " + german_texts[1]).c_str()
    );
}

/// The keys `map_list_order` puts first.
std::vector<std::string> ordered_keys(const mp::MapListFact* rows, std::size_t count) {
    const std::vector<std::size_t> order = mp::map_list_order({rows, count});
    std::vector<std::string> keys;
    keys.reserve(order.size());
    for (const std::size_t index : order)
        keys.emplace_back(rows[index].key);
    return keys;
}

/// Base maps, a pack map and a downloadable map are one name order. A greyed
/// map keeps its place. Case, punctuation and equal names follow the game's
/// own map list.
void check_map_list_order() {
    const mp::MapListFact mixed[] = {
        {"Yerrot Mountains", {}, {}, {}, false},
        {"Wretched Ridges", {}, {}, {}, false},
        {"abysmal_lake@downloads", "Abysmal Lake", {}, "Downloads", true},
        {"ancient_issaquah@downloads", "Ancient Issaquah", {}, "Downloads", true, "doesn't fit"},
    };
    expect(
        ordered_keys(mixed, 4) ==
            std::vector<std::string>{
                "abysmal_lake@downloads",
                "ancient_issaquah@downloads",
                "Wretched Ridges",
                "Yerrot Mountains",
            },
        "base, pack and downloadable maps are one shown-name order"
    );
    const mp::MapListFact grey_first[] = {
        {"grey@id", "B", {}, "Only", true, "doesn't fit"},
        {"B ", {}, {}, {}, false},
    };
    expect(
        ordered_keys(grey_first, 2) == std::vector<std::string>{"grey@id", "B "},
        "a grey reason does not move the row"
    );
    const mp::MapListFact letters[] = {
        {"z", {}, {}, {}, false},
        {"Alpha", {}, {}, {}, false},
        {"alpha", {}, {}, {}, false},
        {"B", {}, {}, {}, false},
    };
    expect(
        ordered_keys(letters, 4) == std::vector<std::string>{"Alpha", "alpha", "B", "z"},
        "letters fold and equal folds keep their order"
    );
    const mp::MapListFact marks[] = {
        {"Coast_To", {}, {}, {}, false},
        {"Coast-To", {}, {}, {}, false},
        {"Coast To Coast", {}, {}, {}, false},
    };
    expect(
        ordered_keys(marks, 3) ==
            std::vector<std::string>{"Coast To Coast", "Coast-To", "Coast_To"},
        "punctuation keeps its byte"
    );
    const mp::MapListFact same[] = {
        {"b@one", "Alpha", {}, "Same", true},
        {"a@two", "Alpha", {}, "Same", true},
    };
    expect(
        ordered_keys(same, 2) == std::vector<std::string>{"b@one", "a@two"},
        "equal shown names keep the order they were gathered in"
    );
    const mp::MapListFact split[] = {
        {"z", {}, {}, {}, false},
        {"B", {}, {}, {}, false},
        {"\\folder", {}, {}, {}, false},
        {"d", {}, {}, {}, false},
        {"A", {}, {}, {}, false},
    };
    expect(
        ordered_keys(split, 5) == std::vector<std::string>{"\\folder", "B", "z", "A", "d"},
        "a name that starts with a backslash splits the list as the game's own list does"
    );
    bool caught = false;
    try {
        std::vector<mp::MapListFact> too_many(3001, mp::MapListFact{"x", {}, {}, {}, false});
        (void)mp::map_list_order(too_many);
    } catch (const std::invalid_argument&) {
        caught = true;
    }
    expect(caught, "a list past the sorting limit is refused");
}

/// Clicks a map row, scrolling it onto the page first.
void click_map_row(Driver& d, mp::Control& names, int32_t row, int32_t ox, int32_t oy) {
    const int32_t visible =
        names.list_item_height > 0 ? std::max(names.height / names.list_item_height, 1) : 1;
    if (row < names.list_first || row >= names.list_first + visible)
        names.list_first = static_cast<int16_t>(std::max(row, int32_t{0}));
    d.pointer_click(
        ox + names.x + 20,
        oy + names.y + 2 + (row - names.list_first) * names.list_item_height +
            names.list_item_height / 2
    );
}

/// Base maps supplied by the bound source, and no scan of the installed maps.
///
/// One of them is an installed map, so the battle room can select it. Its
/// description is not the one in its map file.
struct BoundBaseMaps {
    static constexpr const char* kReal = "Coast To Coast";
    static constexpr const char* kRidge = "ridge";
    static constexpr const char* kDescription = "Bound by the one start scan.";

    static int32_t count(void*) { return 1; }

    static bool at(void*, int32_t index, mp::LobbyPackMap* out) {
        if (index != 0 || out == nullptr)
            return false;
        *out = {PackProbe::kName, "A ring of cinders", "16x16", 16};
        return true;
    }

    static bool prepare(void*, const char*, char*, std::size_t) { return true; }

    static void release(void*) {}

    static int32_t base_count(void*) { return 2; }

    static bool base_at(void*, int32_t index, mp::LobbyPackMap* out) {
        if (out == nullptr)
            return false;
        if (index == 0) {
            *out = {kReal, kDescription, "12 x 12", 16};
            return true;
        }
        if (index == 1) {
            *out = {kRidge, "A high ridge", "20 x 20", 24};
            return true;
        }
        return false;
    }

    mp::LobbyMapSource source() {
        mp::LobbyMapSource bound{};
        bound.context = this;
        bound.count = count;
        bound.at = at;
        bound.prepare = prepare;
        bound.release = release;
        bound.base_count = base_count;
        bound.base_at = base_at;
        return bound;
    }
};

/// The battle room's map list is the bound base maps plus the pack map, and
/// not the installed maps. The binding is cleared when the check ends.
void check_bound_base_maps(Driver& d) {
    struct Guard {
        ~Guard() { mp::multiplayer_bind_map_source({}); }
    } guard;

    BoundBaseMaps bound;
    mp::multiplayer_bind_map_source(bound.source());
    mp::multiplayer_reset();
    if (!host_battleroom(d, "Ridge", "Host"))
        return;
    expect(d.click("MAP"), "MAP opens on the bound base maps");
    auto* modal = mp::multiplayer_modal();
    auto* names = modal != nullptr ? mp::panel_control(*modal, "MAPNAMES") : nullptr;
    expect(names != nullptr, "SELMAP lists the bound maps");
    if (names == nullptr)
        return;
    const std::vector<std::string> listed(names->items.begin(), names->items.end());
    const std::vector<std::string> keys(names->item_keys.begin(), names->item_keys.end());
    std::vector<std::string> expected{
        BoundBaseMaps::kReal, PackProbe::kName, BoundBaseMaps::kRidge
    };
    std::sort(expected.begin(), expected.end());
    const bool other = std::find(listed.begin(), listed.end(), "Two Continents") != listed.end() ||
                       std::find(listed.begin(), listed.end(), "two continents") != listed.end();
    // A pack map's row shows its own name; its key keeps the package.
    std::vector<std::string> shown_names;
    for (const std::string& key : keys)
        shown_names.push_back(mp::multiplayer_shown_map_name(key.c_str()));
    expect(
        listed == shown_names && keys == expected && !other,
        "the bound list is the supplied maps and no other installed map"
    );
    expect(
        mp::panel_text(*modal, "DESCRIPTION") == BoundBaseMaps::kDescription,
        "the selected map's description is the one the source supplied"
    );
}

/// Select Map shows a base map and two pack maps, one titled like the base map.
/// Choosing that pack map by its words or by its name is the same row.
void check_pack_map_titles(Driver& d) {
    struct Guard {
        ~Guard() { mp::multiplayer_bind_map_source({}); }
    } guard;

    struct TitledMaps {
        static int32_t count(void*) { return 2; }

        static bool at(void*, int32_t index, mp::LobbyPackMap* out) {
            if (out == nullptr)
                return false;
            if (index == 0) {
                *out = {
                    "comet@sample-comet",
                    "A catch",
                    "16x16",
                    8,
                    "Comet Catcher",
                    "Cavedog: Comet Catcher"
                };
                return true;
            }
            if (index == 1) {
                *out = {
                    "ponds@sample-ponds",
                    "Many ponds",
                    "32x32",
                    12,
                    "A Plethora of Ponds",
                    "Sample Ponds"
                };
                return true;
            }
            return false;
        }

        static bool prepare(void*, const char*, char*, std::size_t) { return true; }

        static void release(void*) {}

        static int32_t base_count(void*) { return 2; }

        static bool base_at(void*, int32_t index, mp::LobbyPackMap* out) {
            if (out == nullptr)
                return false;
            if (index == 0) {
                *out = {"Coast To Coast", "A shore", "12 x 12", 16};
                return true;
            }
            if (index == 1) {
                *out = {"Comet Catcher", "The original", "16x16", 8};
                return true;
            }
            return false;
        }

        mp::LobbyMapSource source() {
            return {this, count, at, prepare, release, base_count, base_at};
        }
    };

    TitledMaps titled;
    mp::multiplayer_bind_map_source(titled.source());
    mp::multiplayer_reset();
    if (!host_battleroom(d, "Titles", "Host"))
        return;
    expect(d.click("MAP"), "MAP opens on the titled pack maps");
    auto* modal = mp::multiplayer_modal();
    auto* names = modal != nullptr ? mp::panel_control(*modal, "MAPNAMES") : nullptr;
    expect(names != nullptr, "SELMAP lists the titled maps");
    if (names == nullptr)
        return;
    const std::vector<std::string> want_keys{
        "ponds@sample-ponds", "Coast To Coast", "Comet Catcher", "comet@sample-comet"
    };
    const std::vector<std::string> want_text{
        "A Plethora of Ponds",
        "Coast To Coast",
        "Comet Catcher",
        "Comet Catcher (Cavedog: Comet Catcher)",
    };
    const std::vector<std::string> shown(names->items.begin(), names->items.end());
    const std::vector<std::string> keys(names->item_keys.begin(), names->item_keys.end());
    expect(
        shown == want_text && keys == want_keys,
        "the rows are one name order, and the names stay with their titles"
    );
    const int32_t by_text = mp::map_list_row(names->items, names->item_keys, want_text[3]);
    const int32_t by_key = mp::map_list_row(names->items, names->item_keys, "comet@sample-comet");
    const int32_t base = mp::map_list_row(names->items, names->item_keys, "Comet Catcher");
    expect(
        by_text == by_key && by_text == 3 && base == 2 &&
            map_row_key(*names, static_cast<std::size_t>(by_key)) == "comet@sample-comet",
        "the pack map's words and its name choose it, and the base map's words choose the base map"
    );
}

/// A pack map past Select Map's first page is reached by paging the down
/// arrow, then chosen. A player's click on that arrow still steps the knob once.
void check_pack_map_page(Driver& d) {
    struct Guard {
        ~Guard() { mp::multiplayer_bind_map_source({}); }
    } guard;

    struct PageSource {
        static int32_t count(void*) { return 1; }

        static bool at(void*, int32_t index, mp::LobbyPackMap* out) {
            if (index != 0 || out == nullptr)
                return false;
            *out = {"isle_one@oa-test-isles", "One isle", "16x16", 16};
            return true;
        }

        static bool prepare(void*, const char*, char*, std::size_t) { return true; }

        static void release(void*) {}

        // One real installed map, so the battle room has a map file selected.
        // Select Map stays closed when the selected map has no file. Its name
        // sorts ahead of the generated names, and the pack map stays past the
        // page. The generated names start with d, which sorts after Coast and
        // before the pack map's own name.
        static int32_t base_count(void*) { return 37; }

        static bool base_at(void*, int32_t index, mp::LobbyPackMap* out) {
            if (out == nullptr || index < 0 || index >= 37)
                return false;
            if (index == 0) {
                *out = {BoundBaseMaps::kReal, "A shore", "12 x 12", 16};
                return true;
            }
            static char names[36][4]{};
            static bool filled = false;
            if (!filled) {
                for (int place = 0; place < 36; ++place) {
                    names[place][0] = 'd';
                    names[place][1] = static_cast<char>('0' + place / 10);
                    names[place][2] = static_cast<char>('0' + place % 10);
                    names[place][3] = '\0';
                }
                filled = true;
            }
            *out = {names[index - 1], "A plain", "8x8", 8};
            return true;
        }

        mp::LobbyMapSource source() {
            mp::LobbyMapSource bound{};
            bound.context = this;
            bound.count = count;
            bound.at = at;
            bound.prepare = prepare;
            bound.release = release;
            bound.base_count = base_count;
            bound.base_at = base_at;
            return bound;
        }
    };

    PageSource pages;
    mp::multiplayer_bind_map_source(pages.source());
    mp::multiplayer_reset();
    if (!host_battleroom(d, "Isles", "Host"))
        return;
    expect(d.click("MAP"), "MAP opens on a list longer than one page");
    auto* modal = mp::multiplayer_modal();
    auto* names = modal != nullptr ? mp::panel_control(*modal, "MAPNAMES") : nullptr;
    auto* bar = modal != nullptr ? mp::panel_control(*modal, "SLIDER") : nullptr;
    expect(
        names != nullptr && bar != nullptr && mp::multiplayer_modal_kind() == mp::ModalKind::selmap,
        "SELMAP lists the maps and its scroll bar"
    );
    if (names == nullptr || bar == nullptr)
        return;
    int32_t pack_row = -1;
    for (int32_t row = 0; row < static_cast<int32_t>(names->items.size()); ++row) {
        if (map_row_key(*names, static_cast<std::size_t>(row)) == "isle_one@oa-test-isles")
            pack_row = row;
    }
    const int32_t shown =
        names->list_item_height > 0 ? std::max(names->height / names->list_item_height, 1) : 1;
    expect(
        pack_row >= shown && names->list_first == 0,
        "the pack map is listed and starts off the first page"
    );
    if (pack_row < shown)
        return;
    const auto& down = bar->scroll.forward_arrow;
    const auto& up = bar->scroll.back_arrow;
    expect(down.width > 0 && up.width > 0, "SELMAP's scroll arrows are shown");
    if (down.width <= 0 || up.width <= 0)
        return;

    const Art map_art =
        load_art(d.assets, "guis/selmap.gui", "bitmaps/dselectmap2.pcx", "anims/skirmish.gaf");
    const auto [ox, oy] = dialog_offset(map_art, modal->controls[0]);
    const int32_t down_x = ox + down.x + down.width / 2;
    const int32_t down_y = oy + down.y + down.height / 2;
    const int32_t up_x = ox + up.x + up.width / 2;
    const int32_t up_y = oy + up.y + up.height / 2;
    expect(
        mp::multiplayer_list_page_arrow(down_x, down_y) &&
            mp::multiplayer_list_page_arrow(up_x, up_y) &&
            !mp::multiplayer_list_page_arrow(ox + down.x - 1, down_y),
        "the arrows page a list, and a point beside one does not"
    );

    const int32_t knob = bar->scroll.knob;
    d.pointer_click(down_x, down_y);
    expect(
        bar->scroll.knob == knob + 1 && names->list_first == 0,
        "a player's click on the down arrow steps the knob once"
    );

    const int32_t before = names->list_first;
    expect(
        mp::multiplayer_scroll_list_page(down_x, down_y) && names->list_first == before + shown,
        "an endpoint click moves one page, the rows the list shows"
    );
    int steps = 1;
    while (pack_row < names->list_first || pack_row >= names->list_first + shown) {
        if (steps >= 8 || !mp::multiplayer_scroll_list_page(down_x, down_y))
            break;
        ++steps;
    }
    expect(
        pack_row >= names->list_first && pack_row < names->list_first + shown,
        "paging brings the pack map onto the page"
    );
    if (pack_row < names->list_first || pack_row >= names->list_first + shown)
        return;
    const int32_t paged = names->list_first;
    expect(
        mp::multiplayer_scroll_list_page(up_x, up_y) && names->list_first == paged - shown,
        "the up arrow moves one page toward the start"
    );
    int back = 0;
    while (pack_row < names->list_first || pack_row >= names->list_first + shown) {
        if (back >= 8 || !mp::multiplayer_scroll_list_page(down_x, down_y))
            break;
        ++back;
    }
    click_map_row(d, *names, pack_row, ox, oy);
    expect(
        names->list_selection == pack_row &&
            map_row_key(*names, static_cast<std::size_t>(pack_row)) == "isle_one@oa-test-isles" &&
            names->items[static_cast<std::size_t>(pack_row)] ==
                mp::multiplayer_shown_map_name("isle_one@oa-test-isles"),
        "the pack map's row can be chosen once it is on the page"
    );
}

/// The battle room lists an installed pack map, mounts it, refuses one, and a
/// joiner without it sees the name the host sent.
void check_pack_maps(Driver& d) {
    const auto folder = std::filesystem::temp_directory_path() / "oa-p13-isle-of-ashes@archipelago";

    struct Guard {
        std::filesystem::path folder;
        oa::AssetStore* assets = nullptr;

        ~Guard() {
            if (assets != nullptr) {
                assets->observe_lookups({});
                if (assets->pack_layer_mounted())
                    (void)assets->unmount_pack_layer();
            }
            std::error_code error;
            std::filesystem::remove_all(folder, error);
            mp::multiplayer_bind_map_source({});
        }
    } guard{folder, &d.assets};

    expect(write_pack_folder(folder), "the pack map's files are written");

    std::vector<std::string> lookups;
    d.assets.observe_lookups(
        {&lookups, [](void* context, std::string_view path) {
             static_cast<std::vector<std::string>*>(context)->emplace_back(path);
         }}
    );
    PackProbe probe;
    probe.assets = &d.assets;
    probe.folder = folder;
    probe.mount = true;
    mp::multiplayer_bind_map_source(probe.source());
    mp::multiplayer_reset();
    if (!host_battleroom(d, "Ashes", "Host"))
        return;
    expect(d.click("MAP"), "MAP opens on the pack map's list");
    auto* modal = mp::multiplayer_modal();
    auto* names = modal != nullptr ? mp::panel_control(*modal, "MAPNAMES") : nullptr;
    expect(
        names != nullptr && mp::multiplayer_modal_kind() == mp::ModalKind::selmap,
        "SELMAP lists the maps"
    );
    if (names == nullptr)
        return;
    int32_t listed = 0;
    int32_t pack_row = -1;
    int32_t base_row = -1;
    for (int32_t row = 0; row < static_cast<int32_t>(names->items.size()); ++row) {
        if (map_row_key(*names, static_cast<std::size_t>(row)) != PackProbe::kName)
            continue;
        ++listed;
        pack_row = row;
    }
    for (int32_t row = 0; row < static_cast<int32_t>(names->items.size()); ++row) {
        if (map_row_key(*names, static_cast<std::size_t>(row)) != PackProbe::kName) {
            base_row = row;
            break;
        }
    }
    const bool looked_up = std::any_of(lookups.begin(), lookups.end(), [](const std::string& path) {
        return path.find(PackProbe::kName) != std::string::npos;
    });
    expect(listed == 1 && pack_row >= 0, "MAPNAMES lists the pack map once, among the base maps");
    expect(
        pack_row >= 0 && names->items[static_cast<std::size_t>(pack_row)] ==
                             mp::multiplayer_shown_map_name(PackProbe::kName),
        "a pack map with no title shows its name"
    );
    expect(!looked_up, "the pack map's files are not read while the list is built");
    if (pack_row < 0 || base_row < 0)
        return;

    const Art map_art =
        load_art(d.assets, "guis/selmap.gui", "bitmaps/dselectmap2.pcx", "anims/skirmish.gaf");
    const auto [ox, oy] = dialog_offset(map_art, modal->controls[0]);
    probe.prepares = 0;
    probe.releases = 0;
    click_map_row(d, *names, pack_row, ox, oy);
    expect(
        names->list_selection == pack_row && probe.prepares == 1,
        "choosing the pack map mounts its files"
    );
    const bool read_on_choice =
        std::any_of(lookups.begin(), lookups.end(), [](const std::string& path) {
            return path.find(PackProbe::kName) != std::string::npos;
        });
    expect(read_on_choice, "the pack map's files are read once it is chosen");
    click_map_row(d, *names, base_row, ox, oy);
    expect(
        names->list_selection == base_row && probe.releases == 1,
        "choosing a base map unmounts the pack map"
    );
    click_map_row(d, *names, pack_row, ox, oy);
    expect(d.click("LOAD"), "LOAD chooses the pack map");
    d.frame();
    expect(
        mp::panel_text(mp::multiplayer_panel(), "MAPNAME") == "Isle of Ashes",
        "the battle room names the pack map by the title its OTA gives"
    );
    const auto& chosen = mp::local_info(mp::multiplayer_lobby());
    const auto length = ::strnlen(chosen.map_name, sizeof chosen.map_name);
    expect(
        length == std::strlen(PackProbe::kName) &&
            std::memcmp(chosen.map_name, PackProbe::kName, length) == 0 &&
            length < sizeof chosen.map_name && chosen.map_name[length] == '\0' &&
            chosen.map_hash != 0,
        "LOAD writes the pack map's whole name, terminated, and its hash"
    );

    // The index's name and the map file's title are different words. The
    // battle room shows the index's.
    probe.index_title = "Cinder Atoll";
    mp::multiplayer_bind_map_source(probe.source());
    mp::multiplayer_reset();
    if (!host_battleroom(d, "Atoll", "Host"))
        return;
    expect(d.click("MAP"), "MAP opens on the pack map again");
    modal = mp::multiplayer_modal();
    names = modal != nullptr ? mp::panel_control(*modal, "MAPNAMES") : nullptr;
    if (names == nullptr)
        return;
    pack_row = -1;
    for (int32_t row = 0; row < static_cast<int32_t>(names->items.size()); ++row)
        if (map_row_key(*names, static_cast<std::size_t>(row)) == PackProbe::kName)
            pack_row = row;
    if (pack_row < 0)
        return;
    expect(
        names->items[static_cast<std::size_t>(pack_row)] == "Cinder Atoll",
        "the pack map's row shows the name its index gives"
    );
    const auto [ax, ay] = dialog_offset(map_art, modal->controls[0]);
    click_map_row(d, *names, pack_row, ax, ay);
    expect(d.click("LOAD"), "LOAD chooses the pack map by its index name");
    d.frame();
    expect(
        mp::panel_text(mp::multiplayer_panel(), "MAPNAME") == "Cinder Atoll",
        "the battle room names the pack map by the name its index gives, not the map file's title"
    );
    probe.index_title = nullptr;

    probe.mount = false;
    probe.refuse = true;
    mp::multiplayer_bind_map_source(probe.source());
    mp::multiplayer_reset();
    if (!host_battleroom(d, "Refused", "Host"))
        return;
    const std::string kept(
        mp::local_info(mp::multiplayer_lobby()).map_name,
        ::strnlen(
            mp::local_info(mp::multiplayer_lobby()).map_name,
            sizeof mp::local_info(mp::multiplayer_lobby()).map_name
        )
    );
    expect(d.click("MAP"), "MAP opens on a map the game will not play");
    modal = mp::multiplayer_modal();
    names = modal != nullptr ? mp::panel_control(*modal, "MAPNAMES") : nullptr;
    if (names == nullptr)
        return;
    pack_row = -1;
    for (int32_t row = 0; row < static_cast<int32_t>(names->items.size()); ++row)
        if (map_row_key(*names, static_cast<std::size_t>(row)) == PackProbe::kName)
            pack_row = row;
    if (pack_row < 0)
        return;
    const auto [rx, ry] = dialog_offset(map_art, modal->controls[0]);
    click_map_row(d, *names, pack_row, rx, ry);
    auto* picture = mp::panel_control(*modal, "MAPPIC");
    expect(
        names->list_selection == pack_row &&
            map_row_key(*names, static_cast<std::size_t>(pack_row)) == PackProbe::kName &&
            names->items[static_cast<std::size_t>(pack_row)] ==
                mp::multiplayer_shown_map_name(PackProbe::kName) &&
            mp::panel_text(*modal, "DESCRIPTION") == "Doesn't fit this game: too many features" &&
            picture != nullptr && picture->active == 0,
        "a refused map shows its name, why it does not fit, and no picture"
    );
    expect(d.click("LOAD"), "LOAD on the refused map");
    expect(
        mp::multiplayer_message() == probe.reason &&
            mp::multiplayer_modal_kind() == mp::ModalKind::selmap &&
            std::string(
                mp::local_info(mp::multiplayer_lobby()).map_name,
                ::strnlen(
                    mp::local_info(mp::multiplayer_lobby()).map_name,
                    sizeof mp::local_info(mp::multiplayer_lobby()).map_name
                )
            ) == kept,
        "the refusal is shown and the host's map stays"
    );

    mp::multiplayer_bind_map_source({});
    mp::multiplayer_reset();
    if (!host_battleroom(d, "Missing", "Host"))
        return;
    expect(d.click("PREVMENU"), "the host leaves for the joiner");
    if (!d.follow(mp::kScreenGameList, "the joiner returns to the game list"))
        return;
    mp::panel_set_text(mp::multiplayer_panel(), "NICKNAME", "Guest");
    expect(d.click("JOINGAME"), "the joiner joins");
    if (!d.follow(mp::kScreenBattleroom, "the joiner opens the battle room"))
        return;
    auto& lobby = mp::multiplayer_lobby();
    d.frame();
    const std::string own(
        mp::local_info(lobby).map_name,
        ::strnlen(mp::local_info(lobby).map_name, sizeof mp::local_info(lobby).map_name)
    );
    mp::local_info(lobby).options |= mp::option::ready;
    const auto& player = mp::local_player(lobby);
    const std::string who(player.name, ::strnlen(player.name, sizeof player.name));
    const std::string line = "<" + who + "> does not have this map";
    const auto said = chat_lines(lobby, line);
    mp::LobbyEvent joined{};
    joined.kind = mp::LobbyEventKind::player_joined;
    joined.player_id = 0x7700;
    std::snprintf(joined.name, sizeof joined.name, "%s", "Host");
    mp::PlayerSetupInfo host{};
    host.role = mp::kRoleHost;
    host.version_major = 3;
    host.version_minor = 1;
    host.state = mp::kInfoStatePlaying;
    host.map_hash = mp::local_info(lobby).map_hash ^ 0x01010101U;
    std::snprintf(host.map_name, sizeof host.map_name, "%s", PackProbe::kName);
    oa::netgame::PlayerInfoRecord record{};
    std::memcpy(record.info_head, &host, sizeof record.info_head);
    record.player_id = joined.player_id;
    std::memcpy(
        record.info_tail,
        reinterpret_cast<const uint8_t*>(&host) + oa::netgame::player_info_tail_offset,
        sizeof record.info_tail
    );
    mp::LobbyEvent info{};
    info.kind = mp::LobbyEventKind::record;
    info.player_id = joined.player_id;
    std::size_t written = 0;
    (void)oa::netgame::encode_record(record, info.data, sizeof info.data, &written);
    info.size = static_cast<uint16_t>(written);
    auto& loopback = mp::multiplayer_loopback();
    expect(
        mp::loopback_inject(loopback, joined) && mp::loopback_inject(loopback, info),
        "the host's pack map is queued"
    );
    d.frame();
    expect(
        mp::panel_text(mp::multiplayer_panel(), "MAPNAME") ==
                mp::multiplayer_shown_map_name(PackProbe::kName) &&
            chat_lines(lobby, line) == said + 1,
        "the line a 3.1c joiner sees names isle_of_ashes@archipelago, since it receives the same "
        "name"
    );
    expect(
        (mp::local_info(lobby).options & mp::option::ready) == 0 &&
            std::string(
                mp::local_info(lobby).map_name,
                ::strnlen(mp::local_info(lobby).map_name, sizeof mp::local_info(lobby).map_name)
            ) == own,
        "the joiner is not ready and still names its own map"
    );
    d.frame();
    expect(chat_lines(lobby, line) == said + 1, "the missing map is said once");
}

/// A colour of the drawn screen: red, green and blue.
using Rgb = std::array<uint8_t, 3>;

/// The OA badge's border, #7d6a2c, and its amber, #e2b23d, as the design gives them.
constexpr Rgb kBadgeBorder{0x7d, 0x6a, 0x2c};
constexpr Rgb kBadgeAmber{0xe2, 0xb2, 0x3d};

/// A rectangle of the drawn 640x480 screen.
struct Box {
    int32_t x{}, y{}, width{}, height{};
};

/// Returns a control's rectangle on the screen: its own, moved by the panel's offset.
Box badge_box(const mp::Control& control) {
    const auto offset = mp::multiplayer_panel_offset();
    return {control.x + offset.x, control.y + offset.y, control.width, control.height};
}

/// Tells whether a badge the battle room lists lies at a rectangle.
bool same_box(const mp::PresenceBadgeSpot& spot, const Box& box) {
    return spot.x == box.x && spot.y == box.y && spot.width == box.width &&
           spot.height == box.height;
}

/// Tells whether a pixel of the drawn screen has a colour.
bool pixel_is(const Driver& d, int32_t x, int32_t y, const Rgb& color) {
    if (x < 0 || y < 0 || x >= static_cast<int32_t>(d.surface.width) ||
        y >= static_cast<int32_t>(d.surface.height))
        return false;
    const auto at =
        (static_cast<std::size_t>(y) * d.surface.width + static_cast<std::size_t>(x)) * 3U;
    return d.surface.rgb[at] == color[0] && d.surface.rgb[at + 1] == color[1] &&
           d.surface.rgb[at + 2] == color[2];
}

/// Tells whether a badge's text shows: some pixel inside its border is amber.
bool shows_badge_text(const Driver& d, const Box& box) {
    for (int32_t y = box.y + 1; y < box.y + box.height - 1; ++y)
        for (int32_t x = box.x + 1; x < box.x + box.width - 1; ++x)
            if (pixel_is(d, x, y, kBadgeAmber))
                return true;
    return false;
}

/// Returns a presence record with an engine line and a sim hash of 32 equal bytes.
std::vector<uint8_t> presence_with_hash(uint8_t hash_byte) {
    oa::netgame::PresenceRecord record;
    record.engine = oa::netgame::PresenceEngine{"0.8.0", "macOS", "arm64"};
    record.sim_hash = oa::netgame::PresenceDigest{};
    record.sim_hash->fill(hash_byte);
    std::vector<uint8_t> bytes(oa::netgame::presence_record_max_bytes);
    std::size_t written = 0;
    expect(
        oa::netgame::encode_presence(record, bytes.data(), bytes.size(), &written) ==
            oa::netgame::WireError::ok,
        "a test presence record encodes"
    );
    bytes.resize(written);
    return bytes;
}

/// The battle room draws the OA badge on each OA player's row, and the dot on
/// a guest's whose presence record carries another sim hash than the host's,
/// and lists both badges where it draws them.
void check_presence_badges(Driver& d) {
    struct Guard {
        ~Guard() { mp::multiplayer_bind_presence_record(nullptr, 0); }
    } guard;

    mp::multiplayer_reset();
    const auto host_record = presence_with_hash(0x5a);
    mp::multiplayer_bind_presence_record(host_record.data(), host_record.size());
    if (!host_battleroom(d, "Badges", "Host"))
        return;
    auto& lobby = mp::multiplayer_lobby();
    constexpr uint32_t kGuest = 0x4321;
    expect(mp::lobby_add_player(lobby, kGuest, "Guest"), "an OA guest joins");
    const auto slot = mp::slot_for_player_id(lobby, kGuest);
    auto* guest = slot >= 0 ? mp::slot_info(lobby, slot) : nullptr;
    expect(guest != nullptr, "the guest has a slot");
    if (guest == nullptr)
        return;
    guest->state = mp::kInfoStatePlaying;
    guest->color = 1;
    guest->version_major = 3;
    guest->version_minor = 1;
    guest->engine_signature[0] = oa::netgame::engine_signature_first;
    guest->engine_signature[1] = oa::netgame::engine_signature_second;
    const auto guest_record = presence_with_hash(0x77);
    mp::LobbyEvent event{};
    event.kind = mp::LobbyEventKind::record;
    event.player_id = kGuest;
    std::copy(guest_record.begin(), guest_record.end(), event.data);
    event.size = static_cast<uint16_t>(guest_record.size());
    expect(mp::lobby_apply_event(lobby, event), "the guest's record is kept");
    d.frame();
    expect(d.drawn(), "the battle room is drawn with the guest");
    d.snapshot("battleroom-badges");

    const auto* host_disc = control("CD0");
    const auto* guest_disc = control("CD" + std::to_string(slot));
    expect(
        host_disc != nullptr && guest_disc != nullptr, "the host's and the guest's CD rectangles"
    );
    if (host_disc == nullptr || guest_disc == nullptr)
        return;
    const auto host_box = badge_box(*host_disc);
    const auto guest_box = badge_box(*guest_disc);
    expect(guest_disc->active == 0, "the OA guest's row shows no CD icon");
    expect(
        pixel_is(d, guest_box.x, guest_box.y, kBadgeBorder) && shows_badge_text(d, guest_box),
        "the OA guest's row shows the OA badge"
    );
    expect(
        pixel_is(d, guest_box.x + guest_box.width - 1, guest_box.y, kBadgeAmber),
        "the guest's badge carries the dot: its rules differ from the host's"
    );
    expect(
        pixel_is(d, host_box.x + host_box.width - 1, host_box.y, kBadgeBorder),
        "the host's badge still carries no dot"
    );
    mp::PresenceBadgeSpot spots[mp::kSlotCount]{};
    const auto count = mp::multiplayer_presence_badges(spots, mp::kSlotCount);
    expect(
        count == 2 && spots[0].slot == 0 && same_box(spots[0], host_box) &&
            !spots[0].rules_differ && spots[1].slot == slot && same_box(spots[1], guest_box) &&
            spots[1].rules_differ,
        "multiplayer_presence_badges lists both badges, the guest's with the dot"
    );
    expect(
        mp::multiplayer_presence_badges(spots, 1) == 1 && spots[0].slot == 0,
        "a list of one takes the first badge"
    );
}

/// Draws the current screen again, without a tick.
void redraw(Driver& d) {
    const auto* screen = d.desc(d.current);
    screen->draw(&d.ctx, screen->state);
}

/// A colour of one pixel of the drawn screen.
Rgb pixel_at(const Driver& d, int32_t x, int32_t y) {
    if (x < 0 || y < 0 || x >= static_cast<int32_t>(d.surface.width) ||
        y >= static_cast<int32_t>(d.surface.height))
        return {};
    const auto at =
        (static_cast<std::size_t>(y) * d.surface.width + static_cast<std::size_t>(x)) * 3U;
    return {d.surface.rgb[at], d.surface.rgb[at + 1], d.surface.rgb[at + 2]};
}

/// One stop Tab reached, and whether that control is a list or a player's row.
struct FocusStop {
    std::string name;
    bool list = false;
    int32_t slot = -1;
};

/// The name of the focused control, or empty when nothing is focused.
std::string focused_name() {
    const auto& panel = mp::multiplayer_panel();
    if (panel.focus <= 0 || panel.focus >= panel.count)
        return {};
    return std::string(mp::control_name(panel.controls[static_cast<std::size_t>(panel.focus)]));
}

/// The slot whose OA badge the battle room would hand to the information key.
///
/// @return the focused row's slot when that row shows a badge; otherwise none
int32_t focused_badge_slot() {
    const int32_t slot = mp::multiplayer_focused_player_slot();
    if (slot < 0)
        return mp::presence_card_no_slot;
    mp::PresenceBadgeSpot spots[mp::kSlotCount]{};
    const auto count = mp::multiplayer_presence_badges(spots, mp::kSlotCount);
    for (int32_t index = 0; index < count; ++index)
        if (spots[index].slot == slot)
            return slot;
    return mp::presence_card_no_slot;
}

/// Seats a remote player and, when a record is given, marks the row as OA.
///
/// @param lobby the battle room
/// @param player_id the player's id
/// @param name the name the row shows
/// @param record the presence record; empty leaves a player with no OA badge
/// @return the slot, or -1 when the player is not seated
int32_t seat_remote(
    mp::Lobby& lobby, uint32_t player_id, const char* name, const std::vector<uint8_t>& record
) {
    if (!mp::lobby_add_player(lobby, player_id, name))
        return -1;
    const auto slot = mp::slot_for_player_id(lobby, player_id);
    auto* info = slot >= 0 ? mp::slot_info(lobby, slot) : nullptr;
    if (info == nullptr)
        return -1;
    info->state = mp::kInfoStatePlaying;
    info->color = static_cast<uint8_t>(slot);
    info->version_major = 3;
    info->version_minor = 1;
    if (record.empty())
        return slot;
    info->engine_signature[0] = oa::netgame::engine_signature_first;
    info->engine_signature[1] = oa::netgame::engine_signature_second;
    mp::LobbyEvent event{};
    event.kind = mp::LobbyEventKind::record;
    event.player_id = player_id;
    std::copy(record.begin(), record.end(), event.data);
    event.size = static_cast<uint16_t>(record.size());
    if (!mp::lobby_apply_event(lobby, event))
        return -1;
    return slot;
}

/// Presses Tab until the focused control has a name, or the walk is exhausted.
///
/// @param d the screens
/// @param name the control's name
/// @return true when that control is focused
bool tab_until_name(Driver& d, std::string_view name) {
    for (int guard = 0; guard < 64; ++guard) {
        if (focused_name() == name)
            return true;
        d.key(kKeyTab);
    }
    return focused_name() == name;
}

/// Presses Tab until a player's row is focused, or the walk is exhausted.
///
/// @param d the screens
/// @param slot the row's slot
/// @return true when that row is focused
bool tab_until_slot(Driver& d, int32_t slot) {
    for (int guard = 0; guard < 64; ++guard) {
        if (mp::multiplayer_focused_player_slot() == slot)
            return true;
        d.key(kKeyTab);
    }
    return mp::multiplayer_focused_player_slot() == slot;
}

/// Tab and the arrow keys reach each remote player's row, skip the local
/// row, and F1 on a focused OA row opens its card.
void check_player_row_focus(Driver& d) {
    struct Guard {
        ~Guard() { mp::multiplayer_bind_presence_record(nullptr, 0); }
    } guard;

    mp::multiplayer_reset();
    const auto host_record = presence_with_hash(0x11);
    mp::multiplayer_bind_presence_record(host_record.data(), host_record.size());
    if (!host_battleroom(d, "Rows", "Host"))
        return;
    auto& lobby = mp::multiplayer_lobby();
    const auto local = lobby.game->local_player_index;
    const auto* own = control("PLAYER" + std::to_string(local));
    expect(own != nullptr && own->type == mp::ControlType::label, "the local row stays a label");
    const auto first_oa = presence_with_hash(0x21);
    const auto second_oa = presence_with_hash(0x22);
    const int32_t kirby = seat_remote(lobby, 0x4321, "Kirby", first_oa);
    const int32_t mira = seat_remote(lobby, 0x4322, "Mira", second_oa);
    const int32_t classic = seat_remote(lobby, 0x4323, "OldSchool", {});
    expect(
        kirby >= 0 && mira >= 0 && classic >= 0 && kirby != local && mira != local &&
            classic != local,
        "two OA players and one without a badge join other rows"
    );
    if (kirby < 0 || mira < 0 || classic < 0)
        return;
    d.frame();
    expect(focused_name() == "MESSAGE", "the chat line keeps the focus as the room opens");
    expect(mp::multiplayer_focused_player_slot() < 0, "no player's row is focused yet");

    const auto* kirby_row = control("PLAYER" + std::to_string(kirby));
    expect(kirby_row != nullptr, "the first OA player's row is a control");
    if (kirby_row == nullptr)
        return;
    const auto kirby_box = badge_box(*kirby_row);
    redraw(d);
    const Rgb ring_before = pixel_at(d, kirby_box.x - 1, kirby_box.y);
    const auto sent_before = mp::multiplayer_loopback().sent_count;

    std::vector<FocusStop> walked;
    bool back_at_chat = false;
    for (int press = 0; press < 64 && !back_at_chat; ++press) {
        d.key(kKeyTab);
        FocusStop stop;
        stop.name = focused_name();
        const auto& panel = mp::multiplayer_panel();
        if (panel.focus > 0 && panel.focus < panel.count) {
            const auto& focused = panel.controls[static_cast<std::size_t>(panel.focus)];
            stop.list = focused.type == mp::ControlType::list_box;
        }
        stop.slot = mp::multiplayer_focused_player_slot();
        if (stop.name == "MESSAGE")
            back_at_chat = true;
        else
            walked.push_back(stop);
    }
    if (!back_at_chat) {
        std::fprintf(stderr, "focus walk:");
        for (const auto& stop : walked)
            std::fprintf(stderr, " %s", stop.name.c_str());
        std::fprintf(stderr, "\n");
    }
    expect(back_at_chat, "Tab returns to the chat line");
    bool saw_local = false;
    bool saw_kirby = false;
    bool saw_mira = false;
    bool saw_classic = false;
    bool lists_before_rows = true;
    bool seen_row = false;
    for (const auto& stop : walked) {
        if (stop.slot == local)
            saw_local = true;
        if (stop.slot == kirby)
            saw_kirby = true;
        if (stop.slot == mira)
            saw_mira = true;
        if (stop.slot == classic)
            saw_classic = true;
        if (stop.list && seen_row)
            lists_before_rows = false;
        if (stop.slot >= 0)
            seen_row = true;
    }
    expect(
        saw_kirby && saw_mira && saw_classic && !saw_local && lists_before_rows,
        "Tab reaches each remote row after the lists and skips the local row"
    );
    expect(mp::multiplayer_loopback().sent_count == sent_before, "moving the focus sends nothing");

    d.key(kKeyTab, kKeyShift);
    expect(
        mp::multiplayer_focused_player_slot() == classic,
        "Shift+Tab from the chat line focuses the last remote row, and names its slot"
    );
    d.key(kKeyDown);
    expect(focused_name() == "MESSAGE", "Down from the last remote row returns to the chat line");
    d.key(kKeyLeft);
    d.key(kKeyRight);
    expect(focused_name() == "MESSAGE", "Left and Right stay in the chat line");
    d.key(kKeyUp);
    expect(
        mp::multiplayer_focused_player_slot() == classic,
        "the pad's Up from the chat line focuses the last remote row"
    );
    d.key(kKeyLeft);
    expect(
        mp::multiplayer_focused_player_slot() == mira,
        "the pad's Left moves to the remote row above"
    );

    bool stayed_in_list = true;
    for (const auto& stop : walked) {
        if (!stop.list)
            continue;
        if (!tab_until_name(d, stop.name))
            stayed_in_list = false;
        const auto& panel = mp::multiplayer_panel();
        const auto* list = panel.focus > 0 && panel.focus < panel.count
                               ? &panel.controls[static_cast<std::size_t>(panel.focus)]
                               : nullptr;
        if (list == nullptr || list->list_item_height == 0)
            continue;
        const auto selection = list->list_selection;
        d.key(kKeyDown);
        d.key(kKeyUp);
        if (focused_name() != stop.name || list->list_selection != selection)
            stayed_in_list = false;
    }
    expect(stayed_in_list, "Up and Down stay inside a list and move its selection");

    expect(
        tab_until_slot(d, kirby) && mp::multiplayer_focused_player_slot() == kirby,
        "the focused OA row's slot is the one multiplayer_focused_player_slot names"
    );
    redraw(d);
    const Rgb ring_focused = pixel_at(d, kirby_box.x - 1, kirby_box.y);
    expect(
        ring_focused != ring_before, "a focused row is drawn with the battle room's focus marker"
    );
    mp::PresenceCardTrigger trigger;
    expect(trigger.key_info(focused_badge_slot()), "F1 on the focused OA row is taken");
    expect(trigger.open_slot() == kirby, "F1 opens that player's card");
    expect(
        tab_until_slot(d, mira) && trigger.focus(focused_badge_slot()),
        "the card follows the focus to the other OA row"
    );
    expect(trigger.open_slot() == mira, "the card is now the other player's");
    expect(tab_until_slot(d, classic), "Tab reaches the row with no OA badge");
    mp::PresenceCardTrigger classic_key;
    expect(
        focused_badge_slot() == mp::presence_card_no_slot &&
            !classic_key.key_info(focused_badge_slot()),
        "F1 on a focused row with no OA badge opens no card"
    );
    expect(tab_until_name(d, "MESSAGE"), "Tab returns to the chat line again");
    redraw(d);
    expect(
        pixel_at(d, kirby_box.x - 1, kirby_box.y) == ring_before,
        "with the focus back on the chat line the row's focus marker is gone"
    );

    // Return and the pad's A are the information key on a focused row.
    mp::PresenceCardTrigger row_key;

    struct RowKeyGuard {
        ~RowKeyGuard() { mp::multiplayer_bind_row_key(nullptr); }
    } row_key_guard;

    mp::multiplayer_bind_row_key(&row_key);
    const auto kirby_status = mp::slot_player(lobby, kirby).status;
    const auto kirby_used = mp::slot_player(lobby, kirby).in_use;
    const auto kirby_id = mp::slot_player(lobby, kirby).player_id;
    const auto mira_status = mp::slot_player(lobby, mira).status;
    const auto classic_status = mp::slot_player(lobby, classic).status;
    const auto sent_at_keys = mp::multiplayer_loopback().sent_count;
    expect(tab_until_slot(d, classic), "the row with no badge is focused for Return");
    d.key(kKeyReturn);
    expect(
        row_key.open_slot() == mp::presence_card_no_slot &&
            mp::multiplayer_modal_kind() == mp::ModalKind::none &&
            mp::slot_player(lobby, classic).status == classic_status &&
            mp::multiplayer_loopback().sent_count == sent_at_keys,
        "Return on a focused row with no OA badge does nothing and sends nothing"
    );
    expect(tab_until_slot(d, kirby), "the first OA row is focused for Return");
    d.key(kKeyReturn);
    expect(
        row_key.open_slot() == kirby && mp::multiplayer_modal_kind() == mp::ModalKind::none &&
            mp::slot_player(lobby, kirby).status == kirby_status &&
            mp::slot_player(lobby, kirby).in_use == kirby_used &&
            mp::slot_player(lobby, kirby).player_id == kirby_id &&
            mp::slot_player(lobby, mira).status == mira_status &&
            mp::multiplayer_loopback().sent_count == sent_at_keys,
        "Return on a focused OA row opens its card and does not reject the player"
    );

    int32_t computer = -1;
    for (int32_t slot = 0; slot < mp::kSlotCount; ++slot) {
        if (mp::slot_player(lobby, slot).status == mp::kSlotOpen) {
            computer = slot;
            break;
        }
    }
    expect(computer >= 0, "an open slot can hold an AI");
    if (computer >= 0) {
        mp::lobby_add_computer(lobby, computer);
        d.frame();
        const auto sent_at_ai = mp::multiplayer_loopback().sent_count;
        const auto ai_status = mp::slot_player(lobby, computer).status;
        expect(
            tab_until_slot(d, computer) && ai_status == mp::kSlotComputer,
            "the AI's row can take the focus"
        );
        d.key(kKeyReturn);
        expect(
            row_key.open_slot() == kirby &&
                mp::slot_player(lobby, computer).status == mp::kSlotComputer &&
                mp::multiplayer_modal_kind() == mp::ModalKind::none &&
                mp::multiplayer_loopback().sent_count == sent_at_ai,
            "Return on a focused AI row does not cycle it, close the card or send anything"
        );
    }

    const auto rejects_before = rejects_sent();
    expect(tab_until_slot(d, kirby), "the OA row is focused again before the click");
    expect(d.click(("PLAYER" + std::to_string(kirby)).c_str()), "the host clicks that row");
    expect(
        mp::multiplayer_modal_kind() == mp::ModalKind::confirm &&
            mp::slot_for_player_id(lobby, kirby_id) == kirby && rejects_sent() == rejects_before,
        "a click on the row still asks before rejecting, and sends no reject"
    );
    expect(d.click("CHOICE2"), "the question is declined");
    expect(
        mp::multiplayer_modal_kind() == mp::ModalKind::none &&
            mp::slot_player(lobby, kirby).player_id == kirby_id &&
            mp::slot_player(lobby, kirby).status == kirby_status,
        "declining the question leaves the player in the room"
    );
}

} // namespace

int main() {
    check_map_list_words();
    check_map_list_order();
    const std::filesystem::path data = oa::test::require_game_directory("the multiplayer screens");
    mp::multiplayer_reset();
    Driver d(data);
    for (ScreenId id = mp::kScreenProviders; id <= mp::kScreenBattleroom; ++id)
        expect(d.desc(id) != nullptr, "multiplayer screen registered");
    expect(d.registry.rejected == nullptr, "no registration rejected");

    // SELPROV
    d.go(mp::kScreenProviders);
    auto* dplay = mp::panel_control(mp::multiplayer_panel(), "DPLAY");
    expect(dplay != nullptr && dplay->items.size() == 4, "four providers listed");
    d.frame();
    expect(d.drawn(), "SELPROV drawn");
    d.snapshot("selprov");
    // Four providers fit DPLAY's six rows, so its scroll bar and arrows stay hidden.
    {
        const Art provider_art =
            load_art(d.assets, "guis/selprov.gui", "bitmaps/selconnect2.pcx", "anims/selprov.gaf");
        const auto* bar = control("SLIDER");
        expect(
            dplay != nullptr && dplay->height == 96 && dplay->list_last_first == 0 &&
                bar != nullptr && bar->x == 578 && bar->y == 99 && bar->height == 101 &&
                bar->active == 0,
            "DPLAY is trimmed to 96 pixels and its bar is bound and hidden"
        );
        expect(
            shows_background(d, provider_art, 578, 89, 16, 121),
            "the hidden bar and its arrows leave the background as it is"
        );
        expect(
            mp::panel_hit(mp::multiplayer_panel(), 585, 94) == mp::kNoControl,
            "the hidden arrows take no clicks"
        );
    }
    // A double click is two presses in a row: a press anywhere else between
    // two presses on DPLAY's selected row ends the pair.
    {
        d.go(mp::kScreenProviders);
        auto* providers = mp::panel_control(mp::multiplayer_panel(), "DPLAY");
        expect(providers != nullptr, "SELPROV lists the providers again");
        if (providers != nullptr) {
            // Row 1 is TCP/IP.
            providers->list_selection = 1;
            const int32_t row_x = providers->x + providers->width / 2;
            const int32_t row_y = providers->y + providers->list_item_height + 3;
            d.pointer_click(row_x, row_y);
            d.pointer_click(585, 94);
            d.pointer_click(row_x, row_y);
            d.frame();
            expect(
                !d.host.requested.has_value(),
                "a press between two presses on the selected row is no double click"
            );
            d.pointer_click(row_x, row_y);
            d.frame();
            if (d.follow(mp::kScreenTcp, "two presses in a row on the selected row choose it") &&
                d.click("PREV"))
                d.follow(mp::kScreenProviders, "PREV returns to the provider list");
        }
    }
    dplay->list_selection = 1;
    expect(d.click("SELECT"), "SELECT clicked");
    if (!d.follow(mp::kScreenTcp, "TCP/IP provider opens the address dialog"))
        return 1;

    // TCP
    expect(mp::multiplayer_modal_kind() == mp::ModalKind::tcp, "TCP dialog stacked");
    mp::multiplayer_type(&d.ctx, "127.0.0.1");
    expect(mp::panel_text(mp::multiplayer_panel(), "ADDRESS") == "127.0.0.1", "address typed");
    d.frame();
    expect(d.drawn(), "TCP dialog drawn");
    d.snapshot("tcp");
    expect(d.click("OK"), "OK clicked");
    if (!d.follow(mp::kScreenGameList, "address accepted opens the game list"))
        return 1;
    expect(d.host.strings["multiplayer|tcpaddr"] == "127.0.0.1", "address remembered");

    // SELGAME
    expect(mp::panel_control(mp::multiplayer_panel(), "GAMENAME")->items.empty(), "no games yet");
    d.frame();
    expect(d.drawn(), "SELGAME drawn");
    d.snapshot("selgame");
    expect(d.click("STARTNEW"), "STARTNEW clicked");
    if (!d.follow(mp::kScreenNewGame, "STARTNEW opens NEWMULTI"))
        return 1;

    // NEWMULTI
    mp::panel_set_text(mp::multiplayer_panel(), "GAMENAME", "");
    expect(d.click("OK"), "OK without a name");
    expect(mp::multiplayer_message() == "You must enter a game name", "game name required");
    d.frame();
    expect(d.drawn(), "NEWMULTI drawn with message");
    d.snapshot("newmulti");
    expect(d.click("OK"), "click dismisses the message box");
    expect(mp::multiplayer_message().empty(), "message box closed");
    mp::multiplayer_panel().focus = mp::panel_find(mp::multiplayer_panel(), "GAMENAME");
    mp::multiplayer_type(&d.ctx, "Friday");
    mp::panel_set_text(mp::multiplayer_panel(), "NICKNAME", "Host");
    expect(d.click("OK"), "OK clicked");
    if (!d.follow(mp::kScreenBattleroom, "hosting opens the battleroom"))
        return 1;

    // BATTLEROOM (host)
    // The battle room setup ends with the label walk: LOUNGE2.GUI's MEMx (attribs 18)
    // and RESCTEXT (17) gain bit 8; the START button keeps its 65538.
    const auto& gadgets = mp::multiplayer_screen_layout().gadgets;
    const auto attribs = [&](const char* name) {
        for (const auto& gadget : gadgets)
            if (gadget.common.name == name)
                return gadget.common.attributes;
        return -1;
    };
    expect(attribs("MEMx") == 26 && attribs("RESCTEXT") == 25, "battleroom labels shadowed");
    expect(attribs("START") == 65538, "battleroom buttons unchanged");
    auto& lobby = mp::multiplayer_lobby();
    expect(mp::panel_text(mp::multiplayer_panel(), "PLAYER0") == "Host", "host row");
    expect((mp::local_info(lobby).role & mp::kRoleHost) != 0, "local player hosts");
    expect(mp::local_info(lobby).map_name[0] != '\0', "a multiplayer map is selected");
    d.frame();
    expect(d.drawn(), "BATTLEROOM drawn");
    d.snapshot("battleroom");

    // The host's row: a dark Go? light, the colour square with the version on
    // it, the CD icon; START behind its closed doors.
    const Art art = load_art(d.assets);
    const auto* checkbox = sequence(art.screen.shared_sprites, "CHECKBOX");
    const auto* disc = sequence(art.screen.sprites, "CDx");
    const auto* doors = sequence(art.screen.sprites, "battlestart");
    const auto* sides = sequence(art.screen.sprites, "SIDEx");
    expect(
        checkbox != nullptr && disc != nullptr && doors != nullptr && sides != nullptr,
        "the battle room's art loads"
    );
    expect(shows_frame(d, art, checkbox, 0, control("READY0")), "the host's Go? light is dark");
    expect(
        shows_color(d, art, "LOGO0", mp::local_info(lobby).color, "3.1"),
        "the host's colour square shows its colour"
    );
    expect(shows_version(d, art, "LOGO0", "3.1"), "the host's version is centred on its square");
    expect(
        control("CD0") != nullptr && control("CD0")->active == 0 &&
            !shows_frame(d, art, disc, 0, control("CD0")),
        "the host's row shows no CD icon"
    );
    // In its place the host's row shows the OA badge, with no dot: the host's
    // rules are its own.
    if (const auto* host_disc = control("CD0")) {
        const auto box = badge_box(*host_disc);
        expect(
            pixel_is(d, box.x, box.y, kBadgeBorder),
            "the host's badge has its border at the top left"
        );
        expect(shows_badge_text(d, box), "the host's badge says OA in amber");
        expect(
            pixel_is(d, box.x + box.width - 1, box.y, kBadgeBorder),
            "the host's badge has its border at the top right: no dot"
        );
        mp::PresenceBadgeSpot spots[mp::kSlotCount]{};
        const auto count = mp::multiplayer_presence_badges(spots, mp::kSlotCount);
        expect(
            count == 1 && spots[0].slot == 0 && same_box(spots[0], box) && !spots[0].rules_differ,
            "multiplayer_presence_badges lists the host's badge at CD0's rectangle"
        );
    }
    expect(
        control("SIDE0") != nullptr &&
            shows_frame(d, art, sides, control("SIDE0")->stage, control("SIDE0")),
        "SIDE0 keeps its art"
    );
    // A side past the art shows its name on the bevel, and a side the art
    // has a frame for still shows that frame.
    if (sides != nullptr && control("SIDE0") != nullptr && sides->frames.size() > 1 &&
        sides->frames.size() < 7) {
        const auto saved_stage = control("SIDE0")->stage;
        std::vector<std::string> names;
        for (int index = 0; index < 7; ++index)
            names.push_back("Ridge" + std::to_string(index));
        mp::multiplayer_bind_side_names(names);
        mp::multiplayer_bind_side_count(7);
        const auto past = static_cast<int32_t>(sides->frames.size());
        mp::panel_set_stage(mp::multiplayer_panel(), "SIDE0", past);
        d.frame();
        const auto* named = control("SIDE0");
        const Box face = named != nullptr ? badge_box(*named) : Box{};
        const Rgb bevel_top{art.palette[17 * 4], art.palette[17 * 4 + 1], art.palette[17 * 4 + 2]};
        const Rgb bevel_bottom{art.palette[0], art.palette[1], art.palette[2]};
        bool caption_is_name = false;
        for (const auto& gadget : mp::multiplayer_screen_layout().gadgets) {
            if (gadget.common.name != "SIDE0")
                continue;
            if (const auto* fields = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields))
                caption_is_name = fields->text == names[static_cast<std::size_t>(past)];
        }
        expect(
            named != nullptr && caption_is_name && !shows_frame(d, art, sides, 0, named) &&
                pixel_is(d, face.x, face.y, bevel_top) &&
                pixel_is(d, face.x + face.width - 1, face.y + face.height - 1, bevel_bottom),
            "a side past the art shows its name on the bevel"
        );
        mp::panel_set_stage(mp::multiplayer_panel(), "SIDE0", 1);
        d.frame();
        expect(
            shows_frame(d, art, sides, 1, control("SIDE0")),
            "SIDE0 at stage 1 still shows its art frame"
        );
        mp::multiplayer_bind_side_names({});
        mp::panel_set_stage(mp::multiplayer_panel(), "SIDE0", saved_stage);
        d.frame();
    }
    expect(shows_frame(d, art, doors, 0, control("battlestart")), "closed doors hide START");

    // The sliders lie between their arrows and are drawn from the shared
    // SLIDERS art, METAL and ENERGY at 1000 and MAXUNITS at 250.
    const auto* sliders = sequence(art.screen.shared_sprites, "SLIDERS");
    expect(sliders != nullptr && sliders->frames.size() == 20, "the shared SLIDERS art");
    const auto metal_index = mp::panel_find(mp::multiplayer_panel(), "METAL");
    const auto* metal = control("METAL");
    const bool metal_arrows = metal != nullptr && metal->scroll.back_arrow.width != 0 &&
                              metal->scroll.forward_arrow.width != 0;
    expect(
        metal_index != mp::kNoControl && metal != nullptr && metal->x == 519 &&
            metal->width == 102 && metal->scroll.range == 88 && metal->scroll.knob == 9 &&
            metal_arrows,
        "METAL is bound between its arrows"
    );
    if (metal == nullptr || !metal_arrows)
        return 1;
    expect(shows_at(d, art, sliders, kBackArrowFrame, 510, 24), "METAL's back arrow");
    expect(shows_at(d, art, sliders, kForwardArrowFrame, 621, 24), "METAL's forward arrow");
    expect(shows_at(d, art, sliders, kTrackFrame, 551, 24), "METAL's track");
    expect(shows_at(d, art, sliders, kTrackEndFrame, 605, 24), "METAL's track end");
    expect(shows_at(d, art, sliders, kKnobFrame, 531, 27), "METAL's knob at step 9");
    expect(shows_at(d, art, sliders, kKnobFrame, 531, 61), "ENERGY's knob at step 9");
    expect(shows_at(d, art, sliders, kKnobFrame, 608, 377), "MAXUNITS's knob at its last step");
    expect(shows_at(d, art, sliders, kTrackStartFrame, 519, 374), "MAXUNITS's track start");

    // The pointer on METAL, on a clock the test steps.
    const auto real_clock = lobby.services.tick;
    lobby.services.tick = stepped_clock;
    stepped_tick = 5000;
    const auto sent_before = mp::multiplayer_loopback().sent_count;
    d.pointer_click(625, 32);
    d.frame();
    expect(
        metal->scroll.knob == 10 && mp::panel_text(mp::multiplayer_panel(), "METALTEXT") == "1100",
        "a click on the forward arrow steps METAL once"
    );
    expect(mp::multiplayer_loopback().sent_count > sent_before, "the step is sent");
    d.pointer(oa::app::ScreenInputKind::pointer_down, 625, 32);
    d.frame();
    expect(metal->scroll.knob == 11, "a held arrow steps at once");
    expect(
        shows_at(d, art, sliders, kForwardArrowFrame + 1, 621, 24),
        "a held arrow shows its held face"
    );
    for (int wait = 0; wait < 14; ++wait) {
        ++stepped_tick;
        d.frame();
    }
    expect(metal->scroll.knob == 11, "a held arrow waits 15 ticks");
    for (int repeat = 0; repeat < 3; ++repeat) {
        ++stepped_tick;
        d.frame();
    }
    expect(metal->scroll.knob == 14, "then it steps once a tick");
    d.pointer(oa::app::ScreenInputKind::pointer_up, 625, 32);
    ++stepped_tick;
    d.frame();
    expect(metal->scroll.knob == 14, "a released arrow stops");
    expect(
        shows_at(d, art, sliders, kForwardArrowFrame, 621, 24), "a released arrow shows its face"
    );

    // A press on the knob drags it pixel for pixel, once a frame however
    // often the pointer moves in between, so the value is sent once.
    const int32_t grab_x = metal->x + metal->scroll.knob + 6;
    d.pointer(oa::app::ScreenInputKind::pointer_move, grab_x, 32);
    d.pointer(oa::app::ScreenInputKind::pointer_down, grab_x, 32);
    const auto sent_before_drag = mp::multiplayer_loopback().sent_count;
    for (const int32_t moved : {4, 11, 20})
        d.pointer(oa::app::ScreenInputKind::pointer_move, grab_x + moved, 30);
    expect(
        metal->scroll.knob == 14 && mp::multiplayer_loopback().sent_count == sent_before_drag,
        "the pointer's moves alone neither move the knob nor send"
    );
    d.frame();
    d.pointer(oa::app::ScreenInputKind::pointer_up, grab_x + 20, 30);
    d.frame();
    expect(metal->scroll.knob == 34, "dragged 20 pixels, the knob moves 20 steps");
    expect(
        shows_at(d, art, sliders, kKnobFrame, knob_column(*metal), 27),
        "the knob is drawn where it was dragged"
    );

    // A press on the track creeps the knob toward the pointer, one step a
    // tick, and one more as it is released.
    d.pointer(oa::app::ScreenInputKind::pointer_move, 610, 32);
    d.pointer(oa::app::ScreenInputKind::pointer_down, 610, 32);
    d.frame();
    expect(metal->scroll.knob == 34, "a press on the track does not move the knob");
    for (int creep = 0; creep < 5; ++creep) {
        ++stepped_tick;
        d.frame();
    }
    expect(metal->scroll.knob == 39, "a held track creeps one step a tick");
    d.pointer(oa::app::ScreenInputKind::pointer_up, 610, 32);
    d.frame();
    expect(metal->scroll.knob == 40, "the release steps once more");
    expect(
        mp::panel_text(mp::multiplayer_panel(), "METALTEXT") == "4500" &&
            mp::local_info(lobby).metal_hundreds == 45,
        "METAL's value follows its knob"
    );
    expect(
        shows_at(d, art, sliders, kKnobFrame, knob_column(*metal), 27), "the crept knob is drawn"
    );
    lobby.services.tick = real_clock;
    d.snapshot("battleroom-sliders");

    // The option lamps follow the stages through clicks; GAMEOPEN is clicked
    // twice so that the client below can still join.
    for (const auto& option : kOptionArt)
        expect(lamp_follows_stage(d, art, option), option.button);
    for (const auto& option : kOptionArt) {
        const int clicks = std::string_view(option.button) == "GAMEOPEN" ? 2 : 1;
        for (int click = 0; click < clicks; ++click) {
            expect(d.click(option.button), "option clicked");
            d.frame();
            expect(lamp_follows_stage(d, art, option), "option lamp follows a click");
        }
    }
    d.snapshot("battleroom-options");

    // A held option button shows its held face only while the pointer is over it.
    const OptionArt& commander = kOptionArt[0];
    const auto* held = control(commander.button);
    const auto* held_art = sequence(art.screen.shared_sprites, commander.art);
    expect(held != nullptr && held_art != nullptr, "COMMANDER and its stage art");
    if (held == nullptr || held_art == nullptr)
        return 1;
    const auto held_face = held_art->frames.size() - 2;
    const int32_t held_x = held->x + held->width / 2;
    const int32_t held_y = held->y + held->height / 2;
    constexpr int32_t kAwayX = 250, kAwayY = 10;
    expect(
        mp::panel_hit(mp::multiplayer_panel(), kAwayX, kAwayY) == mp::kNoControl,
        "the pointer can leave for a place with no control"
    );
    const auto stage_before = held->stage;
    d.pointer(oa::app::ScreenInputKind::pointer_move, held_x, held_y);
    d.pointer(oa::app::ScreenInputKind::pointer_down, held_x, held_y);
    d.frame();
    expect(lamp_shows(d, art, commander, held_face), "a held option button shows its held face");
    d.pointer(oa::app::ScreenInputKind::pointer_move, kAwayX, kAwayY);
    d.frame();
    expect(lamp_shows(d, art, commander, stage_before), "dragged off, it shows its stage again");
    d.pointer(oa::app::ScreenInputKind::pointer_move, held_x, held_y);
    d.frame();
    expect(lamp_shows(d, art, commander, held_face), "back over it, it shows its held face again");
    d.pointer(oa::app::ScreenInputKind::pointer_up, held_x, held_y);
    d.frame();
    expect(
        held->stage == (stage_before + 1) % held->stages, "released over it, it steps its stage"
    );
    expect(lamp_follows_stage(d, art, commander), "the lamp follows the new stage");
    const auto stage_after = held->stage;
    d.pointer(oa::app::ScreenInputKind::pointer_down, held_x, held_y);
    d.pointer(oa::app::ScreenInputKind::pointer_move, kAwayX, kAwayY);
    d.pointer(oa::app::ScreenInputKind::pointer_up, kAwayX, kAwayY);
    d.frame();
    expect(held->stage == stage_after, "released off it, it keeps its stage");
    expect(lamp_follows_stage(d, art, commander), "the lamp keeps its stage");

    expect(d.click("SIDE0"), "side clicked");
    expect(mp::local_info(lobby).side == 1, "side changed through the screen");
    d.frame();

    // A press on the chat log focuses it, as any list press does in 3.1c:
    // typing then goes nowhere until MESSAGE is clicked again.
    {
        auto& room_panel = mp::multiplayer_panel();
        const auto message_index = mp::panel_find(room_panel, "MESSAGE");
        const auto output_index = mp::panel_find(room_panel, "OUTPUT");
        expect(room_panel.focus == message_index, "MESSAGE has the focus");
        mp::multiplayer_type(&d.ctx, "hello");
        d.key(0x0d);
        lobby.game->gui_flags |= 1U;
        d.frame();
        const auto* output = control("OUTPUT");
        expect(output != nullptr && !output->items.empty(), "the chat line reaches the chat log");
        if (output != nullptr && !output->items.empty()) {
            d.pointer_click(output->x + 20, output->y + 5);
            expect(room_panel.focus == output_index, "a press on the chat log focuses it");
            mp::multiplayer_type(&d.ctx, "lost");
            expect(
                mp::panel_text(room_panel, "MESSAGE").empty(), "typing no longer reaches MESSAGE"
            );
            const auto* message = control("MESSAGE");
            if (message != nullptr)
                d.pointer_click(message->x + 10, message->y + message->height / 2);
            expect(room_panel.focus == message_index, "a click on MESSAGE takes the focus back");
            mp::multiplayer_type(&d.ctx, "back");
            expect(mp::panel_text(room_panel, "MESSAGE") == "back", "and typing reaches it again");
            mp::panel_set_text(room_panel, "MESSAGE", "");
        }
    }

    // Ready grays the host's own controls, so the Go? light is turned on and
    // off again before the other host steps.
    expect(d.click("READY0"), "READY0 clicked");
    d.frame();
    expect(shows_frame(d, art, checkbox, 1, control("READY0")), "the host's Go? light is lit");
    const auto host_color = mp::local_info(lobby).color;
    expect(!d.click("LOGO0"), "a ready host's colour square ignores clicks");
    expect(mp::local_info(lobby).color == host_color, "a ready host keeps its colour");
    expect(d.click("READY0"), "READY0 clicked again");
    d.frame();
    expect(shows_frame(d, art, checkbox, 0, control("READY0")), "the Go? light is dark again");
    expect(
        control("LOGO0") != nullptr && control("LOGO0")->hot, "the colour square takes clicks again"
    );

    expect(d.click("RESTRICTIONS"), "RESTRICTIONS clicked");
    expect(mp::multiplayer_modal_kind() == mp::ModalKind::restrict, "restriction panel open");
    // SLIDER0 is bound with 58 steps; a click on its back arrow takes its
    // row's limit down from No Limit.
    {
        const Art restrict_art = load_art(
            d.assets, "guis/restrict2.gui", "bitmaps/unitrestrict5x.pcx", "anims/commongui.gaf"
        );
        auto* modal = mp::multiplayer_modal();
        const auto first = modal != nullptr ? mp::panel_find(*modal, "SLIDER0") : mp::kNoControl;
        const auto* count = modal != nullptr ? mp::panel_control(*modal, "SLIDER0") : nullptr;
        const auto first_back =
            count != nullptr && first != mp::kNoControl && count->scroll.back_arrow.width != 0
                ? first
                : mp::kNoControl;
        expect(
            count != nullptr && count->x == 302 && count->width == 72 &&
                count->scroll.range == 58 && count->scroll.knob == 57 && !count->grayed &&
                first_back != mp::kNoControl,
            "SLIDER0 is bound with 58 steps at No Limit"
        );
        d.frame();
        expect(mp::panel_text(*modal, "COUNT0") == "No Limit", "COUNT0 reads No Limit");
        expect(
            shows_at(d, restrict_art, sliders, kBackArrowFrame, 293, 66), "SLIDER0's back arrow"
        );
        if (count != nullptr && first_back != mp::kNoControl) {
            expect(
                shows_at(d, restrict_art, sliders, kKnobFrame, knob_column(*count), 69),
                "SLIDER0's knob at its last step"
            );
            d.pointer_click(297, 74);
            d.frame();
            const auto& row =
                mp::multiplayer_restrict().entries[mp::multiplayer_restrict().first_visible];
            expect(
                count->scroll.knob == 56 && mp::panel_text(*modal, "COUNT0") == "99" &&
                    row.limit == 99,
                "a click on SLIDER0's back arrow limits its row to 99"
            );
            expect(
                shows_at(d, restrict_art, sliders, kKnobFrame, knob_column(*count), 69),
                "SLIDER0's knob steps back"
            );
        }
        // SCROLLSLIDER is bound between its arrows and sized from PICLIST's
        // 32-pixel rows; its steps scroll DESCLIST and rebind the count sliders.
        const auto* scroll = modal != nullptr ? mp::panel_control(*modal, "SCROLLSLIDER") : nullptr;
        auto& restrict = mp::multiplayer_restrict();
        const auto units = restrict.count;
        const int32_t knob_size = units > 0 ? 12 * 379 / units : 0;
        expect(
            scroll != nullptr && units > 12 && scroll->x == 447 && scroll->y == 60 &&
                scroll->width == 16 && scroll->height == 379 && scroll->active != 0 &&
                scroll->scroll.knob_size == knob_size && scroll->scroll.range == 379 - knob_size &&
                scroll->scroll.knob == 0,
            "SCROLLSLIDER is bound at 447,60 16x379 with a knob of 12 * 379 / units"
        );
        expect(
            scroll != nullptr && scroll->scroll.back_arrow.x == 447 &&
                scroll->scroll.back_arrow.y == 50 && scroll->scroll.forward_arrow.x == 447 &&
                scroll->scroll.forward_arrow.y == 439,
            "SCROLLSLIDER's arrows sit at 447,50 and 447,439"
        );
        const auto* descriptions =
            modal != nullptr ? mp::panel_control(*modal, "DESCLIST") : nullptr;
        const auto* pictures = modal != nullptr ? mp::panel_control(*modal, "PICLIST") : nullptr;
        expect(
            descriptions != nullptr && pictures != nullptr && descriptions->height == 384 &&
                pictures->height == 384,
            "PICLIST and DESCLIST are trimmed to 384 pixels"
        );
        d.frame();
        expect(
            shows_at(d, restrict_art, sliders, kUpArrowFrame, 447, 50), "SCROLLSLIDER's up arrow"
        );
        expect(
            shows_at(d, restrict_art, sliders, kDownArrowFrame, 447, 439),
            "SCROLLSLIDER's down arrow"
        );
        expect(
            shows_at(d, restrict_art, sliders, kVerticalKnobFrame, 450, 63),
            "SCROLLSLIDER's knob at its top"
        );
        const auto row_text = [&](int32_t row) {
            const auto limit = restrict.entries[row].limit;
            return limit < 0 ? std::string("No Limit") : std::to_string(limit);
        };
        const auto shown_row = [&](int32_t knob) {
            if (scroll == nullptr || scroll->scroll.range < 2)
                return 0;
            return static_cast<int32_t>(
                static_cast<double>(units - 12) * knob / (scroll->scroll.range - 1)
            );
        };
        d.pointer_click(455, 444);
        d.pointer_click(455, 444);
        expect(
            scroll != nullptr &&
                scroll->scroll.knob == 2 && restrict.first_visible ==
                    shown_row(2) && restrict.first_visible >= 1 &&
                descriptions != nullptr && descriptions->list_first == 2 * restrict.first_visible &&
                mp::panel_text(*modal, "COUNT0") == row_text(restrict.first_visible),
            "two down-arrow steps show the units from (units - 12) * 2 / (range - 1) on"
        );
        // The wheel over DESCLIST steps SCROLLSLIDER as its arrow does.
        d.wheel(150, 200, -1.0F);
        expect(
            scroll != nullptr &&
                scroll->scroll.knob == 3 && restrict.first_visible == shown_row(3) &&
                descriptions->list_first == 2 * restrict.first_visible &&
                mp::panel_text(*modal, "COUNT0") == row_text(restrict.first_visible),
            "a wheel notch over DESCLIST steps SCROLLSLIDER and the count sliders follow"
        );
        d.frame();
        if (scroll != nullptr)
            expect(
                shows_at(
                    d, restrict_art, sliders, kVerticalKnobFrame, 450, 63 + scroll->scroll.knob
                ),
                "SCROLLSLIDER's knob is drawn at its step"
            );
    }
    const auto expected_units = restrictable_units(d.assets);
    expect(expected_units > 100, "installed data has restrictable units");
    expect(
        mp::multiplayer_restrict().count == expected_units,
        "restriction rows match the installed unit count"
    );
    auto* list = mp::multiplayer_modal() != nullptr
                     ? mp::panel_control(*mp::multiplayer_modal(), "DESCLIST")
                     : nullptr;
    expect(
        list != nullptr && static_cast<int32_t>(list->items.size()) == expected_units * 2,
        "DESCLIST filled, two lines per unit"
    );
    d.frame();
    expect(d.drawn(), "RESTRICT2 drawn");
    d.snapshot("restrict");
    expect(d.click("OK"), "restriction OK");
    expect(mp::multiplayer_modal_kind() == mp::ModalKind::none, "restriction panel closed");

    expect(d.click("MAP"), "MAP clicked");
    expect(mp::multiplayer_modal_kind() == mp::ModalKind::selmap, "host opens map selection");
    auto* names = mp::multiplayer_modal() != nullptr
                      ? mp::panel_control(*mp::multiplayer_modal(), "MAPNAMES")
                      : nullptr;
    expect(names != nullptr && names->items.size() > 10, "multiplayer maps listed");
    d.frame();
    expect(d.drawn(), "SELMAP drawn");
    d.snapshot("selmap");
    // MAPPIC shows the selected map's minimap fitted to its box, and follows
    // the selection as the host browses the maps, which tells the other
    // players nothing until a map is chosen.
    {
        const Art map_art =
            load_art(d.assets, "guis/selmap.gui", "bitmaps/dselectmap2.pcx", "anims/skirmish.gaf");
        auto* modal = mp::multiplayer_modal();
        auto* picture = modal != nullptr ? mp::panel_control(*modal, "MAPPIC") : nullptr;
        expect(
            picture != nullptr && names != nullptr && names->items.size() > 12 &&
                names->list_selection >= 0,
            "SELMAP has its picture box and its maps"
        );
        if (picture == nullptr || names == nullptr || names->items.size() <= 12 ||
            names->list_selection < 0)
            return 1;
        const auto [ox, oy] = dialog_offset(map_art, modal->controls[0]);
        const auto map_at = [&](int32_t row) {
            return names->items[static_cast<std::size_t>(row)];
        };
        const auto opened = map_at(names->list_selection);
        expect(
            picture->picture_width == picture->width &&
                picture->picture_height == picture->height &&
                picture->picture.size() ==
                    static_cast<std::size_t>(picture->width) * picture->height,
            "MAPPIC holds the selected map's minimap fitted to its box"
        );
        expect(
            shows_preview(
                d,
                map_art,
                read_minimap(d.assets, opened),
                *picture,
                ox + picture->x,
                oy + picture->y
            ),
            "MAPPIC shows the selected map's minimap from one texel in, over the art"
        );
        expect(
            mp::panel_text(*modal, "SIZE") == summary_size(d.assets, opened),
            "SIZE shows the map's memory and player counts"
        );
        // In German the summary names the players as gamedata/translate.tdf
        // gives "Players" for German, through the bound translation.
        {
            oa::data::defs::LocaleTable german{};
            oa::data::defs::locale_table_init(&german);
            const oa::data::defs::Files files = oa::data::defs::asset_store_files(&d.assets);
            (void)oa::data::defs::load_locale_table(
                &files, &german, "gamedata\\translate.tdf", "German"
            );
            mp::multiplayer_bind_translation(
                {&german, [](void* context, std::string_view text) {
                     const std::string source(text);
                     return std::string(
                         oa::data::defs::locale_translate(
                             static_cast<oa::data::defs::LocaleTable*>(context), source.c_str()
                         )
                     );
                 }}
            );
            mp::map_summary_update(mp::multiplayer_lobby(), *modal);
            std::string german_size = summary_size(d.assets, opened);
            const std::size_t players = german_size.find("  Players: ");
            if (players != std::string::npos)
                german_size.replace(players, 11, "  Spieler: ");
            const std::string shown_size(mp::panel_text(*modal, "SIZE"));
            if (shown_size != german_size)
                std::fprintf(stderr, "German SIZE: %s\n", shown_size.c_str());
            expect(
                shown_size == german_size && german_size.find("  Spieler: ") != std::string::npos,
                "SIZE names the players in German as translate.tdf gives it"
            );
            mp::multiplayer_bind_translation({});
            mp::map_summary_update(mp::multiplayer_lobby(), *modal);
            oa::data::defs::locale_table_free(&german);
            expect(
                mp::panel_text(*modal, "SIZE") == summary_size(d.assets, opened),
                "without the translation SIZE names the players as the game holds the word"
            );
        }

        const auto kept_clock = lobby.services.tick;
        const auto stats_tick = lobby.next_stats_tick;
        lobby.services.tick = stepped_clock;
        stepped_tick = 20000;
        lobby.next_stats_tick = stepped_tick + 1000;
        const auto sent = mp::multiplayer_loopback().sent_count;
        const std::string announced(mp::local_info(lobby).map_name);
        const auto announced_hash = mp::local_info(lobby).map_hash;
        const auto options = mp::local_info(lobby).options;
        mp::local_info(lobby).options |= mp::option::ready;
        const auto chat_head = mp::lobby_chat_head(*lobby.game);
        int32_t row = -1;
        Minimap clicked;
        for (int32_t candidate = 1; candidate < 11 && row < 0; ++candidate)
            if (candidate != names->list_selection)
                if (auto map = read_minimap(d.assets, map_at(candidate));
                    !map.pixels.empty() && letterboxed(map)) {
                    row = candidate;
                    clicked = std::move(map);
                }
        expect(row > 0, "the first page lists a map that is not square");
        if (row > 0) {
            d.pointer_click(
                ox + names->x + 20,
                oy + names->y + 2 + (row - names->list_first) * names->list_item_height +
                    names->list_item_height / 2
            );
            d.frame();
            d.frame();
            expect(
                names->list_selection == row &&
                    mp::multiplayer_modal_kind() == mp::ModalKind::selmap,
                "a single click selects another map and SELMAP stays open"
            );
            expect(
                shows_preview(d, map_art, clicked, *picture, ox + picture->x, oy + picture->y),
                "MAPPIC shows the clicked map, its minimap keeping its shape"
            );
            expect(
                mp::panel_text(*modal, "SIZE") == summary_size(d.assets, map_at(row)),
                "SIZE follows the selection"
            );
            d.snapshot("selmap-wide");
            d.key(kKeyDown);
            d.frame();
            expect(
                names->list_selection == row + 1 && shows_preview(
                                                        d,
                                                        map_art,
                                                        read_minimap(d.assets, map_at(row + 1)),
                                                        *picture,
                                                        ox + picture->x,
                                                        oy + picture->y
                                                    ),
                "Down previews the next map"
            );
        }
        expect(
            mp::multiplayer_loopback().sent_count == sent &&
                announced == mp::local_info(lobby).map_name &&
                announced_hash == mp::local_info(lobby).map_hash,
            "browsing the maps sends nothing and keeps the announced map"
        );
        expect(
            mp::lobby_chat_head(*lobby.game) == chat_head &&
                (mp::local_info(lobby).options & mp::option::ready) != 0,
            "nor does the battle room report a missing map or clear the host's ready mark"
        );
        mp::local_info(lobby).options = options;
        // A box without a picture is blank.
        picture->picture.clear();
        d.frame();
        expect(
            shows_blank_box(d, map_art, *picture, ox + picture->x, oy + picture->y),
            "MAPPIC without a picture is filled with the GUI's colour 7"
        );
        mp::map_summary_update(lobby, *modal);
        lobby.services.tick = kept_clock;
        lobby.next_stats_tick = stats_tick;
    }
    // More maps than fit: the bar lies between its arrows beside MAPNAMES,
    // its knob sized from the rows, and it scrolls the list both ways.
    {
        const Art map_art =
            load_art(d.assets, "guis/selmap.gui", "bitmaps/dselectmap2.pcx", "anims/skirmish.gaf");
        const auto* map_sliders = sequence(map_art.screen.shared_sprites, "SLIDERS");
        auto* modal = mp::multiplayer_modal();
        auto* bar = modal != nullptr ? mp::panel_control(*modal, "SLIDER") : nullptr;
        expect(
            names != nullptr && bar != nullptr && map_sliders != nullptr &&
                names->items.size() > 60,
            "SELMAP lists more maps than fit"
        );
        if (names == nullptr || bar == nullptr || map_sliders == nullptr ||
            names->items.size() <= 60)
            return 1;
        const auto& root = modal->controls[0];
        const int32_t ox =
            (640 -
             std::min<int32_t>(static_cast<int32_t>(map_art.screen.background.width), root.width)) /
            2;
        const int32_t oy =
            (480 - std::min<int32_t>(
                       static_cast<int32_t>(map_art.screen.background.height), root.height
                   )) /
            2;
        const auto count = static_cast<int32_t>(names->items.size());
        const auto& up_arrow = bar->scroll.back_arrow;
        const auto& down_arrow = bar->scroll.forward_arrow;
        expect(
            bar->x == 305 && bar->y == 85 && bar->width == 16 && bar->height == 183 &&
                bar->active != 0 && up_arrow.width != 0 && down_arrow.width != 0,
            "SELMAP's bar is bound at 305,85 16x183 and shown"
        );
        if (up_arrow.width == 0 || down_arrow.width == 0)
            return 1;
        expect(
            up_arrow.x == 305 && up_arrow.y == 75 && up_arrow.width == 16 &&
                up_arrow.height == 10 && down_arrow.x == 305 && down_arrow.y == 268 &&
                down_arrow.height == 10,
            "its arrows sit at 305,75 and 305,268"
        );
        const int32_t knob_size = std::max(10, static_cast<int32_t>(12.0 / count * 180));
        expect(
            names->height == 192 && names->list_item_height == 15 &&
                names->list_last_first == count - 12 && bar->scroll.knob_size == knob_size &&
                bar->scroll.range == 183 - knob_size - 3,
            "MAPNAMES shows 12 rows and the knob is 12/count of the bar"
        );
        expect(
            names->list_first == 0 && bar->scroll.knob == 0 &&
                modal->focus == mp::panel_find(*modal, "MAPNAMES"),
            "the list opens on its first page with MAPNAMES focused"
        );
        expect(
            shows_at(d, map_art, map_sliders, kUpArrowFrame, ox + 305, oy + 75), "SELMAP's up arrow"
        );
        expect(
            shows_at(d, map_art, map_sliders, kDownArrowFrame, ox + 305, oy + 268),
            "SELMAP's down arrow"
        );
        expect(
            shows_at(d, map_art, map_sliders, kVerticalKnobFrame, ox + 308, oy + 88),
            "SELMAP's knob at the bar's top"
        );

        const auto kept_clock = lobby.services.tick;
        lobby.services.tick = stepped_clock;
        stepped_tick = 9000;
        const auto selected = names->list_selection;
        const int32_t visible = 192 / 15;
        d.pointer_click(ox + 313, oy + 273);
        expect(
            bar->scroll.knob == 1 && names->list_first == knob_first_row(count, visible, *bar),
            "a click on the down arrow steps the knob once"
        );
        d.pointer_click(ox + 313, oy + 273);
        d.pointer_click(ox + 313, oy + 273);
        expect(
            bar->scroll.knob == 3 && names->list_first == knob_first_row(count, visible, *bar) &&
                names->list_first >= 1,
            "each step scrolls the list to (count - 12) * knob / (range - 1)"
        );
        d.pointer_click(ox + 313, oy + 80);
        expect(
            bar->scroll.knob == 2 && names->list_first == knob_first_row(count, visible, *bar),
            "a click on the up arrow steps it back"
        );
        // A held arrow steps at once, then once a tick after 15 ticks.
        d.pointer(oa::app::ScreenInputKind::pointer_down, ox + 313, oy + 273);
        d.frame();
        expect(bar->scroll.knob == 3, "a held down arrow steps at once");
        for (int wait = 0; wait < 14; ++wait) {
            ++stepped_tick;
            d.frame();
        }
        expect(bar->scroll.knob == 3, "and waits 15 ticks");
        for (int repeat = 0; repeat < 4; ++repeat) {
            ++stepped_tick;
            d.frame();
        }
        d.pointer(oa::app::ScreenInputKind::pointer_up, ox + 313, oy + 273);
        expect(
            bar->scroll.knob == 7 && names->list_first == knob_first_row(count, visible, *bar),
            "then steps once a tick, the list following"
        );
        // A press on the knob drags it pixel for pixel.
        const int32_t grab_y = oy + bar->y + bar->scroll.knob + 5;
        d.pointer(oa::app::ScreenInputKind::pointer_move, ox + 313, grab_y);
        d.pointer(oa::app::ScreenInputKind::pointer_down, ox + 313, grab_y);
        d.pointer(oa::app::ScreenInputKind::pointer_move, ox + 313, grab_y + 40);
        d.frame();
        d.pointer(oa::app::ScreenInputKind::pointer_up, ox + 313, grab_y + 40);
        expect(
            bar->scroll.knob == 47 && names->list_first == knob_first_row(count, visible, *bar),
            "dragged 40 pixels, the knob moves 40 steps and the list follows"
        );
        // A press on the track below the knob creeps it one step a tick, and once more on release.
        const int32_t track_y = oy + bar->y + bar->height - 6;
        d.pointer(oa::app::ScreenInputKind::pointer_move, ox + 313, track_y);
        d.pointer(oa::app::ScreenInputKind::pointer_down, ox + 313, track_y);
        d.frame();
        for (int creep = 0; creep < 5; ++creep) {
            ++stepped_tick;
            d.frame();
        }
        d.pointer(oa::app::ScreenInputKind::pointer_up, ox + 313, track_y);
        expect(
            bar->scroll.knob == 53 && names->list_first == knob_first_row(count, visible, *bar),
            "a held track creeps the knob and the list follows"
        );
        expect(names->list_selection == selected, "the bar never changes the selection");
        d.frame();
        const auto first = names->list_first;
        expect(
            shows_at(d, map_art, map_sliders, kVerticalKnobFrame, ox + 308, oy + bar->y + 53 + 3),
            "the knob is drawn at its step"
        );
        expect(
            shows_text(
                d, map_art, names->items[static_cast<std::size_t>(first + 1)], ox + 62, oy + 88 + 15
            ),
            "the list draws the rows the knob shows"
        );
        // The wheel scrolls a row within the pages and the knob follows.
        const auto sent_before_step = mp::multiplayer_loopback().sent_count;
        d.wheel(ox + 150, oy + 150, -1.0F);
        expect(
            names->list_first == first + 1 && bar->scroll.knob == first_row_knob(*names, *bar),
            "a wheel notch scrolls MAPNAMES a row and the knob follows"
        );
        for (int notch = 0; notch < count; ++notch)
            d.wheel(ox + 150, oy + 150, -1.0F);
        expect(
            names->list_first == count - 12 && bar->scroll.knob == bar->scroll.range,
            "the wheel stops at the last page"
        );
        for (int notch = 0; notch < count; ++notch)
            d.wheel(ox + 313, oy + 150, 1.0F);
        expect(names->list_first == 0 && bar->scroll.knob == 0, "and over the bar at the first");
        expect(
            mp::multiplayer_loopback().sent_count == sent_before_step &&
                names->list_selection == selected,
            "the wheel sends nothing and keeps the selection"
        );
        lobby.services.tick = kept_clock;

        // Opened on a map past the first page, the list scrolls to it.
        names->list_selection = 40;
        expect(d.click("LOAD"), "LOAD clicked on row 40");
        expect(mp::multiplayer_modal_kind() == mp::ModalKind::none, "the map is chosen");
        expect(d.click("MAP"), "MAP clicked again");
        modal = mp::multiplayer_modal();
        names = modal != nullptr ? mp::panel_control(*modal, "MAPNAMES") : nullptr;
        bar = modal != nullptr ? mp::panel_control(*modal, "SLIDER") : nullptr;
        if (names == nullptr || bar == nullptr)
            return 1;
        expect(
            names->list_selection == 40 && names->list_first == 40 &&
                bar->scroll.knob == first_row_knob(*names, *bar),
            "reopened on row 40, MAPNAMES shows it first and the knob stands for it"
        );
        d.frame();
        d.snapshot("selmap-row40");
        expect(
            shows_text(d, map_art, names->items[41], ox + 62, oy + 88 + 15),
            "row 41 is drawn second"
        );
        expect(
            shows_at(
                d,
                map_art,
                map_sliders,
                kVerticalKnobFrame,
                ox + 308,
                oy + bar->y + bar->scroll.knob + 3
            ),
            "the knob is drawn at row 40's step"
        );
        // Down moves the focused list's selection and scrolls at the page's
        // edge; it previews the map and sends nothing.
        const auto sent_before_keys = mp::multiplayer_loopback().sent_count;
        for (int press = 0; press < 11; ++press)
            d.key(kKeyDown);
        expect(names->list_selection == 51 && names->list_first == 40, "Down walks the page");
        d.key(kKeyDown);
        expect(
            names->list_selection == 52 && names->list_first == 41 &&
                bar->scroll.knob == first_row_knob(*names, *bar),
            "Down past the page's last row scrolls a row and the knob follows"
        );
        expect(
            mp::multiplayer_loopback().sent_count == sent_before_keys &&
                mp::multiplayer_modal_kind() == mp::ModalKind::selmap &&
                std::string(mp::local_info(lobby).map_name) == names->items[40],
            "Down only previews: nothing is sent and the chosen map stays"
        );
        d.key(kKeyUp);
        expect(names->list_selection == 51 && names->list_first == 41, "Up moves back on the page");
    }
    if (names != nullptr && names->items.size() > 2) {
        names->list_selection = 2;
        const auto chosen = names->items[2];
        expect(d.click("LOAD"), "LOAD clicked");
        expect(
            std::string(mp::local_info(lobby).map_name) == chosen,
            "map choice stored in the host info"
        );
        expect(
            mp::local_info(lobby).map_hash != 0, "host info carries the chosen map's content hash"
        );
    }
    expect(mp::multiplayer_modal_kind() == mp::ModalKind::none, "map selection closed");

    // Loopback session reflects the host's info.
    auto& loopback = mp::multiplayer_loopback();
    expect(
        loopback.session_count == 1 && std::strncmp(loopback.sessions[0].name, "Friday", 6) == 0,
        "session published with the game name"
    );

    expect(d.click("PREVMENU"), "PREVMENU clicked");
    if (!d.follow(mp::kScreenGameList, "leaving returns to the game list"))
        return 1;

    // Join the hosted game as a client.
    auto* games = mp::panel_control(mp::multiplayer_panel(), "GAMENAME");
    expect(games != nullptr && games->items.size() == 1, "hosted game listed");
    mp::panel_set_text(mp::multiplayer_panel(), "NICKNAME", "Guest");
    expect(d.click("JOINGAME"), "JOINGAME clicked");
    if (!d.follow(mp::kScreenBattleroom, "join opens the battleroom"))
        return 1;
    expect((mp::local_info(lobby).role & mp::kRoleHost) == 0, "client role");
    expect(mp::panel_text(mp::multiplayer_panel(), "MAP") == "View Map", "client map button");
    expect(
        mp::panel_control(mp::multiplayer_panel(), "COMMANDER")->grayed, "client options grayed"
    );
    d.frame();
    expect(d.drawn(), "client BATTLEROOM drawn");
    d.snapshot("battleroom-client");

    // The client's own row and its grayed option lamps. Its host has left,
    // so it has no colour; its version still shows.
    const auto own = std::to_string(lobby.game->local_player_index);
    expect(shows_frame(d, art, checkbox, 0, control("READY" + own)), "the client's Go? light");
    expect(
        control("CD" + own) != nullptr && control("CD" + own)->active == 0,
        "the client's row shows no CD icon"
    );
    expect(
        control("LOGO" + own) != nullptr && control("LOGO" + own)->active == 0,
        "the client has no colour yet"
    );
    expect(shows_version(d, art, "LOGO" + own, "3.1"), "the client's version shows");
    expect(shows_frame(d, art, doors, 0, control("battlestart")), "a client's doors stay closed");
    // The client's sliders and their arrows are grayed and shaded, and ignore the pointer.
    const auto* client_metal = control("METAL");
    expect(client_metal != nullptr && client_metal->grayed, "the client's METAL is locked");
    expect(
        shows_grayed_at(d, art, sliders, kBackArrowFrame, 510, 24) &&
            shows_grayed_at(d, art, sliders, kForwardArrowFrame, 621, 24),
        "the client's METAL arrows are grayed and shaded"
    );
    if (client_metal != nullptr) {
        expect(
            shows_grayed_at(d, art, sliders, kKnobFrame, knob_column(*client_metal), 27) &&
                shows_grayed_at(d, art, sliders, kTrackEndFrame, 605, 24),
            "the client's METAL is grayed and shaded"
        );
        const auto knob = client_metal->scroll.knob;
        d.pointer_click(625, 32);
        d.pointer_click(600, 32);
        d.frame();
        expect(client_metal->scroll.knob == knob, "the client's METAL ignores the pointer");
    }
    d.snapshot("battleroom-client-sliders");
    for (const auto& option : kOptionArt) {
        const auto* button = control(option.button);
        expect(button != nullptr && button->grayed, "client option grayed");
        if (button == nullptr || button->stages < 2)
            continue;
        const auto other = static_cast<std::size_t>((button->stage + 1) % button->stages);
        expect(
            lamp_shows_grayed(d, art, option, button->stage) &&
                !lamp_shows_grayed(d, art, option, other),
            "a grayed option button lights the lamp of its stage, grayed and shaded"
        );
    }

    // VIEW MAP shows the selected map's minimap, and follows the host's
    // choice of map while it is open.
    {
        expect(d.click("MAP"), "the client's View Map clicked");
        auto* modal = mp::multiplayer_modal();
        expect(
            modal != nullptr && mp::multiplayer_modal_kind() == mp::ModalKind::viewmap,
            "View Map opens VIEWMAP"
        );
        auto* picture = modal != nullptr ? mp::panel_control(*modal, "MAPPIC") : nullptr;
        if (picture == nullptr)
            return 1;
        const Art view_art =
            load_art(d.assets, "guis/viewmap.gui", "bitmaps/dviewmap.pcx", "anims/commongui.gaf");
        const auto [vx, vy] = dialog_offset(view_art, modal->controls[0]);
        const std::string shown = lobby.maps.name(lobby.maps.context);
        d.frame();
        d.snapshot("viewmap");
        expect(
            shows_preview(
                d,
                view_art,
                read_minimap(d.assets, shown),
                *picture,
                vx + picture->x,
                vy + picture->y
            ),
            "VIEWMAP shows the map's minimap"
        );
        expect(
            mp::panel_text(*modal, "SIZE") == summary_size(d.assets, shown),
            "VIEWMAP's SIZE shows the map's memory and player counts"
        );
        // A host joins and has chosen another map.
        std::string chosen;
        for (int32_t index = 0; index < lobby.maps.count(lobby.maps.context) && chosen.empty();
             ++index) {
            const std::string candidate = lobby.maps.at(lobby.maps.context, index);
            if (candidate != shown && letterboxed(read_minimap(d.assets, candidate)))
                chosen = candidate;
        }
        mp::LobbyEvent joined{};
        joined.kind = mp::LobbyEventKind::player_joined;
        joined.player_id = 0x7700;
        std::snprintf(joined.name, sizeof(joined.name), "%s", "Host");
        mp::PlayerSetupInfo host{};
        host.role = mp::kRoleHost;
        host.version_major = 3;
        host.version_minor = 1;
        host.state = mp::kInfoStatePlaying;
        std::snprintf(host.map_name, sizeof(host.map_name), "%s", chosen.c_str());
        oa::netgame::PlayerInfoRecord record{};
        std::memcpy(record.info_head, &host, sizeof(record.info_head));
        record.player_id = joined.player_id;
        std::memcpy(
            record.info_tail,
            reinterpret_cast<const uint8_t*>(&host) + oa::netgame::player_info_tail_offset,
            sizeof(record.info_tail)
        );
        mp::LobbyEvent info{};
        info.kind = mp::LobbyEventKind::record;
        info.player_id = joined.player_id;
        std::size_t written = 0;
        (void)oa::netgame::encode_record(record, info.data, sizeof(info.data), &written);
        info.size = static_cast<uint16_t>(written);
        expect(
            !chosen.empty() && mp::loopback_inject(loopback, joined) &&
                mp::loopback_inject(loopback, info),
            "the host's arrival and map are queued"
        );
        d.frame();
        d.frame();
        expect(
            mp::multiplayer_modal_kind() == mp::ModalKind::viewmap &&
                std::string(lobby.maps.name(lobby.maps.context)) == chosen,
            "the open view selects the host's map"
        );
        expect(
            shows_preview(
                d,
                view_art,
                read_minimap(d.assets, chosen),
                *picture,
                vx + picture->x,
                vy + picture->y
            ) && mp::panel_text(*modal, "SIZE") == summary_size(d.assets, chosen),
            "VIEWMAP follows the host's map"
        );
        d.snapshot("viewmap-followed");
        expect(d.click("OK"), "VIEWMAP's OK clicked");
        expect(mp::multiplayer_modal_kind() == mp::ModalKind::none, "VIEWMAP closed");
    }

    // START: a real pointer click starts the game once everyone is ready and
    // the units are synced, and not before.
    mp::multiplayer_reset();
    mp::multiplayer_bind_start(count_start, nullptr);
    if (!host_battleroom(d, "Doors", "Host"))
        return 1;
    auto& room = mp::multiplayer_lobby();
    d.frame();
    const auto* start = control("START");
    const auto* door = control("battlestart");
    const auto* synching = control("SYNCHING");
    expect(start != nullptr && door != nullptr && synching != nullptr, "START and its doors");
    if (start == nullptr || door == nullptr || synching == nullptr)
        return 1;
    const int32_t start_x = start->x + start->width / 2;
    const int32_t start_y = start->y + start->height / 2;
    expect(shows_frame(d, art, doors, 0, door), "the doors are closed over START");
    d.host.sounds.clear();
    d.pointer_click(start_x, start_y);
    expect(starts == 0 && !d.played("BigButton"), "the closed doors take the click on START");

    expect(mp::lobby_add_player(room, 0x1234, "Remote"), "a remote player joins");
    const auto slot = mp::slot_for_player_id(room, 0x1234);
    auto* remote = mp::slot_info(room, slot);
    expect(remote != nullptr, "the remote player has a slot");
    if (remote == nullptr)
        return 1;
    remote->state = mp::kInfoStatePlaying;
    remote->options = static_cast<uint16_t>(remote->options | mp::option::ready);
    remote->status = static_cast<uint16_t>(remote->status | mp::status::has_disc);
    remote->color = 1;
    remote->version_major = 3;
    remote->version_minor = 1;
    expect(d.click("READY0"), "the host gets ready");
    room.game->gui_flags |= 1U;
    d.frame();
    d.frame();
    const auto row = std::to_string(slot);
    expect(
        shows_frame(d, art, checkbox, 3, control("READY" + row)),
        "a ready remote player's grayed Go? light is lit"
    );
    expect(shows_color(d, art, "LOGO" + row, 1, "3.1"), "the remote player's colour square");
    expect(shows_version(d, art, "LOGO" + row, "3.1"), "the remote player's version");
    expect(shows_frame(d, art, disc, 0, control("CD" + row)), "a remote 3.1c player's CD icon");
    expect(!door->hot, "the doors stop taking clicks once everyone is ready");
    expect(
        start->active == 0 && synching->active != 0,
        "SYNCHING stands in for START until the units are synced"
    );
    d.host.sounds.clear();
    d.pointer_click(start_x, start_y);
    expect(starts == 0 && !d.played("BigButton"), "a click on SYNCHING does nothing");

    room.sync.finished = true; // the unit handshake with the remote player is over
    room.game->gui_flags |= 1U;
    d.frame();
    expect(start->active != 0 && !start->grayed, "START shows once the units are synced");
    // The doors open a frame at a time on the lobby's clock, which the test
    // steps here from the tick the real clock reached, so they open however
    // slowly the test runs.
    const auto door_clock = room.services.tick;
    stepped_tick = door_clock(room.services.context);
    room.services.tick = stepped_clock;
    for (int frame = 0; frame < 40 && door->stage < 8; ++frame) {
        stepped_tick += 5;
        d.frame();
    }
    room.services.tick = door_clock;
    expect(door->stage == 8, "the doors open");
    expect(shows_frame(d, art, doors, 8, door), "the open doors are drawn");
    d.snapshot("battleroom-start");
    // START's art is lit at its light level; its top and bottom rows lie
    // clear of the caption, and the open doors' pillars are left out.
    const auto* buttons = sequence(art.screen.shared_sprites, "BUTTONS0");
    const gaf::Frame* start_art = nullptr;
    const gaf::Frame* start_grayed_art = nullptr;
    for (std::size_t index = 0; buttons != nullptr && index + 2 < buttons->frames.size();
         index += 4)
        if (buttons->frames[index].width == start->width &&
            buttons->frames[index].height == start->height) {
            start_art = &buttons->frames[index];
            start_grayed_art = &buttons->frames[index + 2];
            break;
        }
    expect(start_art != nullptr, "START's art is BUTTONS0's frame of its size");
    if (start_art != nullptr) {
        const auto pillars = frame_mask(doors->frames[8], door->x, door->y);
        Compare edge;
        edge.skip = &pillars;
        if (start->light_level != 0)
            edge.light = std::span(art.screen.light_table).subspan(start->light_level * 256U, 256);
        for (const auto& rows :
             {std::pair{0, 5}, std::pair{start->height - 5, start->height + 0}}) {
            edge.first_row = rows.first;
            edge.end_row = rows.second;
            expect(
                shows(compare_frame(
                    d.surface,
                    art.palette,
                    *start_art,
                    start->x,
                    start->y,
                    start->width,
                    start->height,
                    edge
                )),
                "START's art is drawn lit at its light level"
            );
        }
    }

    // Once the host is no longer ready, START grays under the open doors:
    // its grayed face, lit at its last light level, is grayed and shaded,
    // and the doors' pillars drawn over it keep their colours.
    expect(d.click("READY0"), "the host is no longer ready");
    room.game->gui_flags |= 1U;
    d.frame();
    d.frame();
    expect(start->active != 0 && start->grayed, "START grays");
    expect(shows_frame(d, art, doors, 8, door), "the doors' pillars stay over the grayed START");
    if (start_grayed_art != nullptr) {
        const auto pillars = frame_mask(doors->frames[8], door->x, door->y);
        Compare edge;
        edge.skip = &pillars;
        if (start->light_level != 0)
            edge.light = std::span(art.screen.light_table).subspan(start->light_level * 256U, 256);
        for (const auto& rows :
             {std::pair{0, 5}, std::pair{start->height - 5, start->height + 0}}) {
            edge.first_row = rows.first;
            edge.end_row = rows.second;
            expect(
                shows_grayed(d, art, *start_grayed_art, start->x, start->y, edge),
                "the grayed START's art is grayed and shaded"
            );
        }
    }
    d.snapshot("battleroom-start-grayed");
    expect(d.click("READY0"), "the host is ready again");
    room.game->gui_flags |= 1U;
    d.frame();
    d.frame();
    expect(start->active != 0 && !start->grayed, "START is enabled again");

    d.host.sounds.clear();
    d.pointer_click(start_x, start_y);
    expect(starts == 1 && d.played("BigButton"), "a click on START starts the game");
    // The match loader takes over without the battle room being left: no
    // multiplayer screen counts as showing, so a close request is not theirs.
    expect(
        !mp::multiplayer_showing() && !mp::multiplayer_request_exit_confirm(),
        "the battle room still counted as showing once the match started"
    );

    if (!check_player_timeout(d))
        return 1;

    // SELGAME with more games than its ten rows: the bar shows beside the
    // ten columns and scrolls them together; Down selects the next game
    // without joining it.
    mp::multiplayer_reset();
    d.go(mp::kScreenProviders);
    if (auto* providers = mp::panel_control(mp::multiplayer_panel(), "DPLAY"))
        providers->list_selection = 1;
    if (!d.click("SELECT") ||
        !d.follow(mp::kScreenTcp, "the TCP/IP provider is chosen for SELGAME"))
        return 1;
    mp::panel_set_text(mp::multiplayer_panel(), "ADDRESS", "127.0.0.1");
    if (!d.click("OK") || !d.follow(mp::kScreenGameList, "the game list opens for the scroll bar"))
        return 1;
    auto& listed_sessions = mp::multiplayer_loopback();
    for (uint8_t game = 0; game < 12; ++game) {
        mp::SessionEntry entry{};
        entry.instance_guid[0] = static_cast<uint8_t>(40 + game);
        const auto title = "Game " + std::to_string(game);
        std::snprintf(
            entry.name, sizeof(entry.name), "%-16.16s%-15.15s", title.c_str(), "Seven Islands"
        );
        entry.max_players = 10;
        mp::PlayerSetupInfo info{};
        info.options = 2 | mp::option::watching_allowed;
        info.version_major = 3;
        std::memcpy(
            entry.user, reinterpret_cast<const uint8_t*>(&info) + mp::kSessionUserInfoOffset, 16
        );
        expect(mp::loopback_add_session(listed_sessions, entry), "a game is listed");
    }
    expect(d.click("UPDATE"), "UPDATE clicked");
    {
        auto& game_panel = mp::multiplayer_panel();
        const auto* game_names = control("GAMENAME");
        const auto* statuses = control("STATUS");
        const auto* pings = control("PING");
        const auto* bar = control("SLIDER");
        expect(
            game_names != nullptr && statuses != nullptr && pings != nullptr && bar != nullptr &&
                game_names->items.size() == 12,
            "twelve games listed"
        );
        if (game_names == nullptr || statuses == nullptr || pings == nullptr || bar == nullptr)
            return 1;
        expect(
            game_names->height == 160 && statuses->height == 160 &&
                game_names->list_last_first == 2 && bar->x == 618 && bar->y == 123 &&
                bar->height == 170 && bar->active != 0 && bar->scroll.knob_size == 139 &&
                bar->scroll.range == 28,
            "SELGAME's bar shows at 618,123 16x170 with a knob of 10/12 of 167 pixels"
        );
        const Art game_art =
            load_art(d.assets, "guis/selgame.gui", "bitmaps/selectgame2x.pcx", "anims/selgame.gaf");
        const auto* game_sliders = sequence(game_art.screen.shared_sprites, "SLIDERS");
        d.frame();
        d.snapshot("selgame-games");
        expect(shows_at(d, game_art, game_sliders, kUpArrowFrame, 618, 113), "SELGAME's up arrow");
        expect(
            shows_at(d, game_art, game_sliders, kDownArrowFrame, 618, 293), "SELGAME's down arrow"
        );
        expect(shows_at(d, game_art, game_sliders, kVerticalKnobFrame, 621, 126), "SELGAME's knob");
        d.host.requested.reset();
        const auto sent_before_refresh = listed_sessions.sent_count;
        d.key(kKeyDown);
        expect(
            game_names->list_selection == 1 && statuses->list_selection == 1 &&
                !d.host.requested.has_value() && listed_sessions.sent_count == sent_before_refresh,
            "Down selects the next game in every column without joining it"
        );
        for (int step = 0; step < 14; ++step)
            d.pointer_click(626, 298);
        expect(
            bar->scroll.knob == 14 && game_names->list_first == 1 && pings->list_first == 1 &&
                game_names->list_selection == 1,
            "the down arrow scrolls every column"
        );
        d.wheel(330, 200, -1.0F);
        expect(
            game_names->list_first == 2 && statuses->list_first == 2 && bar->scroll.knob == 28,
            "a wheel notch over a column scrolls them all to the last page"
        );
        d.wheel(330, 200, -1.0F);
        expect(game_names->list_first == 2, "and no further");
        d.frame();
        d.snapshot("selgame-games-scrolled");
        // A click on any column selects its game in every column without
        // joining it; a second click on that row joins it, and SELGAME then
        // asks for the name a join needs.
        const auto message_before = mp::multiplayer_message();
        d.pointer_click(200, 122 + 2 + 15 * 3 + 5);
        expect(
            game_names->list_selection == 5 && pings->list_selection == 5 &&
                statuses->list_selection == 5,
            "a click on MAPNAME's fourth row selects game 5 in every column"
        );
        expect(
            mp::multiplayer_message() == message_before &&
                game_panel.focus == mp::panel_find(game_panel, "MAPNAME") &&
                !d.host.requested.has_value() && listed_sessions.sent_count == sent_before_refresh,
            "one click does not join game 5"
        );
        d.pointer_click(200, 122 + 2 + 15 * 3 + 5);
        expect(
            mp::multiplayer_message() == "You must enter your name" &&
                game_panel.focus == mp::panel_find(game_panel, "NICKNAME"),
            "a second click on the row joins game 5, which needs a name"
        );
        expect(d.click("OK"), "the message box is dismissed");
    }
    check_launch_exit(d);
    check_engine_banner(d);
    check_pack_maps(d);
    check_pack_map_page(d);
    check_bound_base_maps(d);
    check_pack_map_titles(d);
    check_presence_badges(d);
    check_player_row_focus(d);

    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("multiplayer screen navigation passed");
    return 0;
}

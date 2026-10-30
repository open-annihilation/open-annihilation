// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Library's screens drawn in the installed game's fonts, over the
// fixture with Ridge selected: Compact in the 640x480 window at 1x, Regular
// in 1920x1080 and Large in 2560x1440 at 2x, and Compact's details page.
// Each is laid out and drawn twice into a picture with a margin round the
// window: the two drawings are byte for byte the same, the margin keeps its
// colour, and every pixel of the window is drawn. In the game's fonts every
// text fits and every part lies in the window. The same holds for the
// Active tab at Compact, Regular and Large, the Updates tab at Compact and
// Large, the Registries tab at Compact, Regular and Large, and Languages at
// Compact, and the Active tab at Compact while the command line chose a mod
// that needs Campaign, whose pill is locked. The Registries pictures dress
// the fixture's built-in registry as Core Prime on that copy only. The
// Languages picture gives the fixture's
// Portugues (Brasil) pack the author "OA team" and an installed copy, on
// that drawing only, so the row shows the OA mark under its check. The
// characters the game's fonts lack are drawn in the modern fonts beside the
// test, as the game draws them. The Active pictures and the Maps pictures
// dress the fixture's maps with made-up minimaps on that drawing only, so
// the tiles, the row and the details show those pictures and not a file
// from the game. Given --frames DIR, it writes the pictures in the modern
// look, Barlow from the fonts beside the test at the window's size class.
// The checks above still measure and draw the game's fonts. It writes
// library-<class>.png, library-details-compact.png, library-active-<class>.png,
// library-active-compact-maps.png, library-active-many-regular.png,
// library-active-locked-compact.png,
// library-updates-<class>.png, library-registries-<class>.png,
// library-languages-compact.png, library-languages-zh-compact.png,
// library-maps-one-large.png, library-maps-several-large.png,
// library-mutators-compact.png, library-mutators-regular.png and
// library-mutators-large.png there.
// languages-zh-compact is 简体中文 installed and in use, on its details page.
// The Mutators pictures add made-up mutators on that drawing only: one with
// a picture, one whose badge is its first tag's icon, one with letters,
// one that needs another mutator and one that does not fit the mod in play.
//
//   oa-ui-library-draw-test --data [--frames DIR]

#include "oa/formats/png.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/present/game_text.hpp"
#include "oa/test/check.hpp"
#include "oa/test/game_assets.hpp"
#include "oa/test/game_data.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/library/game_icon.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/use_question.hpp"
#include "oa/ui/library/screen.hpp"
#include "screen_fixture.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace kit = oa::ui::kit;
namespace lib = oa::ui::library;
namespace renderer = oa::ui::frontend_renderer;
namespace fixture = screen_fixture;
namespace png = oa::formats::png;
namespace text_font = oa::platform::text_font;

/// The points of margin drawn round the window, which nothing may touch.
constexpr int32_t margin_points = 4;
/// The colour the picture starts as; the window's face covers it wherever it draws.
constexpr kit::Colour untouched{0x12, 0x34, 0x56};

/// Draws a line in the modern fonts, as the game does for the characters
/// its fonts lack: each face at its size beside the game font it stands in
/// for, letters without smoothing.
///
/// @param context the font stack
/// @param text the line, in UTF-8
/// @param face the game font it stands in for
/// @param scale screen pixels to a game pixel
/// @param size the text size, in percent
/// @return the line's mask; null when the fonts cannot draw it
std::shared_ptr<const oa::present::TextMask> draw_modern(
    void* context, std::string_view text, oa::present::TextFace face, int32_t scale, int32_t size
) {
    auto& stack = *static_cast<text_font::FontStack*>(context);
    if (text.empty() || text.size() > text_font::max_text_bytes)
        return nullptr;
    const bool label = face == oa::present::TextFace::label;
    const int32_t face_pixels =
        face == oa::present::TextFace::message  ? text_font::message_log_pixel_size
        : face == oa::present::TextFace::status ? text_font::status_readout_pixel_size
                                                : text_font::label_pixel_size;
    text_font::Style style;
    style.pixel_size = std::clamp(
        oa::present::face_pixel_size(face_pixels, scale, size),
        int32_t{1},
        text_font::max_pixel_size
    );
    style.weight = label ? text_font::Weight::regular : text_font::Weight::bold;
    style.rendering = text_font::Rendering::mono;
    const auto drawn = stack.draw(text, style);
    const auto placements = stack.layout(text, style);
    const auto characters = text_font::decode_utf8(text);
    if (!drawn || !placements || !characters)
        return nullptr;
    auto mask = std::make_shared<oa::present::TextMask>();
    mask->width = drawn->width;
    mask->height = drawn->height;
    mask->baseline = drawn->baseline;
    mask->origin = drawn->origin;
    mask->advance = drawn->advance;
    mask->alpha = drawn->alpha;
    std::size_t placed = 0;
    int32_t pen = 0;
    for (const char32_t character : *characters) {
        if (!text_font::is_invisible(character) && placed < placements->size()) {
            const auto& placement = (*placements)[placed++];
            pen = placement.pen + placement.advance;
        }
        mask->character_ends.push_back(pen);
    }
    return mask;
}

/// One picture the test draws.
struct Scene {
    std::string name{};           ///< the file's name after "library-"
    kit::Viewport window{};       ///< the window it is laid out for
    bool details_page{};          ///< Compact's details page instead of the list
    lib::Tab tab{lib::Tab::mods}; ///< the tab shown
    /// Dress the maps with made-up minimaps. The fixture's own inputs stay.
    bool previews{};
    std::string select_registry{}; ///< selected after the tab, when select_key is set
    std::string select_key{};
    /// Extra installed maps, so the Active picture shows the maps pane scrolled.
    bool many{};
    /// On Languages, an installed pack of this key is the language in use.
    std::string in_use_language{};
    /// How far the Active page is scrolled before it is drawn. The layout
    /// clamps this to the page's limit, so a large value shows the maps.
    int32_t maps_scroll{};
    /// On Active, the command line chose the mod played, which needs
    /// Campaign, so that pill is drawn locked with the reason under the pills.
    bool command_line_mod{};
    /// Open USE's question over the selected layout before drawing.
    bool key_use{};
    /// Dress a layout in use and draw the Active tab's KEYS tile.
    bool active_keys{};
};

/// A drawing of a scene: the picture with its margin, and its scale.
struct Drawing {
    renderer::Surface surface{}; ///< the picture, the margin round the window
    int32_t scale{1};            ///< pixels a point
    kit::Point window{};         ///< the window's size, in points
};

/// Dresses the Mutators picture with made-up mutators, on this drawing only.
///
/// Beacon has a picture. Signal has none, so its badge is the economy icon.
/// Plain Mark has no tag, so its badge is its letters. Odd Mark's tag is not
/// one of the eight, so it has no icon. Gatehouse Keep needs Gatehouse.
/// Ridge Walls works with Ridge, and no mod is being played. Both Marks
/// needs another mutator and does not fit this mod.
///
/// @param[in,out] library the scene's library
void dress_mutators(lib::Library& library) {
    lib::Inputs inputs = std::move(library.inputs);
    const auto add = [&](std::string key, std::string name, std::vector<std::string> tags) {
        lib::Listing made;
        made.registry = std::string(fixture::core);
        made.kind = lib::Kind::mutator;
        made.key = std::move(key);
        made.name = std::move(name);
        made.version = "1.0";
        made.author = "Fixture Author";
        made.summary = "A made-up mutator.";
        made.size = 4096;
        lib::MutatorFacts facts;
        facts.tags = std::move(tags);
        made.mutator = std::move(facts);
        inputs.listings.push_back(std::move(made));
    };
    add("beacon", "Beacon", {"gameplay"});
    inputs.listings.back().badge = fixture::gradient_badge();
    add("signal", "Signal", {"economy", "statistics"});
    add("plain-mark", "Plain Mark", {});
    add("odd-mark", "Odd Mark", {"custom"});
    add("gatehouse-keep", "Gatehouse Keep", {"economy"});
    inputs.listings.back().mutator->needs.push_back({"gatehouse", {}, "Gatehouse"});
    add("ridge-walls", "Ridge Walls", {"commanders"});
    inputs.listings.back().mutator->works_with.push_back({"ridge", {}, "Ridge"});
    add("both-marks", "Both Marks", {"economy"});
    inputs.listings.back().mutator->needs.push_back({"north-tower", {}, "North Tower"});
    inputs.listings.back().mutator->works_with.push_back({"ridge", {}, "Ridge"});
    lib::refresh(library, std::move(inputs));
}

/// Dresses the Keys pictures with two made-up layouts, on this drawing only.
///
/// Grid is installed and in use, with two of the player's own changes on
/// it. It changes Q and W, so the keyboard marks those keys. Lefty is installed and is not in use. Column is only a
/// listing. Grid Test is installed from its own file, which no registry
/// lists, so its row's author and summary are its manifest's. OA default
/// stays first and is left out of the count.
///
/// @param[in,out] library the scene's library
void dress_keys(lib::Library& library) {
    lib::Inputs inputs = std::move(library.inputs);
    lib::KeyLayoutFacts grid_facts;
    grid_facts.tags = {"grid"};
    grid_facts.keyboard = "ansi";
    grid_facts.pointer = "right-click";
    grid_facts.changes = 12;
    grid_facts.own_changes = 2;
    lib::KeyMove patrol;
    patrol.command = "Patrol";
    patrol.after.keys = {"Q"};
    patrol.own = true;
    lib::KeyMove cloak;
    cloak.command = "Cloak";
    cloak.after.keys = {"K"};
    cloak.own = true;
    grid_facts.own_list = {patrol, cloak};
    grid_facts.triggers = {"Q", "@KeyW", "MouseLeft", "Nope", "Ctrl"};
    grid_facts.helpers = {"sticky-modifiers"};
    lib::Listing grid;
    grid.registry = std::string(fixture::core);
    grid.kind = lib::Kind::key_layout;
    grid.key = "grid";
    grid.name = "Grid";
    grid.version = "1.0";
    grid.author = "Ada";
    grid.summary = "Orders on a grid under the left hand.";
    grid.size = 1024;
    grid.keys = grid_facts;
    inputs.listings.push_back(grid);
    lib::Installed held;
    held.kind = lib::Kind::key_layout;
    held.key = "grid";
    held.folder = "grid";
    held.name = "Grid";
    held.version = "1.0";
    held.revision = 1;
    held.registry = std::string(fixture::core);
    held.release = 1;
    held.keys = grid_facts;
    inputs.installed.push_back(std::move(held));

    lib::KeyLayoutFacts lefty_facts;
    lefty_facts.tags = {"left-hand"};
    lefty_facts.helpers = {"sticky-modifiers"};
    lib::Listing lefty;
    lefty.registry = std::string(fixture::core);
    lefty.kind = lib::Kind::key_layout;
    lefty.key = "lefty";
    lefty.name = "Lefty";
    lefty.version = "1.0";
    lefty.author = "Bo";
    lefty.summary = "Everything within reach of the left hand.";
    lefty.size = 1024;
    lefty.keys = lefty_facts;
    inputs.listings.push_back(std::move(lefty));
    lib::Installed lefty_held;
    lefty_held.kind = lib::Kind::key_layout;
    lefty_held.key = "lefty";
    lefty_held.folder = "lefty";
    lefty_held.name = "Lefty";
    lefty_held.version = "1.0";
    lefty_held.revision = 1;
    lefty_held.registry = std::string(fixture::core);
    lefty_held.release = 1;
    lefty_held.keys = lefty_facts;
    inputs.installed.push_back(std::move(lefty_held));

    lib::Listing column;
    column.registry = std::string(fixture::core);
    column.kind = lib::Kind::key_layout;
    column.key = "column";
    column.name = "Column";
    column.version = "1.0";
    column.author = "Ada";
    column.summary = "A listed layout.";
    column.size = 1024;
    column.keys = lib::KeyLayoutFacts{};
    inputs.listings.push_back(std::move(column));

    lib::Installed own;
    own.kind = lib::Kind::key_layout;
    own.key = "grid-test";
    own.folder = "grid-test";
    own.name = "Grid Test";
    own.version = "1.0";
    own.author = "Cy";
    own.description = "Orders on a grid, from a file of its own.";
    own.keys = lib::KeyLayoutFacts{};
    own.keys->tags = {"grid"};
    inputs.installed.push_back(std::move(own));
    inputs.keys_layout = "grid";
    // Highland is the fixture's played mod. The host copies that into
    // Inputs::played; the picture does the same so USE can ask "Use it for".
    inputs.played.id = "highland";
    inputs.played.version = "0.4.0";
    inputs.played.name = "Highland";
    lib::refresh(library, std::move(inputs));
}

/// What USE would move in the Keys pictures: twelve commands from Grid to
/// Lefty, Patrol among them on the player's own key, or eleven without it.
///
/// @return the moves
lib::KeyMoves made_up_moves(std::string_view, std::string_view) {
    lib::KeyMoves moves;
    moves.known = true;
    const auto move = [](std::string command,
                         std::vector<std::string> before,
                         std::vector<std::string> after,
                         bool own) {
        lib::KeyMove made;
        made.command = std::move(command);
        made.before.keys = std::move(before);
        made.after.keys = std::move(after);
        made.own = own;
        return made;
    };
    moves.kept = {
        move("Attack", {"A"}, {"Q"}, false),
        move("Move", {"M"}, {"W"}, false),
        move("Patrol", {"P"}, {"Q"}, true),
        move("Guard", {"G"}, {"E"}, false),
        move("Stop", {"S"}, {"R"}, false),
        move("Fire at will", {"Shift", "F"}, {"A"}, false),
        move("Reclaim", {"E"}, {"S"}, false),
        move("Repair", {"R"}, {"D"}, false),
        move("Capture", {"C"}, {"F"}, false),
        move("Cloak", {"K"}, {}, false),
        move("Self-destruct", {"Ctrl", "D"}, {"Ctrl", "X"}, false),
        move("Load", {"L"}, {"V"}, false),
    };
    moves.kept_total = 12;
    moves.clean = moves.kept;
    moves.clean.erase(moves.clean.begin() + 2);
    moves.clean_total = 11;
    return moves;
}

/// Dresses the Languages picture: the fixture's built-in language pack names
/// its author, and an installed copy of it is added, on this drawing only.
/// The row then shows the OA mark under its check. The fixture's own inputs
/// stay as they were.
///
/// @param[in,out] library the scene's library
void dress_languages(lib::Library& library) {
    for (lib::Listing& listing : library.inputs.listings) {
        if (listing.registry == fixture::core && listing.key == "pt-BR")
            listing.author = "OA team";
    }
    lib::Installed pack;
    pack.kind = lib::Kind::language;
    pack.key = "pt-BR";
    pack.folder = "pt-BR";
    pack.name = "Portugues (Brasil)";
    pack.version = "1.0";
    pack.revision = 1;
    pack.author = "OA team";
    pack.registry = std::string(fixture::core);
    pack.release = 1;
    library.inputs.installed.push_back(std::move(pack));
    lib::refresh(library, std::move(library.inputs));
}

/// Walks up from a directory until a relative path is a file.
///
/// @param start where the walk begins
/// @param relative the file sought under each directory
/// @return the file; empty when none of the directories holds it
std::filesystem::path
climb_for(std::filesystem::path start, const std::filesystem::path& relative) {
    std::error_code ignored;
    for (int32_t step = 0; step < 12 && !start.empty(); ++step) {
        const std::filesystem::path candidate = start / relative;
        if (std::filesystem::is_regular_file(candidate, ignored))
            return candidate;
        const std::filesystem::path parent = start.parent_path();
        if (parent == start)
            break;
        start = parent;
    }
    return {};
}

/// The app icon, cropped to the pixels that show, for Core Prime's card.
///
/// The file is the plates the window icon is made from. Empty when it cannot
/// be read, and the card then keeps its lettermark.
///
/// @return the picture
std::optional<lib::Badge> shipped_logo() {
    const std::filesystem::path relative =
        std::filesystem::path("branding") / "open-annihilation-256.png";
    std::error_code ignored;
    std::filesystem::path file;
#ifdef OA_BRANDING_DIR
    const std::filesystem::path given =
        std::filesystem::path(OA_BRANDING_DIR) / "open-annihilation-256.png";
    if (std::filesystem::is_regular_file(given, ignored))
        file = given;
#endif
    if (file.empty())
        file = climb_for(std::filesystem::current_path(ignored), relative);
    if (file.empty())
        file = climb_for(text_font::bundled_font_directory(), relative);
    if (file.empty()) {
        std::fprintf(stderr, "registries picture: the app icon was not found\n");
        ++oa::test::failed_checks();
        return std::nullopt;
    }
    std::ifstream input(file, std::ios::binary);
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    if (!input || length <= 0) {
        std::fprintf(stderr, "registries picture: the app icon could not be read\n");
        ++oa::test::failed_checks();
        return std::nullopt;
    }
    input.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<std::size_t>(length));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(length));
    if (!input) {
        std::fprintf(stderr, "registries picture: the app icon could not be read\n");
        ++oa::test::failed_checks();
        return std::nullopt;
    }
    namespace png = oa::formats::png;
    if (!png::has_signature(bytes))
        return std::nullopt;
    png::Messages messages;
    png::Info info;
    if (!png::read_info(bytes, messages, &info))
        return std::nullopt;
    const png::Header& header = info.header;
    if (header.color_type != png::ColorType::rgb_alpha || header.bit_depth != 8 ||
        header.width == 0 || header.height == 0)
        return std::nullopt;
    const std::size_t stride = png::row_bytes(header, {});
    if (stride == 0 || header.height > std::numeric_limits<std::size_t>::max() / stride)
        return std::nullopt;
    std::vector<uint8_t> pixels(stride * header.height);
    const png::Progress progress = png::read_image(bytes, info, {}, messages, pixels);
    if (progress == png::Progress::none)
        return std::nullopt;
    const std::size_t row_bytes = std::size_t{header.width} * 4U;
    if (pixels.size() / row_bytes < header.height)
        return std::nullopt;
    uint32_t left = header.width;
    uint32_t top = header.height;
    uint32_t right = 0;
    uint32_t bottom = 0;
    for (uint32_t y = 0; y < header.height; ++y) {
        for (uint32_t x = 0; x < header.width; ++x) {
            if (pixels[y * row_bytes + std::size_t{x} * 4U + 3U] == 0)
                continue;
            left = std::min(left, x);
            top = std::min(top, y);
            right = std::max(right, x + 1);
            bottom = std::max(bottom, y + 1);
        }
    }
    if (left >= right || top >= bottom)
        return std::nullopt;
    lib::Badge badge;
    badge.width = right - left;
    badge.height = bottom - top;
    const std::size_t part_row = std::size_t{badge.width} * 4U;
    badge.rgba.reserve(part_row * badge.height);
    for (uint32_t y = top; y < bottom; ++y) {
        const auto first =
            pixels.begin() + static_cast<std::ptrdiff_t>(y * row_bytes + std::size_t{left} * 4U);
        badge.rgba.insert(badge.rgba.end(), first, first + static_cast<std::ptrdiff_t>(part_row));
    }
    return badge;
}

/// Marks one language pack as the one in use, adding it when the fixture
/// does not list it. Built for the picture of 简体中文 in use.
///
/// @param[in,out] library the scene's library
/// @param key the pack's key, which is its tag
void show_language_in_use(lib::Library& library, std::string_view key) {
    for (lib::Installed& installed : library.inputs.installed)
        if (installed.kind == lib::Kind::language)
            installed.in_use = false;
    bool listed = false;
    for (const lib::Listing& listing : library.inputs.listings)
        listed = listed || (listing.kind == lib::Kind::language && listing.key == key);
    if (!listed) {
        library.inputs.listings.push_back(
            fixture::listing(
                fixture::core, lib::Kind::language, key, "简体中文", "1.0", 2 * fixture::mib, "", {}
            )
        );
    }
    lib::Installed pack;
    pack.kind = lib::Kind::language;
    pack.key = std::string(key);
    pack.folder = std::string(key);
    pack.name = "简体中文";
    pack.version = "1.0";
    pack.revision = 4;
    pack.registry = std::string(fixture::core);
    pack.release = 4;
    pack.in_use = true;
    library.inputs.installed.push_back(std::move(pack));
    library.inputs.language.tag = std::string(key);
    library.inputs.language.name = "简体中文";
    library.inputs.language.english = "Chinese (Simplified)";
    library.inputs.language.pack = "简体中文";
    library.inputs.language.version = "1.0";
    library.inputs.language.revision = 4;
    lib::refresh(library, std::move(library.inputs));
}

/// Dresses the Registries pictures: the built-in card carries Core Prime's
/// short name, title, description and author, and the added card a sample of
/// its own. The fixture itself stays Fixture Core. Other tabs are not dressed.
///
/// @param[in,out] library the scene's library
void dress_registries(lib::Library& library) {
    for (lib::Registry& registry : library.inputs.registries) {
        if (registry.id == fixture::core) {
            registry.name = "Core Prime";
            registry.title = "Core Prime - Official Open Annihilation Packages";
            registry.description =
                "The CorePrime.net registry provides packages and content curated by the Open "
                "Annihilation team, and checked for compatibility, as well as official releases "
                "from some leading mods.";
            registry.address = "http://downloads.coreprime.net/v1/catalogue.json";
            registry.author = "Open Annihilation";
            registry.packages = 1;
            registry.checked_ago = 300;
            lib::SigningKey signing_key;
            signing_key.fingerprint = "21fe · 31df · a154 · a261";
            signing_key.public_key = "ed25519:11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=";
            registry.keys = {std::move(signing_key)};
            registry.shipped_logo = true;
            if (std::optional<lib::Badge> logo = shipped_logo())
                registry.icon = std::move(*logo);
        } else if (registry.id == fixture::added) {
            registry.title = "Example Mods - Sample packages";
            registry.description = "A sample registry of mods, including the made-up mod Ridge.";
            registry.address = "https://mods.example.org/oa/registry.yaml";
            registry.packages = 31;
            registry.checked_ago = 2 * 60 * 60;
        }
    }
}

/// Returns a solid minimap wider than it is tall. The pixels are made here:
/// nothing is read from the game's files.
///
/// @param red red
/// @param green green
/// @param blue blue
/// @return an 8 by 2 picture
lib::MapPicture solid_minimap(uint8_t red, uint8_t green, uint8_t blue) {
    lib::MapPicture picture;
    picture.width = 8;
    picture.height = 2;
    picture.rgba.assign(8U * 2U * 4U, 0);
    for (std::size_t index = 0; index + 3 < picture.rgba.size(); index += 4) {
        picture.rgba[index] = red;
        picture.rgba[index + 1] = green;
        picture.rgba[index + 2] = blue;
        picture.rgba[index + 3] = 255;
    }
    return picture;
}

/// Dresses the map pictures. Installed maps gain a minimap and a long
/// description, so the Active tiles show both. A one-map pack's tile has no
/// From line. Shore Pack is
/// one map, not installed, so its row and its details show that minimap. Desert
/// Pack has several maps and no badge, so the Maps row shows the first and the
/// details show the grid. The fixture's own inputs stay as they were.
///
/// @param[in,out] library the scene's library
void dress_map_previews(lib::Library& library) {
    const lib::MapPicture red = solid_minimap(180, 40, 30);
    const lib::MapPicture blue = solid_minimap(30, 70, 160);
    const lib::MapPicture green = solid_minimap(40, 140, 50);
    const std::string note =
        "Black sand, white water and a reef that runs further than one line of this tile can "
        "hold, then the same shore again so the line is cut.";
    for (lib::Listing& listing : library.inputs.listings) {
        if (listing.key == "desert-pack" && listing.maps.size() == 2) {
            listing.maps[0].preview = blue;
            listing.maps[0].description = "Dry and flat.";
            listing.maps[1].preview = green;
            listing.maps[1].description = "A table of stone.";
        }
    }
    lib::Listing shore = fixture::listing(
        fixture::core,
        lib::Kind::map_pack,
        "shore",
        "Shore Pack",
        "1.0",
        fixture::mib,
        "One cove",
        {}
    );
    lib::MapLine cove;
    cove.name = "Cove";
    cove.players = 3;
    cove.size = "10x10";
    cove.description = note;
    cove.preview = red;
    shore.maps.push_back(std::move(cove));
    library.inputs.listings.push_back(std::move(shore));
    if (library.inputs.map_packs.size() >= 2) {
        library.inputs.map_packs[0].maps[0].preview = green;
        library.inputs.map_packs[0].maps[0].description = note;
        library.inputs.map_packs[0].maps[1].preview = blue;
        library.inputs.map_packs[0].maps[1].description = note;
        library.inputs.map_packs[1].maps[0].preview = red;
        library.inputs.map_packs[1].maps[0].description = note;
    }
    lib::refresh(library, std::move(library.inputs));
}

/// Adds one installed pack of many maps, so the Active picture shows the maps
/// pane filling the room under its heading, with its own scroll bar. The
/// fixture's maps stay. This drawing only.
///
/// @param[in,out] library the scene's library
void add_many_maps(lib::Library& library) {
    const lib::MapPicture wash = solid_minimap(90, 110, 140);
    lib::InstalledMaps pack;
    pack.name = "Wide Atlas";
    pack.maps.reserve(24);
    for (int index = 1; index <= 24; ++index) {
        lib::MapLine map;
        map.name = "Atlas " + std::to_string(index);
        map.players = 2;
        map.size = "16x16";
        map.description = "A wide sheet of the atlas.";
        map.preview = wash;
        pack.maps.push_back(std::move(map));
    }
    library.inputs.map_packs.push_back(std::move(pack));
    lib::refresh(library, std::move(library.inputs));
}

/// Dresses the three Active pictures: No Mod, the base game from GOG, every
/// expansion copied and on except Battle Tactics, which turns off when the
/// Library closes. The draft is set after the last refresh, which would
/// otherwise drop it.
///
/// @param[in,out] library the scene's library
void dress_active_choice(lib::Library& library) {
    for (lib::Installed& held : library.inputs.installed) {
        if (held.kind == lib::Kind::mod)
            held.playing = false;
    }
    library.inputs.playing_mod.reset();
    library.inputs.content.base_id = "totala";
    library.inputs.content.source = "from GOG for macOS";
    library.inputs.content.files_differ = 0;
    library.inputs.content.copied = {
        "totala",
        "campaign",
        "core-contingency",
        "battle-tactics",
        "music",
        "cavedog-units",
        "cavedog-tilesets",
    };
    library.inputs.content.enabled = {
        "campaign",
        "core-contingency",
        "battle-tactics",
        "music",
        "cavedog-units",
        "cavedog-tilesets",
    };
    library.inputs.base_game_badge = lib::bundled_game_icon();
    lib::refresh(library, std::move(library.inputs));
    library.content.base_game = "totala";
    library.content.expansions = {
        "campaign", "core-contingency", "music", "cavedog-units", "cavedog-tilesets"
    };
    library.content.last_id = "battle-tactics";
    library.content.last_on = false;
    library.content.edited = true;
}

/// Dresses the Active picture of a mod the command line chose: Highland,
/// played, needs Campaign, every expansion is copied and all but Battle
/// Tactics are on. Campaign's pill is locked, and the others still turn.
///
/// @param[in,out] library the scene's library
void dress_command_line_mod(lib::Library& library) {
    library.inputs.mod_from_command_line = true;
    library.inputs.content.base_id = "totala";
    library.inputs.content.copied = {
        "totala",
        "campaign",
        "core-contingency",
        "battle-tactics",
        "music",
        "cavedog-units",
        "cavedog-tilesets",
    };
    library.inputs.content.enabled = {
        "campaign", "core-contingency", "music", "cavedog-units", "cavedog-tilesets"
    };
    library.inputs.content.mod_required = {"campaign"};
    library.inputs.base_game_badge = lib::bundled_game_icon();
    lib::refresh(library, std::move(library.inputs));
}

/// Lays a scene out and draws it.
///
/// @param scene the scene
/// @param fonts the game's fonts
/// @return the drawing
Drawing draw_scene(const Scene& scene, const kit::Fonts& fonts) {
    lib::Library library = fixture::fixture_library();
    if (scene.tab == lib::Tab::registries)
        dress_registries(library);
    else if (scene.tab == lib::Tab::languages)
        dress_languages(library);
    else if (scene.tab == lib::Tab::mutators)
        dress_mutators(library);
    else if (scene.tab == lib::Tab::keys || scene.active_keys)
        dress_keys(library);
    if (!scene.in_use_language.empty())
        show_language_in_use(library, scene.in_use_language);
    if (scene.previews)
        dress_map_previews(library);
    if (scene.many)
        add_many_maps(library);
    if (scene.command_line_mod)
        dress_command_line_mod(library);
    else if (scene.tab == lib::Tab::active && !scene.many && !scene.active_keys)
        dress_active_choice(library);
    lib::select(library, fixture::ridge_id());
    lib::set_tab(library, scene.tab);
    if (!scene.select_key.empty()) {
        lib::Kind kind = lib::Kind::map_pack;
        if (scene.tab == lib::Tab::languages)
            kind = lib::Kind::language;
        else if (scene.tab == lib::Tab::mutators)
            kind = lib::Kind::mutator;
        else if (scene.tab == lib::Tab::keys)
            kind = lib::Kind::key_layout;
        lib::select(library, {scene.select_registry, kind, scene.select_key});
    }
    lib::ScreenState state;
    state.maps_scroll = scene.maps_scroll;
    if (scene.details_page) {
        lib::open_details(library);
        state.details_page = true;
    }
    if (scene.key_use) {
        library.key_hooks.moves = made_up_moves;
        if (const lib::Entry* entry = lib::selected_entry(library))
            lib::open_key_use(library, *entry);
    }
    const kit::Frame frame = kit::frame_of(scene.window);
    const kit::DisplayList list = lib::library_layout(library, state, frame, fonts);
    Drawing drawing;
    drawing.scale = frame.scale_percent / 100;
    drawing.window = lib::window_size(frame);
    const int32_t width = (drawing.window.x + 2 * margin_points) * drawing.scale;
    const int32_t height = (drawing.window.y + 2 * margin_points) * drawing.scale;
    drawing.surface.width = static_cast<uint32_t>(width);
    drawing.surface.height = static_cast<uint32_t>(height);
    drawing.surface.rgb.resize(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3
    );
    for (std::size_t at = 0; at < drawing.surface.rgb.size(); at += 3) {
        drawing.surface.rgb[at] = untouched.r;
        drawing.surface.rgb[at + 1] = untouched.g;
        drawing.surface.rgb[at + 2] = untouched.b;
    }
    kit::Canvas canvas;
    canvas.surface = &drawing.surface;
    canvas.placement = {
        margin_points * drawing.scale, margin_points * drawing.scale, drawing.scale, {}
    };
    canvas.fonts = &fonts;
    lib::draw_library(canvas, list);

    for (const std::string& problem : fixture::outside_window(list, drawing.window)) {
        std::fprintf(
            stderr, "%s: %s lies outside the window\n", scene.name.c_str(), problem.c_str()
        );
        ++oa::test::failed_checks();
    }
    for (const std::string& problem : fixture::texts_not_fitting(list, fonts)) {
        std::fprintf(stderr, "%s: %s does not fit\n", scene.name.c_str(), problem.c_str());
        ++oa::test::failed_checks();
    }
    return drawing;
}

/// Tells whether a pixel of a drawing keeps the colour it started as.
///
/// @param drawing the drawing
/// @param x the column, in pixels
/// @param y the row, in pixels
/// @return true when nothing drew there
bool kept(const Drawing& drawing, int32_t x, int32_t y) {
    const std::size_t at =
        (static_cast<std::size_t>(y) * drawing.surface.width + static_cast<std::size_t>(x)) * 3;
    return drawing.surface.rgb[at] == untouched.r && drawing.surface.rgb[at + 1] == untouched.g &&
           drawing.surface.rgb[at + 2] == untouched.b;
}

/// Checks that the margin keeps its colour and that every pixel of the window was drawn.
///
/// @param scene the scene
/// @param drawing its drawing
void check_edges(const Scene& scene, const Drawing& drawing) {
    const int32_t left = margin_points * drawing.scale;
    const int32_t right = left + drawing.window.x * drawing.scale;
    const int32_t top = margin_points * drawing.scale;
    const int32_t bottom = top + drawing.window.y * drawing.scale;
    std::size_t outside = 0;
    std::size_t blank = 0;
    for (int32_t y = 0; y < static_cast<int32_t>(drawing.surface.height); ++y) {
        for (int32_t x = 0; x < static_cast<int32_t>(drawing.surface.width); ++x) {
            const bool in_window = x >= left && x < right && y >= top && y < bottom;
            const bool untouched_here = kept(drawing, x, y);
            if (!in_window && !untouched_here)
                ++outside;
            if (in_window && untouched_here)
                ++blank;
        }
    }
    if (outside != 0)
        std::fprintf(
            stderr, "%s: %zu pixels drawn outside the window\n", scene.name.c_str(), outside
        );
    if (blank != 0)
        std::fprintf(stderr, "%s: %zu pixels of the window not drawn\n", scene.name.c_str(), blank);
    OA_CHECK(outside == 0);
    OA_CHECK(blank == 0);
}

/// Writes a drawing's window as a PNG file.
///
/// @param drawing the drawing
/// @param path the file
/// @return true when it was written
bool write_window(const Drawing& drawing, const std::filesystem::path& path) {
    const auto width = static_cast<std::size_t>(drawing.window.x * drawing.scale);
    const auto height = static_cast<std::size_t>(drawing.window.y * drawing.scale);
    const auto margin = static_cast<std::size_t>(margin_points * drawing.scale);
    std::vector<uint8_t> rows;
    rows.reserve(width * height * 3);
    for (std::size_t y = 0; y < height; ++y) {
        const std::size_t start = ((y + margin) * drawing.surface.width + margin) * 3;
        rows.insert(
            rows.end(),
            drawing.surface.rgb.begin() + static_cast<std::ptrdiff_t>(start),
            drawing.surface.rgb.begin() + static_cast<std::ptrdiff_t>(start + width * 3)
        );
    }
    png::Header header;
    header.width = static_cast<uint32_t>(width);
    header.height = static_cast<uint32_t>(height);
    header.bit_depth = 8;
    header.color_type = png::ColorType::rgb;
    std::vector<uint8_t> file;
    if (!png::write(png::Image{header, {}, rows}, &file))
        return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size())
    );
    return static_cast<bool>(output);
}

/// Returns a window's viewport at a scale.
///
/// @param width the canvas's width, in pixels
/// @param height the canvas's height, in pixels
/// @param percent the scale, as a percentage
/// @return the viewport
kit::Viewport window_of(int32_t width, int32_t height, int32_t percent) {
    kit::Viewport viewport;
    viewport.width = width;
    viewport.height = height;
    viewport.scale_percent = percent;
    return viewport;
}

} // namespace

int main(int argc, char** argv) {
    std::optional<std::filesystem::path> frames;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        if (argument == "--frames" && index + 1 < argc)
            frames = std::filesystem::path(argv[++index]);
        else if (argument != oa::test::kGameDataSwitch) {
            std::fprintf(stderr, "draw: unexpected argument %s\n", argv[index]);
            return 2;
        }
    }
    oa::AssetStore assets = oa::test::require_game_assets("the Library's pictures");
    const kit::Fonts fonts = kit::load_game_fonts(assets);
    const std::unique_ptr<text_font::FontStack> modern =
        text_font::FontStack::open(text_font::bundled_font_directory());
    if (modern) {
        oa::present::GameTextHooks hooks;
        hooks.context = modern.get();
        hooks.draw = draw_modern;
        oa::present::set_game_text_hooks(hooks);
    } else {
        std::printf(
            "draw: no modern fonts beside the test; the characters the game's fonts lack are not "
            "drawn\n"
        );
    }
    if (frames) {
        std::error_code error;
        std::filesystem::create_directories(*frames, error);
        if (error) {
            std::fprintf(stderr, "draw: cannot make %s\n", frames->string().c_str());
            return 1;
        }
    }

    const std::vector<Scene> scenes{
        {"compact", window_of(640, 480, 100), false},
        {"regular", window_of(1920, 1080, 200), false},
        {"large", window_of(2560, 1440, 200), false},
        {"details-compact", window_of(640, 480, 100), true},
        {"active-compact", window_of(640, 480, 100), false, lib::Tab::active, true},
        {"active-compact-maps",
         window_of(640, 480, 100),
         false,
         lib::Tab::active,
         true,
         {},
         {},
         false,
         {},
         100000},
        {"active-regular", window_of(1920, 1080, 200), false, lib::Tab::active, true},
        {"active-large", window_of(2560, 1440, 200), false, lib::Tab::active, true},
        {"active-locked-compact",
         window_of(640, 480, 100),
         false,
         lib::Tab::active,
         true,
         {},
         {},
         false,
         {},
         0,
         true},
        {"active-many-regular",
         window_of(1920, 1080, 200),
         false,
         lib::Tab::active,
         true,
         {},
         {},
         true},
        {"updates-compact", window_of(640, 480, 100), false, lib::Tab::updates},
        {"updates-large", window_of(2560, 1440, 200), false, lib::Tab::updates},
        {"registries-compact", window_of(640, 480, 100), false, lib::Tab::registries},
        {"registries-regular", window_of(1920, 1080, 200), false, lib::Tab::registries},
        {"registries-large", window_of(2560, 1440, 200), false, lib::Tab::registries},
        {"languages-compact", window_of(640, 480, 100), false, lib::Tab::languages},
        {"languages-zh-compact",
         window_of(640, 480, 100),
         true,
         lib::Tab::languages,
         false,
         std::string(fixture::core),
         "zh-Hans",
         false,
         "zh-Hans"},
        {"maps-one-large",
         window_of(2560, 1440, 200),
         false,
         lib::Tab::maps,
         true,
         std::string(fixture::core),
         "shore"},
        {"maps-several-large",
         window_of(2560, 1440, 200),
         false,
         lib::Tab::maps,
         true,
         std::string(fixture::added),
         "desert-pack"},
        {"mutators-compact",
         window_of(640, 480, 100),
         false,
         lib::Tab::mutators,
         false,
         std::string(fixture::core),
         "signal"},
        {"mutators-regular",
         window_of(1920, 1080, 200),
         false,
         lib::Tab::mutators,
         false,
         std::string(fixture::core),
         "signal"},
        {"mutators-large",
         window_of(2560, 1440, 200),
         false,
         lib::Tab::mutators,
         false,
         std::string(fixture::core),
         "signal"},
        {"keys-compact",
         window_of(640, 480, 100),
         true,
         lib::Tab::keys,
         false,
         std::string(fixture::core),
         "grid"},
        {"keys-regular",
         window_of(1920, 1080, 200),
         false,
         lib::Tab::keys,
         false,
         std::string(fixture::core),
         "grid"},
        {"keys-large",
         window_of(2560, 1440, 200),
         false,
         lib::Tab::keys,
         false,
         std::string(fixture::core),
         "grid"},
        {"keys-use-compact",
         window_of(640, 480, 100),
         false,
         lib::Tab::keys,
         false,
         std::string(fixture::core),
         "lefty",
         false,
         {},
         0,
         false,
         true},
        {"keys-use-regular",
         window_of(1920, 1080, 200),
         false,
         lib::Tab::keys,
         false,
         std::string(fixture::core),
         "lefty",
         false,
         {},
         0,
         false,
         true},
        {"keys-use-large",
         window_of(2560, 1440, 200),
         false,
         lib::Tab::keys,
         false,
         std::string(fixture::core),
         "lefty",
         false,
         {},
         0,
         false,
         true},
        {"keys-lefty-large",
         window_of(2560, 1440, 200),
         false,
         lib::Tab::keys,
         false,
         std::string(fixture::core),
         "lefty"},
        {"active-keys-regular",
         window_of(1920, 1080, 200),
         false,
         lib::Tab::active,
         false,
         {},
         {},
         false,
         {},
         0,
         false,
         false,
         true},
    };
    for (const Scene& scene : scenes) {
        const Drawing first = draw_scene(scene, fonts);
        const Drawing second = draw_scene(scene, fonts);
        if (first.surface.rgb != second.surface.rgb)
            std::fprintf(stderr, "%s: two drawings differ\n", scene.name.c_str());
        OA_CHECK(first.surface.rgb == second.surface.rgb);
        check_edges(scene, first);
        if (frames) {
            kit::Fonts shown = fonts;
            if (modern) {
                shown.modern = modern.get();
                shown.size_class = kit::frame_of(scene.window).size_class;
            }
            const Drawing pictured = draw_scene(scene, shown);
            const std::filesystem::path path = *frames / ("library-" + scene.name + ".png");
            const bool written = write_window(pictured, path);
            if (!written)
                std::fprintf(stderr, "draw: cannot write %s\n", path.string().c_str());
            OA_CHECK(written);
        }
    }
    oa::present::set_game_text_hooks({});
    return oa::test::check_exit_status();
}

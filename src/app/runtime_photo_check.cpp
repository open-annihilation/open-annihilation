// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-photo: photo mode over the skirmish's fight, in a window.
// mode enters, hides the interface and leaves. camera frames the fight from
// the whole map to eight times, pans, follows and turns. capture saves a
// photo of that fight in the render mode in use. live saves a photo of the
// turned view.
// One pass enters, moves the camera within play's limits and leaves;
// another never enters. At the last tick the two worlds read back alike,
// and the view photo mode replaced is the view it puts back. Pointer,
// key and resize steps go through the window a player would use.
// --check-photo time: one pass freezes, steps, draws each eighth and runs
// slowly; the other never enters. A shared match and a watcher hold the picture.
// --check-photo pad-touch: a controller opens photo mode, frames the shot and
// takes it, then the phone's MENU does the same with a drag and a pinch.
// --check-photo looks: each colour grade on that fight, then the same fight
// with no look. Full filters the preview and the photo of one frame.
// --check-photo tiles: one frozen Full frame, drawn whole and in tiles,
// at 640×480 and 1920×1080, at size 3, at Enhanced 2× and below zoom 1.
// --check-photo tiles-live: one frozen turned frame, drawn whole and in
// tiles, including a 4096 photo against four tiles of 2048.
// --check-photo depth-live: one frozen turned frame with depth of field, with
// haze, and twice with both off.
// --check-photo poster: a poster two views wide equals two photos of that
// frozen tick placed side by side. On Full the poster is drawn in tiles.
#include "oa/app/runtime.hpp"

#include "engine_settings_state.hpp"
#include "net_options.hpp"
#include "oa_layer_check.hpp"
#include "full_presentation.hpp"
#include "language_state.hpp"
#include "oa_layer.hpp"
#include "pad_state.hpp"
#include "photo_camera.hpp"
#include "touch_state.hpp"
#include "photo_clipboard.hpp"
#include "photo_state.hpp"
#include "render_run.hpp"

#include "oa/app/far_view.hpp"
#include "oa/app/frame_coordinates.hpp"
#include "oa/app/frame_pacing.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/base/game_loop.hpp"
#include "oa/base/sha256.hpp"
#include "oa/core/player.h"
#include "oa/core/player_setup.h"
#include "oa/core/unit.h"
#include "oa/data/defs/layout.hpp"
#include "oa/data/key_library/library.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/formats/oakeys/manifest.hpp"
#include "oa/formats/json.hpp"
#include "oa/formats/png.hpp"
#include "full_supersampling.hpp"
#include "renderer_effect_sources.hpp"
#include "oa/core/world.h"
#include "oa/present/photo/photo_sizes.hpp"
#include "oa/present/photo/photo_tiles.hpp"
#include "oa/present/world_renderer/world_draw_order.hpp"
#include "oa/sim/unit_spawn/spawn.hpp"
#include "renderer_live_gpu.hpp"
#include "oa/present/photo/looks.hpp"
#include "oa/present/world_renderer/world_fog.hpp"
#include "oa/sim/simulation_state.hpp"

#include "oa/platform/files.hpp"
#include "oa/session/demo.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/touch_hud.hpp"
#include "oa/data/input_actions/actions.hpp"
#include "oa/sim/messages.hpp"
#include "oa/sim/model_runtime/instance.hpp"
#include "oa/sim/spatial_state/spatial.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/photo_panel/crop.hpp"
#include "oa/ui/photo_panel/layout.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/ui/hud/game_fields.hpp"
#include "oa/ui/hud/kill_board.hpp"
#include "oa/ui/hud/resource_panel.hpp"
#include "oa/ui/key_editor/overlay.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#ifndef OA_ENGINE_VERSION
#error OA_ENGINE_VERSION is set by the build
#endif

namespace oa::app {
namespace {

namespace actions = oa::data::input_actions;
namespace console = oa::ui::console;
namespace fs = std::filesystem;
namespace layout = oa::ui::display_layout;
namespace library = oa::data::key_library;
namespace oakeys = oa::formats::oakeys;

/// Ticks each pass of the fight plays, one drawn frame a tick.
constexpr int kPassTicks = 120;
/// The tick photo mode opens on, before that tick's frame.
constexpr int kEnterTick = 30;
/// The tick photo mode closes on, after that tick's frame.
constexpr int kLeaveTick = 90;
/// The fight's armies, each side, as the director view check plants them.
constexpr std::size_t kArmy = 8;
/// The zoom both passes hold outside photo mode.
constexpr float kPlayZoom = 1.0F;
/// The photo camera's zoom, within play's limits and no wider on the map
/// than the play view at kPlayZoom on the check's window.
constexpr float kPhotoZoomNear = 1.25F;
constexpr float kPhotoZoomFar = 1.35F;
/// Map pixels one photo pan step covers, before the zoom divides it. The
/// next tick steps the other way, so the view comes back.
constexpr double kPanStep = 4.0;

/// Primary on this platform: Command on a Mac, Ctrl elsewhere.
#ifdef SDL_PLATFORM_MACOS
constexpr SDL_Keymod kPrimaryModifier = SDL_KMOD_LGUI;
#else
constexpr SDL_Keymod kPrimaryModifier = SDL_KMOD_LCTRL;
#endif
/// The modifier that is not Primary on this platform: Ctrl on a Mac, the
/// Windows or Super key elsewhere. Its C reaches no photo command.
#ifdef SDL_PLATFORM_MACOS
constexpr SDL_Keymod kOtherModifier = SDL_KMOD_LCTRL;
#else
constexpr SDL_Keymod kOtherModifier = SDL_KMOD_LGUI;
#endif

/// Offers the check's hook counted, and the type of the latest one.
struct ClipboardCatch {
    int offers = 0;
    std::string mime;
};

ClipboardCatch clipboard_catch;

/// Stands in for the clipboard. The bytes are counted, not kept.
///
/// @param mime_type the type offered
/// @return true, the clipboard took them
bool catch_photo_clipboard(const char* mime_type, const uint8_t*, std::size_t) {
    ++clipboard_catch.offers;
    clipboard_catch.mime = mime_type != nullptr ? mime_type : "";
    return true;
}

/// The check's hook stands in for the clipboard, including when a require
/// throws, so the run does not replace whatever text the clipboard holds.
struct ClipboardHookHold {
    ClipboardHookHold() {
        clipboard_catch = {};
        set_photo_clipboard_hook(catch_photo_clipboard);
    }

    ClipboardHookHold(const ClipboardHookHold&) = delete;
    ClipboardHookHold& operator=(const ClipboardHookHold&) = delete;

    ~ClipboardHookHold() { set_photo_clipboard_hook(nullptr); }
};

/// The extension state the shared and watcher passes report.
uint32_t photo_extension_bits = 0;

/// The extension state hook for the check. It cannot capture.
///
/// @return photo_extension_bits
uint32_t photo_extension_state(void*, const Runtime&) {
    return photo_extension_bits;
}

/// The bit of a context, or 0 when the table has no such name.
///
/// @param name the context id
/// @return the bit
uint32_t context_bit(std::string_view name) {
    const std::span<const actions::Context> table = actions::contexts();
    for (std::size_t index = 0; index < table.size() && index < 32; ++index) {
        if (table[index].name == name)
            return uint32_t{1} << static_cast<uint32_t>(index);
    }
    return 0;
}

/// The player's view, the fields leaving puts back.
struct KeptView {
    layout::MatchLayout placed{};
    int32_t camera_x{};
    int32_t camera_z{};
    float zoom{};
    float zoom_target{};
    bool tracking{};
    uint16_t tracked_unit{};
    bool exact_held{};
    double exact_x{};
    double exact_z{};
    int32_t exact_camera_x{};
    int32_t exact_camera_z{};
    double exact_drawn_x{};
    double exact_drawn_z{};
    MatchCommand command{};
    uint16_t build{};
    bool hold_held{};
    double hold_x{};
    double hold_z{};
};

/// One unit's selection and orders.
struct UnitOrders {
    uint16_t id{};
    uint16_t type{};
    uint32_t flags{};
    uint32_t primary{};
    uint32_t secondary{};

    /// @return true when the selection and the orders are the same
    [[nodiscard]] bool operator==(const UnitOrders&) const = default;
};

/// The Game fields photo mode must not write.
struct MatchFields {
    uint16_t console{};
    uint8_t visibility{};
    uint16_t sim_run{};
    std::vector<char> messages{};
};

/// Reads a preferences file's bytes. An absent file is empty.
///
/// @param path the preferences file the check was given
/// @return the bytes
/// @throws std::runtime_error when no preferences file was given
std::vector<char> file_bytes(const std::optional<std::filesystem::path>& path) {
    if (!path.has_value())
        throw std::runtime_error("photo mode check: no preferences file");
    std::ifstream in(*path, std::ios::binary);
    if (!in)
        return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// True when the picture holds more than one colour.
///
/// @param rgb the picture, three bytes a pixel
/// @return true when two pixels differ
bool more_than_one_colour(const std::vector<uint8_t>& rgb) {
    if (rgb.size() < 6)
        return false;
    const uint8_t red = rgb[0];
    const uint8_t green = rgb[1];
    const uint8_t blue = rgb[2];
    for (std::size_t index = 3; index + 2 < rgb.size(); index += 3) {
        if (rgb[index] != red || rgb[index + 1] != green || rgb[index + 2] != blue)
            return true;
    }
    return false;
}

/// The file name in a "Saved …" status, without a copy note after it.
///
/// @param status the status line
/// @return the file name, or empty when the line does not start with "Saved "
std::string saved_photo_name(std::string_view status) {
    constexpr std::string_view kSavedStatus = "Saved ";
    if (!status.starts_with(kSavedStatus))
        return {};
    std::string_view name = status.substr(kSavedStatus.size());
    constexpr std::string_view kCopied = "  Copied";
    constexpr std::string_view kNotCopied = "  Photo not copied:";
    if (name.ends_with(kCopied))
        name.remove_suffix(kCopied.size());
    else if (const std::size_t at = name.rfind(kNotCopied); at != std::string_view::npos)
        name = name.substr(0, at);
    return std::string(name);
}

/// The pinned SHA-256 of the Off photo's RGB rows. The capture check prints
/// the digest it measured, then requires this text.
constexpr std::string_view kPhotoCaptureSha256 =
    "48703554acde88c7f988913c5cf3aa78e0ddf8c1959277fadd4660c261f4afb5";

/// Removes PNG files and half-written photos from a folder.
///
/// @param folder the screenshots folder
void remove_saved_pictures(const fs::path& folder) {
    std::error_code walk_error;
    if (!fs::is_directory(folder, walk_error))
        return;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder, walk_error)) {
        const std::string text = oa::platform::path_to_generic_utf8(entry.path().filename());
        if ((text.size() >= 4 && text.ends_with(".png")) ||
            text.find(".partial") != std::string::npos)
            fs::remove(entry.path(), walk_error);
    }
}

/// Throws when a half-written photo is still in the folder.
///
/// @param folder the screenshots folder
void require_no_partials(const fs::path& folder) {
    std::error_code walk_error;
    if (!fs::is_directory(folder, walk_error))
        return;
    for (const fs::directory_entry& entry : fs::directory_iterator(folder, walk_error)) {
        if (oa::platform::path_to_generic_utf8(entry.path().filename()).find(".partial") !=
            std::string::npos)
            throw std::runtime_error("photo capture check: a partial photo was left behind");
    }
}

/// One player the capture check expects a photo to name, or to leave out.
struct RosterEntry {
    std::string name;
    std::string side;
};

/// The players a photo of this match records, watchers left out.
///
/// @param world the match
/// @return the players, in slot order
std::vector<RosterEntry> photo_roster(const oa::World& world) {
    std::vector<RosterEntry> roster;
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
        const oa::Player& player = world.game.players[slot];
        if (!oa::sim::simulation_state::player_active(player))
            continue;
        const oa::PlayerSetupInfo* info = oa::world_player_info(&world, &player);
        if (info != nullptr && (info->options & OA_SETUP_OPTION_WATCHER) != 0)
            continue;
        RosterEntry entry;
        const void* end = std::memchr(player.name, 0, sizeof player.name);
        const std::size_t length =
            end == nullptr ? sizeof player.name
                           : static_cast<std::size_t>(static_cast<const char*>(end) - player.name);
        entry.name.assign(player.name, length);
        if (info != nullptr)
            entry.side = oa::data::defs::side_name(info->side);
        roster.push_back(std::move(entry));
    }
    return roster;
}

/// Reads the photo's text chunks and checks the record against the match.
///
/// @param picture the file
/// @param width the picture's width
/// @param height the picture's height
/// @param tick the match's tick
/// @param map the map's name
/// @param names_on true when player names are in the photo
/// @param roster the players, as the match holds them
/// @param shots_text the screenshots folder, which must not appear
void require_photo_chunks(
    const fs::path& picture,
    uint32_t width,
    uint32_t height,
    uint32_t tick,
    std::string_view map,
    bool names_on,
    const std::vector<RosterEntry>& roster,
    std::string_view shots_text
) {
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("photo capture check: " + what);
    };
    std::ifstream in(picture, std::ios::binary);
    if (!in)
        fail("the photo could not be read");
    const std::vector<char> stored(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()
    );
    const std::span<const uint8_t> file(
        reinterpret_cast<const uint8_t*>(stored.data()), stored.size()
    );
    oa::formats::png::Messages messages{};
    oa::formats::png::Info info{};
    if (!oa::formats::png::read_info(file, messages, &info))
        fail("the photo is not a PNG");
    const oa::formats::png::Header header = oa::formats::png::header_of(info, messages);
    if (header.width != width || header.height != height)
        fail("the photo's size is not the picture's");
    std::vector<oa::formats::png::TextChunk> texts;
    if (!oa::formats::png::read_texts(file, &texts))
        fail("the photo's text could not be read");
    static constexpr const char* kKeywords[] = {
        "Title", "Description", "Software", "Creation Time", "Source", "OpenAnnihilation"
    };
    static constexpr oa::formats::png::TextKind kKinds[] = {
        oa::formats::png::TextKind::utf8,
        oa::formats::png::TextKind::utf8,
        oa::formats::png::TextKind::latin1,
        oa::formats::png::TextKind::latin1,
        oa::formats::png::TextKind::latin1,
        oa::formats::png::TextKind::utf8
    };
    if (texts.size() != 6)
        fail("the photo has " + std::to_string(texts.size()) + " text chunks");
    std::string chunks[6];
    for (std::size_t at = 0; at < 6; ++at) {
        if (texts[at].keyword != kKeywords[at] || texts[at].kind != kKinds[at])
            fail(std::string("text chunk ") + std::to_string(at) + " is not " + kKeywords[at]);
        chunks[at] = std::string(texts[at].text);
    }
    if (chunks[0].empty() || chunks[0].find(',') == std::string::npos)
        fail("the title is " + chunks[0]);
    if (!map.empty() && chunks[0].find(map) == std::string::npos)
        fail("the title misses the map: " + chunks[0]);
    const std::string software = std::string("Open Annihilation ") + OA_ENGINE_VERSION;
    if (chunks[2] != software)
        fail("Software is " + chunks[2]);
    if (chunks[3].size() < 29 || !chunks[3].ends_with(" GMT"))
        fail("Creation Time is " + chunks[3]);
    if (chunks[4] != "Skirmish")
        fail("Source is " + chunks[4]);
    for (const std::string& text : chunks) {
        if (!shots_text.empty() && text.find(shots_text) != std::string::npos)
            fail("the photo names its folder");
    }
    oa::formats::json::JsonError error;
    const std::optional<oa::formats::json::Json> record =
        oa::formats::json::parse_json(chunks[5], error);
    if (!record.has_value())
        fail("the record is not JSON");
    const oa::formats::json::Json* version = record->find("photo");
    if (version == nullptr || !version->integer().has_value() || *version->integer() != 1)
        fail("the record is not version 1");
    const oa::formats::json::Json* game = record->find("game");
    const oa::formats::json::Json* game_tick = game == nullptr ? nullptr : game->find("tick");
    if (game_tick == nullptr || !game_tick->integer().has_value() ||
        *game_tick->integer() != static_cast<int64_t>(tick))
        fail("the record's tick is not the match's");
    const oa::formats::json::Json* size = record->find("size");
    const oa::formats::json::Json* size_width = size == nullptr ? nullptr : size->find("width");
    const oa::formats::json::Json* size_height = size == nullptr ? nullptr : size->find("height");
    if (size_width == nullptr || size_height == nullptr || !size_width->integer().has_value() ||
        !size_height->integer().has_value() ||
        *size_width->integer() != static_cast<int64_t>(width) ||
        *size_height->integer() != static_cast<int64_t>(height))
        fail("the record's size is not the picture's");
    const oa::formats::json::Json* players = record->find("players");
    if (players == nullptr || players->type() != oa::formats::json::JsonType::array)
        fail("the record has no players");
    std::string listed;
    bool human = false;
    for (const RosterEntry& entry : roster) {
        if (!listed.empty())
            listed += ", ";
        listed += entry.name + "/" + entry.side;
        if (entry.name == "Player" && entry.name != entry.side)
            human = true;
    }
    if (!human)
        fail("the fight has no player named Player (" + listed + ")");
    bool saw_player = false;
    for (const oa::formats::json::Json& element : players->elements()) {
        const oa::formats::json::Json* name = element.find("name");
        if (names_on) {
            if (name == nullptr || name->string() == nullptr)
                fail("a player has no name");
            if (name->string() != nullptr && *name->string() == "Player")
                saw_player = true;
        } else if (name != nullptr) {
            fail("a name was written while player names were off");
        }
        const oa::formats::json::Json* team = element.find("team");
        if (team == nullptr || team->type() != oa::formats::json::JsonType::null)
            fail("a player with no team was numbered");
    }
    if (!names_on && chunks[1].find("team") != std::string::npos)
        fail("the description names a team");
    if (names_on && !saw_player)
        fail("no player is named Player (" + listed + ")");
    if (!names_on) {
        for (const RosterEntry& entry : roster) {
            if (entry.name.empty() || entry.name == entry.side)
                continue;
            if (!map.empty() && map.find(entry.name) != std::string::npos)
                continue;
            for (const std::string& text : chunks) {
                if (text.find(entry.name) != std::string::npos)
                    fail("the name " + entry.name + " was written while player names were off");
            }
        }
    }
}

/// The photo's clock. Thirty ticks are one second. Under an hour the text is
/// minutes and seconds. From an hour it is hours, minutes and seconds.
///
/// @param tick the match's tick
/// @return the clock
std::string photo_clock(uint32_t tick) {
    const uint32_t seconds = tick / 30U;
    const uint32_t second = seconds % 60U;
    const uint32_t minute = (seconds / 60U) % 60U;
    const uint32_t hour = seconds / 3600U;
    const auto pair = [](uint32_t value) {
        const std::string text = std::to_string(value);
        return text.size() >= 2 ? text : std::string("0") + text;
    };
    if (hour == 0U)
        return std::to_string(minute) + ":" + pair(second);
    return std::to_string(hour) + ":" + pair(minute) + ":" + pair(second);
}

/// The held photo records the tick it was held at, and the title's time is
/// that tick's clock.
///
/// @param picture the file
/// @param held_tick the tick when the picture was held
void require_held_moment(const fs::path& picture, uint32_t held_tick) {
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("photo capture check: " + what);
    };
    std::ifstream in(picture, std::ios::binary);
    if (!in)
        fail("the held photo could not be read");
    const std::vector<char> stored(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()
    );
    const std::span<const uint8_t> file(
        reinterpret_cast<const uint8_t*>(stored.data()), stored.size()
    );
    std::vector<oa::formats::png::TextChunk> texts;
    if (!oa::formats::png::read_texts(file, &texts))
        fail("the held photo's text could not be read");
    std::string title;
    std::string record_text;
    for (const oa::formats::png::TextChunk& chunk : texts) {
        if (chunk.keyword == "Title")
            title = std::string(chunk.text);
        else if (chunk.keyword == "OpenAnnihilation")
            record_text = std::string(chunk.text);
    }
    if (title.empty() || record_text.empty())
        fail("the held photo has no title or record");
    oa::formats::json::JsonError error;
    const std::optional<oa::formats::json::Json> record =
        oa::formats::json::parse_json(record_text, error);
    if (!record.has_value())
        fail("the held photo's record is not JSON");
    const oa::formats::json::Json* game = record->find("game");
    const oa::formats::json::Json* game_tick = game == nullptr ? nullptr : game->find("tick");
    if (game_tick == nullptr || !game_tick->integer().has_value() ||
        *game_tick->integer() != static_cast<int64_t>(held_tick))
        fail("the held photo's tick is not the moment it was held");
    const std::string clock = photo_clock(held_tick);
    const std::string suffix = std::string(", ") + clock;
    const bool titled =
        title == clock || (title.size() >= suffix.size() &&
                           title.compare(title.size() - suffix.size(), suffix.size(), suffix) == 0);
    if (!titled)
        fail("the held photo's title time is " + title);
}

/// The SHA-256 of RGB rows, as 64 lower-case hex digits.
///
/// @param bytes the rows
/// @return the digest
std::string sha256_hex(const std::vector<uint8_t>& bytes) {
    const auto hex = oa::base::sha256::to_hex(oa::base::sha256::digest_of(bytes));
    return std::string(hex.data(), hex.size());
}

/// An 8-bit RGB PNG, decoded to packed rows.
struct DecodedPng {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rows;
};

/// Decodes an 8-bit RGB PNG at the size the file stores.
///
/// @param picture the file
/// @return the picture
DecodedPng decode_png(const fs::path& picture) {
    std::ifstream in(picture, std::ios::binary);
    if (!in)
        throw std::runtime_error("photo capture check: the photo could not be read");
    const std::vector<char> stored(
        (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()
    );
    const std::span<const uint8_t> file(
        reinterpret_cast<const uint8_t*>(stored.data()), stored.size()
    );
    oa::formats::png::Messages messages{};
    oa::formats::png::Info info{};
    if (!oa::formats::png::read_info(file, messages, &info))
        throw std::runtime_error("photo capture check: the photo is not a PNG");
    const oa::formats::png::Header header = oa::formats::png::header_of(info, messages);
    if (header.color_type != oa::formats::png::ColorType::rgb || header.bit_depth != 8)
        throw std::runtime_error("photo capture check: the photo is not 8-bit RGB");
    const oa::formats::png::Transforms transforms{};
    const std::size_t row = oa::formats::png::row_bytes(header, transforms);
    if (row != static_cast<std::size_t>(header.width) * 3U)
        throw std::runtime_error("photo capture check: the photo rows are not packed RGB");
    DecodedPng decoded;
    decoded.width = header.width;
    decoded.height = header.height;
    decoded.rows.resize(static_cast<std::size_t>(header.height) * row);
    if (oa::formats::png::read_image(file, info, transforms, messages, decoded.rows) !=
        oa::formats::png::Progress::end)
        throw std::runtime_error("photo capture check: the photo did not decode");
    return decoded;
}

/// Decodes an 8-bit RGB PNG of the battlefield's size.
///
/// @param picture the file
/// @param width the battlefield width
/// @param height the battlefield height
/// @return packed RGB rows
std::vector<uint8_t> png_rgb(const fs::path& picture, uint32_t width, uint32_t height) {
    DecodedPng decoded = decode_png(picture);
    if (decoded.width != width || decoded.height != height) {
        throw std::runtime_error(
            "photo capture check: photo " + std::to_string(decoded.width) + "x" +
            std::to_string(decoded.height) + " battlefield " + std::to_string(width) + "x" +
            std::to_string(height)
        );
    }
    return std::move(decoded.rows);
}

/// How many pixels of a photo drawn in tiles may differ from the same photo
/// drawn whole: one in this many, rounded up, and at least one. The
/// maintainer's ruling of 2026-11-10 (P15) is 0.01%. The card rasterises a
/// tile from another origin than the whole photo, which leaves a few pixels
/// a few levels off at model edges and on the water, and a follow-up may
/// tighten the bound.
constexpr uint64_t kTilePixelsPerAllowed = 10000;

/// The bound for a photo zoom below 1 on SDL's software renderer: 0.5%. A
/// player's Full always draws on a graphics card, and that renderer is a
/// path only the checks use. There a tile's pixels land differently from the
/// whole photo's, by about a quarter of a percent in the check's frame, where
/// a card differs by a few thousandths of one. The card's check holds the
/// case to 0.01%. Every other case on that renderer keeps the 0.01%.
constexpr uint64_t kSoftwareBelowZoomPixelsPerAllowed = 200;

/// A seam is a line along a tile edge, so it puts a good many differing
/// pixels on the edge columns and rows, more than half of all that differ. A
/// model edge or a ripple that crosses a tile edge touches a few pixels
/// there, 2 and 7 in the runs measured, and a few on their own are not a
/// seam. The faults the check exists for put 66 (a unit and a feature drawn
/// in the other order) and 2,427 (a view moved off the map's corner) there.
/// The handful is 16: over what one crossing touches, and well under the
/// least seam measured.
constexpr uint64_t kTileSeamHandful = 16;

/// The differing pixels the ruling allows in a photo.
///
/// @param pixels the photo's pixels
/// @param per_allowed one differing pixel is allowed in this many
/// @return that share of them rounded up, and at least 1
constexpr uint64_t tile_allowance(uint64_t pixels, uint64_t per_allowed) {
    return std::max<uint64_t>(1U, (pixels + per_allowed - 1U) / per_allowed);
}

/// How a photo drawn in tiles differs from the same photo drawn whole. A
/// pixel differs when any of its channels does. A tile edge pixel is a
/// column or a row next to a tile boundary, on either side of it.
struct TileGap {
    uint64_t pixels{0};    ///< the photo's pixels
    uint64_t allowed{0};   ///< the most that may differ
    uint64_t different{0}; ///< pixels that differ
    uint64_t on_edge{0};   ///< of those, pixels on a tile edge
    int max_delta{0};      ///< the largest channel gap
    int32_t first_x{-1};   ///< the first differing pixel
    int32_t first_y{-1};
    int first_channel{0};
    std::string places; ///< the first few differing pixels, whole value / tiled value per channel

    /// True when more pixels differ than the ruling allows.
    [[nodiscard]] bool too_many() const noexcept { return different > allowed; }

    /// True when the differences run along the tile edges.
    [[nodiscard]] bool seam() const noexcept {
        return on_edge > kTileSeamHandful && on_edge * 2U > different;
    }

    /// True when the tiled photo is as near the whole one as the ruling asks.
    [[nodiscard]] bool within() const noexcept { return !too_many() && !seam(); }
};

/// Measures a photo drawn in tiles against the same photo drawn whole.
///
/// @param whole the whole photo's packed RGB rows
/// @param tiled the tiled photo's rows, the same size
/// @param width the photos' width
/// @param height the photos' height
/// @param plan the tiles the photo was drawn in, whose boundaries are the edges
/// @param per_allowed one differing pixel is allowed in this many
/// @return the gap
TileGap measure_tile_gap(
    const std::vector<uint8_t>& whole,
    const std::vector<uint8_t>& tiled,
    uint32_t width,
    uint32_t height,
    const oa::present::photo::TilePlan& plan,
    uint64_t per_allowed = kTilePixelsPerAllowed
) {
    TileGap gap;
    gap.pixels = static_cast<uint64_t>(width) * height;
    gap.allowed = tile_allowance(gap.pixels, per_allowed);
    std::vector<char> seam_column(width, 0);
    std::vector<char> seam_row(height, 0);
    for (const oa::present::photo::Tile& tile : plan.tiles) {
        if (tile.x > 0 && static_cast<uint32_t>(tile.x) < width) {
            seam_column[static_cast<std::size_t>(tile.x)] = 1;
            seam_column[static_cast<std::size_t>(tile.x - 1)] = 1;
        }
        if (tile.y > 0 && static_cast<uint32_t>(tile.y) < height) {
            seam_row[static_cast<std::size_t>(tile.y)] = 1;
            seam_row[static_cast<std::size_t>(tile.y - 1)] = 1;
        }
    }
    for (uint32_t row = 0; row < height; ++row) {
        for (uint32_t column = 0; column < width; ++column) {
            const std::size_t at = (static_cast<std::size_t>(row) * width + column) * 3U;
            const int red = std::abs(static_cast<int>(whole[at]) - static_cast<int>(tiled[at]));
            const int green =
                std::abs(static_cast<int>(whole[at + 1U]) - static_cast<int>(tiled[at + 1U]));
            const int blue =
                std::abs(static_cast<int>(whole[at + 2U]) - static_cast<int>(tiled[at + 2U]));
            const int delta = std::max(std::max(red, green), blue);
            if (delta == 0)
                continue;
            ++gap.different;
            gap.max_delta = std::max(gap.max_delta, delta);
            if (seam_column[column] != 0 || seam_row[row] != 0)
                ++gap.on_edge;
            if (gap.first_x < 0) {
                gap.first_x = static_cast<int32_t>(column);
                gap.first_y = static_cast<int32_t>(row);
                gap.first_channel = red != 0 ? 0 : green != 0 ? 1 : 2;
            }
            if (gap.different <= 16U) {
                gap.places += " " + std::to_string(column) + "," + std::to_string(row) + " (" +
                              std::to_string(static_cast<int>(whole[at])) + "/" +
                              std::to_string(static_cast<int>(tiled[at])) + " " +
                              std::to_string(static_cast<int>(whole[at + 1U])) + "/" +
                              std::to_string(static_cast<int>(tiled[at + 1U])) + " " +
                              std::to_string(static_cast<int>(whole[at + 2U])) + "/" +
                              std::to_string(static_cast<int>(tiled[at + 2U])) + ")";
            }
        }
    }
    return gap;
}

/// The check's words for a gap: the count with its bound, the pixels on a
/// tile edge, the largest channel gap and the first pixel. A gap that is not
/// within the ruling says which rule it broke and lists the first pixels.
///
/// @param gap the measure
/// @return the words, after "where: "
std::string tile_gap_words(const TileGap& gap) {
    std::string words = std::to_string(gap.different) + " pixels differ (" +
                        std::to_string(gap.allowed) + " allowed), " + std::to_string(gap.on_edge) +
                        " on a tile edge, up to " + std::to_string(gap.max_delta) + ", first at " +
                        std::to_string(gap.first_x) + "," + std::to_string(gap.first_y) +
                        " channel " + std::to_string(gap.first_channel);
    if (gap.seam())
        words += ", a seam along a tile edge";
    else if (gap.too_many())
        words += ", more than the ruling allows";
    if (!gap.within())
        words += gap.places;
    return words;
}

/// Photo mode's own words, which a fresh visit's line starts with.
constexpr std::string_view kFreshPhotoLine = "Photo mode  Esc leaves";

/// The line a fresh visit shows, before any photo of that visit. Entering
/// photo mode in a skirmish freezes the battle, so Frozen and the game
/// clock follow photo mode's own words.
///
/// @param frozen whether entering froze the battle
/// @param tick the battle's tick
/// @return the line
std::string fresh_photo_line(bool frozen, uint32_t tick) {
    std::string line{kFreshPhotoLine};
    if (!frozen)
        return line;
    char clock[64]{};
    oa::ui::hud::format_game_time(clock, sizeof clock, tick);
    line += "  ";
    line += oa::data::languages::interface_text("Frozen");
    line += "  ";
    line += clock;
    return line;
}

/// Saved Gamma 20, value * (1/24) + 0.5, the step the options screen uses.
constexpr float kCheckGamma = static_cast<float>(20.0 * 0.041666668 + 0.5);

/// Maps the processor's battlefield through `table`, one channel at a time.
///
/// @param world the battlefield the frame left
/// @param table the player's gamma table
/// @return packed RGB, or empty when the battlefield has no picture
std::vector<uint8_t>
gamma_mapped(const renderer::Surface& world, const std::array<uint8_t, 256>& table) {
    const std::size_t pixels = static_cast<std::size_t>(world.width) * world.height;
    if (world.width == 0 || world.height == 0 || world.rgb.size() != pixels * 3U)
        return {};
    std::vector<uint8_t> rgb = world.rgb;
    for (std::size_t index = 0; index < pixels; ++index) {
        rgb[index * 3U] = table[rgb[index * 3U]];
        rgb[index * 3U + 1U] = table[rgb[index * 3U + 1U]];
        rgb[index * 3U + 2U] = table[rgb[index * 3U + 2U]];
    }
    return rgb;
}

/// Requires the photo to equal the presented frame outside the panel's chrome.
///
/// The presented frame is the whole window, as capture_render_target reads it.
/// The photo is the battlefield, drawn at `origin_x`, `origin_y` in that
/// window. When the battlefield is the window, the origin is zero and the
/// sizes are the same.
///
/// @param photo the photo's packed RGB
/// @param width the photo's width
/// @param height the photo's height
/// @param presented the frame the window presented
/// @param origin_x the battlefield's left edge in the window
/// @param origin_y the battlefield's top edge in the window
/// @param layout_width the match layout the panel was drawn in
/// @param layout_height the match layout the panel was drawn in
void require_presented_battlefield(
    const std::vector<uint8_t>& photo,
    uint32_t width,
    uint32_t height,
    const renderer::Surface& presented,
    int origin_x,
    int origin_y,
    int layout_width,
    int layout_height
) {
    const bool fits = origin_x >= 0 && origin_y >= 0 &&
                      static_cast<uint64_t>(origin_x) + width <= presented.width &&
                      static_cast<uint64_t>(origin_y) + height <= presented.height;
    if (!fits ||
        presented.rgb.size() != static_cast<std::size_t>(presented.width) * presented.height * 3U ||
        photo.size() != static_cast<std::size_t>(width) * height * 3U) {
        throw std::runtime_error(
            "photo capture check: presented " + std::to_string(presented.width) + "x" +
            std::to_string(presented.height) + " photo " + std::to_string(width) + "x" +
            std::to_string(height) + " at " + std::to_string(origin_x) + "," +
            std::to_string(origin_y)
        );
    }
    // The panel, the chips, the key line and the toast are drawn on the
    // Open Annihilation layer after the photo is read. Skip those bands.
    std::size_t compared = 0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const int window_x = origin_x + static_cast<int>(x);
            const int window_y = origin_y + static_cast<int>(y);
            // The panel is laid out in the match, which can be smaller than
            // the presented frame. Skip the chrome in that layout.
            if (oa::ui::photo_panel::covers_chrome(window_x, window_y, layout_width, layout_height))
                continue;
            const std::size_t photo_at = (static_cast<std::size_t>(y) * width + x) * 3U;
            const std::size_t shown_at = (static_cast<std::size_t>(window_y) * presented.width +
                                          static_cast<uint32_t>(window_x)) *
                                         3U;
            if (photo[photo_at] != presented.rgb[shown_at] ||
                photo[photo_at + 1U] != presented.rgb[shown_at + 1U] ||
                photo[photo_at + 2U] != presented.rgb[shown_at + 2U]) {
                throw std::runtime_error(
                    "photo capture check: the photo differs from the presented frame at " +
                    std::to_string(x) + "," + std::to_string(y) + " window " +
                    std::to_string(window_x) + "," + std::to_string(window_y) + " presented " +
                    std::to_string(presented.width) + "x" + std::to_string(presented.height) +
                    " layout " + std::to_string(layout_width) + "x" +
                    std::to_string(layout_height) + " origin " + std::to_string(origin_x) + "," +
                    std::to_string(origin_y)
                );
            }
            ++compared;
        }
    }
    if (compared == 0)
        throw std::runtime_error("photo capture check: the photo panel covers the picture");
}

/// Points `capture` at `frame` until the end of the scope.
struct CaptureHold {
    renderer::Surface*& slot;

    CaptureHold(renderer::Surface*& held, renderer::Surface& frame) : slot(held) { slot = &frame; }

    CaptureHold(const CaptureHold&) = delete;
    CaptureHold& operator=(const CaptureHold&) = delete;

    ~CaptureHold() { slot = nullptr; }
};

/// Ticks a shared match runs under a held picture before its photo.
constexpr int kHeldTicks = 30;

/// Tells whether the window shows one of its pixels for each layout pixel,
/// as it does everywhere but at native density.
///
/// @param window the game's window, or none
/// @return true when the window's pixels are the layout's
bool window_one_to_one(SDL_Window* window) {
    return window == nullptr || SDL_GetWindowPixelDensity(window) < 1.5F;
}

/// Has the extension report a shared match until the end of the scope, then
/// puts its state hook back.
struct SharedMatchHold {
    Extension& extension;
    uint32_t (*state)(void*, const Runtime&);
    void* context;

    explicit SharedMatchHold(Extension& held)
        : extension(held), state(held.state), context(held.context) {
        photo_extension_bits = extension_state::multiplayer | extension_state::shared_match;
        extension.state = photo_extension_state;
    }

    SharedMatchHold(const SharedMatchHold&) = delete;
    SharedMatchHold& operator=(const SharedMatchHold&) = delete;

    ~SharedMatchHold() {
        extension.state = state;
        extension.context = context;
        photo_extension_bits = 0;
    }
};

/// One frame of the pad-touch check, and how long View is held past its delay.
constexpr uint64_t kPadTouchFrameMs = 16;
constexpr uint64_t kPadTouchHoldMs = 450;
/// Frames after a stand-in pad is attached, so the runtime can open it.
constexpr int kPadTouchAttachFrames = 3;
/// A stick pushed past the dead zone, and a trigger pulled or at rest.
constexpr float kPadTouchStick = 0.9F;
constexpr float kPadTouchTrigger = 1.0F;
/// The stand-in pad's USB numbers: an Xbox pad, plus the four paddles that
/// make View the game layer.
constexpr uint16_t kPadTouchVendor = 0x045e;
constexpr uint16_t kPadTouchProduct = 0x02ea;
/// The joystick axis ends the binding's ranges are measured against.
constexpr int kPadTouchAxisMost = SDL_JOYSTICK_AXIS_MAX;
constexpr int kPadTouchAxisLeast = SDL_JOYSTICK_AXIS_MIN;
/// The check's own touch device: SDL does not know it, and the check allows that.
constexpr SDL_TouchID kPadTouchDevice = 1;

/// The buttons the stand-in has: an Xbox pad's, and the four paddles.
constexpr std::array<SDL_GamepadButton, 19> kPadTouchButtons{
    SDL_GAMEPAD_BUTTON_SOUTH,          SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST,           SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_BACK,           SDL_GAMEPAD_BUTTON_GUIDE,
    SDL_GAMEPAD_BUTTON_START,          SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK,    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_DPAD_UP,
    SDL_GAMEPAD_BUTTON_DPAD_DOWN,      SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT,     SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,
    SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,   SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2,
    SDL_GAMEPAD_BUTTON_LEFT_PADDLE2,
};

/// The joystick input SDL binds one gamepad button or axis to.
struct PadTouchBinding {
    SDL_GamepadBindingType type{SDL_GAMEPAD_BINDTYPE_NONE};
    int index{};
    int hat_mask{};
    int input_least{};
    int input_most{};
    int output_least{};
    int output_most{};
};

/// The stand-in pad. Closing the check's joystick and detaching it are its
/// own, including when a check fails.
struct PadTouchStandIn {
    SDL_JoystickID id{};
    SDL_Joystick* joystick{};
    std::array<PadTouchBinding, SDL_GAMEPAD_BUTTON_COUNT> buttons{};
    std::array<PadTouchBinding, SDL_GAMEPAD_AXIS_COUNT> axes{};

    PadTouchStandIn() = default;
    PadTouchStandIn(const PadTouchStandIn&) = delete;
    PadTouchStandIn& operator=(const PadTouchStandIn&) = delete;

    ~PadTouchStandIn() {
        if (joystick != nullptr)
            SDL_CloseJoystick(joystick);
        joystick = nullptr;
        if (id != 0)
            SDL_DetachVirtualJoystick(id);
        id = 0;
    }
};

/// Attaches the stand-in and reads the joystick inputs its buttons and axes
/// are bound to.
///
/// @param stand_in the stand-in
/// @throws std::runtime_error when SDL refuses it
void attach_pad_touch(PadTouchStandIn& stand_in) {
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("photo pad-touch failed: " + what);
    };
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = kPadTouchVendor;
    desc.product_id = kPadTouchProduct;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis)
        desc.axis_mask |= 1U << static_cast<unsigned>(axis);
    for (const SDL_GamepadButton button : kPadTouchButtons)
        desc.button_mask |= 1U << static_cast<unsigned>(button);
    desc.nbuttons = static_cast<Uint16>(kPadTouchButtons.size());
    desc.name = "photo pad";
    stand_in.id = SDL_AttachVirtualJoystick(&desc);
    if (stand_in.id == 0)
        fail(std::string("SDL refused the stand-in pad: ") + SDL_GetError());
    stand_in.joystick = SDL_OpenJoystick(stand_in.id);
    if (stand_in.joystick == nullptr)
        fail(std::string("cannot open the stand-in pad: ") + SDL_GetError());
    SDL_Gamepad* gamepad = SDL_OpenGamepad(stand_in.id);
    if (gamepad == nullptr)
        fail(std::string("SDL does not take the stand-in as a gamepad: ") + SDL_GetError());
    int count = 0;
    SDL_GamepadBinding** bindings = SDL_GetGamepadBindings(gamepad, &count);
    for (int at = 0; bindings != nullptr && at < count; ++at) {
        const SDL_GamepadBinding& binding = *bindings[at];
        PadTouchBinding found{};
        found.type = binding.input_type;
        if (binding.input_type == SDL_GAMEPAD_BINDTYPE_BUTTON) {
            found.index = binding.input.button;
        } else if (binding.input_type == SDL_GAMEPAD_BINDTYPE_HAT) {
            found.index = binding.input.hat.hat;
            found.hat_mask = binding.input.hat.hat_mask;
        } else if (binding.input_type == SDL_GAMEPAD_BINDTYPE_AXIS) {
            found.index = binding.input.axis.axis;
            found.input_least = binding.input.axis.axis_min;
            found.input_most = binding.input.axis.axis_max;
        }
        if (binding.output_type == SDL_GAMEPAD_BINDTYPE_BUTTON && binding.output.button >= 0 &&
            binding.output.button < SDL_GAMEPAD_BUTTON_COUNT) {
            stand_in.buttons[static_cast<std::size_t>(binding.output.button)] = found;
        } else if (
            binding.output_type == SDL_GAMEPAD_BINDTYPE_AXIS && binding.output.axis.axis >= 0 &&
            binding.output.axis.axis < SDL_GAMEPAD_AXIS_COUNT
        ) {
            found.output_least = binding.output.axis.axis_min;
            found.output_most = binding.output.axis.axis_max;
            stand_in.axes[static_cast<std::size_t>(binding.output.axis.axis)] = found;
        }
    }
    SDL_free(bindings);
    SDL_CloseGamepad(gamepad);
    if (stand_in.buttons[SDL_GAMEPAD_BUTTON_SOUTH].type == SDL_GAMEPAD_BINDTYPE_NONE)
        fail("the stand-in has no binding for A");
    if (stand_in.axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER].type != SDL_GAMEPAD_BINDTYPE_AXIS)
        fail("the stand-in has no binding for L2");
    if (stand_in.axes[SDL_GAMEPAD_AXIS_LEFTX].type != SDL_GAMEPAD_BINDTYPE_AXIS ||
        stand_in.axes[SDL_GAMEPAD_AXIS_RIGHTX].type != SDL_GAMEPAD_BINDTYPE_AXIS)
        fail("the stand-in has no stick bindings");
}

/// Sets one button of the stand-in through the joystick input it is bound to.
///
/// @param stand_in the stand-in
/// @param button the gamepad button
/// @param down whether it is down
/// @throws std::runtime_error when it cannot be set
void set_pad_touch_button(PadTouchStandIn& stand_in, SDL_GamepadButton button, bool down) {
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("photo pad-touch failed: " + what);
    };
    if (stand_in.joystick == nullptr)
        fail("the stand-in pad is not attached");
    const PadTouchBinding& binding = stand_in.buttons[static_cast<std::size_t>(button)];
    bool ok = false;
    switch (binding.type) {
    case SDL_GAMEPAD_BINDTYPE_BUTTON:
        ok = SDL_SetJoystickVirtualButton(stand_in.joystick, binding.index, down);
        break;
    case SDL_GAMEPAD_BINDTYPE_HAT:
        ok = SDL_SetJoystickVirtualHat(
            stand_in.joystick,
            binding.index,
            static_cast<Uint8>(down ? binding.hat_mask : SDL_HAT_CENTERED)
        );
        break;
    case SDL_GAMEPAD_BINDTYPE_AXIS:
        ok = SDL_SetJoystickVirtualAxis(
            stand_in.joystick,
            binding.index,
            static_cast<Sint16>(down ? binding.input_most : binding.input_least)
        );
        break;
    case SDL_GAMEPAD_BINDTYPE_NONE:
        fail("the stand-in has no binding for a button");
        break;
    default:
        fail("the stand-in binds a button in a way the check cannot press");
        break;
    }
    if (!ok)
        fail(std::string("cannot set the stand-in's button: ") + SDL_GetError());
}

/// Sets one axis of the stand-in, inverting the binding's ranges.
///
/// @param stand_in the stand-in
/// @param axis the gamepad axis
/// @param value -1..1 for a stick (y down), 0..1 for a trigger
/// @throws std::runtime_error when it cannot be set
void set_pad_touch_axis(PadTouchStandIn& stand_in, SDL_GamepadAxis axis, float value) {
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("photo pad-touch failed: " + what);
    };
    if (stand_in.joystick == nullptr)
        fail("the stand-in pad is not attached");
    const PadTouchBinding& binding = stand_in.axes[static_cast<std::size_t>(axis)];
    if (binding.type != SDL_GAMEPAD_BINDTYPE_AXIS)
        fail("the stand-in has no binding for an axis");
    const double wanted = value >= 0.0F ? static_cast<double>(value) * kPadTouchAxisMost
                                        : static_cast<double>(-value) * kPadTouchAxisLeast;
    double part = 0.0;
    if (binding.output_most != binding.output_least)
        part = (wanted - static_cast<double>(binding.output_least)) /
               static_cast<double>(binding.output_most - binding.output_least);
    const double raw = static_cast<double>(binding.input_least) +
                       part * static_cast<double>(binding.input_most - binding.input_least);
    const auto clamped = static_cast<Sint16>(
        std::clamp(static_cast<int>(std::lround(raw)), kPadTouchAxisLeast, kPadTouchAxisMost)
    );
    if (!SDL_SetJoystickVirtualAxis(stand_in.joystick, binding.index, clamped))
        fail(std::string("cannot set the stand-in's axis: ") + SDL_GetError());
}

} // namespace

void Runtime::check_photo_keys() {
    /// Puts the preferences file back to the bytes the check started with.
    struct PreferencesHold {
        Runtime& runtime;
        bool existed{false};
        std::vector<char> bytes;

        explicit PreferencesHold(Runtime& host) : runtime(host) {
            const auto& path = host.options_.preferences_file;
            if (path.has_value())
                existed = fs::exists(*path);
            bytes = file_bytes(path);
        }

        PreferencesHold(const PreferencesHold&) = delete;
        PreferencesHold& operator=(const PreferencesHold&) = delete;

        ~PreferencesHold() {
            const auto& path = runtime.options_.preferences_file;
            if (path.has_value()) {
                if (!existed) {
                    std::error_code error;
                    fs::remove(*path, error);
                } else {
                    std::ofstream out(*path, std::ios::binary | std::ios::trunc);
                    if (out)
                        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                }
            }
            runtime.preferences_dirty_ = false;
        }
    };

    const PreferencesHold preferences_hold(*this);
    const ClipboardHookHold clipboard_hold;
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("photo keys failed: " + what);
    };
    const auto require = [&](bool ok, const std::string& what) {
        if (!ok)
            fail(what);
    };
    require(sdl_.window != nullptr, "the check has no window");
    require(
        options_.user_folder && !options_.user_folder->empty() && !user_folder_.empty(),
        "the run has no folder of the player's own (--user-folder)"
    );

    const auto read_orders = [this] {
        std::vector<UnitOrders> orders;
        const oa::World& world = match_->state();
        for (uint32_t slot = 0; slot < world.unit_slot_count; ++slot) {
            const oa::Unit* unit = oa::world_unit_at(&world, slot);
            if (unit == nullptr || unit->type_index == 0)
                continue;
            orders.push_back(
                {unit->id,
                 unit->type_index,
                 unit->flags,
                 static_cast<uint32_t>(unit->primary_order),
                 static_cast<uint32_t>(unit->secondary_order)}
            );
        }
        return orders;
    };

    struct View {
        int32_t camera_x{};
        int32_t camera_z{};
        float zoom{};
        float zoom_target{};
        uint16_t selected{};
    };

    const auto read_view = [this] {
        return View{
            match_camera_x_, match_camera_z_, match_zoom_, match_zoom_target_, selected_match_unit_
        };
    };
    const auto same_view = [](const View& left, const View& right) {
        return left.camera_x == right.camera_x && left.camera_z == right.camera_z &&
               left.zoom == right.zoom && left.zoom_target == right.zoom_target &&
               left.selected == right.selected;
    };

    struct Shots {
        int png{};
        int pcx{};
    };

    const auto count_shots = [](const fs::path& root) {
        Shots shots;
        std::error_code error;
        if (!fs::exists(root, error))
            return shots;
        const fs::recursive_directory_iterator end;
        for (fs::recursive_directory_iterator it(
                 root, fs::directory_options::skip_permission_denied, error
             );
             !error && it != end;
             it.increment(error)) {
            if (error || !it->is_regular_file(error))
                continue;
            const fs::path ext = it->path().extension();
            if (ext == ".png" || ext == ".PNG")
                ++shots.png;
            else if (ext == ".pcx" || ext == ".PCX")
                ++shots.pcx;
        }
        return shots;
    };
    bool running = true;
    const auto dispatch = [&](SDL_Event& event) {
        dispatch_event(event, running);
        require(running, "a key ended the run");
    };
    const auto tap = [&](SDL_Scancode scancode, SDL_Keymod mod = SDL_KMOD_NONE) {
        SDL_Event event{};
        event.key.windowID = SDL_GetWindowID(sdl_.window);
        event.key.scancode = scancode;
        // The unshifted character, so Shift+. stays the letter "." the chord names.
        event.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, true);
        event.key.mod = mod;
        for (const bool down : {true, false}) {
            event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.down = down;
            event.key.repeat = false;
            dispatch(event);
        }
    };
    const auto window_point = [&](float frame_x, float frame_y) {
        float window_x = 0;
        float window_y = 0;
        require(
            frame_to_window(sdl_.renderer, frame_x, frame_y, &window_x, &window_y),
            "the point is not on the window"
        );
        return std::pair<float, float>{window_x, window_y};
    };
    const auto click_left = [&](float frame_x, float frame_y) {
        const auto [window_x, window_y] = window_point(frame_x, frame_y);
        SDL_Event event{};
        event.motion.windowID = SDL_GetWindowID(sdl_.window);
        event.button.windowID = event.motion.windowID;
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.x = window_x;
        event.motion.y = window_y;
        dispatch(event);
        for (const bool down : {true, false}) {
            event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            event.button.button = SDL_BUTTON_LEFT;
            event.button.down = down;
            event.button.clicks = 1;
            event.button.x = window_x;
            event.button.y = window_y;
            dispatch(event);
        }
    };
    // A photo asked for before a photo-mode frame waits until one is presented.
    const auto show_frame = [&] {
        render_match_surface();
        present_match_layers();
    };
    // The match fills this window, as a player sizes it. The play battlefield
    // is the part beside the side column; a photo is the whole window.
    const auto size_window = [&](int width, int height) {
        require(
            sdl_.window != nullptr && SDL_SetWindowSize(sdl_.window, width, height) &&
                SDL_SyncWindow(sdl_.window),
            std::string("the window did not resize: ") + SDL_GetError()
        );
        SDL_Event resized{};
        resized.type = SDL_EVENT_WINDOW_RESIZED;
        resized.window.windowID = SDL_GetWindowID(sdl_.window);
        resized.window.data1 = width;
        resized.window.data2 = height;
        dispatch(resized);
    };
    const auto overlay_words = [&] {
        namespace editor = oa::ui::key_editor;
        const LayerScreen* screen = oa_layer().find(editor::name_keys_overlay);
        require(screen != nullptr, "the keys overlay is not open");
        const oa::ui::kit::DisplayList list = screen->display_list();
        for (const oa::ui::kit::Control& control : list.controls) {
            if (control.name == editor::name_keys_overlay_list)
                return control.text;
        }
        fail("the keys overlay lists nothing");
        return std::string{};
    };
    const auto photo_file = [&] {
        const std::string saved = saved_photo_name(status_);
        require(!saved.empty(), "status is " + status_);
        return photo_shots_folder() / oa::platform::path_from_utf8(saved);
    };
    const auto window_photo = [&](const DecodedPng& image, uint32_t play_w, uint32_t play_h) {
        int window_w = 0;
        int window_h = 0;
        require(
            sdl_.window != nullptr && SDL_GetWindowSize(sdl_.window, &window_w, &window_h),
            "the window's size could not be read"
        );
        int output_w = 0;
        int output_h = 0;
        if (sdl_.renderer != nullptr)
            std::ignore = SDL_GetRenderOutputSize(sdl_.renderer, &output_w, &output_h);
        const bool window_sized = (image.width == static_cast<uint32_t>(window_w) &&
                                   image.height == static_cast<uint32_t>(window_h)) ||
                                  (output_w > 0 && image.width == static_cast<uint32_t>(output_w) &&
                                   image.height == static_cast<uint32_t>(output_h));
        require(
            window_sized,
            "photo " + std::to_string(image.width) + "x" + std::to_string(image.height) +
                " window " + std::to_string(window_w) + "x" + std::to_string(window_h) +
                " output " + std::to_string(output_w) + "x" + std::to_string(output_h) + " play " +
                std::to_string(play_w) + "x" + std::to_string(play_h)
        );
        require(
            image.width != play_w || image.height != play_h,
            "the photo is the play battlefield " + std::to_string(play_w) + "x" +
                std::to_string(play_h)
        );
    };

    const fs::path own_file = user_folder_ / std::string(library::keys_folder_name) /
                              library::own_folder_name("oa-default") /
                              std::string(oakeys::manifest_file);
    std::error_code remove_error;
    fs::remove(own_file, remove_error);
    reread_key_bindings();

    start_benchmark_skirmish();
    match_layout_ = lay_out_match(options_.match_width, options_.match_height);
    spawn_combat_armies(kArmy);
    render_match_surface();
    const std::vector<char> preferences_settled = file_bytes(options_.preferences_file);
    const fs::path pictures = user_folder_;

    // Outside photo mode Ctrl+F9 is the screenshot.
    const Shots before_shot = count_shots(pictures);
    tap(SDL_SCANCODE_F9, SDL_KMOD_LCTRL);
    const Shots after_shot = count_shots(pictures);
    require(after_shot.pcx == before_shot.pcx + 1, "Ctrl+F9 wrote no screenshot");
    require(after_shot.png == before_shot.png, "Ctrl+F9 wrote a photo outside photo mode");
    require(!photo_mode(), "Ctrl+F9 entered photo mode");

    // F9 and Enter in one input batch, before any photo-mode frame. Off.
    // 1280x720 is the window the review measured: the play battlefield is
    // smaller, and the photo is the window.
    size_window(1280, 720);
    const uint32_t play_w = match_world_cpu_.width;
    const uint32_t play_h = match_world_cpu_.height;
    const Shots before_same = count_shots(pictures);
    tap(SDL_SCANCODE_F9);
    tap(SDL_SCANCODE_RETURN);
    require(photo_mode() && photo_frozen(), "F9 and Enter did not open photo mode frozen");
    require(
        count_shots(pictures).png == before_same.png,
        "a photo was written before a photo-mode frame"
    );
    show_frame();
    wait_for_photos();
    require(count_shots(pictures).png == before_same.png + 1, "entering wrote no photo");
    const DecodedPng entered = decode_png(photo_file());
    window_photo(entered, play_w, play_h);
    show_frame();
    take_photo();
    wait_for_photos();
    const DecodedPng later = decode_png(photo_file());
    require(entered.rows == later.rows, "a photo on entering differs from the next frozen frame");

    tap(SDL_SCANCODE_SLASH, kPrimaryModifier);
    show_frame();
    const std::string listed = overlay_words();
    require(listed.find("Keys in photo mode") != std::string::npos, listed);
    require(listed.find("Take photo") != std::string::npos, listed);
    require(listed.find("\nAccept ") == std::string::npos, listed);
    require(listed.find("\nPress ") == std::string::npos, listed);
    require(listed.find("\nPage up ") == std::string::npos, listed);
    require(listed.find("\nPage down ") == std::string::npos, listed);
    // Only keys that answer in photo mode: the camera, the speed, Escape.
    require(listed.find("\nPan up ") != std::string::npos, listed);
    require(listed.find("\nMark area ") != std::string::npos, listed);
    require(listed.find("\nSpeed up ") != std::string::npos, listed);
    require(listed.find("\nBack ") != std::string::npos, listed);
    require(listed.find("\nOpen chat ") == std::string::npos, listed);
    require(listed.find("\nStatus strip ") == std::string::npos, listed);
    require(listed.find("\nScreenshot ") == std::string::npos, listed);
    require(listed.find("\nUnit info ") == std::string::npos, listed);
    const Shots before_open = count_shots(pictures);
    take_photo();
    wait_for_photos();
    require(count_shots(pictures).png == before_open.png + 1, "the open panel wrote no photo");
    const DecodedPng open_panel = decode_png(photo_file());
    tap(SDL_SCANCODE_SLASH, kPrimaryModifier);
    require(
        oa_layer().find(oa::ui::key_editor::name_keys_overlay) == nullptr,
        "Primary+/ did not close the keys overlay"
    );
    take_photo();
    wait_for_photos();
    const DecodedPng closed_panel = decode_png(photo_file());
    require(open_panel.rows == closed_panel.rows, "the keys overlay is in the photo");
    tap(SDL_SCANCODE_F9);
    require(!photo_mode(), "F9 did not leave after the keys overlay");
    require(
        oa_layer().find(oa::ui::key_editor::name_keys_overlay) == nullptr,
        "leaving kept a keys overlay opened in photo mode"
    );

    tap(SDL_SCANCODE_F9);
    require(photo_mode() && photo_frozen(), "F9 did not open photo mode frozen");
    tap(SDL_SCANCODE_SPACE);
    require(
        photo_mode() && !photo_frozen() && photo_slow_speed() == 0,
        "Space did not let the frozen clock run"
    );
    tap(SDL_SCANCODE_MINUS);
    require(photo_slow_speed() == 9, "speed down at the match's speed did not start Slow 9");
    tap(SDL_SCANCODE_EQUALS);
    require(
        photo_slow_speed() == 0 && !photo_frozen(),
        "speed up from Slow 9 did not run at the match's speed"
    );
    tap(SDL_SCANCODE_EQUALS);
    require(
        photo_slow_speed() == 0 && !photo_frozen(), "speed up at the match's speed slowed the clock"
    );
    tap(SDL_SCANCODE_SPACE);
    require(photo_frozen(), "Space did not freeze the running clock");

    const uint32_t tick_before = match_->state().game.tick;
    tap(SDL_SCANCODE_PERIOD);
    require(match_->state().game.tick == tick_before + 1, "the step did not advance one tick");
    require(photo_frozen() && photo_shutter_eighth() == 8, "the step did not freeze on the tick");
    tap(SDL_SCANCODE_COMMA, SDL_KMOD_LSHIFT);
    require(photo_shutter_eighth() == 7, "Shift+, did not move the shutter back");
    tap(SDL_SCANCODE_PERIOD, SDL_KMOD_LSHIFT);
    require(photo_shutter_eighth() == 8, "Shift+. did not move the shutter on");

    tap(SDL_SCANCODE_EQUALS);
    require(photo_slow_speed() == 1 && !photo_frozen(), "speed up did not start slow motion");
    tap(SDL_SCANCODE_SPACE);
    require(photo_frozen() && photo_slow_speed() == 0, "Space did not freeze slow motion");

    const float zoom_before = match_zoom_target_;
    tap(SDL_SCANCODE_PAGEUP);
    require(match_zoom_target_ > zoom_before, "PageUp did not zoom in");
    require(photo_mode(), "PageUp left photo mode");

    const View turned = read_view();
    tap(SDL_SCANCODE_LEFT, SDL_KMOD_LALT);
    require(same_view(turned, read_view()), "Alt+Left moved the camera");
    const std::string turn_reason =
        std::string(oa::data::languages::interface_text("Needs Full acceleration"));
    require(status_.find(turn_reason) != std::string::npos, "Alt+Left gave no reason");
    require(photo_mode(), "Alt+Left left photo mode");

    // M starts a mark the size of the view. Right grows it and does not pan.
    // A click that does not move keeps that mark.
    const int32_t mark_camera_x = match_camera_x_;
    const int32_t mark_camera_z = match_camera_z_;
    tap(SDL_SCANCODE_M);
    require(photo_ != nullptr && photo_->poster, "M did not choose a poster");
    require(
        photo_->poster_extent == static_cast<uint8_t>(oa::present::photo::PosterExtent::marked),
        "M did not choose Area"
    );
    require(
        photo_->poster_mark_w >= 64 && photo_->poster_mark_h >= 64,
        "the mark is under 64 map pixels"
    );
    const int32_t marked_w = photo_->poster_mark_w;
    const int32_t marked_h = photo_->poster_mark_h;
    require(
        match_camera_x_ == mark_camera_x && match_camera_z_ == mark_camera_z, "M moved the camera"
    );
    tap(SDL_SCANCODE_RIGHT);
    require(photo_->poster_mark_w > marked_w, "Right did not grow the mark");
    require(photo_->poster_mark_h == marked_h, "Right changed the mark's height");
    require(match_camera_x_ == mark_camera_x && match_camera_z_ == mark_camera_z, "Right panned");
    const int32_t grown_w = photo_->poster_mark_w;
    const int32_t grown_x = photo_->poster_mark_x;

    const std::vector<UnitOrders> orders = read_orders();
    const View quiet = read_view();
    const int offers_before_match = clipboard_catch.offers;
    tap(SDL_SCANCODE_S);
    click_left(24.0F, 24.0F);
    // Ctrl+C on a Mac, where Primary is Command. Elsewhere Ctrl+C is
    // Primary+C, which copies (below), so the other modifier is pressed.
    tap(SDL_SCANCODE_C, kOtherModifier);
    require(read_orders() == orders, "a match key changed a unit");
    require(same_view(quiet, read_view()), "a match key moved the view");
    require(!chat_composing_, "a match key opened the chat line");
    require(
        clipboard_catch.offers == offers_before_match,
        "C with the modifier other than Primary copied a photo"
    );
    require(photo_mode(), "a match key left photo mode");
    require(
        photo_->poster_mark_w == grown_w && photo_->poster_mark_x == grown_x,
        "a click without a drag replaced the mark"
    );
    set_photo_poster(false);

    const Shots before_photo = count_shots(pictures);
    tap(SDL_SCANCODE_RETURN);
    show_frame();
    wait_for_photos();
    const Shots after_photo = count_shots(pictures);
    require(after_photo.png == before_photo.png + 1, "Enter wrote no photo");
    require(after_photo.pcx == before_photo.pcx, "Enter wrote a screenshot");
    require(photo_mode() && !chat_composing_, "Enter opened the chat line");
    const int offered = clipboard_catch.offers;
    tap(SDL_SCANCODE_C, kPrimaryModifier);
    require(clipboard_catch.offers == offered + 1, "Primary+C did not offer the photo");
    require(photo_mode(), "Primary+C left photo mode");

    // Ctrl+F9 is still Take photo on the default layout. The scratch layout
    // below keeps only K, so this press has to happen before that layout.
    const Shots before_inside = count_shots(pictures);
    tap(SDL_SCANCODE_F9, SDL_KMOD_LCTRL);
    wait_for_photos();
    const Shots after_inside = count_shots(pictures);
    require(after_inside.png == before_inside.png + 1, "Ctrl+F9 wrote no photo");
    require(after_inside.pcx == before_inside.pcx, "Ctrl+F9 wrote a screenshot in photo mode");
    require(photo_mode(), "Ctrl+F9 left photo mode");

    tap(SDL_SCANCODE_F9);
    require(!photo_mode(), "F9 did not leave photo mode");
    tap(SDL_SCANCODE_F9);
    require(photo_mode(), "F9 did not enter again");
    tap(SDL_SCANCODE_ESCAPE);
    require(!photo_mode(), "Escape did not leave photo mode");

    oakeys::Manifest manifest;
    manifest.format = oakeys::format_version;
    manifest.id = "own-oa-default";
    manifest.name = "Your changes";
    manifest.version.major = 1;
    manifest.version.minor = 0;
    manifest.version.parts = 2;
    manifest.author.name = "You";
    manifest.tags = {"own"};
    manifest.base = oakeys::Base::oa_default;
    oakeys::BindingEntry binding;
    binding.action = "photo.capture";
    binding.chords.push_back(oakeys::ChordText{"K", 0, 0});
    manifest.bindings.push_back(std::move(binding));
    std::error_code made;
    fs::create_directories(own_file.parent_path(), made);
    require(!made, "the own layout folder was not made");
    std::ofstream out(own_file, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(out), "the own layout was not opened");
    out << oakeys::write_manifest(manifest);
    require(static_cast<bool>(out), "the own layout was not written");
    out.close();
    reread_key_bindings();

    tap(SDL_SCANCODE_F9);
    require(photo_mode(), "F9 did not enter on the own layout");
    const Shots before_k = count_shots(pictures);
    tap(SDL_SCANCODE_K);
    show_frame();
    wait_for_photos();
    const Shots after_k = count_shots(pictures);
    require(after_k.png == before_k.png + 1, "K wrote no photo");
    tap(SDL_SCANCODE_RETURN);
    wait_for_photos();
    require(count_shots(pictures).png == after_k.png, "Enter still took a photo");
    require(photo_mode() && !chat_composing_, "Enter opened the chat line");
    tap(SDL_SCANCODE_ESCAPE);
    require(!photo_mode(), "Escape did not leave the own layout");

    require(
        file_bytes(options_.preferences_file) == preferences_settled, "the preferences file changed"
    );
    if (photo_mode())
        leave_photo_mode();
    if (match_ != nullptr) {
        leave_match();
        load(Screen::main_menu);
    }
    std::cout << "photo keys: ok" << std::endl;
}

void Runtime::check_photo_panel() {
    struct PreferencesHold {
        Runtime& runtime;
        bool existed{false};
        std::vector<char> bytes;

        explicit PreferencesHold(Runtime& host) : runtime(host) {
            const auto& path = host.options_.preferences_file;
            if (path.has_value())
                existed = fs::exists(*path);
            bytes = file_bytes(path);
        }

        PreferencesHold(const PreferencesHold&) = delete;
        PreferencesHold& operator=(const PreferencesHold&) = delete;

        ~PreferencesHold() {
            const auto& path = runtime.options_.preferences_file;
            if (path.has_value()) {
                if (!existed) {
                    std::error_code error;
                    fs::remove(*path, error);
                } else {
                    std::ofstream out(*path, std::ios::binary | std::ios::trunc);
                    if (out)
                        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                }
            }
            runtime.preferences_dirty_ = false;
        }
    };

    const PreferencesHold preferences_hold(*this);
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("photo panel failed: " + what);
    };
    const auto require = [&](bool ok, const std::string& what) {
        if (!ok)
            fail(what);
    };
    require(sdl_.window != nullptr, "the check has no window");
    require(
        options_.user_folder && !options_.user_folder->empty() && !user_folder_.empty(),
        "the run has no folder of the player's own (--user-folder)"
    );

    bool running = true;
    const auto dispatch = [&](SDL_Event& event) {
        dispatch_event(event, running);
        require(running, "a key ended the run");
    };
    const auto tap = [&](SDL_Scancode scancode) {
        SDL_Event event{};
        event.key.windowID = SDL_GetWindowID(sdl_.window);
        event.key.scancode = scancode;
        event.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, true);
        event.key.mod = SDL_KMOD_NONE;
        for (const bool down : {true, false}) {
            event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.down = down;
            event.key.repeat = false;
            dispatch(event);
        }
    };
    const auto window_point = [&](float frame_x, float frame_y) {
        float window_x = 0;
        float window_y = 0;
        require(
            frame_to_window(sdl_.renderer, frame_x, frame_y, &window_x, &window_y),
            "the point is not on the window"
        );
        return std::pair<float, float>{window_x, window_y};
    };
    const auto click_at = [&](float frame_x, float frame_y) {
        const auto [window_x, window_y] = window_point(frame_x, frame_y);
        SDL_Event event{};
        event.motion.windowID = SDL_GetWindowID(sdl_.window);
        event.button.windowID = event.motion.windowID;
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.x = window_x;
        event.motion.y = window_y;
        dispatch(event);
        for (const bool down : {true, false}) {
            event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            event.button.button = SDL_BUTTON_LEFT;
            event.button.down = down;
            event.button.clicks = 1;
            event.button.x = window_x;
            event.button.y = window_y;
            dispatch(event);
        }
    };
    const auto drag_at = [&](float from_x, float from_y, float to_x, float to_y) {
        const auto [start_x, start_y] = window_point(from_x, from_y);
        const auto [end_x, end_y] = window_point(to_x, to_y);
        SDL_Event event{};
        event.motion.windowID = SDL_GetWindowID(sdl_.window);
        event.button.windowID = event.motion.windowID;
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.x = start_x;
        event.motion.y = start_y;
        dispatch(event);
        event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = true;
        event.button.clicks = 1;
        event.button.x = start_x;
        event.button.y = start_y;
        dispatch(event);
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.x = end_x;
        event.motion.y = end_y;
        dispatch(event);
        event.type = SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.down = false;
        event.button.x = end_x;
        event.button.y = end_y;
        dispatch(event);
    };
    const auto show_frame = [&] {
        render_match_surface();
        present_match_layers();
    };
    const auto size_window = [&](int width, int height) {
        require(
            sdl_.window != nullptr && SDL_SetWindowSize(sdl_.window, width, height) &&
                SDL_SyncWindow(sdl_.window),
            std::string("the window did not resize: ") + SDL_GetError()
        );
        SDL_Event resized{};
        resized.type = SDL_EVENT_WINDOW_RESIZED;
        resized.window.windowID = SDL_GetWindowID(sdl_.window);
        resized.window.data1 = width;
        resized.window.data2 = height;
        dispatch(resized);
    };
    const auto screen = [&]() -> LayerScreen* {
        LayerScreen* found = oa_layer().find("photo-mode");
        require(found != nullptr, "the photo panel is not open");
        return found;
    };
    const auto control_at = [&](std::string_view name) {
        const oa::ui::kit::DisplayList list = screen()->display_list();
        for (const oa::ui::kit::Control& control : list.controls) {
            if (control.name == name) {
                return std::pair<float, float>{
                    static_cast<float>(control.rect.x) +
                        static_cast<float>(control.rect.width) / 2.0F,
                    static_cast<float>(control.rect.y) +
                        static_cast<float>(control.rect.height) / 2.0F
                };
            }
        }
        fail(std::string(name) + " is not on the panel");
        return std::pair<float, float>{};
    };
    const auto has_name = [&](std::string_view name) {
        const oa::ui::kit::DisplayList list = screen()->display_list();
        for (const oa::ui::kit::Control& control : list.controls)
            if (control.name == name)
                return true;
        return false;
    };
    const auto focused_name = [&] {
        const LayerScreen* open = screen();
        const oa::ui::kit::Interaction focus = open->interaction();
        const oa::ui::kit::DisplayList list = open->display_list();
        const oa::ui::kit::Control* control = oa::ui::kit::control_of(list, focus.focused);
        require(control != nullptr, "the panel has no focused control");
        return control->name;
    };
    const auto ring_shown = [&] {
        const LayerScreen* open = screen();
        if (!open->interaction().focus_shown)
            return false;
        const oa::ui::kit::DisplayList list = open->display_list();
        for (const oa::ui::kit::Item& item : list.items)
            if (item.role == oa::ui::kit::Role::focus_ring)
                return true;
        return false;
    };
    const auto count_png = [&] {
        int png = 0;
        std::error_code error;
        if (!fs::exists(user_folder_, error))
            return png;
        const fs::recursive_directory_iterator end;
        for (fs::recursive_directory_iterator it(
                 user_folder_, fs::directory_options::skip_permission_denied, error
             );
             !error && it != end;
             it.increment(error)) {
            if (error || !it->is_regular_file(error))
                continue;
            const fs::path ext = it->path().extension();
            if (ext == ".png" || ext == ".PNG")
                ++png;
        }
        return png;
    };
    const auto latest_photo = [&] {
        const std::string saved = saved_photo_name(status_);
        require(!saved.empty(), "status is " + status_);
        return decode_png(photo_shots_folder() / oa::platform::path_from_utf8(saved));
    };
    const auto take = [&] {
        const auto [x, y] = control_at("photo.take");
        click_at(x, y);
        show_frame();
        wait_for_photos();
        return latest_photo();
    };
    const auto pixel = [](const DecodedPng& image, uint32_t x, uint32_t y) {
        const std::size_t at = (static_cast<std::size_t>(y) * image.width + x) * 3U;
        return std::array<uint8_t, 3>{image.rows[at], image.rows[at + 1U], image.rows[at + 2U]};
    };
    const auto shows_status = [](const oa::Game& game) {
        if (game.text_lines <= 0)
            return false;
        if (game.message_filter == 1 || game.message_filter == 2)
            return true;
        return game.message_filter == 3 && game.screen_chat != 0;
    };

    start_benchmark_skirmish();
    match_layout_ = lay_out_match(options_.match_width, options_.match_height);
    spawn_combat_armies(kArmy);
    render_match_surface();
    const std::vector<char> preferences_settled = file_bytes(options_.preferences_file);

    const int before_miss = count_png();
    tap(SDL_SCANCODE_F9);
    tap(SDL_SCANCODE_RETURN);
    require(photo_mode(), "F9 did not open photo mode");
    tap(SDL_SCANCODE_F9);
    require(!photo_mode(), "leaving before a frame stayed in photo mode");
    require(match_ != nullptr, "leaving a waiting photo left no match");
    require(shows_status(match_->state().game), "the message log would not draw Photo not taken");
    bool said = false;
    for (const std::string& line : match_message_lines()) {
        if (line.find("Photo not taken") != std::string::npos)
            said = true;
    }
    require(said, "the message log did not say Photo not taken");
    require(count_png() == before_miss, "a waiting photo was written");

    const char* names[] = {
        "photo.leave",
        "photo.section.camera",
        "photo.zoom",
        "photo.turn",
        "photo.tilt",
        "photo.classic",
        "photo.follow",
        "photo.section.time",
        "photo.time-mode",
        "photo.speed",
        "photo.step",
        "photo.between",
        "photo.section.look",
        "photo.selection",
        "photo.health",
        "photo.guides",
        "photo.section.capture",
        "photo.size",
        "photo.poster",
        "photo.smooth",
        "photo.copy-to-clipboard",
        "photo.player-names",
        "photo.take",
        "photo.copy",
    };
    const int widths[] = {640, 1280, 1920};
    const int heights[] = {480, 720, 1080};
    for (int size = 0; size < 3; ++size) {
        if (photo_mode())
            leave_photo_mode();
        size_window(widths[size], heights[size]);
        const std::string where =
            std::to_string(widths[size]) + "x" + std::to_string(heights[size]);
        tap(SDL_SCANCODE_F9);
        show_frame();
        require(photo_mode() && photo_frozen(), where + " did not open frozen");
        require(!ring_shown(), where + " drew a focus ring before Tab");
        require(focused_name() == "photo.leave", where + " did not open on Leave");
        std::vector<std::string> seen;
        seen.push_back(focused_name());
        tap(SDL_SCANCODE_TAB);
        require(ring_shown(), where + " did not show the ring on the first Tab");
        seen.push_back(focused_name());
        for (int step = 0; step < 40; ++step) {
            tap(SDL_SCANCODE_TAB);
            seen.push_back(focused_name());
        }
        for (const char* name : names) {
            const bool found = std::find(seen.begin(), seen.end(), name) != seen.end();
            require(found, where + " Tab missed " + name);
        }
        const oa::ui::kit::DisplayList listed = screen()->display_list();
        for (const oa::ui::kit::Control& control : listed.controls) {
            require(!control.name.empty(), where + " has a control with no name");
            const bool readout = control.name == "photo.clock" || control.name == "photo.file";
            if (readout) {
                require(
                    control.kind == oa::ui::kit::ControlKind::area && !control.focusable,
                    where + " " + control.name + " is still a button"
                );
                require(
                    std::find(seen.begin(), seen.end(), control.name) == seen.end(),
                    where + " Tab stopped on " + control.name
                );
            }
            if (control.name.rfind("photo.section.", 0) == 0)
                require(control.expanded, where + " " + control.name + " is not expanded");
        }
        bool freeze_lit = false;
        const std::vector<oa::ui::kit::AutomationEntry> parts =
            oa::ui::kit::automation_parts(listed, screen()->interaction());
        for (const oa::ui::kit::AutomationEntry& entry : parts) {
            if (entry.name == "photo.time-mode.freeze" && entry.chosen)
                freeze_lit = true;
        }
        require(freeze_lit, where + " Freeze is not the lit mode");

        const int32_t camera_before = match_camera_x_;
        const uint16_t selected_before = selected_match_unit_;
        const float from_x = static_cast<float>(widths[size]) / 4.0F;
        const float from_y = static_cast<float>(heights[size]) / 2.0F;
        drag_at(from_x, from_y, from_x - 80.0F, from_y);
        require(match_camera_x_ != camera_before, where + " a battlefield drag did not pan");
        require(selected_match_unit_ == selected_before, where + " a battlefield drag selected");

        tap(SDL_SCANCODE_H);
        require(has_name("photo.hint"), where + " H did not leave the hint");
        require(!has_name("photo.take"), where + " H left Take photo showing");
        renderer::Surface clean;
        {
            const CaptureHold hold(capture_frame_, clean);
            show_frame();
        }
        tap(SDL_SCANCODE_H);
        require(has_name("photo.take"), where + " H did not show the panel");
        require(!has_name("photo.hint"), where + " H left the hint showing");
        const DecodedPng plain = take();
        require(
            plain.width == clean.width && plain.height == clean.height,
            where + " photo " + std::to_string(plain.width) + "x" + std::to_string(plain.height) +
                " presented " + std::to_string(clean.width) + "x" + std::to_string(clean.height)
        );
        std::size_t compared = 0;
        for (uint32_t y = 0; y < plain.height; ++y) {
            for (uint32_t x = 0; x < plain.width; ++x) {
                if (oa::ui::photo_panel::covers_chrome(
                        static_cast<int>(x),
                        static_cast<int>(y),
                        static_cast<int>(plain.width),
                        static_cast<int>(plain.height)
                    ))
                    continue;
                const auto photo_pixel = pixel(plain, x, y);
                const std::size_t at = (static_cast<std::size_t>(y) * clean.width + x) * 3U;
                require(
                    photo_pixel[0] == clean.rgb[at] && photo_pixel[1] == clean.rgb[at + 1U] &&
                        photo_pixel[2] == clean.rgb[at + 2U],
                    where + " the photo differs from the battlefield at " + std::to_string(x) +
                        "," + std::to_string(y)
                );
                ++compared;
            }
        }
        require(compared > 0, where + " the chrome covers the photo");

        tap(SDL_SCANCODE_G);
        const DecodedPng guided = take();
        require(guided.rows == plain.rows, where + " a guide was in the photo");
        for (int more = 0; more < 6; ++more)
            tap(SDL_SCANCODE_G);
        const DecodedPng square = take();
        require(
            square.width == square.height && square.width > 0, where + " a 1:1 crop is not square"
        );
        const uint32_t crop_x = (plain.width - square.width) / 2U;
        const uint32_t crop_y = (plain.height - square.height) / 2U;
        const uint32_t mid_x = square.width / 2U;
        const uint32_t mid_y = square.height / 2U;
        require(
            pixel(square, mid_x, mid_y) == pixel(plain, crop_x + mid_x, crop_y + mid_y),
            where + " the cropped centre is not the battlefield's centre"
        );
    }

    require(
        file_bytes(options_.preferences_file) == preferences_settled, "the preferences file changed"
    );
    if (photo_mode())
        leave_photo_mode();
    if (match_ != nullptr) {
        leave_match();
        load(Screen::main_menu);
    }
    std::cout << "photo panel: ok" << std::endl;
}

void Runtime::check_photo_pad_touch() {
    /// Puts the preferences file back. The fight may write its own rows; the
    /// check compares the file after that, and this puts the original back.
    struct PreferencesHold {
        Runtime& runtime;
        bool existed{false};
        std::vector<char> bytes;

        explicit PreferencesHold(Runtime& host) : runtime(host) {
            const auto& path = host.options_.preferences_file;
            if (path.has_value())
                existed = fs::exists(*path);
            bytes = file_bytes(path);
        }

        PreferencesHold(const PreferencesHold&) = delete;
        PreferencesHold& operator=(const PreferencesHold&) = delete;

        ~PreferencesHold() {
            const auto& path = runtime.options_.preferences_file;
            if (path.has_value()) {
                if (!existed) {
                    std::error_code error;
                    fs::remove(*path, error);
                } else {
                    std::ofstream out(*path, std::ios::binary | std::ios::trunc);
                    if (out)
                        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                }
            }
            runtime.preferences_dirty_ = false;
        }
    };

    /// Opens only the check's virtual pad, then puts that choice back.
    struct PadsOnly {
        Runtime& runtime;
        bool before;

        explicit PadsOnly(Runtime& host)
            : runtime(host), before(host.pad_state().virtual_pads_only) {
            host.pad_state().virtual_pads_only = true;
        }

        PadsOnly(const PadsOnly&) = delete;
        PadsOnly& operator=(const PadsOnly&) = delete;

        ~PadsOnly() { runtime.pad_state().virtual_pads_only = before; }
    };

    const PreferencesHold preferences_hold(*this);
    const PadsOnly pads_only(*this);
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("photo pad-touch failed: " + what);
    };
    const auto require = [&](bool ok, const std::string& what) {
        if (!ok)
            fail(what);
    };
    require(sdl_.window != nullptr, "the check has no window");
    start_benchmark_skirmish();
    const std::vector<char> preferences_settled = file_bytes(options_.preferences_file);

    const auto read_orders = [this] {
        std::vector<UnitOrders> orders;
        const oa::World& world = match_->state();
        for (uint32_t slot = 0; slot < world.unit_slot_count; ++slot) {
            const oa::Unit* unit = oa::world_unit_at(&world, slot);
            if (unit == nullptr || unit->type_index == 0)
                continue;
            orders.push_back(
                {unit->id,
                 unit->type_index,
                 unit->flags,
                 static_cast<uint32_t>(unit->primary_order),
                 static_cast<uint32_t>(unit->secondary_order)}
            );
        }
        return orders;
    };

    uint64_t clock_ns = 1'000'000'000ULL;
    bool running = true;
    const auto step = [&](uint64_t ms) {
        // The pad and the touch controls share this clock. The frame clock
        // stays the check's own, so the fight does not jump.
        clock_ns += ms * 1'000'000ULL;
        pad_state().check_clock_ns = clock_ns;
        touch_state().dispatch.check_clock_ns = clock_ns;
        SDL_PumpEvents();
        SDL_Event event{};
        while (SDL_PollEvent(&event))
            dispatch_event(event, running);
        require(running, "a frame ended the run");
        idle_tick();
    };
    const auto steps = [&](uint64_t total_ms) {
        for (uint64_t elapsed = 0; elapsed < total_ms; elapsed += kPadTouchFrameMs)
            step(std::min(kPadTouchFrameMs, total_ms - elapsed));
    };

    PadTouchStandIn stand_in;
    attach_pad_touch(stand_in);
    for (int frame = 0; frame < kPadTouchAttachFrames; ++frame)
        step(kPadTouchFrameMs);
    const OpenPad* opened = nullptr;
    for (const OpenPad& candidate : pad_state().pads)
        if (candidate.id == stand_in.id)
            opened = &candidate;
    require(opened != nullptr, "the runtime did not open the stand-in pad");
    require(opened->traits.grips, "the stand-in has no grips, so View is not the game layer");

    const auto tap_button = [&](SDL_GamepadButton button) {
        set_pad_touch_button(stand_in, button, true);
        step(kPadTouchFrameMs);
        set_pad_touch_button(stand_in, button, false);
        step(kPadTouchFrameMs);
    };

    // View is held past its delay, so letting go is the end of the hold and
    // not a tap of unit info. L2 then opens photo mode.
    set_pad_touch_button(stand_in, SDL_GAMEPAD_BUTTON_BACK, true);
    steps(kPadTouchHoldMs);
    set_pad_touch_axis(stand_in, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, kPadTouchTrigger);
    step(kPadTouchFrameMs);
    require(photo_mode(), "L2 with View held did not enter photo mode");
    set_pad_touch_axis(stand_in, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0.0F);
    step(kPadTouchFrameMs);
    set_pad_touch_button(stand_in, SDL_GAMEPAD_BUTTON_BACK, false);
    step(kPadTouchFrameMs);
    require(photo_mode(), "letting go of View left photo mode");

    render_match_surface();
    present_match_layers();
    // Entering holds a local fight, so the frames that frame the shot do not
    // move it. An order the sticks gave would show against that.
    require(photo_frozen(), "entering photo mode did not hold the fight");
    const std::vector<UnitOrders> pad_orders = read_orders();
    const std::array<double, 2> place_before = match_view_place();
    set_pad_touch_axis(stand_in, SDL_GAMEPAD_AXIS_LEFTX, kPadTouchStick);
    set_pad_touch_axis(stand_in, SDL_GAMEPAD_AXIS_LEFTY, 0.0F);
    steps(400);
    const std::array<double, 2> place_after = match_view_place();
    require(
        place_after[0] != place_before[0] || place_after[1] != place_before[1],
        "the left stick did not pan"
    );
    set_pad_touch_axis(stand_in, SDL_GAMEPAD_AXIS_LEFTX, 0.0F);
    set_pad_touch_axis(stand_in, SDL_GAMEPAD_AXIS_LEFTY, 0.0F);
    step(kPadTouchFrameMs);

    const float zoom_before = match_zoom();
    const float target_before = match_zoom_target_ > 0.0F ? match_zoom_target_ : match_zoom_;
    set_pad_touch_axis(stand_in, SDL_GAMEPAD_AXIS_RIGHTX, 0.0F);
    set_pad_touch_axis(stand_in, SDL_GAMEPAD_AXIS_RIGHTY, -kPadTouchStick);
    steps(400);
    require(match_zoom_target_ > target_before, "the right stick did not aim a closer zoom");
    require(match_zoom() > zoom_before, "the right stick did not zoom in");
    set_pad_touch_axis(stand_in, SDL_GAMEPAD_AXIS_RIGHTX, 0.0F);
    set_pad_touch_axis(stand_in, SDL_GAMEPAD_AXIS_RIGHTY, 0.0F);
    step(kPadTouchFrameMs);
    require(read_orders() == pad_orders, "a stick gave an order");

    render_match_surface();
    present_match_layers();
    tap_button(SDL_GAMEPAD_BUTTON_SOUTH);
    wait_for_photos();
    const std::string saved = saved_photo_name(status_);
    require(!saved.empty(), "A wrote no photo (" + status_ + ")");
    const fs::path saved_file = photo_shots_folder() / oa::platform::path_from_utf8(saved);
    require(fs::is_regular_file(saved_file), "A named a photo that is not a file");

    // Entering held the fight, so the first X lets it run, as the freeze key
    // does, and the next X holds it again.
    tap_button(SDL_GAMEPAD_BUTTON_WEST);
    require(!photo_frozen(), "X did not let the held fight run");
    tap_button(SDL_GAMEPAD_BUTTON_WEST);
    require(photo_frozen(), "X did not freeze");
    const std::vector<UnitOrders> frozen_orders = read_orders();
    tap_button(SDL_GAMEPAD_BUTTON_NORTH);
    require(!photo_panel_shown(), "Y did not hide the panel");
    require(read_orders() == frozen_orders, "a controller input gave an order");

    // The D-pad walks the panel. A presses a focused button and the ring
    // goes, so the next A takes a photo again; on a slider A takes one.
    const auto focused_name = [&]() -> std::string {
        const LayerScreen* open = oa_layer().find("photo-mode");
        if (open == nullptr)
            return {};
        const oa::ui::kit::Interaction focus = open->interaction();
        if (!oa::ui::kit::focus_ring_visible(focus))
            return {};
        const oa::ui::kit::DisplayList list = open->display_list();
        const oa::ui::kit::Control* control = oa::ui::kit::control_of(list, focus.focused);
        return control != nullptr ? control->name : std::string();
    };
    // Down to the last control, then up: the open panel has some 30 stops.
    const auto pad_reach = [&](std::string_view name) {
        for (int press = 0; press < 80 && focused_name() != name; ++press)
            tap_button(press < 40 ? SDL_GAMEPAD_BUTTON_DPAD_DOWN : SDL_GAMEPAD_BUTTON_DPAD_UP);
        require(focused_name() == name, "the D-pad did not reach " + std::string(name));
    };
    tap_button(SDL_GAMEPAD_BUTTON_NORTH);
    require(photo_panel_shown(), "Y did not show the panel again");
    tap_button(SDL_GAMEPAD_BUTTON_DPAD_DOWN);
    require(photo_focus_ring_shown(), "the D-pad did not show the panel's focus ring");
    pad_reach("photo.section.look");
    require(photo_->section_look, "the Look section was not open");
    tap_button(SDL_GAMEPAD_BUTTON_SOUTH);
    require(!photo_->section_look, "A did not press the focused section header");
    require(!photo_focus_ring_shown(), "the ring stayed after A pressed a button");
    pad_reach("photo.section.look");
    tap_button(SDL_GAMEPAD_BUTTON_SOUTH);
    require(photo_->section_look, "A did not open the Look section again");
    tap_button(SDL_GAMEPAD_BUTTON_SOUTH);
    wait_for_photos();
    const std::string again = saved_photo_name(status_);
    require(!again.empty() && again != saved, "A after a press wrote no photo (" + status_ + ")");
    pad_reach("photo.zoom");
    tap_button(SDL_GAMEPAD_BUTTON_SOUTH);
    wait_for_photos();
    const std::string on_slider = saved_photo_name(status_);
    require(
        !on_slider.empty() && on_slider != again,
        "A on a focused slider wrote no photo (" + status_ + ")"
    );
    require(read_orders() == frozen_orders, "the panel's D-pad and A gave an order");
    tap_button(SDL_GAMEPAD_BUTTON_EAST);
    require(!photo_mode(), "B did not leave photo mode");

    // The phone. A cancelled finger turns the touch controls on and gives no
    // order; MENU's Photo mode then enters, and a drag and a pinch frame.
    steps(kPadTouchFrameMs * 3);
    touch_state().dispatch.accept_unregistered_touch = true;
    namespace hud = oa::ui::touch_hud;
    uint64_t finger_id = 1;
    const auto send_finger = [&](SDL_EventType type, uint64_t id, float x, float y) {
        float window_x = x;
        float window_y = y;
        if (!frame_to_window(sdl_.renderer, x, y, &window_x, &window_y))
            fail(std::string("cannot place a finger on the window: ") + SDL_GetError());
        int width = 0;
        int height = 0;
        if (!SDL_GetWindowSize(sdl_.window, &width, &height) || width <= 0 || height <= 0)
            fail(std::string("the window has no size: ") + SDL_GetError());
        SDL_Event event{};
        event.type = type;
        event.tfinger.timestamp = clock_ns;
        event.tfinger.touchID = kPadTouchDevice;
        event.tfinger.fingerID = id;
        event.tfinger.x = window_x / static_cast<float>(width);
        event.tfinger.y = window_y / static_cast<float>(height);
        const bool lifted = type == SDL_EVENT_FINGER_UP || type == SDL_EVENT_FINGER_CANCELED;
        event.tfinger.pressure = lifted ? 0.0F : 1.0F;
        event.tfinger.windowID = SDL_GetWindowID(sdl_.window);
        dispatch_event(event, running);
        require(running, "a finger ended the run");
    };
    const auto tap_at = [&](float x, float y) {
        const uint64_t id = finger_id++;
        send_finger(SDL_EVENT_FINGER_DOWN, id, x, y);
        send_finger(SDL_EVENT_FINGER_UP, id, x, y);
        step(kPadTouchFrameMs);
    };
    const float canvas_w = static_cast<float>(std::max(match_layout_.width, 1));
    const float canvas_h = static_cast<float>(std::max(match_layout_.height, 1));
    send_finger(SDL_EVENT_FINGER_DOWN, finger_id, canvas_w * 0.55F, canvas_h * 0.45F);
    send_finger(SDL_EVENT_FINGER_CANCELED, finger_id, canvas_w * 0.55F, canvas_h * 0.45F);
    ++finger_id;
    steps(kPadTouchFrameMs * 2);
    require(touch_state().frame_ready, "touch laid out no frame");
    require(touch_phone_class(), "the window is not the phone layout");

    const auto centre_of = [&](hud::Control which, int index) {
        const hud::Frame& laid = touch_state().frame;
        for (std::size_t at = 0; at < laid.control_count; ++at) {
            const hud::ControlRect& control = laid.controls[at];
            if (control.control != which)
                continue;
            if (index >= 0 && control.index != static_cast<uint8_t>(index))
                continue;
            return std::pair<float, float>{
                static_cast<float>(control.rect.x) + static_cast<float>(control.rect.width) * 0.5F,
                static_cast<float>(control.rect.y) + static_cast<float>(control.rect.height) * 0.5F
            };
        }
        fail(
            index < 0 ? std::string("a control is not on the frame")
                      : std::string("a menu item is not on the frame")
        );
        return std::pair<float, float>{};
    };
    const auto [menu_x, menu_y] = centre_of(hud::Control::menu, -1);
    tap_at(menu_x, menu_y);
    require(touch_state().hud.sheet == hud::Sheet::phone_menu, "MENU did not open the phone menu");
    const auto [item_x, item_y] =
        centre_of(hud::Control::menu_item, static_cast<int>(hud::PhoneMenuItem::photo_mode));
    tap_at(item_x, item_y);
    require(photo_mode(), "the phone menu's Photo mode did not enter");
    require(photo_frozen(), "the phone's photo mode did not hold the fight");
    const std::vector<UnitOrders> touch_orders = read_orders();

    // Every control a finger presses is at least 44 points. A row the view
    // cuts is judged on the part that shows. A row wholly outside the view
    // stays listed, and it is not judged against the chips or the key line.
    // A tap on a header, a switch and the time strip lands at the centre.
    const auto photo_controls = [&] {
        const LayerScreen* open = oa_layer().find("photo-mode");
        require(open != nullptr, "the phone's photo panel is not open");
        return open->display_list();
    };
    const auto find_control = [&](const char* name, oa::ui::kit::Rect& rect) {
        const oa::ui::kit::DisplayList list = photo_controls();
        for (const oa::ui::kit::Control& control : list.controls) {
            if (control.name == name) {
                rect = control.rect;
                return true;
            }
        }
        return false;
    };
    const auto require_finger_rows = [&] {
        const oa::ui::kit::DisplayList list = photo_controls();
        int presses = 0;
        for (const oa::ui::kit::Control& control : list.controls) {
            const bool finger = control.kind == oa::ui::kit::ControlKind::button ||
                                control.kind == oa::ui::kit::ControlKind::toggle ||
                                control.kind == oa::ui::kit::ControlKind::slider ||
                                control.kind == oa::ui::kit::ControlKind::levels;
            if (!finger)
                continue;
            ++presses;
            require(
                control.rect.height >= 44,
                "a phone control is under 44 points (" + control.name + ")"
            );
            oa::ui::kit::Rect shown = control.rect;
            if (control.clip.width > 0 && control.clip.height > 0)
                shown = oa::ui::kit::intersect(control.rect, control.clip);
            if (shown.width <= 0 || shown.height <= 0)
                continue;
            require(
                shown.x >= 0 && shown.y >= 72 && shown.x + shown.width <= match_layout_.width &&
                    shown.y + shown.height <= match_layout_.height - 44,
                "a phone control meets the chips or the key line (" + control.name + ")"
            );
        }
        require(presses > 0, "the phone panel laid out no pressable control");
    };
    const auto centre_of_rect = [](const oa::ui::kit::Rect& rect) {
        return std::pair<float, float>{
            static_cast<float>(rect.x) + static_cast<float>(rect.width) * 0.5F,
            static_cast<float>(rect.y) + static_cast<float>(rect.height) * 0.5F
        };
    };
    const auto centre_inside = [](const oa::ui::kit::Control& control) {
        const oa::ui::kit::Point mid{
            control.rect.x + control.rect.width / 2, control.rect.y + control.rect.height / 2
        };
        if (control.clip.width > 0 && control.clip.height > 0)
            return oa::ui::kit::contains(control.clip, mid);
        return oa::ui::kit::contains(control.rect, mid);
    };
    // A finger reports a drag as many small moves, and the rows follow each.
    const auto drag_panel = [&](float from_x, float from_y, float to_y) {
        constexpr int kMoves = 8;
        const uint64_t id = finger_id++;
        send_finger(SDL_EVENT_FINGER_DOWN, id, from_x, from_y);
        for (int move = 1; move <= kMoves; ++move) {
            const float part = static_cast<float>(move) / static_cast<float>(kMoves);
            send_finger(SDL_EVENT_FINGER_MOTION, id, from_x, from_y + (to_y - from_y) * part);
        }
        send_finger(SDL_EVENT_FINGER_UP, id, from_x, to_y);
        step(kPadTouchFrameMs);
    };
    // Every row stays in the list, in or out of the view. Scroll until the
    // named row's centre is in the part a press can reach. A centre above the
    // view is dragged down; one below is dragged up.
    const auto scroll_until = [&](const char* name) {
        for (int attempt = 0; attempt < 14; ++attempt) {
            const oa::ui::kit::DisplayList listed = photo_controls();
            const oa::ui::kit::Control* target = nullptr;
            const oa::ui::kit::Control* anchor = nullptr;
            for (const oa::ui::kit::Control& control : listed.controls) {
                if (target == nullptr && control.name == name)
                    target = &control;
                const bool pinned = control.name == "photo.leave" || control.name == "photo.take" ||
                                    control.name == "photo.hint";
                const bool finger = control.kind == oa::ui::kit::ControlKind::button ||
                                    control.kind == oa::ui::kit::ControlKind::toggle ||
                                    control.kind == oa::ui::kit::ControlKind::slider ||
                                    control.kind == oa::ui::kit::ControlKind::levels;
                if (pinned || !finger || anchor != nullptr || !centre_inside(control))
                    continue;
                anchor = &control;
            }
            if (target != nullptr && centre_inside(*target))
                return target->rect;
            if (anchor == nullptr) {
                std::string seen;
                for (const oa::ui::kit::Control& control : listed.controls) {
                    seen += control.name;
                    seen += '@';
                    seen += std::to_string(control.rect.y);
                    seen += 'x';
                    seen += std::to_string(control.rect.height);
                    seen += ' ';
                }
                require(
                    false,
                    std::string("the panel has no row to scroll for ") + name + " (" + seen + ")"
                );
                return oa::ui::kit::Rect{};
            }
            const oa::ui::kit::Rect anchor_rect = anchor->rect;
            const float view_top = anchor->clip.height > 0 ? static_cast<float>(anchor->clip.y)
                                                           : static_cast<float>(anchor_rect.y);
            const float target_y = target == nullptr
                                       ? view_top
                                       : static_cast<float>(target->rect.y) +
                                             static_cast<float>(target->rect.height) * 0.5F;
            const bool above = target != nullptr && target_y < view_top;
            const auto [anchor_x, anchor_y] = centre_of_rect(anchor_rect);
            drag_panel(anchor_x, anchor_y, above ? anchor_y + 64.0F : anchor_y - 64.0F);
            require_finger_rows();
        }
        require(false, std::string("scrolling did not show ") + name);
        return oa::ui::kit::Rect{};
    };
    require_finger_rows();
    oa::ui::kit::Rect header{};
    require(find_control("photo.section.camera", header), "the camera header is not on the panel");
    const auto [header_x, header_y] = centre_of_rect(header);
    tap_at(header_x, header_y);
    require(!photo_->section_camera, "a tap on the camera header did not fold it");
    tap_at(static_cast<float>(header.x) - 4.0F, header_y);
    require(photo_->section_camera, "a tap beside the camera header did not open it");
    require_finger_rows();

    // A drag that starts on the zoom slider moves the rows the whole way the
    // finger goes, and neither moves the slider nor zooms.
    oa::ui::kit::Rect zoom_row{};
    require(find_control("photo.zoom", zoom_row), "the zoom slider is not on the panel");
    const auto [zoom_x, zoom_y] = centre_of_rect(zoom_row);
    const float panel_zoom_before = match_zoom_target_;
    drag_panel(zoom_x, zoom_y, zoom_y - 24.0F);
    oa::ui::kit::Rect zoom_moved{};
    require(find_control("photo.zoom", zoom_moved), "a drag of 24 points hid the zoom slider");
    require(zoom_moved.y == zoom_row.y - 24, "the rows did not follow the finger");
    require(match_zoom_target_ == panel_zoom_before, "a drag on the zoom slider zoomed");
    drag_panel(zoom_x, zoom_y - 24.0F, zoom_y);
    require(find_control("photo.zoom", zoom_moved), "a drag back hid the zoom slider");
    require(zoom_moved.y == zoom_row.y, "a drag back did not put the rows back");

    const oa::ui::kit::Rect time_header = scroll_until("photo.section.time");
    const auto [time_x, time_y] = centre_of_rect(time_header);
    tap_at(time_x, time_y);
    const oa::ui::kit::Rect strip = scroll_until("photo.time-mode");
    require(strip.height >= 44, "the time strip is under 44 points");
    // The fight is already frozen, so a tap on Freeze changes nothing. Slow is
    // the middle. The finger lifts before the frame steps, and Freeze is put
    // back before the next step, so the fight does not run a tick.
    const float slow_x = static_cast<float>(strip.x) + static_cast<float>(strip.width) * 0.5F;
    const float slow_y = static_cast<float>(strip.y) + static_cast<float>(strip.height) * 0.5F;
    const uint64_t slow_id = finger_id++;
    send_finger(SDL_EVENT_FINGER_DOWN, slow_id, slow_x, slow_y);
    send_finger(SDL_EVENT_FINGER_UP, slow_id, slow_x, slow_y);
    require(photo_slow_speed() > 0 && !photo_frozen(), "a tap on Slow did not slow the fight");
    bool slow_lit = false;
    {
        const oa::ui::kit::DisplayList listed = photo_controls();
        for (const oa::ui::kit::Item& item : listed.items) {
            const auto* levels = std::get_if<oa::ui::kit::LevelsLook>(&item.look);
            if (levels != nullptr && levels->chosen == 1)
                slow_lit = true;
        }
    }
    require(slow_lit, "Slow is not the time strip's chosen level");
    require(photo_freeze(), "Freeze did not hold the fight after Slow");
    require(photo_frozen() && photo_slow_speed() == 0, "the fight was still running after Freeze");

    const oa::ui::kit::Rect look_header = scroll_until("photo.section.look");
    const auto [look_x, look_y] = centre_of_rect(look_header);
    tap_at(look_x, look_y);
    const oa::ui::kit::Rect toggle = scroll_until("photo.selection");
    const auto [switch_x, switch_y] = centre_of_rect(toggle);
    const bool selection_before = photo_->show_selection;
    tap_at(switch_x, switch_y);
    require(
        photo_->show_selection != selection_before, "a tap on Show selection did not switch it"
    );
    tap_at(static_cast<float>(toggle.x) - 4.0F, switch_y);
    require(
        photo_->show_selection == selection_before, "a tap beside Show selection did not switch it"
    );
    require_finger_rows();

    // The panel, the chips and the key line take the right edge, the top and
    // the bottom. The drag and the pinch stay on the open battlefield.
    const std::array<double, 2> dragged_from = match_view_place();
    const float drag_x = canvas_w * 0.22F;
    const float drag_y = canvas_h * 0.45F;
    const uint64_t drag_id = finger_id++;
    send_finger(SDL_EVENT_FINGER_DOWN, drag_id, drag_x, drag_y);
    send_finger(SDL_EVENT_FINGER_MOTION, drag_id, drag_x + 160.0F, drag_y);
    send_finger(SDL_EVENT_FINGER_UP, drag_id, drag_x + 160.0F, drag_y);
    step(kPadTouchFrameMs);
    const std::array<double, 2> dragged_to = match_view_place();
    require(
        dragged_to[0] != dragged_from[0] || dragged_to[1] != dragged_from[1],
        "a one-finger drag did not pan"
    );

    const float zoomed_before = match_zoom();
    const float pinch_x = canvas_w * 0.28F;
    const float pinch_y = canvas_h * 0.45F;
    const uint64_t left_finger = finger_id++;
    const uint64_t right_finger = finger_id++;
    send_finger(SDL_EVENT_FINGER_DOWN, left_finger, pinch_x - 24.0F, pinch_y);
    send_finger(SDL_EVENT_FINGER_DOWN, right_finger, pinch_x + 24.0F, pinch_y);
    send_finger(SDL_EVENT_FINGER_MOTION, left_finger, pinch_x - 96.0F, pinch_y);
    send_finger(SDL_EVENT_FINGER_MOTION, right_finger, pinch_x + 96.0F, pinch_y);
    send_finger(SDL_EVENT_FINGER_UP, left_finger, pinch_x - 96.0F, pinch_y);
    send_finger(SDL_EVENT_FINGER_UP, right_finger, pinch_x + 96.0F, pinch_y);
    require(match_zoom() > zoomed_before, "a pinch did not zoom in");

    // Area. L1 marks the view. The left stick grows that mark and does not
    // pan. A one-finger drag marks, a tap keeps the mark, and two fingers pan.
    if (match_zoom_target_ > 0.0F)
        match_zoom_ = match_zoom_target_;
    const std::array<double, 2> mark_place = match_view_place();
    // The frames around L1: the panel draws the marked rectangle at once,
    // though nothing on the panel was pressed.
    const auto presented = [&] {
        renderer::Surface shot;
        {
            const CaptureHold hold(capture_frame_, shot);
            step(kPadTouchFrameMs);
        }
        return shot;
    };
    const renderer::Surface unmarked = presented();
    tap_button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    require(photo_ != nullptr && photo_->poster, "L1 did not choose a poster");
    require(
        photo_->poster_extent == static_cast<uint8_t>(oa::present::photo::PosterExtent::marked),
        "L1 did not choose Area"
    );
    {
        const renderer::Surface marked = presented();
        const std::optional<std::array<int32_t, 4>> drawn = poster_mark_window();
        require(drawn.has_value(), "L1's mark has no rectangle in the window");
        const int32_t edge_x = (*drawn)[0];
        const auto frame_w = static_cast<int32_t>(marked.width);
        const auto frame_h = static_cast<int32_t>(marked.height);
        require(
            marked.rgb.size() == unmarked.rgb.size() && edge_x >= 0 && edge_x < frame_w,
            "L1's rectangle has no left edge in the frame"
        );
        const int32_t top = std::clamp((*drawn)[1], int32_t{0}, frame_h);
        const int32_t bottom = std::clamp((*drawn)[1] + (*drawn)[3], int32_t{0}, frame_h);
        int32_t rows = 0;
        int32_t changed = 0;
        for (int32_t row = top; row < bottom; ++row) {
            const std::size_t at =
                (static_cast<std::size_t>(row) * marked.width + static_cast<std::size_t>(edge_x)) *
                3U;
            ++rows;
            if (marked.rgb[at] != unmarked.rgb[at] || marked.rgb[at + 1] != unmarked.rgb[at + 1] ||
                marked.rgb[at + 2] != unmarked.rgb[at + 2])
                ++changed;
        }
        require(
            rows > 0 && changed * 2 > rows,
            "the marked rectangle was not drawn after L1 (" + std::to_string(changed) + " of " +
                std::to_string(rows) + " rows of its left edge)"
        );
    }
    const int32_t l1_width = photo_->poster_mark_w;
    require(l1_width >= 64 && photo_->poster_mark_h >= 64, "L1's mark is under 64 map pixels");
    const int32_t l1_height = photo_->poster_mark_h;
    // A push across that drifts a little down, inside the dead zone, as a
    // real stick does: only the width grows.
    set_pad_touch_axis(stand_in, SDL_GAMEPAD_AXIS_LEFTX, kPadTouchStick);
    set_pad_touch_axis(stand_in, SDL_GAMEPAD_AXIS_LEFTY, 0.08F);
    steps(200);
    set_pad_touch_axis(stand_in, SDL_GAMEPAD_AXIS_LEFTX, 0.0F);
    set_pad_touch_axis(stand_in, SDL_GAMEPAD_AXIS_LEFTY, 0.0F);
    step(kPadTouchFrameMs);
    require(photo_->poster_mark_w > l1_width, "the left stick did not grow the mark");
    require(
        photo_->poster_mark_h == l1_height, "a push across that drifted down grew the mark's height"
    );
    const std::array<double, 2> after_stick = match_view_place();
    require(
        after_stick[0] == mark_place[0] && after_stick[1] == mark_place[1],
        "the left stick panned while Area was chosen"
    );
    const int32_t stick_width = photo_->poster_mark_w;
    const int32_t stick_x = photo_->poster_mark_x;
    const float mark_x = canvas_w * 0.22F;
    const float mark_y = canvas_h * 0.45F;
    const uint64_t mark_finger = finger_id++;
    send_finger(SDL_EVENT_FINGER_DOWN, mark_finger, mark_x, mark_y);
    send_finger(SDL_EVENT_FINGER_UP, mark_finger, mark_x, mark_y);
    step(kPadTouchFrameMs);
    require(
        photo_->poster_mark_w == stick_width && photo_->poster_mark_x == stick_x,
        "a tap replaced the mark"
    );
    send_finger(SDL_EVENT_FINGER_DOWN, mark_finger, mark_x, mark_y);
    send_finger(SDL_EVENT_FINGER_MOTION, mark_finger, mark_x + 160.0F, mark_y);
    send_finger(SDL_EVENT_FINGER_UP, mark_finger, mark_x + 160.0F, mark_y);
    step(kPadTouchFrameMs);
    const std::array<double, 2> after_mark_drag = match_view_place();
    require(
        after_mark_drag[0] == mark_place[0] && after_mark_drag[1] == mark_place[1],
        "a one-finger drag panned while Area was chosen"
    );
    require(
        photo_->poster_mark_w != stick_width || photo_->poster_mark_x != stick_x,
        "a one-finger drag did not mark"
    );
    const std::array<double, 2> before_two = match_view_place();
    const float two_x = canvas_w * 0.22F;
    const float two_y = canvas_h * 0.40F;
    const uint64_t two_left = finger_id++;
    const uint64_t two_right = finger_id++;
    send_finger(SDL_EVENT_FINGER_DOWN, two_left, two_x, two_y);
    send_finger(SDL_EVENT_FINGER_DOWN, two_right, two_x + 48.0F, two_y);
    send_finger(SDL_EVENT_FINGER_MOTION, two_left, two_x + 80.0F, two_y);
    send_finger(SDL_EVENT_FINGER_MOTION, two_right, two_x + 128.0F, two_y);
    send_finger(SDL_EVENT_FINGER_UP, two_left, two_x + 80.0F, two_y);
    send_finger(SDL_EVENT_FINGER_UP, two_right, two_x + 128.0F, two_y);
    step(kPadTouchFrameMs);
    const std::array<double, 2> after_two = match_view_place();
    require(
        after_two[0] != before_two[0] || after_two[1] != before_two[1],
        "a two-finger drag did not pan"
    );
    set_photo_poster(false);

    // A finger the system cancels on Take photo presses nothing; a tap there
    // takes the photo.
    const auto take_centre = [&] {
        const LayerScreen* open = oa_layer().find("photo-mode");
        require(open != nullptr, "the phone's photo panel is not open");
        const oa::ui::kit::DisplayList list = open->display_list();
        for (const oa::ui::kit::Control& control : list.controls)
            if (control.name == "photo.take")
                return std::pair<float, float>{
                    static_cast<float>(control.rect.x) +
                        static_cast<float>(control.rect.width) / 2.0F,
                    static_cast<float>(control.rect.y) +
                        static_cast<float>(control.rect.height) / 2.0F
                };
        fail("Take photo is not on the phone's panel");
        return std::pair<float, float>{};
    };
    const std::string before_reach = saved_photo_name(status_);
    const auto [take_x, take_y] = take_centre();
    oa::ui::kit::Rect take_rect{};
    require(find_control("photo.take", take_rect), "Take photo is not on the phone's panel");
    tap_at(static_cast<float>(take_rect.x) - 4.0F, take_y);
    wait_for_photos();
    const std::string reached = saved_photo_name(status_);
    require(!reached.empty() && reached != before_reach, "a tap beside Take photo wrote no photo");
    const uint64_t cancelled_id = finger_id++;
    send_finger(SDL_EVENT_FINGER_DOWN, cancelled_id, take_x, take_y);
    send_finger(SDL_EVENT_FINGER_CANCELED, cancelled_id, take_x, take_y);
    step(kPadTouchFrameMs);
    wait_for_photos();
    require(saved_photo_name(status_) == reached, "a cancelled finger on Take photo took a photo");
    tap_at(take_x, take_y);
    wait_for_photos();
    const std::string tapped = saved_photo_name(status_);
    require(!tapped.empty() && tapped != reached, "a tap on Take photo wrote no photo");
    require(read_orders() == touch_orders, "a touch input gave an order");
    require(
        file_bytes(options_.preferences_file) == preferences_settled, "the preferences file changed"
    );
    oa::ui::kit::Rect leave_rect{};
    require(find_control("photo.leave", leave_rect), "Leave is not on the phone's panel");
    const auto [leave_x, leave_y] = centre_of_rect(leave_rect);
    tap_at(leave_x, leave_y);
    require(!photo_mode(), "a tap on Leave stayed in photo mode");
    if (photo_mode())
        leave_photo_mode();
    if (match_ != nullptr) {
        leave_match();
        load(Screen::main_menu);
    }
    std::cout << "photo pad-touch: ok" << std::endl;
}

void Runtime::check_photo_looks() {
    /// Puts the preferences file back. The fights write their own rows.
    struct PreferencesHold {
        Runtime& runtime;
        bool existed{false};
        std::vector<char> bytes;

        explicit PreferencesHold(Runtime& host) : runtime(host) {
            const auto& preferences = host.options_.preferences_file;
            if (preferences.has_value())
                existed = fs::exists(*preferences);
            bytes = file_bytes(preferences);
        }

        PreferencesHold(const PreferencesHold&) = delete;
        PreferencesHold& operator=(const PreferencesHold&) = delete;

        ~PreferencesHold() {
            const auto& preferences = runtime.options_.preferences_file;
            if (preferences.has_value()) {
                if (!existed) {
                    std::error_code remove_error;
                    fs::remove(*preferences, remove_error);
                } else {
                    std::ofstream out(*preferences, std::ios::binary | std::ios::trunc);
                    if (out)
                        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                }
            }
            runtime.preferences_dirty_ = false;
        }
    };

    const PreferencesHold preferences_hold(*this);

    struct GammaHold {
        Runtime& runtime;
        float previous;

        explicit GammaHold(Runtime& host) : runtime(host), previous(host.display_gamma_) {
            runtime.set_display_gamma(kCheckGamma);
        }

        GammaHold(const GammaHold&) = delete;
        GammaHold& operator=(const GammaHold&) = delete;

        ~GammaHold() { runtime.set_display_gamma(previous); }
    };

    const GammaHold gamma_hold(*this);

    struct PictureSweep {
        Runtime& runtime;

        explicit PictureSweep(Runtime& host) : runtime(host) {}

        PictureSweep(const PictureSweep&) = delete;
        PictureSweep& operator=(const PictureSweep&) = delete;

        ~PictureSweep() {
            remove_saved_pictures(screenshots_folder(runtime.user_folder_, ""));
            remove_saved_pictures(screenshots_folder(runtime.user_folder_, "ridge"));
        }
    };

    const PictureSweep picture_sweep(*this);
    const ClipboardHookHold clipboard_hold;
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("photo looks check: " + what);
    };
    const auto require = [&](bool ok, const std::string& what) {
        if (!ok)
            fail(what);
    };
    require(!gamma_identity_, "the gamma is the identity");
    using HardwareAcceleration = oa::ui::engine_settings::HardwareAcceleration;
    const HardwareAcceleration asked =
        options_.hardware_acceleration.value_or(HardwareAcceleration::off);
    const auto step_tick = [this] {
        try {
            step_match_simulation();
        } catch (const std::exception& step_error) {
            report_match_tick_error(step_error.what());
        }
    };
    const auto clear_look = [this] {
        if (photo_ == nullptr)
            return;
        photo_->look_preset = 0;
        photo_->look_vignette = 0;
        photo_->look_roundness = 50;
        photo_->look_grain = 0;
        photo_->look_grain_size = 2;
        photo_->look_frame = 0;
        photo_->look_depth = false;
        photo_->look_tilt_centre = 50;
        photo_->look_tilt_width = 48;
        photo_->look_tilt_radius = 0;
        photo_->look_haze = 0;
    };
    const auto set_grade = [&](int preset) {
        require(photo_ != nullptr, "photo mode holds no look");
        clear_look();
        photo_->look_preset = static_cast<uint8_t>(std::clamp(preset, 0, 5));
    };
    const auto start_fight = [&] {
        start_benchmark_skirmish();
        match_layout_ = lay_out_match(options_.match_width, options_.match_height);
        require(
            least_match_zoom() <= kPlayZoom && most_match_zoom() >= kPlayZoom,
            "zoom 1 is outside the match's play limits"
        );
        match_zoom_ = kPlayZoom;
        match_zoom_target_ = kPlayZoom;
        spawn_combat_armies(kArmy);
        oa::Game& game = match_->state().game;
        console::set_console_flags(
            game,
            static_cast<uint16_t>(
                console::console_flags(game) | console::console_flag::selection_boxes
            )
        );
        oa::Unit* commander = nullptr;
        for (const auto& slot : match_->world().slots) {
            if (slot.unit != nullptr && slot.record.type_index != 0 &&
                slot.record.owner_index == match_local_player_) {
                commander = oa::world_unit_at(&match_->state(), slot.unit_index);
                break;
            }
        }
        require(commander != nullptr, "the fight has no local commander");
        commander->flags |= OA_UNIT_FLAG_SELECTED;
        match_command_ = MatchCommand::attack;
        pending_build_type_ = 0;
        set_display_gamma(kCheckGamma);
        require(!gamma_identity_, "the gamma is the identity");
    };
    const auto end_fight = [this] {
        if (photo_mode())
            leave_photo_mode();
        if (match_ != nullptr) {
            leave_match();
            load(Screen::main_menu);
        }
    };
    const auto clear_pictures = [&] {
        remove_saved_pictures(screenshots_folder(user_folder_, ""));
        remove_saved_pictures(screenshots_folder(user_folder_, "ridge"));
        if (match_ != nullptr)
            remove_saved_pictures(photo_shots_folder());
    };
    const auto file_contains = [&](const fs::path& path, std::string_view needle) {
        std::ifstream in(path, std::ios::binary);
        require(static_cast<bool>(in), "the photo could not be read");
        const std::string bytes(
            (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()
        );
        return bytes.find(needle) != std::string::npos;
    };

    if (asked == HardwareAcceleration::full) {
        start_fight();
        clear_pictures();
        for (int tick = 1; tick <= kEnterTick; ++tick) {
            step_tick();
            if (tick == kEnterTick)
                require(enter_photo_mode(), "photo mode did not open");
            if (photo_mode()) {
                set_photo_show_selection(false);
                set_photo_show_health_bars(false);
            }
            render_match_surface();
        }
        require(photo_mode() && full_presentation(), "Full is not presenting");
        set_display_gamma(kCheckGamma);
        require(!gamma_identity_, "the gamma is the identity");
        set_grade(2);
        const std::vector<char> prefs = file_bytes(options_.preferences_file);
        renderer::Surface presented;
        {
            const CaptureHold hold(capture_frame_, presented);
            render_match_surface();
            present_match_layers();
        }
        require(full_ != nullptr && full_->drawn, "Full did not draw the frame");
        std::vector<uint8_t> plain;
        uint32_t width = 0;
        uint32_t height = 0;
        std::string reason;
        require(
            capture_photo_pixels(plain, width, height, reason),
            reason.empty() ? std::string("the frame was not read") : reason
        );
        std::vector<uint8_t> expected = plain;
        apply_photo_look_rgb(expected, width, height, 1, 0, 0);
        require(expected != plain, "the look left the full picture unchanged");
        take_photo();
        wait_for_photos();
        const std::string saved = saved_photo_name(status_);
        require(!saved.empty(), "status is " + status_);
        const fs::path file = photo_shots_folder() / oa::platform::path_from_utf8(saved);
        const std::vector<uint8_t> rows = png_rgb(file, width, height);
        require(rows == expected, "the photo is not the look on Full's capture");
        require_presented_battlefield(
            rows,
            width,
            height,
            presented,
            match_layout_.left,
            match_layout_.top,
            match_layout_.width,
            match_layout_.height
        );
        require(file_contains(file, "\"preset\":\"dusk\""), "the record does not name the grade");
        require(file_bytes(options_.preferences_file) == prefs, "the preferences file changed");
        end_fight();
        clear_pictures();
        std::printf("photo looks full: ok\n");
        std::fflush(stdout);
        return;
    }
    if (asked != HardwareAcceleration::off)
        fail("looks check runs Off or Full");

    const char* grade_names[] = {"1997", "dusk", "arctic", "noir", "bleach"};
    const char* grade_pins[] = {
        "8eccd1658608578a6a9d4bd738123b62a7ba5890a5dcf0f138cc7f3117a05a8d",
        "3d5ee740bf5e1851918a14aede799015dfe17ac8d67e9b75346dcac1bbf910c6",
        "8f77a8078ddb59e32d643cdfb7ed40808dd20cc1eabba4a0eff9f99d5f95c7d9",
        "ad0691e77cce3e42eda2c4bbbca8d856cccae8c873ea0a8db37df768355830dc",
        "ab365a16fd5ea24e439e93935a115751424d9d190ca82e29d7552c638176666f",
    };
    std::vector<char> prefs_before;
    const auto play_pass = [&](bool shoot) {
        start_fight();
        clear_pictures();
        for (int tick = 1; tick <= kPassTicks; ++tick) {
            step_tick();
            if (shoot && tick == kEnterTick)
                require(enter_photo_mode(), "photo mode did not open");
            if (photo_mode()) {
                set_photo_show_selection(false);
                set_photo_show_health_bars(false);
            }
            if (shoot && tick == kEnterTick) {
                set_display_gamma(kCheckGamma);
                require(!gamma_identity_, "the gamma is the identity");
                require(photo_mode(), "photo mode was not open");
                prefs_before = file_bytes(options_.preferences_file);
                bool digest_missed = false;
                const auto shoot_one = [&](const char* name,
                                           std::string_view expect_sha,
                                           bool graded) {
                    renderer::Surface presented;
                    {
                        const CaptureHold hold(capture_frame_, presented);
                        render_match_surface();
                        present_match_layers();
                    }
                    std::vector<uint8_t> expected = gamma_mapped(match_world_cpu_, gamma_table_);
                    require(!expected.empty(), "the battlefield could not be copied");
                    const uint32_t width = match_world_cpu_.width;
                    const uint32_t height = match_world_cpu_.height;
                    if (graded)
                        apply_photo_look_rgb(expected, width, height, 1, 0, 0);
                    take_photo();
                    wait_for_photos();
                    const std::string saved = saved_photo_name(status_);
                    require(!saved.empty(), std::string("status is ") + status_);
                    const fs::path file =
                        photo_shots_folder() / oa::platform::path_from_utf8(saved);
                    const std::vector<uint8_t> rows = png_rgb(file, width, height);
                    require(
                        rows == expected, std::string(name) + " is not the filtered battlefield"
                    );
                    require_presented_battlefield(
                        rows,
                        width,
                        height,
                        presented,
                        match_layout_.left,
                        match_layout_.top,
                        match_layout_.width,
                        match_layout_.height
                    );
                    const std::string sha = sha256_hex(rows);
                    if (sha != expect_sha) {
                        std::fprintf(stderr, "photo looks check: %s %s\n", name, sha.c_str());
                        digest_missed = true;
                    }
                    return file;
                };
                clear_look();
                const fs::path plain = shoot_one("none", kPhotoCaptureSha256, false);
                require(
                    !accelerated_presentation() && !full_presentation(),
                    "Off is presenting on the card"
                );
                require(!file_contains(plain, "\"look\":"), "a look was written with none on");
                for (int grade = 0; grade < 5; ++grade) {
                    set_grade(grade + 1);
                    const fs::path saved = shoot_one(grade_names[grade], grade_pins[grade], true);
                    if (grade == 1) {
                        require(
                            file_contains(
                                saved,
                                "\"look\":{\"preset\":\"dusk\",\"vignette\":0,\"roundness\":50,"
                                "\"grain\":0,\"grain_size\":2,\"grain_pixels\":2,\"frame\":"
                                "\"none\",\"tilt_shift\":null,\"haze\":null,"
                                "\"depth_of_field\":null}"
                            ),
                            "the dusk record is not the look"
                        );
                    }
                }
                // A square crop with a frame, a vignette and grain. The photo
                // is the look on the crop, and the preview inside the crop is
                // that photo: the look sits where the photo has it.
                {
                    using oa::ui::photo_panel::Guide;
                    clear_look();
                    photo_->look_preset = 2;
                    photo_->look_vignette = 70;
                    photo_->look_roundness = 100;
                    photo_->look_grain = 40;
                    photo_->look_grain_size = 3;
                    photo_->look_frame = 1;
                    photo_->guide = static_cast<uint8_t>(Guide::crop_1_1);
                    renderer::Surface presented;
                    {
                        const CaptureHold hold(capture_frame_, presented);
                        render_match_surface();
                        present_match_layers();
                    }
                    std::vector<uint8_t> expected = gamma_mapped(match_world_cpu_, gamma_table_);
                    require(!expected.empty(), "the battlefield could not be copied");
                    uint32_t width = match_world_cpu_.width;
                    uint32_t height = match_world_cpu_.height;
                    const oa::ui::kit::Rect crop = oa::ui::photo_panel::crop_bounds(
                        static_cast<int32_t>(width), static_cast<int32_t>(height), Guide::crop_1_1
                    );
                    oa::ui::photo_panel::crop_centred(expected, width, height, Guide::crop_1_1);
                    require(width == height && crop.width > 0, "the crop is not square");
                    apply_photo_look_rgb(expected, width, height, 1, crop.x, crop.y);
                    take_photo();
                    wait_for_photos();
                    const std::string saved = saved_photo_name(status_);
                    require(!saved.empty(), std::string("status is ") + status_);
                    const std::vector<uint8_t> rows = png_rgb(
                        photo_shots_folder() / oa::platform::path_from_utf8(saved), width, height
                    );
                    require(rows == expected, "the cropped photo is not the look on the crop");
                    // The guide's lines lie on the crop's edges, over the
                    // battlefield: compare from three pixels inside them.
                    constexpr uint32_t kInside = 3;
                    require(width > kInside * 2 && height > kInside * 2, "the crop is too small");
                    const uint32_t inner_w = width - kInside * 2;
                    const uint32_t inner_h = height - kInside * 2;
                    std::vector<uint8_t> inner(static_cast<std::size_t>(inner_w) * inner_h * 3U);
                    for (uint32_t row = 0; row < inner_h; ++row)
                        std::memcpy(
                            inner.data() + static_cast<std::size_t>(row) * inner_w * 3U,
                            rows.data() +
                                (static_cast<std::size_t>(row + kInside) * width + kInside) * 3U,
                            static_cast<std::size_t>(inner_w) * 3U
                        );
                    require_presented_battlefield(
                        inner,
                        inner_w,
                        inner_h,
                        presented,
                        match_layout_.left + crop.x + static_cast<int>(kInside),
                        match_layout_.top + crop.y + static_cast<int>(kInside),
                        match_layout_.width,
                        match_layout_.height
                    );
                    photo_->guide = 0;
                }
                // The caption bar, with the panel hidden, on the whole
                // battlefield and on a 3:2 crop, whose bar lies higher than
                // the window's bottom at 640x480. The key line keeps clear of
                // the bar, so the presented bar is the photo's bar, letters
                // included.
                using oa::ui::photo_panel::Guide;
                for (const Guide caption_guide : {Guide::off, Guide::crop_3_2}) {
                    const bool panel_was_shown = photo_->panel_shown;
                    clear_look();
                    photo_->look_frame = static_cast<uint8_t>(oa::present::photo::Frame::caption);
                    photo_->panel_shown = false;
                    photo_->guide = static_cast<uint8_t>(caption_guide);
                    renderer::Surface presented;
                    {
                        const CaptureHold hold(capture_frame_, presented);
                        render_match_surface();
                        present_match_layers();
                    }
                    std::vector<uint8_t> expected = gamma_mapped(match_world_cpu_, gamma_table_);
                    require(!expected.empty(), "the battlefield could not be copied");
                    uint32_t width = match_world_cpu_.width;
                    uint32_t height = match_world_cpu_.height;
                    oa::ui::kit::Rect kept{
                        0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)
                    };
                    if (caption_guide != Guide::off) {
                        kept = oa::ui::photo_panel::crop_bounds(
                            static_cast<int32_t>(width), static_cast<int32_t>(height), caption_guide
                        );
                        oa::ui::photo_panel::crop_centred(expected, width, height, caption_guide);
                        require(
                            kept.width == static_cast<int32_t>(width) &&
                                kept.height == static_cast<int32_t>(height) &&
                                kept.y + kept.height <
                                    static_cast<int32_t>(match_world_cpu_.height),
                            "the 3:2 crop does not leave rows under it"
                        );
                    }
                    apply_photo_look_rgb(expected, width, height, 1, kept.x, kept.y);
                    take_photo();
                    wait_for_photos();
                    const std::string saved = saved_photo_name(status_);
                    require(!saved.empty(), std::string("status is ") + status_);
                    const std::vector<uint8_t> rows = png_rgb(
                        photo_shots_folder() / oa::platform::path_from_utf8(saved), width, height
                    );
                    require(
                        rows == expected, "the caption photo is not the look on the battlefield"
                    );
                    const int32_t bar = oa::present::photo::caption_bar_height(
                        static_cast<int32_t>(width), static_cast<int32_t>(height)
                    );
                    // A crop's guide lines lie on its edges, over the
                    // battlefield: compare from three pixels inside them.
                    const uint32_t inset = caption_guide == Guide::off ? 0U : 3U;
                    require(
                        bar > 0 && static_cast<uint32_t>(bar) < height &&
                            static_cast<uint32_t>(bar) > inset && width > inset * 2U,
                        "the caption bar has no rows"
                    );
                    const uint32_t first = height - static_cast<uint32_t>(bar);
                    for (uint32_t y = first; y < height - inset; ++y) {
                        for (uint32_t x = inset; x < width - inset; ++x) {
                            const int window_x = match_layout_.left + kept.x + static_cast<int>(x);
                            const int window_y = match_layout_.top + kept.y + static_cast<int>(y);
                            const bool inside = window_x >= 0 && window_y >= 0 &&
                                                static_cast<uint32_t>(window_x) < presented.width &&
                                                static_cast<uint32_t>(window_y) < presented.height;
                            require(inside, "the caption bar is outside the presented frame");
                            const std::size_t photo_at =
                                (static_cast<std::size_t>(y) * width + x) * 3U;
                            const std::size_t shown_at =
                                (static_cast<std::size_t>(window_y) * presented.width +
                                 static_cast<uint32_t>(window_x)) *
                                3U;
                            if (rows[photo_at] != presented.rgb[shown_at] ||
                                rows[photo_at + 1U] != presented.rgb[shown_at + 1U] ||
                                rows[photo_at + 2U] != presented.rgb[shown_at + 2U]) {
                                throw std::runtime_error(
                                    std::string("photo looks check: the caption bar differs at ") +
                                    std::to_string(x) + "," + std::to_string(y) +
                                    (caption_guide == Guide::off ? "" : " on the 3:2 crop")
                                );
                            }
                        }
                    }
                    photo_->panel_shown = panel_was_shown;
                }
                photo_->guide = 0;
                clear_look();
                photo_->look_tilt_centre = 40;
                photo_->look_tilt_width = 48;
                photo_->look_tilt_radius = 12;
                const fs::path tilted = shoot_one(
                    "tilt-shift",
                    "4ada27657d9be50634b8f001263dc29342c0c37b37f79eb867611244387b6729",
                    true
                );
                require(
                    file_contains(
                        tilted,
                        "\"tilt_shift\":{\"centre\":40,\"width\":48,\"width_pixels\":48,"
                        "\"radius\":12,\"radius_pixels\":12}"
                    ),
                    "the tilt-shift record is not the band"
                );
                require(file_contains(tilted, "\"haze\":null"), "the tilt-shift photo hazed");
                clear_look();
                photo_->look_haze = 60;
                const fs::path hazed = shoot_one(
                    "haze", "0d48363ca8cd9fd9e83e5a7e9ef50b96c3d3afb1ce021f550f4beb8686065136", true
                );
                require(file_contains(hazed, "\"tilt_shift\":null"), "the haze photo blurred");
                require(file_contains(hazed, "\"haze\":60"), "the haze record is not the amount");
                // Ground the player has never seen stays black under haze:
                // every pixel the fog's black pass draws keeps its colour,
                // and the rest is hazed as with the whole map watched, where
                // the view draws no fog. The fight's view has that ground on
                // its right, above explored ground and below it.
                clear_look();
                photo_->look_haze = 100;
                {
                    namespace wr = oa::present::world_renderer;
                    render_match_surface();
                    const uint32_t width = match_world_cpu_.width;
                    const uint32_t height = match_world_cpu_.height;
                    const std::vector<uint8_t> drawn_rows =
                        gamma_mapped(match_world_cpu_, gamma_table_);
                    require(
                        !drawn_rows.empty() && width > 8 && height > 8, "the battlefield is empty"
                    );
                    std::vector<uint8_t> hazed_rows = drawn_rows;
                    apply_photo_look_rgb(hazed_rows, width, height, 1, 0, 0);
                    std::vector<uint8_t> unfogged = drawn_rows;
                    const WatchedSight watched = watched_sight_;
                    watched_sight_ = WatchedSight::whole_map;
                    apply_photo_look_rgb(unfogged, width, height, 1, 0, 0);
                    watched_sight_ = watched;
                    // The black pass alone, drawn by the view's own fog over
                    // a colour the fog never draws.
                    const uint8_t viewer = match_view_player();
                    const auto zoom_fp = static_cast<uint32_t>(
                        std::lround(static_cast<double>(match_zoom_) * 65536.0)
                    );
                    wr::FogGrid grid = wr::build_fog_grid(
                        match_->sight(),
                        viewer,
                        match_->player_coverage(viewer),
                        {false, true, allied_mapped_bits()},
                        match_camera_x_,
                        match_camera_z_,
                        wr::fog_map_span(zoom_fp, static_cast<int32_t>(width)),
                        wr::fog_map_span(zoom_fp, static_cast<int32_t>(height))
                    );
                    for (wr::FogTile& tile : grid.tiles)
                        tile.unseen = 0;
                    wr::Surface fogged;
                    fogged.width = width;
                    fogged.height = height;
                    fogged.rgb.assign(static_cast<std::size_t>(width) * height * 3U, 0);
                    for (std::size_t at = 1; at < fogged.rgb.size(); at += 3U)
                        fogged.rgb[at] = 255;
                    wr::FogShading shading = fog_shading_;
                    shading.dithered = false;
                    wr::draw_fog_grid(
                        fogged,
                        {0,
                         0,
                         static_cast<int32_t>(width),
                         static_cast<int32_t>(height),
                         match_camera_x_,
                         match_camera_z_,
                         zoom_fp},
                        grid,
                        fog_tiles_,
                        shading,
                        nullptr
                    );
                    std::size_t black = 0;
                    std::size_t lit = 0;
                    std::size_t unlike = 0;
                    for (std::size_t at = 0; at < fogged.rgb.size(); at += 3U) {
                        const bool drawn = fogged.rgb[at] != 0 || fogged.rgb[at + 1U] != 255 ||
                                           fogged.rgb[at + 2U] != 0;
                        const auto same_as = [&](const std::vector<uint8_t>& other) {
                            return hazed_rows[at] == other[at] &&
                                   hazed_rows[at + 1U] == other[at + 1U] &&
                                   hazed_rows[at + 2U] == other[at + 2U];
                        };
                        if (drawn) {
                            ++black;
                            lit += same_as(drawn_rows) ? 0U : 1U;
                        } else {
                            unlike += same_as(unfogged) ? 0U : 1U;
                        }
                    }
                    require(
                        black > 1000 && lit == 0 && unlike == 0,
                        "haze 100 lit " + std::to_string(lit) + " of the fog's " +
                            std::to_string(black) + " black pixels and hazed " +
                            std::to_string(unlike) + " others unlike the haze with no fog"
                    );
                }
                const fs::path hidden = shoot_one(
                    "haze-unexplored",
                    "403de60aed3465161f109ec4231547ac7900311d8083b6b3dff599e7a93d9731",
                    true
                );
                require(file_contains(hidden, "\"haze\":100"), "the haze record is not 100");
                {
                    using oa::ui::photo_panel::Guide;
                    photo_->guide = static_cast<uint8_t>(Guide::crop_1_1);
                    renderer::Surface presented;
                    {
                        const CaptureHold hold(capture_frame_, presented);
                        render_match_surface();
                        present_match_layers();
                    }
                    std::vector<uint8_t> expected = gamma_mapped(match_world_cpu_, gamma_table_);
                    require(!expected.empty(), "the battlefield could not be copied");
                    uint32_t width = match_world_cpu_.width;
                    uint32_t height = match_world_cpu_.height;
                    const oa::ui::kit::Rect crop = oa::ui::photo_panel::crop_bounds(
                        static_cast<int32_t>(width), static_cast<int32_t>(height), Guide::crop_1_1
                    );
                    oa::ui::photo_panel::crop_centred(expected, width, height, Guide::crop_1_1);
                    require(width == height && crop.width > 0, "the haze crop is not square");
                    apply_photo_look_rgb(expected, width, height, 1, crop.x, crop.y);
                    take_photo();
                    wait_for_photos();
                    const std::string saved = saved_photo_name(status_);
                    require(!saved.empty(), std::string("status is ") + status_);
                    const std::vector<uint8_t> rows = png_rgb(
                        photo_shots_folder() / oa::platform::path_from_utf8(saved), width, height
                    );
                    require(rows == expected, "the cropped haze photo is not the look on the crop");
                    constexpr uint32_t kInside = 3;
                    require(width > kInside * 2 && height > kInside * 2, "the haze crop is small");
                    const uint32_t inner_w = width - kInside * 2;
                    const uint32_t inner_h = height - kInside * 2;
                    std::vector<uint8_t> inner(static_cast<std::size_t>(inner_w) * inner_h * 3U);
                    for (uint32_t row = 0; row < inner_h; ++row)
                        std::memcpy(
                            inner.data() + static_cast<std::size_t>(row) * inner_w * 3U,
                            rows.data() +
                                (static_cast<std::size_t>(row + kInside) * width + kInside) * 3U,
                            static_cast<std::size_t>(inner_w) * 3U
                        );
                    require_presented_battlefield(
                        inner,
                        inner_w,
                        inner_h,
                        presented,
                        match_layout_.left + crop.x + static_cast<int>(kInside),
                        match_layout_.top + crop.y + static_cast<int>(kInside),
                        match_layout_.width,
                        match_layout_.height
                    );
                    photo_->guide = 0;
                }
                clear_look();
                const fs::path again = shoot_one("none-again", kPhotoCaptureSha256, false);
                require(
                    !file_contains(again, "\"look\":"), "a look stayed on after it was cleared"
                );
                require(!digest_missed, "a pinned digest did not match");
            } else {
                render_match_surface();
            }
            if (tick == kLeaveTick)
                leave_photo_mode();
        }
        require(!photo_mode(), "photo mode was still open at the last tick");
        DrawReadBack read = match_draw_read_back();
        end_fight();
        return read;
    };
    const DrawReadBack shot = play_pass(true);
    const DrawReadBack quiet = play_pass(false);
    const char* difference = draw_read_back_difference(shot, quiet);
    require(difference == nullptr, difference == nullptr ? std::string() : std::string(difference));
    require(file_bytes(options_.preferences_file) == prefs_before, "the preferences file changed");
    clear_pictures();
    std::printf("photo looks: ok\n");
    std::fflush(stdout);
}

void Runtime::check_photo_replay() {
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("photo replay failed: " + what);
    };
    const auto require = [&](bool ok, const std::string& what) {
        if (!ok)
            fail(what);
    };
    // --hardware-acceleration=full draws the photo in Full. The check that
    // names the flag requires that; a run that does not stays as it is.
    const bool full_photo = options_.hardware_acceleration.value_or(
                                oa::ui::engine_settings::HardwareAcceleration::off
                            ) == oa::ui::engine_settings::HardwareAcceleration::full;
    const auto require_full_photo = [&] {
        if (!full_photo)
            return;
        require(
            photo_capture_machine().kind == oa::present::photo::CaptureKind::full,
            "the photo is not drawn by Full"
        );
    };
    require(sdl_.window != nullptr && sdl_.renderer != nullptr, "the check has no window");
    require(
        options_.user_folder && !options_.user_folder->empty() && !user_folder_.empty(),
        "the run has no folder of the player's own (--user-folder)"
    );
    const fs::path recorded = net_options().play_demo;
    if (recorded.empty()) {
        // A build without the fixture recording (OA_REPLAY_FIXTURES) names none.
        std::cout << "photo replay: skipped: no recording was named (--play-demo)" << std::endl;
        return;
    }

    // Puts the preferences file back, and stops the run writing it on the way out.
    struct PreferencesHold {
        Runtime& runtime;
        bool existed{false};
        std::vector<char> bytes;

        explicit PreferencesHold(Runtime& host) : runtime(host) {
            const auto& path = host.options_.preferences_file;
            if (path.has_value())
                existed = fs::exists(*path);
            bytes = file_bytes(path);
        }

        PreferencesHold(const PreferencesHold&) = delete;
        PreferencesHold& operator=(const PreferencesHold&) = delete;

        ~PreferencesHold() {
            const auto& path = runtime.options_.preferences_file;
            if (path.has_value()) {
                if (!existed) {
                    std::error_code error;
                    fs::remove(*path, error);
                } else {
                    std::ofstream out(*path, std::ios::binary | std::ios::trunc);
                    if (out)
                        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                }
            }
            runtime.preferences_dirty_ = false;
        }
    };

    const PreferencesHold preferences_hold(*this);

    std::ifstream stream(recorded, std::ios::binary);
    require(
        static_cast<bool>(stream),
        "the recording did not open: " + oa::platform::path_to_utf8(recorded)
    );
    const std::vector<uint8_t> recording_bytes{
        std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()
    };
    require(!recording_bytes.empty(), "the recording is empty");
    const oa::base::sha256::Digest file_digest = oa::base::sha256::digest_of(recording_bytes);
    const std::array<char, oa::base::sha256::hex_size> file_hex =
        oa::base::sha256::to_hex(file_digest);
    const std::string file_sha(file_hex.data(), file_hex.size());
    constexpr std::string_view kFixtureSha =
        "b169f5184bf6993055ddf52026ad99cf301130831859ce77d4ef335c0e7b8a8c";
    require(file_sha == kFixtureSha, "the recording's digest is " + file_sha);
    const std::string file_name = oa::platform::path_to_utf8(recorded.filename());
    const std::string parent_text = oa::platform::path_to_utf8(recorded.parent_path());
    require(
        file_name.find('/') == std::string::npos && file_name.find('\\') == std::string::npos,
        "the recording's name has a folder"
    );

    // The full path is kept. The name handed to the playback is the file alone.
    replay_recording_path_ = recorded;
    const RecordingInput input{
        file_name.c_str(), recording_bytes.data(), recording_bytes.size(), true
    };
    ReplayHooks hooks{};
    RecordingInfo info{};
    bool opened = false;
    try {
        opened =
            call_hook_or_raise<&Extension::open_recording>(extension_, *this, input, hooks, info);
    } catch (const std::exception& open_error) {
        fail(std::string("the recording did not play: ") + open_error.what());
    }
    require(opened, "no extension played the recording");

    bool running = true;
    const uint64_t clock_start_ns = frame_pacing::steady_now_ns();
    frame_run_clock_ns_ = clock_start_ns;
    match_timing_.previous_clock = oa::base::game_loop::scaled_clock(
        static_cast<uint32_t>(clock_start_ns / 1'000'000ULL), match_clock_scale()
    );
    match_timing_.remainder = 0.0F;
    const auto bump = [&](uint32_t milliseconds) {
        if (frame_run_clock_ns_)
            *frame_run_clock_ns_ += static_cast<uint64_t>(milliseconds) * 1'000'000ULL;
        run_frame(running);
        require(running, "a frame ended the run");
    };
    const auto read_playback = [&] {
        ReplayPlaybackView playback{};
        const bool filled = call_hook_or_report<&Extension::replay_state>(
            extension_, hook_error_report(), *this, &playback
        );
        require(filled && match_ != nullptr, "the replay state was not read");
        return playback;
    };
    bool started = false;
    for (int frame_index = 0; frame_index < 400; ++frame_index) {
        bump(50);
        if (screen_ == Screen::match && match_ != nullptr &&
            (current_extension_state() & extension_state::replay) != 0) {
            started = true;
            break;
        }
    }
    require(started, "the replay did not start");
    require_full_photo();
    if (LayerScreen* welcome = oa_layer().find("first-run"))
        oa_layer().close(welcome);
    require(
        SDL_SetWindowSize(sdl_.window, 1280, 720) && SDL_SyncWindow(sdl_.window),
        std::string("the window did not resize: ") + SDL_GetError()
    );
    apply_output_mode();
    bump(50);

    call_hook_or_report<&Extension::replay_set_speed>(extension_, hook_error_report(), *this, 1000);
    call_hook_or_report<&Extension::replay_play>(extension_, hook_error_report(), *this);
    bool playing = false;
    for (int frame_index = 0; frame_index < 30; ++frame_index) {
        bump(50);
        const ReplayPlaybackView playback = read_playback();
        const bool bit = (match_->state().game.sim_run_flags & oa::ui::console::kSimRunPaused) != 0;
        if (!playback.paused && !bit && !playback.skipping) {
            playing = true;
            break;
        }
    }
    require(playing, "the replay stayed paused");

    // Open may have written the file. Photo mode must not write it again.
    const std::vector<char> preferences_settled = file_bytes(options_.preferences_file);

    // A seek while the replay is playing keeps going in the frames that wait
    // for it to land. Pause, land on the tick, then play again. Entering
    // photo mode sees that play and does not freeze it.
    call_hook_or_report<&Extension::replay_pause>(extension_, hook_error_report(), *this);
    constexpr uint32_t kSeekTick = 900;
    require(replay_photo_seek(kSeekTick), "the seek was not asked");
    const auto land_on = [&](uint32_t target, const char* why) {
        for (int frame_index = 0; frame_index < 4000; ++frame_index) {
            bump(200);
            const ReplayPlaybackView playback = read_playback();
            const bool pending = replay_photo_seek_pending();
            const uint32_t game_tick = match_->state().game.tick;
            if (!playback.skipping && !pending && playback.tick == target && game_tick == target)
                return;
            if (!playback.skipping && !pending && frame_index > 1 &&
                (playback.tick != target || game_tick != target)) {
                fail(
                    std::string(why) + " landed on tick " + std::to_string(playback.tick) +
                    " game " + std::to_string(game_tick)
                );
            }
        }
        fail(std::string(why) + " did not land");
    };
    land_on(kSeekTick, "the seek to 900");
    call_hook_or_report<&Extension::replay_set_speed>(extension_, hook_error_report(), *this, 1000);
    call_hook_or_report<&Extension::replay_play>(extension_, hook_error_report(), *this);

    require(replay_viewer_command("watch.replay-bar"), "the bar did not pin");
    bump(50);
    const auto listed = [this] {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        return controls;
    };
    const auto find_named = [&](std::string_view name) -> std::optional<AutomationControl> {
        const std::vector<AutomationControl> controls = listed();
        for (const AutomationControl& control : controls) {
            if (control.name == name && control.visible && control.window_width > 0 &&
                control.window_height > 0)
                return control;
        }
        return std::nullopt;
    };
    const std::optional<AutomationControl> photo_button = find_named("oa.replay-viewer.photo");
    if (!photo_button.has_value()) {
        std::string names;
        const std::vector<AutomationControl> controls = listed();
        for (const AutomationControl& control : controls) {
            if (control.name.find("replay-viewer") == std::string::npos)
                continue;
            if (!names.empty())
                names += ' ';
            names += control.name;
        }
        fail("the photo button is not shown: " + names);
    }
    const layout::Point pixel{
        photo_button->window_x + photo_button->window_width / 2,
        photo_button->window_y + photo_button->window_height / 2
    };
    const auto pointer_at = [&](SDL_EventType type, uint8_t button) {
        SDL_Event event = window_pointer_event(sdl_.window, sdl_.renderer, type, pixel, button);
        dispatch_event(event, running);
        require(running, "a pointer event ended the run");
    };
    pointer_at(SDL_EVENT_MOUSE_MOTION, 0);
    pointer_at(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
    pointer_at(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
    bump(50);
    require(photo_mode(), "the photo button did not open photo mode");
    require(!photo_frozen(), "a playing replay opened frozen");
    {
        const std::vector<AutomationControl> controls = listed();
        bool saw_time = false;
        for (const AutomationControl& control : controls) {
            require(
                control.name.find("replay-viewer.") == std::string::npos,
                "the bar stayed in the tree: " + control.name
            );
            if (control.name.find("photo.time-mode") != std::string::npos)
                saw_time = true;
        }
        require(saw_time, "the photo panel's time control is not shown");
    }

    const int32_t origin_x = match_camera_x_;
    const int32_t origin_z = match_camera_z_;
    set_camera_position(origin_x + 80, origin_z + 40, 0);
    bump(50);
    if (match_camera_x_ == origin_x && match_camera_z_ == origin_z)
        set_camera_position(origin_x - 80, origin_z - 40, 0);
    bump(50);
    require(
        match_camera_x_ != origin_x || match_camera_z_ != origin_z,
        "the camera did not move to frame the photo"
    );
    const int32_t framed_x = match_camera_x_;
    const int32_t framed_z = match_camera_z_;
    const auto camera_held = [&](const char* why) {
        require(
            match_camera_x_ == framed_x && match_camera_z_ == framed_z,
            std::string(why) + " moved the camera to " + std::to_string(match_camera_x_) + "," +
                std::to_string(match_camera_z_)
        );
    };

    // The frames that open photo mode may play a tick or two past the seek.
    // The steps are counted from the tick photo mode is standing on.
    const uint32_t at_photo = match_->state().game.tick;
    uint32_t stepped = at_photo;
    for (int step_index = 0; step_index < 3; ++step_index) {
        require(photo_step(), "a step forward was refused at " + std::to_string(stepped));
        ++stepped;
        land_on(stepped, "a step forward");
        camera_held("a step forward");
    }
    for (int step_index = 0; step_index < 2; ++step_index) {
        require(photo_step_back(), "a step back was refused at " + std::to_string(stepped));
        --stepped;
        land_on(stepped, "a step back");
        camera_held("a step back");
    }
    require(stepped == at_photo + 1U, "the steps did not land one tick after the seek");

    render_match_surface();
    present_match_layers();
    require_full_photo();
    take_photo();
    wait_for_photos();
    const std::string saved = saved_photo_name(status_);
    require(!saved.empty(), "no photo was saved: " + status_);
    const fs::path picture = photo_shots_folder() / oa::platform::path_from_utf8(saved);
    std::ifstream picture_file(picture, std::ios::binary);
    require(static_cast<bool>(picture_file), "the photo could not be read");
    const std::vector<char> stored{
        std::istreambuf_iterator<char>(picture_file), std::istreambuf_iterator<char>()
    };
    const std::span<const uint8_t> file(
        reinterpret_cast<const uint8_t*>(stored.data()), stored.size()
    );
    std::vector<oa::formats::png::TextChunk> texts;
    require(oa::formats::png::read_texts(file, &texts), "the photo's text could not be read");
    const std::string shots_text = oa::platform::path_to_generic_utf8(photo_shots_folder());
    const std::string recorded_text = oa::platform::path_to_utf8(recorded);
    std::string source;
    std::string record_text;
    for (const oa::formats::png::TextChunk& chunk : texts) {
        const std::string chunk_text(chunk.text);
        if (!parent_text.empty() && chunk_text.find(parent_text) != std::string::npos)
            fail("the photo names a folder");
        if (!shots_text.empty() && chunk_text.find(shots_text) != std::string::npos)
            fail("the photo names the screenshots folder");
        if (!recorded_text.empty() && recorded_text != file_name &&
            chunk_text.find(recorded_text) != std::string::npos)
            fail("the photo names the recording's path");
        if (chunk.keyword == "Source")
            source = chunk_text;
        if (chunk.keyword == "OpenAnnihilation")
            record_text = chunk_text;
    }
    require(source == "Replay", "Source is " + source);
    oa::formats::json::JsonError json_error;
    const std::optional<oa::formats::json::Json> record =
        oa::formats::json::parse_json(record_text, json_error);
    require(record.has_value(), "the record is not JSON");
    const oa::formats::json::Json* game = record->find("game");
    const oa::formats::json::Json* kind = game == nullptr ? nullptr : game->find("kind");
    const oa::formats::json::Json* game_tick = game == nullptr ? nullptr : game->find("tick");
    require(
        kind != nullptr && kind->string() != nullptr && *kind->string() == "replay",
        "the record's kind is not a replay"
    );
    require(
        game_tick != nullptr && game_tick->integer().has_value() &&
            *game_tick->integer() == static_cast<int64_t>(stepped),
        "the record's tick is not the stepped tick"
    );
    const oa::formats::json::Json* replay = record->find("replay");
    require(replay != nullptr, "the record has no replay");
    const oa::formats::json::Json* replay_file = replay->find("file");
    const oa::formats::json::Json* replay_sha = replay->find("sha256");
    const oa::formats::json::Json* replay_tick = replay->find("tick");
    require(
        replay_file != nullptr && replay_file->string() != nullptr &&
            *replay_file->string() == file_name,
        "the record's file is not the recording's name"
    );
    require(
        replay_file->string()->find('/') == std::string::npos &&
            replay_file->string()->find('\\') == std::string::npos,
        "the record's file names a folder"
    );
    require(
        replay_sha != nullptr && replay_sha->string() != nullptr &&
            *replay_sha->string() == file_sha,
        "the record's digest is not the recording's"
    );
    require(
        replay_tick != nullptr && replay_tick->integer().has_value() &&
            *replay_tick->integer() == static_cast<int64_t>(stepped),
        "the record's replay tick is not the stepped tick"
    );

    // Slow, Freeze and Run are the playback's own speed and pause. The keys
    // step back and jump through the bar's seek, and the framed view stays.
    const auto tap = [&](SDL_Scancode scancode) {
        SDL_Event event{};
        event.key.windowID = SDL_GetWindowID(sdl_.window);
        event.key.scancode = scancode;
        event.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, true);
        event.key.mod = SDL_KMOD_NONE;
        for (const bool down : {true, false}) {
            event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.down = down;
            event.key.repeat = false;
            dispatch_event(event, running);
            require(running, "a key ended the run");
        }
    };
    require(photo_slow(1), "Slow was refused");
    {
        const ReplayPlaybackView playback = read_playback();
        require(
            !playback.paused && playback.asked_milli == 500,
            "Slow did not play the replay at half speed"
        );
        require(
            !photo_frozen() && photo_slow_speed() == 1,
            "photo mode does not read the replay's slow play"
        );
    }
    for (int frame_index = 0; frame_index < 5; ++frame_index)
        bump(200);
    require(match_->state().game.tick > stepped, "the replay did not play slowly");
    camera_held("slow play");
    tap(SDL_SCANCODE_SPACE);
    require(read_playback().paused && photo_frozen(), "Space did not pause the replay");
    tap(SDL_SCANCODE_SPACE);
    {
        const ReplayPlaybackView playback = read_playback();
        require(
            !playback.paused && playback.asked_milli == 1000 && !photo_frozen(),
            "Run did not play at the speed photo mode opened at"
        );
    }
    tap(SDL_SCANCODE_SPACE);
    require(read_playback().paused, "Space did not pause the replay again");
    // A paused frame, so the bar reads the tick the jumps count from.
    bump(50);
    const uint32_t before_scrub = match_->state().game.tick;
    tap(SDL_SCANCODE_J);
    land_on(before_scrub - 300U, "J");
    camera_held("J");
    require(read_playback().paused, "J dropped the pause");
    tap(SDL_SCANCODE_L);
    land_on(before_scrub, "L");
    camera_held("L");
    tap(SDL_SCANCODE_COMMA);
    land_on(before_scrub - 1U, "the comma");
    camera_held("the comma");
    // Leaving keeps this pause and this speed.
    require(photo_slow(1) && photo_freeze(), "Slow, then Freeze, was refused");

    const ReplayPlaybackView before_end = read_playback();
    // A windowed replay leaves the match when the game's outcome is reached,
    // which this recording does before its last recorded tick. The jump stops
    // on a keyframe after the world has settled and before that outcome, so
    // the step back still has a match. It is still a jump back from late in
    // the recording.
    constexpr uint32_t kNearEnd = 42000;
    uint32_t end_target = kNearEnd;
    if (before_end.length_known > 1 && before_end.length_known < kNearEnd)
        end_target = before_end.length_known - 1;
    // The framed camera has to survive this jump, the same as a step.
    arm_photo_replay_camera();
    if (!replay_photo_seek(end_target)) {
        if (photo_ != nullptr) {
            photo_->guard_camera = false;
            photo_->guard_awaiting = false;
        }
        fail("the jump to the end was not asked");
    }
    uint32_t end_tick = 0;
    bool ended = false;
    for (int frame_index = 0; frame_index < 8000; ++frame_index) {
        bump(200);
        const ReplayPlaybackView playback = read_playback();
        end_tick = match_->state().game.tick;
        if (!playback.skipping && !replay_photo_seek_pending() && end_tick > 40000U) {
            ended = true;
            break;
        }
    }
    require(ended, "the jump to the end stayed at tick " + std::to_string(end_tick));
    camera_held("the jump to the end");
    require(photo_step_back(), "the step back from the end was refused");
    bool stepped_end = false;
    uint32_t back_tick = end_tick;
    for (int frame_index = 0; frame_index < 4000; ++frame_index) {
        bump(200);
        const ReplayPlaybackView playback = read_playback();
        back_tick = match_->state().game.tick;
        if (!playback.skipping && !replay_photo_seek_pending() && back_tick > 40000U &&
            back_tick < end_tick) {
            stepped_end = true;
            break;
        }
        if (!playback.skipping && !replay_photo_seek_pending() && frame_index > 1 &&
            (back_tick == 0 || back_tick >= end_tick)) {
            fail("the step back from the end landed on tick " + std::to_string(back_tick));
        }
    }
    require(stepped_end, "the step back from the end stayed at tick " + std::to_string(back_tick));
    camera_held("the step back from the end");

    leave_photo_mode();
    require(!photo_mode(), "photo mode stayed open");
    {
        const ReplayPlaybackView playback = read_playback();
        require(playback.paused, "leaving photo mode unpaused the replay");
        require(playback.asked_milli == 500, "leaving photo mode changed the viewing speed");
        const std::vector<AutomationControl> controls = listed();
        bool saw_play = false;
        for (const AutomationControl& control : controls) {
            if (control.name == "oa.replay-viewer.play")
                saw_play = true;
            require(
                control.name.find("photo.time-mode") == std::string::npos,
                "the photo panel stayed open"
            );
        }
        require(saw_play, "the playback bar did not come back");
    }

    call_hook_or_report<&Extension::replay_set_speed>(extension_, hook_error_report(), *this, 1000);
    call_hook_or_report<&Extension::replay_play>(extension_, hook_error_report(), *this);
    // The recording's pinned end is tick 42965. A windowed frame that reaches
    // the game's outcome leaves the match on that same frame, short of the
    // recording, so the rest is played on the replay clock alone. A short
    // step near the end keeps one frame from passing the pinned tick: the
    // digest mixes the tick in.
    constexpr uint32_t kPinnedEnd = 42965;
    constexpr uint64_t kPinnedDigest = 0x3884654d74a1ad81ULL;
    const auto play_on = [&](uint32_t milliseconds) {
        if (frame_run_clock_ns_)
            *frame_run_clock_ns_ += static_cast<uint64_t>(milliseconds) * 1'000'000ULL;
        begin_loop_frame();
        take_frame_time();
        advance_match_clock(frame_clock_milliseconds());
    };
    uint64_t digest = 0;
    uint32_t final_tick = back_tick;
    bool saw_digest = false;
    for (int frame_index = 0; frame_index < 8000; ++frame_index) {
        if (match_ == nullptr || screen_ != Screen::match)
            break;
        final_tick = match_->state().game.tick;
        if (final_tick >= 40000U) {
            digest = oa::session::demo::demo_world_digest(&match_->state());
            saw_digest = true;
            if (final_tick >= kPinnedEnd)
                break;
        }
        const uint32_t step_ms = final_tick + 60U >= kPinnedEnd ? 16U : 50U;
        play_on(step_ms);
    }
    std::printf(
        "photo replay: tick %u digest %016llx\n",
        final_tick,
        static_cast<unsigned long long>(digest)
    );
    require(saw_digest, "the replay ended at tick " + std::to_string(final_tick));
    require(
        digest == kPinnedDigest,
        "the end digest is " + std::to_string(digest) + " at tick " + std::to_string(final_tick)
    );
    require(
        file_bytes(options_.preferences_file) == preferences_settled, "the preferences file changed"
    );
    if (full_photo)
        std::printf("photo replay: full\n");
    std::printf("photo replay: ok\n");
    std::fflush(stdout);
}

int Runtime::check_photo_tiles() {
    struct PreferencesHold {
        Runtime& runtime;
        bool existed{false};
        std::vector<char> bytes;

        explicit PreferencesHold(Runtime& host) : runtime(host) {
            const auto& preferences = host.options_.preferences_file;
            if (preferences.has_value())
                existed = fs::exists(*preferences);
            bytes = file_bytes(preferences);
        }

        PreferencesHold(const PreferencesHold&) = delete;
        PreferencesHold& operator=(const PreferencesHold&) = delete;

        ~PreferencesHold() {
            const auto& preferences = runtime.options_.preferences_file;
            if (preferences.has_value()) {
                if (!existed) {
                    std::error_code remove_error;
                    fs::remove(*preferences, remove_error);
                } else {
                    std::ofstream out(*preferences, std::ios::binary | std::ios::trunc);
                    if (out)
                        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                }
            }
            runtime.preferences_dirty_ = false;
        }
    };

    struct PictureSweep {
        Runtime& runtime;

        explicit PictureSweep(Runtime& host) : runtime(host) {}

        PictureSweep(const PictureSweep&) = delete;
        PictureSweep& operator=(const PictureSweep&) = delete;

        ~PictureSweep() {
            remove_saved_pictures(screenshots_folder(runtime.user_folder_, ""));
            remove_saved_pictures(screenshots_folder(runtime.user_folder_, "ridge"));
        }
    };

    struct TileEdgeReset {
        int32_t& edge;

        explicit TileEdgeReset(int32_t& held) : edge(held) { edge = 0; }

        TileEdgeReset(const TileEdgeReset&) = delete;
        TileEdgeReset& operator=(const TileEdgeReset&) = delete;

        ~TileEdgeReset() { edge = 0; }
    };

    namespace photo = oa::present::photo;
    namespace settings = oa::ui::engine_settings;

    const PreferencesHold preferences_hold(*this);
    const PictureSweep picture_sweep(*this);
    const TileEdgeReset edge_reset(photo_check_tile_edge_);
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("photo tiles failed: " + what);
    };
    const auto require = [&](bool ok, const std::string& what) {
        if (!ok)
            fail(what);
    };
    const settings::HardwareAcceleration asked =
        options_.hardware_acceleration.value_or(settings::HardwareAcceleration::off);
    require(asked == settings::HardwareAcceleration::full, "needs --hardware-acceleration=full");

    settings::EngineSettings chosen = engine_settings();
    chosen.anti_aliasing = settings::AntiAliasing::x4;
    chosen.weapon_lighting = settings::default_weapon_lighting;
    chosen.wreck_glow = settings::default_wreck_glow;
    chosen.heat_distortion = settings::default_heat_distortion;
    chosen.water_ripples = settings::default_water_ripples;
    chosen.coastal_waves = settings::default_coastal_waves;
    chosen.underwater_shimmer = settings::default_underwater_shimmer;
    chosen.naval_reflections = settings::default_naval_reflections;
    chosen.new_unit_shadows = true;
    chosen.fog_of_war = settings::FogOfWar::smooth;
    apply_engine_settings(chosen);

    const auto step_tick = [this] {
        try {
            step_match_simulation();
        } catch (const std::exception& step_error) {
            report_match_tick_error(step_error.what());
        }
    };
    const auto end_fight = [this] {
        if (photo_mode())
            leave_photo_mode();
        if (match_ != nullptr) {
            leave_match();
            load(Screen::main_menu);
        }
    };

    // Steel Jungle has open water, as the water effects' check uses it.
    exercise_click(menu::resource_name(menu::Button::single_player));
    exercise_click(entry::resource_name(entry::Button::skirmish));
    skirmish_settings_.map_name = "Steel Jungle";
    require(select_map("Steel Jungle") != 0, "cannot select Steel Jungle");
    state_.player_count = 2;
    require(map_player_capacity() >= 2, "Steel Jungle lacks two start positions");
    exercise_click(skirmish::resource_name(skirmish::Button::start));
    require(screen_ == Screen::match && match_ != nullptr, "the fight did not start");
    match_layout_ = lay_out_match(options_.match_width, options_.match_height);
    require(
        least_match_zoom() <= kPlayZoom && most_match_zoom() >= kPlayZoom,
        "zoom 1 is outside the match's play limits"
    );
    match_zoom_ = kPlayZoom;
    match_zoom_target_ = kPlayZoom;
    match_tracking_ = false;
    exact_view_.held = false;
    spawn_combat_armies(kArmy);
    require(match_ != nullptr && selected_tnt_.has_value(), "the fight did not start");

    const auto& world = match_->state();
    const int32_t map_w = world.game.map_width;
    const int32_t map_h = world.game.map_height;
    const int32_t sea = world.game.sea_level;
    const auto wet_at = [&](int32_t x, int32_t z) {
        if (x < 0 || z < 0 || x >= map_w || z >= map_h)
            return false;
        const oa::MapPlot* plot = oa::world_plot(&world, x, z);
        return plot != nullptr && static_cast<int32_t>(plot->height) < sea;
    };
    const auto scan_rect =
        [&](int32_t origin_x, int32_t origin_z, int32_t span_x, int32_t span_z, int32_t stride) {
            for (int32_t z = origin_z; z < origin_z + span_z; z += stride) {
                for (int32_t x = origin_x; x < origin_x + span_x; x += stride) {
                    if (wet_at(x, z))
                        return std::array<int32_t, 2>{x, z};
                }
            }
            return std::array<int32_t, 2>{-1, -1};
        };

    const int32_t seen_w = std::max(visible_map_width(), 1);
    const int32_t seen_h = std::max(visible_map_height(), 1);
    std::array<int32_t, 2> water = scan_rect(match_camera_x_, match_camera_z_, seen_w, seen_h, 16);
    int32_t centre_x = match_camera_x_ + seen_w / 2;
    int32_t centre_z = match_camera_z_ + seen_h / 2;
    bool moved_to_water = false;
    if (water[0] < 0) {
        const std::array<int32_t, 2> anywhere = scan_rect(0, 0, map_w, map_h, 32);
        require(anywhere[0] >= 0, "the map has no water");
        centre_x = std::clamp(anywhere[0], 0, std::max(map_w - 1, 0));
        centre_z = std::clamp(anywhere[1], 0, std::max(map_h - 1, 0));
        for (int32_t dz = -160; dz <= 160 && wet_at(centre_x, centre_z); dz += 32) {
            for (int32_t dx = -160; dx <= 160; dx += 32) {
                const int32_t land_x = std::clamp(anywhere[0] + dx, 0, std::max(map_w - 1, 0));
                const int32_t land_z = std::clamp(anywhere[1] + dz, 0, std::max(map_h - 1, 0));
                if (!wet_at(land_x, land_z)) {
                    centre_x = land_x;
                    centre_z = land_z;
                    break;
                }
            }
        }
        int32_t origin_x = centre_x - seen_w / 2;
        int32_t origin_z = centre_z - seen_h / 2;
        if (map_w > seen_w)
            origin_x = std::clamp(origin_x, 0, map_w - seen_w);
        if (map_h > seen_h)
            origin_z = std::clamp(origin_z, 0, map_h - seen_h);
        match_camera_x_ = origin_x;
        match_camera_z_ = origin_z;
        exact_view_.held = false;
        moved_to_water = true;
        water = scan_rect(match_camera_x_, match_camera_z_, seen_w, seen_h, 16);
    }
    require(water[0] >= 0, "the framed view has no water");

    const auto place_unit = [&](uint16_t type, uint8_t player, int32_t x, int32_t z, int32_t lift) {
        oa::sim::unit_spawn::Request request;
        request.player = player;
        request.type = type;
        request.finished = true;
        request.state = kGroundOccupancyState;
        const uint32_t fixed_x = static_cast<uint32_t>(x) << 16;
        const uint32_t fixed_z = static_cast<uint32_t>(z) << 16;
        request.position = {
            fixed_x,
            static_cast<uint32_t>(match_->map_height(fixed_x, fixed_z) + lift) << 16,
            fixed_z
        };
        return match_->create(request);
    };
    const uint8_t enemy = static_cast<uint8_t>(match_local_player_ == 0 ? 1 : 0);
    if (moved_to_water) {
        const auto own = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMPW");
        const auto foe = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "CORAK");
        require(own != 0 && foe != 0, "the fight lacks a rifle unit");
        for (int index = 0; index < 4; ++index) {
            const int32_t along = centre_z - 48 + index * 24;
            require(
                place_unit(own, match_local_player_, centre_x - 80, along, 0) != nullptr,
                "a rifle unit did not spawn"
            );
            require(
                place_unit(foe, enemy, centre_x + 80, along, 0) != nullptr,
                "a rifle unit did not spawn"
            );
        }
    }

    const auto tank = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMSTUMP");
    require(tank != 0, "the fight lacks ARMSTUMP");
    const int32_t wreck_x = std::clamp(centre_x + 96, 0, std::max(map_w - 1, 0));
    const int32_t wreck_z = std::clamp(centre_z, 0, std::max(map_h - 1, 0));
    auto* wreck_unit = place_unit(tank, match_local_player_, wreck_x, wreck_z, 0);
    require(wreck_unit != nullptr && wreck_unit->unit != nullptr, "the wreck tank did not spawn");
    wreck_unit->unit->object_present = true;
    wreck_unit->unit->health = 1;
    wreck_unit->unit->previous_health_percent = 0;
    const auto wrecks_before = match_->wrecks().size();
    match_->apply_damage_event(*wreck_unit, nullptr, 2, 1, 0);

    uint16_t aircraft_type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMPEEP");
    if (aircraft_type == 0)
        aircraft_type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMFIG");
    require(aircraft_type != 0, "the fight lacks an aircraft");
    const int32_t craft_x = std::clamp(water[0], 0, std::max(map_w - 1, 0));
    const int32_t craft_z = std::clamp(water[1], 0, std::max(map_h - 1, 0));
    auto* aircraft = place_unit(aircraft_type, match_local_player_, craft_x, craft_z, 96);
    require(aircraft != nullptr && aircraft->unit != nullptr, "could not spawn an aircraft");

    bool projectile_light = false;
    bool muzzle_light = false;
    bool impact_light = false;
    bool hot_wreck = false;
    for (int frame = 0;
         frame < 300 && !(projectile_light && muzzle_light && impact_light && hot_wreck);
         ++frame) {
        step_tick();
        require(match_->fault() == nullptr, "the fight raised a simulation fault");
        const auto sources = collect_match_effect_sources();
        for (const auto& source : sources) {
            projectile_light =
                projectile_light || source.origin == renderer_effect_sources::Origin::projectile;
            muzzle_light = muzzle_light || source.origin == renderer_effect_sources::Origin::muzzle;
            impact_light = impact_light || source.origin == renderer_effect_sources::Origin::impact;
            hot_wreck = hot_wreck || source.kind == renderer_effect_sources::Kind::wreck;
        }
        hot_wreck = hot_wreck && match_->wrecks().size() > wrecks_before;
    }
    require(projectile_light, "the fight produced no projectile light");
    require(muzzle_light, "the fight produced no muzzle light");
    require(impact_light, "the fight produced no impact light");
    require(hot_wreck, "the destroyed tank produced no burning wreck");
    require(wet_at(craft_x, craft_z) || water[0] >= 0, "the framed view has no water");

    // The match's own load writes the engine's rows. Put the photo's
    // anti-aliasing and its effects back before the frame is drawn.
    apply_engine_settings(chosen);
    remove_saved_pictures(screenshots_folder(user_folder_, ""));
    remove_saved_pictures(screenshots_folder(user_folder_, "ridge"));
    remove_saved_pictures(photo_shots_folder());
    require(enter_photo_mode(), "photo mode did not open");
    require(photo_mode() && photo_frozen() && photo_ != nullptr, "photo mode was not frozen");
    set_photo_show_selection(false);
    set_photo_show_health_bars(false);
    photo_->look_preset = 0;
    photo_->look_vignette = 0;
    photo_->look_grain = 0;
    photo_->look_frame = 0;
    set_photo_smooth_factor(1);
    set_photo_size_factor(1);
    // Without --force-capable this is the graphics card's run: where no card
    // draws Full, and under SDL's software renderer, it skips. With it, the
    // check runs on whichever renderer the video driver gives.
    const auto software_renderer = [this] {
        return full_ != nullptr && full_->executor.capabilities().geometry_truncates;
    };
    if (!options_.force_capable && (!full_presentation() || software_renderer())) {
        end_fight();
        return 77;
    }
    require(full_presentation() && full_ != nullptr, "Full is not presenting");
    require(
        engine_settings().anti_aliasing == settings::AntiAliasing::x4, "anti-aliasing is not 4×"
    );
    render_match_surface();
    present_match_layers();
    require(full_->supersample > 1, "Full is not supersampling");
    const uint32_t factor = full_->supersample;
    const std::vector<char> preferences_settled = file_bytes(options_.preferences_file);
    const auto preferences_values = preference_values_;
    const uint32_t frozen_tick = match_->simulation().tick;

    const auto take_sized = [&](int size_factor) {
        set_photo_size_factor(size_factor);
        set_photo_smooth_factor(1);
        set_photo_size_factor(size_factor);
        require(
            photo_ != nullptr && photo_->size_factor == size_factor && photo_->smooth_factor == 1,
            "size " + std::to_string(size_factor) + " was not offered"
        );
        take_photo();
        wait_for_photos();
        const std::string saved = saved_photo_name(status_);
        require(!saved.empty(), "status is " + status_);
        const fs::path file = photo_shots_folder() / oa::platform::path_from_utf8(saved);
        std::error_code exists_error;
        require(fs::is_regular_file(file, exists_error), saved);
        return decode_png(file);
    };

    const int32_t field_w = match_layout_.battlefield_width();
    const int32_t field_h = match_layout_.battlefield_height();
    const int32_t framed_camera_x = match_camera_x_;
    const int32_t framed_camera_z = match_camera_z_;
    // Each comparison outside the ruling is kept, and the check still draws
    // the later sizes. One failure at the end names every one of them.
    std::string mismatches;
    bool tiles_matched = true;
    // The photo drawn whole, then again in tiles of at most `edge`, from the
    // camera as it is. At most 0.01% of the pixels may differ (the
    // maintainer's ruling of 2026-11-10), and not along the tile edges: more
    // than a handful of the differing pixels on the edges, and over half of
    // them, is a seam, and fails. `size_factor` is the photo's size. The
    // anti-aliasing factor is the window's at the moment of the photo, which
    // a zoom below 1 and Enhanced 2× each set for themselves.
    const auto compare_tiles = [&](int32_t edge,
                                   int size_factor,
                                   const char* where,
                                   uint64_t per_allowed = kTilePixelsPerAllowed) {
        const int32_t field_w_now = match_layout_.battlefield_width();
        const int32_t field_h_now = match_layout_.battlefield_height();
        const uint32_t factor_now = full_ != nullptr ? full_->supersample : uint32_t{1};
        const DecodedPng whole = take_sized(size_factor);
        require(
            photo_->sized_by_full && !photo_->sized_by_live, "Full did not draw the whole photo"
        );
        require(photo_->sized_supersample == factor_now, "Full lowered the anti-aliasing factor");
        require(
            full_ != nullptr && full_->supersample == factor_now, "the window's factor changed"
        );
        require(
            whole.width ==
                    static_cast<uint32_t>(field_w_now) * static_cast<uint32_t>(size_factor) &&
                whole.height ==
                    static_cast<uint32_t>(field_h_now) * static_cast<uint32_t>(size_factor),
            std::string(where) + ": the photo is not " + std::to_string(size_factor) +
                " times the battlefield"
        );
        photo_check_tile_edge_ = edge;
        const DecodedPng tiled = take_sized(size_factor);
        photo_check_tile_edge_ = 0;
        require(
            photo_->sized_by_full && !photo_->sized_by_live, "Full did not draw the tiled photo"
        );
        require(photo_->sized_supersample == factor_now, "Full lowered the anti-aliasing factor");
        require(
            full_ != nullptr && full_->supersample == factor_now, "the window's factor changed"
        );
        require(
            tiled.width == whole.width && tiled.height == whole.height &&
                tiled.rows.size() == whole.rows.size(),
            "the tiled photo is a different size"
        );
        require(match_->simulation().tick == frozen_tick, "the fight advanced between the photos");

        photo::TileRequest tile_request;
        tile_request.width = static_cast<int32_t>(whole.width);
        tile_request.height = static_cast<int32_t>(whole.height);
        tile_request.zoom =
            (match_zoom_ > 0.0F ? match_zoom_ : 1.0F) * static_cast<float>(size_factor);
        tile_request.supersample = static_cast<int>(factor_now);
        tile_request.grain = static_cast<int>(full_supersampling::target_grain);
        tile_request.texture_limit = static_cast<int32_t>(std::min(
            render_texture_limit(), static_cast<uint32_t>(std::numeric_limits<int32_t>::max())
        ));
        tile_request.tile_edge = edge;
        tile_request.reach = photo::full_tile_reach(tile_request.zoom);
        const photo::TilePlan tile_plan = photo::plan_tiles(tile_request);
        require(
            tile_plan.fits && tile_plan.tiles.size() > 1,
            std::string(where) + ": the check did not draw more than one tile"
        );

        const TileGap gap = measure_tile_gap(
            whole.rows, tiled.rows, whole.width, whole.height, tile_plan, per_allowed
        );
        tiles_matched = gap.within();
        if (gap.different != 0) {
            const std::string found = std::string(where) + ": " + tile_gap_words(gap);
            if (!gap.within()) {
                if (!mismatches.empty())
                    mismatches += "; ";
                mismatches += found;
            }
            std::printf("photo tiles full: %s\n", found.c_str());
            std::fflush(stdout);
        }
        return tile_plan;
    };
    const auto window_frame = [&] {
        renderer::Surface frame;
        photo_->toast_on = false;
        const CaptureHold hold(capture_frame_, frame);
        render_match_surface();
        present_match_layers();
        return frame;
    };

    const int32_t long_side = std::max(field_w, field_h) * 2;
    const int32_t quarter = std::max(int32_t{1}, long_side / 4);
    compare_tiles(quarter, 2, "framed");
    const bool framed_matched = tiles_matched;
    // Two photo pixels longer, the tiles start off the game's draw rows,
    // which are counted from the camera, 16 map pixels each. A tile keeps
    // the whole photo's order of the units and features it draws.
    const renderer::Surface window_before = window_frame();
    const DrawReadBack read_before = match_draw_read_back();
    const photo::TilePlan off_rows = compare_tiles(quarter + 2, 2, "off the draw rows");
    const bool off_rows_matched = tiles_matched;
    bool rows_moved = false;
    for (const photo::Tile& tile : off_rows.tiles)
        rows_moved = rows_moved || tile.map_row % oa::present::world_renderer::draw_row_depth != 0;
    require(rows_moved, "no tile started off the draw rows");
    // The window draws as it did before the tiles. The file name the panel
    // shows carries the clock's second, so its box may change.
    const renderer::Surface window_after = window_frame();
    require(
        window_after.width == window_before.width && window_after.height == window_before.height &&
            window_after.rgb.size() == window_before.rgb.size() && !window_before.rgb.empty(),
        "the window frames differ in size"
    );
    oa::ui::kit::Rect file_box{};
    if (LayerScreen* open = oa_layer().find("photo-mode")) {
        const oa::ui::kit::DisplayList listed = open->display_list();
        for (const oa::ui::kit::Control& control : listed.controls) {
            if (control.name == "photo.file")
                file_box = control.rect;
        }
    }
    const double density =
        sdl_.window == nullptr ? 1.0 : static_cast<double>(SDL_GetWindowPixelDensity(sdl_.window));
    const auto to_px = [&](int32_t points) {
        return static_cast<int64_t>(std::lround(static_cast<double>(points) * density));
    };
    for (uint32_t row = 0; row < window_after.height; ++row) {
        for (uint32_t column = 0; column < window_after.width; ++column) {
            const std::size_t at =
                (static_cast<std::size_t>(row) * window_after.width + column) * 3U;
            if (window_after.rgb[at] == window_before.rgb[at] &&
                window_after.rgb[at + 1U] == window_before.rgb[at + 1U] &&
                window_after.rgb[at + 2U] == window_before.rgb[at + 2U])
                continue;
            const bool in_file =
                column >= to_px(file_box.x) && column < to_px(file_box.x + file_box.width) &&
                row >= to_px(file_box.y) && row < to_px(file_box.y + file_box.height);
            require(
                in_file,
                "the window changed at " + std::to_string(column) + "," + std::to_string(row) +
                    " after a photo in tiles"
            );
        }
    }
    const char* world_difference = draw_read_back_difference(read_before, match_draw_read_back());
    require(
        world_difference == nullptr,
        world_difference == nullptr ? std::string{} : std::string(world_difference)
    );
    std::printf("photo tiles full: the window is as it was\n");
    std::fflush(stdout);
    // Size 3, still framed. An edge of 399 photo pixels is where a model
    // edge left the whole photo; the long side's quarter is the other.
    const int32_t size_three_long = std::max(field_w, field_h) * 3;
    const int32_t size_three_quarter = std::max(int32_t{1}, size_three_long / 4);
    require(size_three_long > 399, "size 3 has no room for an edge of 399");
    compare_tiles(399, 3, "size 3");
    const bool size_three_edge = tiles_matched;
    compare_tiles(size_three_quarter, 3, "size 3 at a quarter");
    if (size_three_edge && tiles_matched) {
        std::printf(
            "photo tiles full: size 3 matches at edges %d and %d\n", 399, size_three_quarter
        );
        std::fflush(stdout);
    }
    // Window zoom 0.45 and size 2 is a photo zoom of 0.9. Full draws that
    // through the moved target, and the tiles must match there too.
    require(
        least_match_zoom() <= 0.45F && most_match_zoom() >= 0.45F,
        "zoom 0.45 is outside the match's play limits"
    );
    match_zoom_ = 0.45F;
    match_zoom_target_ = 0.45F;
    exact_view_.held = false;
    render_match_surface();
    present_match_layers();
    // On SDL's software renderer this one case has the wider bound, and the
    // check says so. A graphics card is held to 0.01% here too.
    const bool software_bound = software_renderer();
    if (software_bound) {
        std::printf(
            "photo tiles full: below zoom 1 on SDL's software renderer is held to 0.5%% of the "
            "pixels, not 0.01%%: only checks use that renderer, and it places a tile's pixels "
            "differently from the whole photo's here; native-photo-tiles-full-gpu holds the case "
            "to 0.01%% on a graphics card\n"
        );
        std::fflush(stdout);
    }
    compare_tiles(
        quarter,
        2,
        "below zoom 1",
        software_bound ? kSoftwareBelowZoomPixelsPerAllowed : kTilePixelsPerAllowed
    );
    const bool below_matched = tiles_matched;
    match_zoom_ = kPlayZoom;
    match_zoom_target_ = kPlayZoom;
    match_camera_x_ = framed_camera_x;
    match_camera_z_ = framed_camera_z;
    exact_view_.held = false;
    render_match_surface();
    present_match_layers();
    if (below_matched) {
        std::printf("photo tiles full: below zoom 1 matches at edge %d\n", quarter);
        std::fflush(stdout);
    }
    // Enhanced 2×, framed, then back to 4× for the corner.
    bool enhanced_matched = false;
    {
        settings::EngineSettings two_samples = engine_settings();
        two_samples.anti_aliasing = settings::AntiAliasing::x2;
        apply_engine_settings(two_samples);
        render_match_surface();
        present_match_layers();
        require(full_ != nullptr && full_->supersample == 2, "Enhanced 2× did not take");
        compare_tiles(quarter, 2, "enhanced 2x");
        enhanced_matched = tiles_matched;
        apply_engine_settings(chosen);
        render_match_surface();
        present_match_layers();
        require(full_ != nullptr && full_->supersample == factor, "Enhanced 4× did not return");
    }
    if (enhanced_matched) {
        std::printf("photo tiles full: enhanced 2x matches at edge %d\n", quarter);
        std::fflush(stdout);
    }
    // At the map's corner, the view half past its edges as the window's
    // limits hold it. A tile shows its part of that view, not another part
    // of the map.
    const auto past_the_corner = [&] {
        match_camera_x_ = std::numeric_limits<int32_t>::min() / 2;
        match_camera_z_ = std::numeric_limits<int32_t>::min() / 2;
        exact_view_.held = false;
        view_hold_ = {};
        render_match_surface();
        present_match_layers();
        require(
            match_camera_x_ < 0 && match_camera_z_ < 0,
            "the view does not reach past the map's corner"
        );
    };
    past_the_corner();
    compare_tiles(quarter, 2, "past the map's corner");
    if (framed_matched && off_rows_matched && tiles_matched) {
        std::printf(
            "photo tiles full: whole and tiled match at edges %d and %d, and past the map's "
            "corner at %d,%d\n",
            quarter,
            quarter + 2,
            match_camera_x_,
            match_camera_z_
        );
        std::fflush(stdout);
    }
    // The same corner at Enhanced 2×. A model edge there is part of the
    // tile comparison.
    bool enhanced_corner_matched = false;
    {
        settings::EngineSettings two_samples = engine_settings();
        two_samples.anti_aliasing = settings::AntiAliasing::x2;
        apply_engine_settings(two_samples);
        past_the_corner();
        require(full_ != nullptr && full_->supersample == 2, "Enhanced 2× did not take");
        compare_tiles(quarter, 2, "enhanced 2x past the corner");
        enhanced_corner_matched = tiles_matched;
        apply_engine_settings(chosen);
        render_match_surface();
        present_match_layers();
        require(full_ != nullptr && full_->supersample == factor, "Enhanced 4× did not return");
    }
    if (enhanced_corner_matched) {
        std::printf("photo tiles full: enhanced 2x past the corner matches at edge %d\n", quarter);
        std::fflush(stdout);
    }
    // 1920×1080, framed and past the corner. The window is put back after.
    const int32_t opened_width = options_.match_width;
    const int32_t opened_height = options_.match_height;
    require(
        sdl_.window != nullptr && SDL_SetWindowSize(sdl_.window, 1920, 1080) &&
            SDL_SyncWindow(sdl_.window),
        std::string("the window did not resize: ") + SDL_GetError()
    );
    options_.match_width = 1920;
    options_.match_height = 1080;
    match_layout_ = lay_out_match(1920, 1080);
    match_camera_x_ = framed_camera_x;
    match_camera_z_ = framed_camera_z;
    match_zoom_ = kPlayZoom;
    match_zoom_target_ = kPlayZoom;
    exact_view_.held = false;
    render_match_surface();
    present_match_layers();
    const int32_t wide_long =
        std::max(match_layout_.battlefield_width(), match_layout_.battlefield_height()) * 2;
    const int32_t wide_quarter = std::max(int32_t{1}, wide_long / 4);
    compare_tiles(wide_quarter, 2, "1920x1080");
    const bool wide_matched = tiles_matched;
    if (wide_matched) {
        std::printf("photo tiles full: 1920x1080 matches at edge %d\n", wide_quarter);
        std::fflush(stdout);
    }
    past_the_corner();
    compare_tiles(wide_quarter, 2, "1920x1080 past the corner");
    if (tiles_matched) {
        std::printf(
            "photo tiles full: 1920x1080 past the corner matches at edge %d\n", wide_quarter
        );
        std::fflush(stdout);
    }
    require(
        SDL_SetWindowSize(sdl_.window, opened_width, opened_height) && SDL_SyncWindow(sdl_.window),
        std::string("the window did not return: ") + SDL_GetError()
    );
    options_.match_width = opened_width;
    options_.match_height = opened_height;
    match_layout_ = lay_out_match(opened_width, opened_height);
    require(
        file_bytes(options_.preferences_file) == preferences_settled &&
            preference_values_ == preferences_values,
        "the preferences file changed"
    );
    if (!mismatches.empty())
        fail(mismatches);
    end_fight();
    std::printf("photo tiles full: ok\n");
    std::fflush(stdout);
    return 0;
}

int Runtime::check_photo_tiles_live() {
    struct PreferencesHold {
        Runtime& runtime;
        bool existed{false};
        std::vector<char> bytes;

        explicit PreferencesHold(Runtime& host) : runtime(host) {
            const auto& preferences = host.options_.preferences_file;
            if (preferences.has_value())
                existed = fs::exists(*preferences);
            bytes = file_bytes(preferences);
        }

        PreferencesHold(const PreferencesHold&) = delete;
        PreferencesHold& operator=(const PreferencesHold&) = delete;

        ~PreferencesHold() {
            const auto& preferences = runtime.options_.preferences_file;
            if (preferences.has_value()) {
                if (!existed) {
                    std::error_code remove_error;
                    fs::remove(*preferences, remove_error);
                } else {
                    std::ofstream out(*preferences, std::ios::binary | std::ios::trunc);
                    if (out)
                        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                }
            }
            runtime.preferences_dirty_ = false;
        }
    };

    struct PictureSweep {
        Runtime& runtime;

        explicit PictureSweep(Runtime& host) : runtime(host) {}

        PictureSweep(const PictureSweep&) = delete;
        PictureSweep& operator=(const PictureSweep&) = delete;

        ~PictureSweep() {
            remove_saved_pictures(screenshots_folder(runtime.user_folder_, ""));
            remove_saved_pictures(screenshots_folder(runtime.user_folder_, "ridge"));
        }
    };

    struct TileEdgeReset {
        int32_t& edge;

        explicit TileEdgeReset(int32_t& held) : edge(held) { edge = 0; }

        TileEdgeReset(const TileEdgeReset&) = delete;
        TileEdgeReset& operator=(const TileEdgeReset&) = delete;

        ~TileEdgeReset() { edge = 0; }
    };

    namespace photo = oa::present::photo;
    namespace settings = oa::ui::engine_settings;

    const PreferencesHold preferences_hold(*this);
    const PictureSweep picture_sweep(*this);
    const TileEdgeReset edge_reset(photo_check_tile_edge_);
    const TileEdgeReset viewport_reset(photo_check_viewport_limit_);
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("photo tiles failed: " + what);
    };
    const auto require = [&](bool ok, const std::string& what) {
        if (!ok)
            fail(what);
    };
    const settings::HardwareAcceleration asked =
        options_.hardware_acceleration.value_or(settings::HardwareAcceleration::off);
    require(asked == settings::HardwareAcceleration::full, "needs --hardware-acceleration=full");

    const int32_t opened_width = options_.match_width;
    const int32_t opened_height = options_.match_height;
    const auto resize_window = [&](int32_t width, int32_t height) {
        require(
            sdl_.window != nullptr && SDL_SetWindowSize(sdl_.window, width, height) &&
                SDL_SyncWindow(sdl_.window),
            std::string("the window did not resize: ") + SDL_GetError()
        );
        options_.match_width = width;
        options_.match_height = height;
    };
    resize_window(1024, 1024);

    settings::EngineSettings chosen = engine_settings();
    chosen.allow_full_3d = true;
    chosen.anti_aliasing = settings::AntiAliasing::off;
    chosen.weapon_lighting = settings::default_weapon_lighting;
    chosen.wreck_glow = settings::default_wreck_glow;
    chosen.heat_distortion = settings::default_heat_distortion;
    chosen.water_ripples = settings::default_water_ripples;
    chosen.coastal_waves = settings::default_coastal_waves;
    chosen.underwater_shimmer = settings::default_underwater_shimmer;
    chosen.naval_reflections = settings::default_naval_reflections;
    chosen.new_unit_shadows = true;
    apply_engine_settings(chosen);

    const auto step_tick = [this] {
        try {
            step_match_simulation();
        } catch (const std::exception& step_error) {
            report_match_tick_error(step_error.what());
        }
    };
    const auto end_fight = [this] {
        if (photo_mode())
            leave_photo_mode();
        if (match_ != nullptr) {
            leave_match();
            load(Screen::main_menu);
        }
    };

    exercise_click(menu::resource_name(menu::Button::single_player));
    exercise_click(entry::resource_name(entry::Button::skirmish));
    skirmish_settings_.map_name = "Steel Jungle";
    require(select_map("Steel Jungle") != 0, "cannot select Steel Jungle");
    state_.player_count = 2;
    require(map_player_capacity() >= 2, "Steel Jungle lacks two start positions");
    exercise_click(skirmish::resource_name(skirmish::Button::start));
    require(screen_ == Screen::match && match_ != nullptr, "the fight did not start");
    match_layout_ = lay_out_match(1024, 1024);
    require(
        least_match_zoom() <= kPlayZoom && most_match_zoom() >= kPlayZoom,
        "zoom 1 is outside the match's play limits"
    );
    match_zoom_ = kPlayZoom;
    match_zoom_target_ = kPlayZoom;
    match_tracking_ = false;
    exact_view_.held = false;
    spawn_combat_armies(kArmy);
    require(match_ != nullptr && selected_tnt_.has_value(), "the fight did not start");

    const auto& world = match_->state();
    const int32_t map_w = world.game.map_width;
    const int32_t map_h = world.game.map_height;
    const int32_t sea = world.game.sea_level;
    const auto wet_at = [&](int32_t x, int32_t z) {
        if (x < 0 || z < 0 || x >= map_w || z >= map_h)
            return false;
        const oa::MapPlot* plot = oa::world_plot(&world, x, z);
        return plot != nullptr && static_cast<int32_t>(plot->height) < sea;
    };
    const auto scan_rect =
        [&](int32_t origin_x, int32_t origin_z, int32_t span_x, int32_t span_z, int32_t stride) {
            for (int32_t z = origin_z; z < origin_z + span_z; z += stride) {
                for (int32_t x = origin_x; x < origin_x + span_x; x += stride) {
                    if (wet_at(x, z))
                        return std::array<int32_t, 2>{x, z};
                }
            }
            return std::array<int32_t, 2>{-1, -1};
        };
    const int32_t seen_w = std::max(visible_map_width(), 1);
    const int32_t seen_h = std::max(visible_map_height(), 1);
    std::array<int32_t, 2> water = scan_rect(match_camera_x_, match_camera_z_, seen_w, seen_h, 16);
    int32_t centre_x = match_camera_x_ + seen_w / 2;
    int32_t centre_z = match_camera_z_ + seen_h / 2;
    if (water[0] < 0) {
        const std::array<int32_t, 2> anywhere = scan_rect(0, 0, map_w, map_h, 32);
        require(anywhere[0] >= 0, "the map has no water");
        centre_x = std::clamp(anywhere[0], 0, std::max(map_w - 1, 0));
        centre_z = std::clamp(anywhere[1], 0, std::max(map_h - 1, 0));
        int32_t origin_x = centre_x - seen_w / 2;
        int32_t origin_z = centre_z - seen_h / 2;
        if (map_w > seen_w)
            origin_x = std::clamp(origin_x, 0, map_w - seen_w);
        if (map_h > seen_h)
            origin_z = std::clamp(origin_z, 0, map_h - seen_h);
        match_camera_x_ = origin_x;
        match_camera_z_ = origin_z;
        exact_view_.held = false;
        water = scan_rect(match_camera_x_, match_camera_z_, seen_w, seen_h, 16);
    }
    require(water[0] >= 0, "the framed view has no water");

    const auto place_unit = [&](uint16_t type, uint8_t player, int32_t x, int32_t z, int32_t lift) {
        oa::sim::unit_spawn::Request request;
        request.player = player;
        request.type = type;
        request.finished = true;
        request.state = kGroundOccupancyState;
        const uint32_t fixed_x = static_cast<uint32_t>(x) << 16;
        const uint32_t fixed_z = static_cast<uint32_t>(z) << 16;
        request.position = {
            fixed_x,
            static_cast<uint32_t>(match_->map_height(fixed_x, fixed_z) + lift) << 16,
            fixed_z
        };
        return match_->create(request);
    };
    const auto tank = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMSTUMP");
    require(tank != 0, "the fight lacks ARMSTUMP");
    const int32_t wreck_x = std::clamp(centre_x + 96, 0, std::max(map_w - 1, 0));
    const int32_t wreck_z = std::clamp(centre_z, 0, std::max(map_h - 1, 0));
    auto* wreck_unit = place_unit(tank, match_local_player_, wreck_x, wreck_z, 0);
    require(wreck_unit != nullptr && wreck_unit->unit != nullptr, "the wreck tank did not spawn");
    wreck_unit->unit->object_present = true;
    wreck_unit->unit->health = 1;
    wreck_unit->unit->previous_health_percent = 0;
    const auto wrecks_before = match_->wrecks().size();
    match_->apply_damage_event(*wreck_unit, nullptr, 2, 1, 0);
    uint16_t aircraft_type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMPEEP");
    if (aircraft_type == 0)
        aircraft_type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMFIG");
    require(aircraft_type != 0, "the fight lacks an aircraft");
    auto* aircraft = place_unit(
        aircraft_type,
        match_local_player_,
        std::clamp(water[0], 0, std::max(map_w - 1, 0)),
        std::clamp(water[1], 0, std::max(map_h - 1, 0)),
        96
    );
    require(aircraft != nullptr && aircraft->unit != nullptr, "could not spawn an aircraft");

    bool projectile_light = false;
    bool hot_wreck = false;
    for (int frame = 0; frame < 300 && !(projectile_light && hot_wreck); ++frame) {
        step_tick();
        require(match_->fault() == nullptr, "the fight raised a simulation fault");
        for (const auto& source : collect_match_effect_sources()) {
            projectile_light =
                projectile_light || source.origin == renderer_effect_sources::Origin::projectile;
            hot_wreck = hot_wreck || source.kind == renderer_effect_sources::Kind::wreck;
        }
        hot_wreck = hot_wreck && match_->wrecks().size() > wrecks_before;
    }
    require(projectile_light, "the fight produced no projectile light");
    require(hot_wreck, "the destroyed tank produced no burning wreck");

    apply_engine_settings(chosen);
    remove_saved_pictures(screenshots_folder(user_folder_, ""));
    remove_saved_pictures(screenshots_folder(user_folder_, "ridge"));
    remove_saved_pictures(photo_shots_folder());
    require(enter_photo_mode(), "photo mode did not open");
    require(photo_mode() && photo_frozen() && photo_ != nullptr, "photo mode was not frozen");
    match_layout_ = layout::make_battlefield_layout(1024, 1024);
    require(
        match_layout_.battlefield_width() == 1024 && match_layout_.battlefield_height() == 1024,
        "the battlefield is not 1024 pixels"
    );
    set_photo_show_selection(false);
    set_photo_show_health_bars(false);
    photo_->look_preset = 0;
    photo_->look_vignette = 0;
    photo_->look_grain = 0;
    photo_->look_frame = 0;
    photo_->look_depth_of_field = 0;
    photo_->look_haze = 0;
    photo_->guide = 0;
    set_photo_smooth_factor(1);
    set_photo_size_factor(1);

    bool turned = false;
    try {
        turned = turn_live_camera(20.0F, 12.0F);
    } catch (const std::exception&) {
        turned = false;
    }
    bool skip = false;
    try {
        render_match_surface();
    } catch (const std::exception&) {
        skip = true;
    }
    if (!turned || skip || live_renderer_unavailable() || !live_renderer_active() ||
        live_renderer_failed()) {
        end_fight();
        return 77;
    }
    const std::vector<char> preferences_settled = file_bytes(options_.preferences_file);
    const auto preferences_values = preference_values_;
    const uint32_t frozen_tick = match_->simulation().tick;
    const int32_t framed_camera_x = match_camera_x_;
    const int32_t framed_camera_z = match_camera_z_;

    const auto take_sized = [&](int size_factor) {
        set_photo_size_factor(size_factor);
        set_photo_smooth_factor(1);
        set_photo_size_factor(size_factor);
        require(
            photo_ != nullptr && photo_->size_factor == size_factor && photo_->smooth_factor == 1,
            "size " + std::to_string(size_factor) + " was not offered"
        );
        take_photo();
        wait_for_photos();
        const std::string saved = saved_photo_name(status_);
        require(!saved.empty(), "status is " + status_);
        const fs::path file = photo_shots_folder() / oa::platform::path_from_utf8(saved);
        std::error_code exists_error;
        require(fs::is_regular_file(file, exists_error), saved);
        return decode_png(file);
    };
    const auto turned_plan_for = [&](int size_factor, int32_t edge) {
        const int32_t field_w = match_layout_.battlefield_width();
        const int32_t field_h = match_layout_.battlefield_height();
        const photo::Machine machine = photo_capture_machine();
        photo::TurnedTileRequest request;
        request.width = field_w * size_factor;
        request.height = field_h * size_factor;
        request.zoom = (match_zoom_ > 0.0F ? match_zoom_ : 1.0F) * static_cast<float>(size_factor);
        request.draw_limit = photo::turned_tile_draw_limit(machine);
        request.tile_edge = edge;
        request.blur =
            photo::photo_tilt_radius(std::clamp(photo_->look_depth_of_field, 0, 32), size_factor);
        return photo::plan_turned_tiles(request);
    };
    const auto compare_tiles = [&](int32_t edge,
                                   int size_factor,
                                   const char* where,
                                   int32_t viewport_cap = 0,
                                   bool use_tolerance = false,
                                   bool seam_only = false) {
        // Screen size is the window's picture unless a check asks to draw
        // it, which a turned photo at the sampling edge has to, so the tiles
        // compare with that draw.
        struct SizedCapture {
            Runtime& self;

            explicit SizedCapture(Runtime& runtime) : self(runtime) {
                self.photo_check_sized_capture_ = true;
            }

            ~SizedCapture() { self.photo_check_sized_capture_ = false; }
        } sized_capture{*this};
        const int32_t field_w = match_layout_.battlefield_width();
        const int32_t field_h = match_layout_.battlefield_height();
        photo_check_tile_edge_ = 0;
        const DecodedPng whole = take_sized(size_factor);
        require(
            photo_->sized_by_live && !photo_->sized_by_full,
            "the turned view did not draw the photo"
        );
        require(
            whole.width == static_cast<uint32_t>(field_w) * static_cast<uint32_t>(size_factor) &&
                whole.height == static_cast<uint32_t>(field_h) * static_cast<uint32_t>(size_factor),
            std::string(where) + ": the photo is not " + std::to_string(size_factor) +
                " times the battlefield"
        );
        photo_check_tile_edge_ = edge;
        photo_check_viewport_limit_ = viewport_cap;
        photo_check_tiles_drawn_ = 0;
        photo_check_tiles_shifted_ = 0;
        photo_check_widest_tile_viewport_ = 0;
        const DecodedPng tiled = take_sized(size_factor);
        photo_check_tile_edge_ = 0;
        photo_check_viewport_limit_ = 0;
        require(
            photo_->sized_by_live && !photo_->sized_by_full,
            "the turned view did not draw the tiles"
        );
        // A forced viewport is the card's to keep: every tile the card drew
        // had its projection offset, through a viewport no wider than that.
        if (viewport_cap > 0) {
            require(
                photo_check_tiles_drawn_ > 1 &&
                    photo_check_tiles_shifted_ == photo_check_tiles_drawn_ &&
                    photo_check_widest_tile_viewport_ <= static_cast<uint32_t>(viewport_cap),
                std::string(where) + ": the card drew " +
                    std::to_string(photo_check_tiles_shifted_) + " of " +
                    std::to_string(photo_check_tiles_drawn_) +
                    " tiles offset, the widest through " +
                    std::to_string(photo_check_widest_tile_viewport_) + " of " +
                    std::to_string(viewport_cap)
            );
            std::printf(
                "photo tiles live: %s: %u tiles offset, the widest viewport %u of %d\n",
                where,
                photo_check_tiles_shifted_,
                photo_check_widest_tile_viewport_,
                viewport_cap
            );
            std::fflush(stdout);
        }
        require(
            tiled.width == whole.width && tiled.height == whole.height &&
                tiled.rows.size() == whole.rows.size(),
            "the tiled photo is a different size"
        );
        require(match_->simulation().tick == frozen_tick, "the fight advanced between the photos");
        const photo::TilePlan plan = turned_plan_for(size_factor, edge);
        require(
            plan.fits && plan.tiles.size() > 1,
            std::string(where) + ": the check did not draw more than one tile"
        );
        for (const photo::Tile& tile : plan.tiles) {
            require(
                tile.draw_width <= photo::kTurnedEdge && tile.draw_height <= photo::kTurnedEdge,
                std::string(where) + ": a tile draws past 4096"
            );
            require(
                tile.draw_x % photo::kLiveLightTile == 0 &&
                    tile.draw_y % photo::kLiveLightTile == 0,
                std::string(where) + ": a tile misses the light grid"
            );
        }
        const TileGap gap =
            measure_tile_gap(whole.rows, tiled.rows, whole.width, whole.height, plan);
        if (gap.different != 0) {
            std::printf("photo tiles live: %s: %s\n", where, tile_gap_words(gap).c_str());
            std::fflush(stdout);
        }
        if (seam_only) {
            require(!gap.seam(), std::string(where) + ": " + tile_gap_words(gap));
        } else if (use_tolerance) {
            require(gap.within(), std::string(where) + ": " + tile_gap_words(gap));
        } else {
            require(gap.on_edge == 0, std::string(where) + ": " + tile_gap_words(gap));
            require(gap.different <= gap.allowed, std::string(where) + ": " + tile_gap_words(gap));
        }
        return plan;
    };
    const auto window_frame = [&] {
        renderer::Surface frame;
        photo_->toast_on = false;
        const CaptureHold hold(capture_frame_, frame);
        render_match_surface();
        present_match_layers();
        return frame;
    };
    const auto one_piece_factor = [&] {
        const photo::Machine machine = photo_capture_machine();
        const int32_t cap = machine.edge_limit > 0
                                ? std::min(machine.edge_limit, photo::kTurnedEdge)
                                : photo::kTurnedEdge;
        const int32_t field_w = match_layout_.battlefield_width();
        const int32_t field_h = match_layout_.battlefield_height();
        for (const int candidate : {4, 3, 2, 1}) {
            if (field_w * candidate <= cap && field_h * candidate <= cap)
                return candidate;
        }
        return 1;
    };

    const renderer::Surface window_before = window_frame();
    const photo::TilePlan halved = compare_tiles(2048, 4, "4096");
    require(halved.tiles.size() == 4, "4096 did not split into four tiles");
    for (const photo::Tile& tile : halved.tiles)
        require(tile.width == 2048 && tile.height == 2048, "a 4096 tile did not keep 2048");
    require(
        halved.border >= photo::turned_tile_border(0) && halved.border % photo::kLiveLightTile == 0,
        "the 4096 border does not cover the lights"
    );
    std::printf("photo tiles live: 4096 matches in four tiles of 2048\n");
    std::fflush(stdout);

    photo_->look_depth_of_field = 32;
    const photo::TilePlan blurred = compare_tiles(2048, 4, "4096 depth of field");
    require(blurred.tiles.size() == 4, "depth of field did not keep four tiles");
    require(
        blurred.border >= photo::turned_tile_border(photo::photo_tilt_radius(32, 4)),
        "the blur border is short"
    );
    photo_->look_depth_of_field = 0;
    int32_t widest_draw = 1;
    for (const photo::TilePlan& planned : {halved, blurred}) {
        for (const photo::Tile& tile : planned.tiles)
            widest_draw = std::max(widest_draw, std::max(tile.draw_width, tile.draw_height));
    }
    const int64_t play_samples =
        static_cast<int64_t>(std::max(turned_frame_sampling().supersample, uint32_t{1}));
    const int64_t tile_view = static_cast<int64_t>(widest_draw) * play_samples;
    const int64_t photo_view = int64_t{4096} * play_samples;
    require(
        tile_view < photo_view && photo_view <= std::numeric_limits<int32_t>::max(),
        "the tile viewport is not inside the photo's"
    );
    const int32_t forced = static_cast<int32_t>(tile_view + (photo_view - tile_view) / 2);
    require(
        forced > tile_view && forced < photo_view, "the forced viewport is not between the two"
    );
    const auto within_forced = [&](const photo::TilePlan& planned) {
        for (const photo::Tile& tile : planned.tiles) {
            const photo::TurnedViewport window = photo::turned_tile_viewport(
                4096,
                4096,
                tile.draw_x,
                tile.draw_y,
                tile.draw_width,
                tile.draw_height,
                static_cast<int32_t>(play_samples),
                forced,
                -1.0e8F,
                1.0e8F
            );
            require(
                window.shifted && window.fits && window.width <= static_cast<float>(forced) &&
                    window.height <= static_cast<float>(forced),
                "the forced limit did not move the viewport onto the tile"
            );
        }
    };
    const photo::TilePlan forced_halved = compare_tiles(2048, 4, "4096 within the card", forced);
    require(forced_halved.tiles.size() == 4, "the card's viewport did not keep four tiles");
    for (const photo::Tile& tile : forced_halved.tiles)
        require(tile.width == 2048 && tile.height == 2048, "a forced tile did not keep 2048");
    within_forced(forced_halved);
    std::printf("photo tiles live: 4096 within the card matches in four tiles of 2048\n");
    std::fflush(stdout);
    photo_->look_depth_of_field = 32;
    const photo::TilePlan forced_blur =
        compare_tiles(2048, 4, "4096 depth of field within the card", forced, true);
    require(forced_blur.tiles.size() == 4, "depth of field within the card lost a tile");
    within_forced(forced_blur);
    photo_->look_depth_of_field = 0;
    std::printf("photo tiles live: 4096 with depth of field stays within the card\n");
    std::fflush(stdout);
    // The size the panel shows is the check's own, put back so the window
    // compare sees the frame it drew before the photos.
    set_photo_size_factor(1);
    std::printf("photo tiles live: 4096 with depth of field matches\n");
    std::fflush(stdout);

    const renderer::Surface window_after = window_frame();
    require(
        window_after.width == window_before.width && window_after.height == window_before.height &&
            window_after.rgb.size() == window_before.rgb.size() && !window_before.rgb.empty(),
        "the window frames differ in size"
    );
    oa::ui::kit::Rect file_box{};
    if (LayerScreen* open = oa_layer().find("photo-mode")) {
        const oa::ui::kit::DisplayList listed = open->display_list();
        for (const oa::ui::kit::Control& control : listed.controls) {
            if (control.name == "photo.file")
                file_box = control.rect;
        }
    }
    const double density =
        sdl_.window == nullptr ? 1.0 : static_cast<double>(SDL_GetWindowPixelDensity(sdl_.window));
    const auto to_px = [&](int32_t points) {
        return static_cast<int64_t>(std::lround(static_cast<double>(points) * density));
    };
    for (uint32_t row = 0; row < window_after.height; ++row) {
        for (uint32_t column = 0; column < window_after.width; ++column) {
            const std::size_t at =
                (static_cast<std::size_t>(row) * window_after.width + column) * 3U;
            if (window_after.rgb[at] == window_before.rgb[at] &&
                window_after.rgb[at + 1U] == window_before.rgb[at + 1U] &&
                window_after.rgb[at + 2U] == window_before.rgb[at + 2U])
                continue;
            const bool in_file =
                column >= to_px(file_box.x) && column < to_px(file_box.x + file_box.width) &&
                row >= to_px(file_box.y) && row < to_px(file_box.y + file_box.height);
            require(
                in_file,
                "the window changed at " + std::to_string(column) + "," + std::to_string(row) +
                    " after a photo in tiles"
            );
        }
    }

    match_camera_x_ = std::numeric_limits<int32_t>::min() / 2;
    match_camera_z_ = std::numeric_limits<int32_t>::min() / 2;
    exact_view_.held = false;
    view_hold_ = {};
    render_match_surface();
    present_match_layers();
    require(
        match_camera_x_ < 0 && match_camera_z_ < 0, "the view does not reach past the map's corner"
    );
    const int32_t corner_long =
        std::max(match_layout_.battlefield_width(), match_layout_.battlefield_height()) * 2;
    compare_tiles(std::max(int32_t{32}, corner_long / 4), 2, "past the map's corner");
    std::printf(
        "photo tiles live: past the corner matches at %d,%d\n", match_camera_x_, match_camera_z_
    );
    std::fflush(stdout);
    match_camera_x_ = framed_camera_x;
    match_camera_z_ = framed_camera_z;
    exact_view_.held = false;
    render_match_surface();

    for (const settings::AntiAliasing level :
         {settings::AntiAliasing::x2, settings::AntiAliasing::x4}) {
        settings::EngineSettings samples = engine_settings();
        samples.anti_aliasing = level;
        apply_engine_settings(samples);
        render_match_surface();
        const int factor = one_piece_factor();
        const int32_t photo_long =
            std::max(match_layout_.battlefield_width(), match_layout_.battlefield_height()) *
            factor;
        const int32_t edge = std::max(int32_t{32}, photo_long / 4);
        require(edge < photo_long, "enhanced anti-aliasing left no tile edge");
        const uint32_t level_supersample = std::max(turned_frame_sampling().supersample, 1U);
        const photo::TilePlan enhanced_plan = compare_tiles(
            edge, factor, level == settings::AntiAliasing::x2 ? "enhanced 2x" : "enhanced 4x"
        );
        std::printf(
            "photo tiles live: enhanced %dx matches size %d at edge %d\n",
            static_cast<int>(level),
            factor,
            edge
        );
        std::fflush(stdout);
        // The same tiles through a viewport shorter than the whole photo's,
        // at this level's supersample: the projection is offset onto each.
        // Offset tiles round their vertices differently from the photo drawn
        // whole, more so with more samples a pixel, so this pass holds the
        // seam rule and reports the count: offset tiles have no count bound
        // of their own yet.
        int32_t enhanced_widest = 1;
        for (const photo::Tile& tile : enhanced_plan.tiles)
            enhanced_widest =
                std::max(enhanced_widest, std::max(tile.draw_width, tile.draw_height));
        const int64_t enhanced_tile_view =
            static_cast<int64_t>(enhanced_widest) * static_cast<int64_t>(level_supersample);
        const int64_t enhanced_photo_view =
            static_cast<int64_t>(photo_long) * static_cast<int64_t>(level_supersample);
        require(
            enhanced_tile_view < enhanced_photo_view &&
                enhanced_photo_view <= std::numeric_limits<int32_t>::max(),
            "the enhanced tile viewport is not inside the photo's"
        );
        const auto enhanced_forced = static_cast<int32_t>(
            enhanced_tile_view + (enhanced_photo_view - enhanced_tile_view) / 2
        );
        compare_tiles(
            edge,
            factor,
            level == settings::AntiAliasing::x2 ? "enhanced 2x within the card"
                                                : "enhanced 4x within the card",
            enhanced_forced,
            true,
            true
        );
    }
    chosen.anti_aliasing = settings::AntiAliasing::off;
    apply_engine_settings(chosen);
    render_match_surface();

    const photo::TilePlan odd = compare_tiles(500, 2, "odd edge");
    for (const photo::Tile& tile : odd.tiles) {
        require(tile.width <= 500 && tile.height <= 500, "an odd edge kept more than 500");
        require(
            tile.width % photo::kLiveLightTile == 0 ||
                tile.x + tile.width == static_cast<int32_t>(1024 * 2),
            "an odd edge was not snapped onto the light grid"
        );
    }
    std::printf("photo tiles live: an edge of 500 snaps and matches\n");
    std::fflush(stdout);

    resize_window(640, 480);
    match_layout_ = layout::make_battlefield_layout(640, 480);
    require(
        match_layout_.battlefield_width() == 640 && match_layout_.battlefield_height() == 480,
        "the battlefield is not 640x480"
    );
    settings::EngineSettings enhanced = engine_settings();
    enhanced.anti_aliasing = settings::AntiAliasing::x4;
    apply_engine_settings(enhanced);
    render_match_surface();
    const photo::Plan offered = photo_size_plan();
    for (const int factor : {2, 3, 4}) {
        bool found = false;
        for (const photo::Offer& offer : offered.sizes) {
            if (offer.factor == factor) {
                require(
                    offer.offered, "size " + std::to_string(factor) + " was not offered at 640x480"
                );
                found = true;
            }
        }
        require(found, "size " + std::to_string(factor) + " is missing at 640x480");
    }
    std::printf("photo tiles live: 640x480 at enhanced 4x offers 2x, 3x and 4x\n");
    std::fflush(stdout);
    chosen.anti_aliasing = settings::AntiAliasing::off;
    apply_engine_settings(chosen);

    resize_window(1920, 1080);
    match_layout_ = layout::make_battlefield_layout(1920, 1080);
    match_camera_x_ = framed_camera_x;
    match_camera_z_ = framed_camera_z;
    match_zoom_ = kPlayZoom;
    match_zoom_target_ = kPlayZoom;
    exact_view_.held = false;
    render_match_surface();
    require(
        match_layout_.battlefield_width() == 1920 && match_layout_.battlefield_height() == 1080,
        "the battlefield is not 1920x1080"
    );
    const int32_t wide_long = 3840;
    compare_tiles(std::max(int32_t{32}, wide_long / 4), 2, "1920x1080");
    std::printf("photo tiles live: 1920x1080 matches\n");
    std::fflush(stdout);

    // A wide window at Enhanced 2× keeps the card's multisampling, which is
    // limited by area. A square tile as long as the window's sampling edge
    // would lose it, so each size offered there must still be drawn.
    settings::EngineSettings wide_samples = engine_settings();
    wide_samples.anti_aliasing = settings::AntiAliasing::x2;
    apply_engine_settings(wide_samples);
    render_match_surface();
    const TurnedFrameSampling wide_play = turned_frame_sampling();
    require(
        !wide_play.device_multisamples || wide_play.multisample > 1,
        "1920x1080 at Enhanced 2x did not multisample"
    );
    int wide_tiled = 0;
    const photo::Plan wide_offers = photo_size_plan();
    for (const photo::Offer& offer : wide_offers.sizes) {
        if (offer.factor < 2 || !offer.offered)
            continue;
        const DecodedPng wide_photo = take_sized(offer.factor);
        require(
            photo_->sized_by_live && wide_photo.width == static_cast<uint32_t>(offer.width) &&
                wide_photo.height == static_cast<uint32_t>(offer.height),
            "1920x1080 at Enhanced 2x did not draw size " + std::to_string(offer.factor)
        );
        if (turned_plan_for(offer.factor, 0).tiles.size() > 1)
            ++wide_tiled;
    }
    require(wide_tiled > 0, "1920x1080 at Enhanced 2x drew no size in tiles");
    set_photo_size_factor(1);
    chosen.anti_aliasing = settings::AntiAliasing::off;
    apply_engine_settings(chosen);
    std::printf("photo tiles live: 1920x1080 at enhanced 2x draws every offered size\n");
    std::fflush(stdout);

    resize_window(opened_width, opened_height);
    match_layout_ = lay_out_match(opened_width, opened_height);
    require(
        file_bytes(options_.preferences_file) == preferences_settled &&
            preference_values_ == preferences_values,
        "the preferences file changed"
    );
    end_fight();
    std::printf("photo tiles live: ok\n");
    std::fflush(stdout);
    return 0;
}

int Runtime::check_photo() {
    switch (options_.photo_group) {
    case PhotoGroup::mode:
        check_photo_mode();
        return 0;
    case PhotoGroup::time:
        check_photo_time();
        return 0;
    case PhotoGroup::camera:
        check_photo_camera();
        return 0;
    case PhotoGroup::capture:
        check_photo_capture();
        return 0;
    case PhotoGroup::live:
        return check_photo_live();
    case PhotoGroup::keys:
        check_photo_keys();
        return 0;
    case PhotoGroup::panel:
        check_photo_panel();
        return 0;
    case PhotoGroup::sizes:
        return check_photo_sizes();
    case PhotoGroup::pad_touch:
        check_photo_pad_touch();
        return 0;
    case PhotoGroup::looks:
        check_photo_looks();
        return 0;
    case PhotoGroup::replay:
        check_photo_replay();
        return 0;
    case PhotoGroup::tiles:
        return check_photo_tiles();
    case PhotoGroup::tiles_live:
        return check_photo_tiles_live();
    case PhotoGroup::depth_live:
        return check_photo_depth_live();
    case PhotoGroup::poster:
        return check_photo_poster();
    }
    throw std::runtime_error("photo mode check: unknown group");
}

void Runtime::check_photo_mode() {
    /// Puts the preferences file back to the bytes the check started with.
    /// The fights write their own rows; those are put back too, and the run
    /// is stopped from writing the file again on the way out.
    struct PreferencesHold {
        Runtime& runtime;
        bool existed{false};
        std::vector<char> bytes;

        explicit PreferencesHold(Runtime& host) : runtime(host) {
            const auto& path = host.options_.preferences_file;
            if (path.has_value())
                existed = std::filesystem::exists(*path);
            bytes = file_bytes(path);
        }

        PreferencesHold(const PreferencesHold&) = delete;
        PreferencesHold& operator=(const PreferencesHold&) = delete;

        ~PreferencesHold() {
            const auto& path = runtime.options_.preferences_file;
            if (path.has_value()) {
                if (!existed) {
                    std::error_code error;
                    std::filesystem::remove(*path, error);
                } else {
                    std::ofstream out(*path, std::ios::binary | std::ios::trunc);
                    if (out)
                        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                }
            }
            runtime.preferences_dirty_ = false;
        }
    };

    /// Puts the extension state back, and clears the check's bits.
    struct ExtensionHold {
        Runtime& runtime;
        uint32_t (*state)(void*, const Runtime&);
        void* context;

        explicit ExtensionHold(Runtime& host)
            : runtime(host), state(host.extension_.state), context(host.extension_.context) {}

        ExtensionHold(const ExtensionHold&) = delete;
        ExtensionHold& operator=(const ExtensionHold&) = delete;

        ~ExtensionHold() {
            runtime.extension_.state = state;
            runtime.extension_.context = context;
            photo_extension_bits = 0;
        }
    };

    const PreferencesHold preferences_hold(*this);
    const ExtensionHold extension_hold(*this);
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("photo mode check: " + what);
    };
    const auto require = [&](bool ok, const std::string& what) {
        if (!ok)
            fail(what);
    };
    const auto step_tick = [this] {
        try {
            step_match_simulation();
        } catch (const std::exception& error) {
            report_match_tick_error(error.what());
        }
    };
    const auto same_layout = [](const layout::MatchLayout& left, const layout::MatchLayout& right) {
        return left.width == right.width && left.height == right.height &&
               left.left == right.left && left.top == right.top && left.bottom == right.bottom &&
               left.hud_width == right.hud_width && left.hud_height == right.hud_height &&
               left.scale == right.scale && left.column_scale == right.column_scale &&
               left.phone == right.phone && left.placed_count == right.placed_count;
    };
    const auto read_fields = [this] {
        MatchFields fields;
        const oa::Game& game = match_->state().game;
        fields.console = console::console_flags(game);
        fields.visibility = game.visibility_flags;
        fields.sim_run = game.sim_run_flags;
        fields.messages.resize(static_cast<std::size_t>(OA_CHAT_LINE_COUNT) * OA_CHAT_LINE_BYTES);
        std::memcpy(fields.messages.data(), game.chat_lines, fields.messages.size());
        return fields;
    };
    const auto same_fields = [](const MatchFields& left, const MatchFields& right) {
        return left.console == right.console && left.visibility == right.visibility &&
               left.sim_run == right.sim_run && left.messages == right.messages;
    };
    const auto read_orders = [this] {
        std::vector<UnitOrders> orders;
        const oa::World& world = match_->state();
        for (uint32_t slot = 0; slot < world.unit_slot_count; ++slot) {
            const oa::Unit* unit = oa::world_unit_at(&world, slot);
            if (unit == nullptr || unit->type_index == 0)
                continue;
            orders.push_back(
                {unit->id,
                 unit->type_index,
                 unit->flags,
                 static_cast<uint32_t>(unit->primary_order),
                 static_cast<uint32_t>(unit->secondary_order)}
            );
        }
        return orders;
    };
    const auto keep_view = [this] {
        KeptView kept;
        kept.placed = match_layout_;
        kept.camera_x = match_camera_x_;
        kept.camera_z = match_camera_z_;
        kept.zoom = match_zoom_;
        kept.zoom_target = match_zoom_target_;
        kept.tracking = match_tracking_;
        kept.tracked_unit = tracked_match_unit_;
        kept.exact_held = exact_view_.held;
        kept.exact_x = exact_view_.x;
        kept.exact_z = exact_view_.z;
        kept.exact_camera_x = exact_view_.camera_x;
        kept.exact_camera_z = exact_view_.camera_z;
        kept.exact_drawn_x = exact_view_.drawn_x;
        kept.exact_drawn_z = exact_view_.drawn_z;
        kept.command = match_command_;
        kept.build = pending_build_type_;
        kept.hold_held = view_hold_.held;
        kept.hold_x = view_hold_.centre_x;
        kept.hold_z = view_hold_.centre_z;
        return kept;
    };
    const auto view_restored = [&](const KeptView& kept) {
        return same_layout(match_layout_, kept.placed) && match_camera_x_ == kept.camera_x &&
               match_camera_z_ == kept.camera_z && match_zoom_ == kept.zoom &&
               match_zoom_target_ == kept.zoom_target && match_tracking_ == kept.tracking &&
               tracked_match_unit_ == kept.tracked_unit && exact_view_.held == kept.exact_held &&
               exact_view_.x == kept.exact_x && exact_view_.z == kept.exact_z &&
               exact_view_.camera_x == kept.exact_camera_x &&
               exact_view_.camera_z == kept.exact_camera_z &&
               exact_view_.drawn_x == kept.exact_drawn_x &&
               exact_view_.drawn_z == kept.exact_drawn_z && view_hold_.held == kept.hold_held &&
               view_hold_.centre_x == kept.hold_x && view_hold_.centre_z == kept.hold_z &&
               match_command_ == kept.command && pending_build_type_ == kept.build;
    };
    const auto hold_play_zoom = [&] {
        const float least = least_match_zoom();
        const float most = most_match_zoom();
        require(
            least <= kPlayZoom && most >= kPlayZoom, "zoom 1 is outside the match's play limits"
        );
        match_zoom_ = kPlayZoom;
        match_zoom_target_ = kPlayZoom;
    };
    const auto arm_and_select = [&] {
        oa::Game& game = match_->state().game;
        console::set_console_flags(
            game,
            static_cast<uint16_t>(
                console::console_flags(game) | console::console_flag::selection_boxes
            )
        );
        oa::Unit* commander = nullptr;
        for (const auto& slot : match_->world().slots) {
            if (slot.unit != nullptr && slot.record.type_index != 0 &&
                slot.record.owner_index == match_local_player_) {
                commander = oa::world_unit_at(&match_->state(), slot.unit_index);
                break;
            }
        }
        require(commander != nullptr, "the fight has no local commander");
        commander->flags |= OA_UNIT_FLAG_SELECTED;
        match_command_ = MatchCommand::attack;
        pending_build_type_ = 0;
    };
    const auto start_fight = [&] {
        start_benchmark_skirmish();
        match_layout_ = lay_out_match(options_.match_width, options_.match_height);
        hold_play_zoom();
        spawn_combat_armies(kArmy);
        arm_and_select();
    };
    const auto end_fight = [this] {
        if (photo_mode())
            leave_photo_mode();
        if (match_ != nullptr) {
            leave_match();
            load(Screen::main_menu);
        }
    };
    const auto move_photo_camera = [&](int tick) {
        const float least = least_match_zoom();
        const float most = most_match_zoom();
        const float want = (tick % 2) == 0 ? kPhotoZoomNear : kPhotoZoomFar;
        const float zoom = std::max(want, least);
        require(zoom <= most, "the photo zoom leaves play's limits");
        require(zoom >= kPhotoZoomNear, "the photo zoom would draw a wider map than play");
        match_zoom_ = zoom;
        match_zoom_target_ = zoom;
        scroll_match_view((tick % 2) == 0 ? 1 : -1, 0, kPanStep);
        set_photo_show_selection((tick % 4) < 2);
        set_photo_show_health_bars((tick % 4) >= 2);
    };
    const auto play_pass = [&](bool enter) {
        start_fight();
        for (int tick = 1; tick <= kPassTicks; ++tick) {
            step_tick();
            if (enter && tick == kEnterTick)
                require(enter_photo_mode(), "photo mode did not open");
            if (enter && photo_mode())
                move_photo_camera(tick);
            render_match_surface();
            if (enter && tick == kLeaveTick)
                leave_photo_mode();
        }
        require(!photo_mode(), "photo mode was still open at the last tick");
        DrawReadBack read = match_draw_read_back();
        end_fight();
        return read;
    };
    const auto play_pair = [&](uint32_t bits) {
        photo_extension_bits = bits;
        extension_.state = photo_extension_state;
        const DrawReadBack entered = play_pass(true);
        const DrawReadBack stayed = play_pass(false);
        const char* difference = draw_read_back_difference(entered, stayed);
        require(difference == nullptr, difference == nullptr ? "" : std::string(difference));
    };

    leave_photo_mode();
    require(!photo_mode(), "photo mode was open before the check");

    start_fight();
    match_finished_ = true;
    require(!enter_photo_mode() && !photo_mode(), "a finished match entered photo mode");
    match_finished_ = false;
    chat_composing_ = true;
    require(!enter_photo_mode() && !photo_mode(), "an open chat line entered photo mode");
    chat_composing_ = false;
    require(enter_photo_mode(), "photo mode did not open");
    require(enter_photo_mode() && photo_mode(), "entering again changed photo mode");
    const layout::MatchLayout battlefield = layout::make_battlefield_layout(
        match_layout_.width > 0 ? match_layout_.width : options_.match_width,
        match_layout_.height > 0 ? match_layout_.height : options_.match_height
    );
    require(
        same_layout(match_layout_, battlefield) && match_layout_.hud_width == 0 &&
            !match_layout_.phone,
        "photo mode is not the battlefield alone"
    );
    leave_photo_mode();
    require(!photo_mode(), "leaving did not end photo mode");
    end_fight();
    require(!enter_photo_mode() && !photo_mode(), "photo mode opened with no match");
    // The first fight writes the engine's own rows into an empty file.
    // Photo mode must add nothing after that. The hold puts the caller's
    // bytes back on the way out, empty file included.
    const std::vector<char> preferences_settled = file_bytes(options_.preferences_file);

    const auto check_interface = [&](uint32_t bits) {
        photo_extension_bits = bits;
        extension_.state = photo_extension_state;
        start_fight();
        oa::Unit* commander = nullptr;
        uint16_t commander_slot = 0;
        for (const auto& slot : match_->world().slots) {
            if (slot.unit_index == 0 || slot.unit == nullptr ||
                slot.owner_index != match_local_player_ || slot.unit->type_index == 0)
                continue;
            commander = oa::world_unit_at(&match_->state(), slot.unit_index);
            commander_slot = slot.unit_index;
            break;
        }
        require(commander != nullptr && commander_slot != 0, "the fight has no local commander");
        bool running = true;
        const auto dispatch = [&](SDL_Event& event) {
            dispatch_event(event, running);
            require(running, "a key ended the run");
        };
        const auto tap = [&](SDL_Scancode scancode, SDL_Keymod mod = SDL_KMOD_NONE) {
            require(sdl_.window != nullptr, "the check has no window");
            SDL_Event event{};
            event.key.windowID = SDL_GetWindowID(sdl_.window);
            event.key.scancode = scancode;
            event.key.key = SDL_GetKeyFromScancode(scancode, mod, true);
            event.key.mod = mod;
            for (const bool down : {true, false}) {
                event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                event.key.down = down;
                event.key.repeat = false;
                dispatch(event);
            }
        };
        const auto window_point = [&](float frame_x, float frame_y) {
            float window_x = 0;
            float window_y = 0;
            require(
                frame_to_window(sdl_.renderer, frame_x, frame_y, &window_x, &window_y),
                "the point is not on the window"
            );
            return std::pair<float, float>{window_x, window_y};
        };
        const auto motion = [&](float window_x, float window_y) {
            SDL_Event event{};
            event.type = SDL_EVENT_MOUSE_MOTION;
            event.motion.windowID = SDL_GetWindowID(sdl_.window);
            event.motion.x = window_x;
            event.motion.y = window_y;
            dispatch(event);
        };
        const auto button = [&](uint8_t which, bool down, float window_x, float window_y) {
            SDL_Event event{};
            event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
            event.button.windowID = SDL_GetWindowID(sdl_.window);
            event.button.button = which;
            event.button.down = down;
            event.button.clicks = 1;
            event.button.x = window_x;
            event.button.y = window_y;
            dispatch(event);
        };
        const auto click_at = [&](uint8_t which, float frame_x, float frame_y) {
            const auto [window_x, window_y] = window_point(frame_x, frame_y);
            motion(window_x, window_y);
            button(which, true, window_x, window_y);
            button(which, false, window_x, window_y);
        };
        const auto same_orders = [&](const std::vector<UnitOrders>& had, const char* what) {
            require(read_orders() == had, std::string(what) + " changed a unit");
        };
        const KeptView kept = keep_view();
        const MatchFields before = read_fields();
        require(enter_photo_mode(), "photo mode did not open");
        render_match_surface();
        require(match_hud_cpu_.rgb.empty(), "the HUD layer was drawn");
        require(
            same_layout(
                match_layout_,
                layout::make_battlefield_layout(kept.placed.width, kept.placed.height)
            ),
            "the layout is not the battlefield at the window's size"
        );
        require(same_fields(before, read_fields()), "photo mode wrote the match");
        set_photo_show_selection(false);
        set_photo_show_health_bars(false);
        render_match_surface();
        const std::vector<uint8_t> plain = match_world_cpu_.rgb;
        set_photo_show_selection(true);
        render_match_surface();
        require(match_world_cpu_.rgb != plain, "Show selection drew the same frame");
        require(same_fields(before, read_fields()), "Show selection wrote the match");
        // Posted after the field compare: a line changes the message log.
        // The language stays the one the report was posted in, so the
        // under-attack ending photo mode reads is the one just written.
        if ((bits & extension_state::shared_match) != 0) {
            post_unit_report(commander_slot, "Under Attack");
            post_match_message("photo check", oa::sim::messages::kind_player_chat);
            const std::string under_attack = photo_latest_under_attack();
            require(!under_attack.empty(), "no under-attack line was posted");
            char clock[64]{};
            oa::ui::hud::format_game_time(clock, sizeof clock, match_->state().game.tick);
            const std::string status = photo_status_line();
            require(
                status.find(under_attack) != std::string::npos, "the under-attack line is missing"
            );
            require(
                std::string_view(clock).size() > 0 && status.find(clock) != std::string::npos,
                "the clock is missing"
            );
            require(photo_chat_lines_since_enter() == 1, "the chat count is not 1");
            require(status.find(" 1 ") != std::string::npos, "the chat count is missing");
        }
        const std::string kept_language = language_state().choice;
        language_state().choice = "de";
        apply_language_now();
        const std::string translated = photo_status_line();
        require(
            translated.find("Fotomodus") != std::string::npos ||
                translated.find("Esc beendet") != std::string::npos,
            "the photo line was not translated"
        );
        language_state().choice = kept_language;
        apply_language_now();
        move_photo_camera(0);
        leave_photo_mode();
        require(view_restored(kept), "leaving did not put the view back");

        // Follow is what T starts, and the frames after leaving are the
        // camera step. A pan while following, and T with follow off, both
        // leave follow as it was on entering.
        const auto follow_ref = [this] { return match_->state().game.follow_unit; };
        const auto settle_follow = [this] {
            for (int frame = 0; frame < 3; ++frame)
                move_match_camera();
        };
        tap(SDL_SCANCODE_T);
        require(
            match_tracking_ && tracked_match_unit_ == commander_slot && follow_ref() != 0,
            "T did not start follow"
        );
        const bool followed = match_tracking_;
        const uint16_t followed_unit = tracked_match_unit_;
        const uint32_t followed_ref = follow_ref();
        require(enter_photo_mode(), "photo mode did not open with follow on");
        settle_follow();
        require(!match_tracking_ && follow_ref() == 0, "follow carried into photo mode");
        tap(SDL_SCANCODE_T);
        require(match_tracking_ && follow_ref() != 0, "T did not follow in photo mode");
        scroll_match_view(1, 0, kPanStep);
        require(!match_tracking_ && follow_ref() == 0, "a pan left follow on in photo mode");
        leave_photo_mode();
        settle_follow();
        require(
            match_tracking_ == followed && tracked_match_unit_ == followed_unit &&
                follow_ref() == followed_ref,
            "leaving did not put follow back"
        );
        scroll_match_view(1, 0, kPanStep);
        require(!match_tracking_ && follow_ref() == 0, "a pan did not end follow");
        const uint32_t off_ref = follow_ref();
        require(enter_photo_mode(), "photo mode did not open with follow off");
        tap(SDL_SCANCODE_T);
        require(match_tracking_ && follow_ref() != 0, "T did not follow in photo mode");
        leave_photo_mode();
        settle_follow();
        require(
            !match_tracking_ && tracked_match_unit_ == 0 && follow_ref() == off_ref,
            "T in photo mode left follow on"
        );

        require(enter_photo_mode(), "photo mode did not open again");
        render_match_surface();
        const oa::ui::key_bindings::Contexts contexts = input_contexts();
        const uint32_t play_bit = context_bit("play");
        const uint32_t watch = context_bit("watch");
        const uint32_t placing = context_bit("placing");
        const uint32_t box = context_bit("box");
        const uint32_t panel = context_bit("panel");
        const uint32_t unit_info = context_bit("unit-info");
        require((contexts.active & context_bit("global")) != 0, "global is inactive");
        require((contexts.active & context_bit("match")) != 0, "match is inactive");
        require((contexts.active & context_bit("oa-screens")) != 0, "oa-screens is inactive");
        require((contexts.active & play_bit) == 0, "play is active");
        require((contexts.active & watch) == 0, "watch is active");
        require((contexts.active & placing) == 0, "placing is active");
        require((contexts.active & box) == 0, "box is active");
        require((contexts.active & panel) == 0, "panel is active");
        require((contexts.active & unit_info) == 0, "unit-info is active");

        const std::vector<UnitOrders> orders = read_orders();
        const uint16_t selected_before = selected_match_unit_;
        tap(SDL_SCANCODE_S);
        same_orders(orders, "Stop");
        require(match_command_ == MatchCommand::attack, "Stop disarmed the order");
        require(pending_build_type_ == 0, "Stop armed a build");
        require(commander != nullptr, "the commander is gone");
        const auto point = project_match_point(
            live_viewport(match_camera_x_, match_camera_z_),
            {static_cast<uint32_t>(commander->position.x),
             static_cast<uint32_t>(commander->position.y),
             static_cast<uint32_t>(commander->position.z)}
        );
        const auto gap = [](float left, float right) {
            return left > right ? left - right : right - left;
        };
        const float corner_x = 24.0F;
        const float corner_y = 24.0F;
        const float far_x = static_cast<float>(std::max(match_layout_.width - 24, 24));
        const float far_y = static_cast<float>(std::max(match_layout_.height - 24, 24));
        const float commander_x = static_cast<float>(point.x);
        const float commander_y = static_cast<float>(point.y);
        const float near_gap = gap(corner_x, commander_x) + gap(corner_y, commander_y);
        const float far_gap = gap(far_x, commander_x) + gap(far_y, commander_y);
        const float ground_x = far_gap > near_gap ? far_x : corner_x;
        const float ground_y = far_gap > near_gap ? far_y : corner_y;
        require(
            gap(ground_x, commander_x) + gap(ground_y, commander_y) > 80.0F,
            "open ground is on the commander"
        );
        click_at(SDL_BUTTON_LEFT, ground_x, ground_y);
        same_orders(orders, "a left click with Attack armed");
        require(match_command_ == MatchCommand::attack, "a left click used up the armed order");
        require(pending_build_type_ == 0, "a left click armed a build");
        // The other commander starts far from the camera. The click lands on
        // an enemy of the fight that the battlefield shows.
        oa::Unit* enemy = nullptr;
        float enemy_x = 0;
        float enemy_y = 0;
        const auto on_battlefield = [&](float x, float y) {
            return x >= 8.0F && y >= 8.0F && x < static_cast<float>(match_layout_.width - 8) &&
                   y < static_cast<float>(match_layout_.height - 8);
        };
        for (const auto& slot : match_->world().slots) {
            if (slot.unit_index == 0 || slot.unit == nullptr || slot.unit->type_index == 0 ||
                slot.owner_index == match_local_player_)
                continue;
            oa::Unit* candidate = oa::world_unit_at(&match_->state(), slot.unit_index);
            if (candidate == nullptr)
                continue;
            const auto enemy_point = project_match_point(
                live_viewport(match_camera_x_, match_camera_z_),
                {static_cast<uint32_t>(candidate->position.x),
                 static_cast<uint32_t>(candidate->position.y),
                 static_cast<uint32_t>(candidate->position.z)}
            );
            const float projected_x = static_cast<float>(enemy_point.x);
            const float projected_y = static_cast<float>(enemy_point.y);
            if (!on_battlefield(projected_x, projected_y) ||
                gap(projected_x, commander_x) + gap(projected_y, commander_y) <= 20.0F)
                continue;
            enemy = candidate;
            enemy_x = projected_x;
            enemy_y = projected_y;
            break;
        }
        require(enemy != nullptr, "the fight has no enemy on the battlefield");
        click_at(SDL_BUTTON_LEFT, enemy_x, enemy_y);
        same_orders(orders, "a click on an enemy");
        require(
            match_command_ == MatchCommand::attack, "a click on an enemy used up the armed order"
        );
        require(pending_build_type_ == 0, "a click on an enemy armed a build");
        require(
            selected_match_unit_ == selected_before, "a click on an enemy changed the selection"
        );
        match_command_ = MatchCommand::none;
        click_at(SDL_BUTTON_LEFT, ground_x, ground_y);
        same_orders(orders, "a left click with nothing armed");
        require(
            match_command_ == MatchCommand::none, "a left click with nothing armed armed an order"
        );
        require(pending_build_type_ == 0, "a left click with nothing armed armed a build");
        match_command_ = MatchCommand::attack;
        click_at(SDL_BUTTON_RIGHT, ground_x, ground_y);
        same_orders(orders, "a right click");
        require(match_command_ == MatchCommand::attack, "a right click cancelled the armed order");
        require(pending_build_type_ == 0, "a right click armed a build");
        const float drag_x = ground_x < static_cast<float>(match_layout_.width) / 2.0F
                                 ? ground_x + 36.0F
                                 : ground_x - 36.0F;
        const float drag_y = ground_y < static_cast<float>(match_layout_.height) / 2.0F
                                 ? ground_y + 28.0F
                                 : ground_y - 28.0F;
        const auto [drag_from_x, drag_from_y] = window_point(ground_x, ground_y);
        const auto [drag_to_x, drag_to_y] = window_point(drag_x, drag_y);
        motion(drag_from_x, drag_from_y);
        button(SDL_BUTTON_LEFT, true, drag_from_x, drag_from_y);
        motion(drag_to_x, drag_to_y);
        button(SDL_BUTTON_LEFT, false, drag_to_x, drag_to_y);
        same_orders(orders, "a box drag");
        require(match_command_ == MatchCommand::attack, "a box drag used up the armed order");
        require(pending_build_type_ == 0, "a box drag armed a build");

        commander->flags &= ~OA_UNIT_FLAG_SELECTED;
        selected_match_unit_ = 0;
        const std::vector<UnitOrders> cleared = read_orders();
        // Nothing is armed, so a click that reached the match would select
        // the commander.
        match_command_ = MatchCommand::none;
        click_at(SDL_BUTTON_LEFT, commander_x, commander_y);
        same_orders(cleared, "a click on the commander");
        require(selected_match_unit_ == 0, "a click on the commander selected a unit");
        require(
            (commander->flags & OA_UNIT_FLAG_SELECTED) == 0, "a click on the commander selected it"
        );
        require(match_command_ == MatchCommand::none, "a click on the commander armed an order");
        match_command_ = MatchCommand::attack;
        tap(SDL_SCANCODE_HOME);
        require(selected_match_unit_ == 0, "Home selected a unit");
        require((commander->flags & OA_UNIT_FLAG_SELECTED) == 0, "Home selected the commander");
        same_orders(cleared, "Home");
        require(photo_mode(), "Home left photo mode");
        tap(SDL_SCANCODE_RETURN);
        require(!chat_composing_, "Enter opened the chat line");
        require(photo_mode(), "Enter left photo mode");
        const uint16_t board = match_->state().game.graphics_flags;
        tap(SDL_SCANCODE_F4);
        require(
            match_->state().game.graphics_flags == board &&
                (match_->state().game.graphics_flags & oa::ui::hud::kGraphicsBoardPinned) ==
                    (board & oa::ui::hud::kGraphicsBoardPinned),
            "F4 pinned the kills board"
        );
        require(photo_mode(), "F4 left photo mode");
        tap(SDL_SCANCODE_SLASH, kPrimaryModifier);
        require(
            oa_layer().find(oa::ui::key_editor::name_keys_overlay) != nullptr,
            "the keys overlay did not open"
        );
        require(photo_mode(), "the keys overlay key left photo mode");
        tap(SDL_SCANCODE_SLASH, kPrimaryModifier);
        require(
            oa_layer().find(oa::ui::key_editor::name_keys_overlay) == nullptr,
            "the keys overlay stayed open"
        );
        require(photo_mode(), "closing the keys overlay left photo mode");

        // The resize event is the one a window system posts. The layout it
        // lays out is read from the window, so the window changes first.
        const SDL_Point canvas_before = match_canvas_point();
        const int next_width = canvas_before.x == 1024 && canvas_before.y == 768 ? 800 : 1024;
        const int next_height = canvas_before.x == 1024 && canvas_before.y == 768 ? 600 : 768;
        require(
            sdl_.window != nullptr && SDL_SetWindowSize(sdl_.window, next_width, next_height) &&
                SDL_SyncWindow(sdl_.window),
            std::string("the window did not resize: ") + SDL_GetError()
        );
        SDL_Event resized{};
        resized.type = SDL_EVENT_WINDOW_RESIZED;
        resized.window.windowID = SDL_GetWindowID(sdl_.window);
        resized.window.data1 = next_width;
        resized.window.data2 = next_height;
        dispatch(resized);
        const SDL_Point canvas_after = match_canvas_point();
        require(
            canvas_after.x != canvas_before.x || canvas_after.y != canvas_before.y,
            "the window's size did not change"
        );
        // The same width, height and play layout apply_output_mode just stored.
        const SDL_Point canvas = match_canvas_point();
        int canvas_width = canvas.x;
        int canvas_height = canvas.y;
        int window_width = 0;
        int window_height = 0;
        if (sdl_.window != nullptr)
            SDL_GetWindowSize(sdl_.window, &window_width, &window_height);
        const SDL_Point scaled = scaled_frame_size();
        oa::ui::display_layout::Insets safe{};
        if (scaled.x > 0 && scaled.y > 0) {
            canvas_width = window_width = scaled.x;
            canvas_height = window_height = scaled.y;
        } else {
            safe = window_safe_insets();
        }
        auto saved_play = make_window_match_layout(
            canvas_width, canvas_height, window_width, window_height, safe
        );
        saved_play = layout::fit_side_column(saved_play, side_column_page_rows());
        saved_play = make_room_for_clock_line(saved_play);
        require(photo_ != nullptr, "a resize left photo mode");
        require(same_layout(photo_->layout, saved_play), "a resize did not keep the play layout");
        require(saved_play.hud_width != 0, "the play layout of the new size has no side column");
        require(
            same_layout(
                match_layout_, layout::make_battlefield_layout(canvas_width, canvas_height)
            ) && match_layout_.hud_width == 0 &&
                !match_layout_.phone,
            "a resize did not lay out the battlefield alone"
        );
        tap(SDL_SCANCODE_ESCAPE);
        require(!photo_mode(), "Escape left photo mode open");
        require(match_command_ == MatchCommand::attack, "Escape dropped the armed order");
        require(pending_build_type_ == 0, "Escape armed a build");
        require(same_layout(match_layout_, saved_play), "leaving put back the old window's layout");

        require(enter_photo_mode(), "photo mode did not open for the menu");
        tap(SDL_SCANCODE_F2);
        require(!photo_mode(), "F2 left photo mode open");
        require(pause_menu_shown() && match_paused_, "F2 did not open the game menu");
        resume_match_pause();
        require(!pause_menu_shown() && !match_paused_, "the game menu stayed open");
        require(enter_photo_mode(), "photo mode did not open for Settings");
        tap(SDL_SCANCODE_COMMA, kPrimaryModifier);
        require(!photo_mode(), "Settings left photo mode open");
        require(pause_menu_shown() && match_paused_, "Settings did not open the game menu");
        require(oa_layer().find("settings") != nullptr, "Settings did not open");
        // Settings is a modal screen. The next fight enters photo mode only
        // once it and the game menu are gone.
        if (LayerScreen* settings_screen = oa_layer().find("settings"))
            oa_layer().close(settings_screen);
        resume_match_pause();
        // The macOS application menu's Settings… item posts an event of
        // its own, registered on every platform.
        require(engine_settings_menu_event_ != 0, "the Settings item has no event");
        require(enter_photo_mode(), "photo mode did not open for the Settings item");
        SDL_Event chosen{};
        chosen.type = engine_settings_menu_event_;
        dispatch(chosen);
        require(!photo_mode(), "the Settings item left photo mode open");
        require(
            pause_menu_shown() && match_paused_, "the Settings item did not open the game menu"
        );
        require(oa_layer().find("settings") != nullptr, "the Settings item did not open Settings");
        if (LayerScreen* item_screen = oa_layer().find("settings"))
            oa_layer().close(item_screen);
        resume_match_pause();
        end_fight();
    };

    play_pair(0);
    check_interface(0);
    play_pair(extension_state::multiplayer | extension_state::shared_match);
    check_interface(extension_state::multiplayer | extension_state::shared_match);
    play_pair(
        extension_state::multiplayer | extension_state::shared_match |
        extension_state::local_watcher
    );
    check_interface(
        extension_state::multiplayer | extension_state::shared_match |
        extension_state::local_watcher
    );

    require(
        file_bytes(options_.preferences_file) == preferences_settled, "the preferences file changed"
    );
    std::printf("photo mode: kept the battlefield\n");
    std::fflush(stdout);
}

void Runtime::check_photo_time() {
    /// Puts the preferences file back to the bytes the check started with.
    struct PreferencesHold {
        Runtime& runtime;
        bool existed{false};
        std::vector<char> bytes;

        explicit PreferencesHold(Runtime& host) : runtime(host) {
            const auto& path = host.options_.preferences_file;
            if (path.has_value())
                existed = std::filesystem::exists(*path);
            bytes = file_bytes(path);
        }

        PreferencesHold(const PreferencesHold&) = delete;
        PreferencesHold& operator=(const PreferencesHold&) = delete;

        ~PreferencesHold() {
            const auto& path = runtime.options_.preferences_file;
            if (path.has_value()) {
                if (!existed) {
                    std::error_code error;
                    std::filesystem::remove(*path, error);
                } else {
                    std::ofstream out(*path, std::ios::binary | std::ios::trunc);
                    if (out)
                        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                }
            }
            runtime.preferences_dirty_ = false;
        }
    };

    /// Puts the extension state back, and clears the check's bits.
    struct ExtensionHold {
        Runtime& runtime;
        uint32_t (*state)(void*, const Runtime&);
        void* context;

        explicit ExtensionHold(Runtime& host)
            : runtime(host), state(host.extension_.state), context(host.extension_.context) {}

        ExtensionHold(const ExtensionHold&) = delete;
        ExtensionHold& operator=(const ExtensionHold&) = delete;

        ~ExtensionHold() {
            runtime.extension_.state = state;
            runtime.extension_.context = context;
            photo_extension_bits = 0;
        }
    };

    struct GamePace {
        uint16_t flags{};
        uint16_t requested{};
        uint16_t current{};
    };

    // The caller's bytes, put back when the hold ends, empty file included.
    // A fight writes the engine's own rows; those are compared separately,
    // after the fight that never enters photo mode.
    const std::vector<char> preferences_at_start = file_bytes(options_.preferences_file);
    {
        const PreferencesHold preferences_hold(*this);
        const ExtensionHold extension_hold(*this);
        const auto fail = [](const std::string& what) {
            throw std::runtime_error("photo time check: " + what);
        };
        const auto require = [&](bool ok, const std::string& what) {
            if (!ok)
                fail(what);
        };
        const auto step_tick = [this] {
            try {
                step_match_simulation();
            } catch (const std::exception& error) {
                report_match_tick_error(error.what());
            }
        };
        const auto pace_of = [this] {
            const oa::Game& game = match_->state().game;
            return GamePace{game.sim_run_flags, game.requested_speed, game.current_speed};
        };
        const auto pace_same = [](const GamePace& left, const GamePace& right) {
            return left.flags == right.flags && left.requested == right.requested &&
                   left.current == right.current;
        };
        const auto tick_now = [this] { return match_->state().game.tick; };
        const auto play_until = [&](uint32_t tick) {
            while (tick_now() < tick) {
                step_tick();
                render_match_surface();
            }
        };
        const auto start_fight = [&] {
            start_benchmark_skirmish();
            match_layout_ = lay_out_match(options_.match_width, options_.match_height);
            const float least = least_match_zoom();
            const float most = most_match_zoom();
            require(least <= kPlayZoom && most >= kPlayZoom, "zoom 1 is outside play's limits");
            match_zoom_ = kPlayZoom;
            match_zoom_target_ = kPlayZoom;
            spawn_combat_armies(kArmy);
            // A unit walking through the eighths, the same order in both
            // passes, so a frame between ticks is not the whole tick's.
            uint16_t walker = 0;
            for (const auto& slot : match_->world().slots) {
                if (slot.unit == nullptr || slot.record.type_index == 0 ||
                    slot.record.owner_index != match_local_player_ ||
                    !match_->takes_move_order(slot.unit_index))
                    continue;
                walker = slot.unit_index;
                break;
            }
            require(walker != 0, "the fight has no unit that can walk");
            const auto x =
                static_cast<int32_t>(match_->world().slots[walker].unit->position[0] >> 16);
            const auto z0 =
                static_cast<int32_t>(match_->world().slots[walker].unit->position[2] >> 16);
            const auto map_h = static_cast<int32_t>(selected_tnt_->tile_height * 32U);
            const auto z = std::min(z0 + 256, map_h - 64);
            const oa::sim::ground_orders::Point point{
                static_cast<int32_t>(static_cast<uint32_t>(x) << 16),
                static_cast<int32_t>(
                    static_cast<uint32_t>(match_->map_height(
                        static_cast<uint32_t>(x) << 16, static_cast<uint32_t>(z) << 16
                    ))
                    << 16
                ),
                static_cast<int32_t>(static_cast<uint32_t>(z) << 16)
            };
            match_->issue_ground_move(walker, point, false);
        };
        const auto end_fight = [this] {
            if (photo_mode())
                leave_photo_mode();
            if (match_ != nullptr) {
                leave_match();
                load(Screen::main_menu);
            }
        };
        const auto clock_text = [this] {
            char clock[64]{};
            oa::ui::hud::format_game_time(clock, sizeof clock, match_->state().game.tick);
            return std::string(clock);
        };

        const auto play_entered = [&] {
            start_fight();
            const GamePace pace = pace_of();
            play_until(30);
            require(tick_now() == 30, "the fight did not reach tick 30");
            require(enter_photo_mode(), "photo mode did not open");
            require(photo_frozen(), "entering did not freeze the match");
            require(!match_clock_steps(), "the frozen match's clock still steps");
            require(!photo_freeze(), "freezing again acted");
            require(!photo_hold_picture(), "hold picture acted on a local match");
            require(pace_same(pace, pace_of()), "freeze wrote the pause or the speed");
            const std::string frozen_word{oa::data::languages::interface_text("Frozen")};
            const std::string frozen_clock = clock_text();
            std::vector<uint8_t> frozen_frame;
            for (int frame = 0; frame < 40; ++frame) {
                render_match_surface();
                require(tick_now() == 30, "a frozen draw advanced the tick");
                if (frame == 0)
                    frozen_frame = match_world_cpu_.rgb;
                require(
                    !frozen_frame.empty() && match_world_cpu_.rgb == frozen_frame,
                    "two frozen draws of the same frame differ"
                );
            }
            const std::string frozen_line = photo_status_line();
            require(
                frozen_line.find(frozen_word) != std::string::npos, "the line does not say Frozen"
            );
            require(
                frozen_line.find(frozen_clock) != std::string::npos,
                "the frozen line has no game clock"
            );
            for (int step = 0; step < 5; ++step) {
                require(photo_step(), "a single step did not act");
                render_match_surface();
            }
            require(tick_now() == 35, "five steps did not reach tick 35");
            require(photo_frozen(), "a step did not leave the match frozen");
            require(!match_clock_steps(), "the clock steps after a single step");
            require(pace_same(pace, pace_of()), "step wrote the pause or the speed");
            // The frame of the whole tick, before the eighths replace it.
            const std::vector<uint8_t> whole_tick_frame = match_world_cpu_.rgb;
            std::vector<uint8_t> eighth_zero;
            std::vector<uint8_t> eighth_eight;
            for (int eighth = 0; eighth <= 8; ++eighth) {
                require(photo_between(eighth), "an eighth between ticks did not act");
                render_match_surface();
                if (eighth == 0)
                    eighth_zero = match_world_cpu_.rgb;
                if (eighth == 8)
                    eighth_eight = match_world_cpu_.rgb;
            }
            require(
                !eighth_zero.empty() && eighth_zero != eighth_eight,
                "the frame at eighth 0 matches the frame at eighth 8"
            );
            require(
                eighth_eight == whole_tick_frame,
                "eighth 8 is not the frame of the whole tick before the eighths"
            );
            require(!photo_between(9), "an eighth past the tick acted");
            require(tick_now() == 35, "drawing between ticks advanced the tick");
            require(pace_same(pace, pace_of()), "between ticks wrote the pause or the speed");
            require(!photo_slow(0) && !photo_slow(10), "a speed outside 1 to 9 acted");
            require(photo_slow(1), "slow did not act");
            require(photo_slow_speed() == 1, "slow is not speed 1");
            require(!photo_between(4), "between ticks acted while playing slowly");
            require(!match_clock_steps(), "the slow match's clock still steps");
            require(pace_same(pace, pace_of()), "slow wrote the pause or the speed");
            const std::string slow_line = photo_status_line();
            require(
                slow_line.find(oa::data::languages::interface_text("Slow")) != std::string::npos &&
                    slow_line.find(" 3") != std::string::npos,
                "the line does not say Slow at 3 ticks a second"
            );
            require(smooth_motion(), "smooth motion is off");
            const uint64_t interval = frame_pacing::kNanosecondsPerSecond / 3U;
            for (int grant = 0; grant < 15; ++grant) {
                frame_time_ns_ += interval;
                grant_photo_slow_ticks();
                render_match_surface();
                // Half an interval later the picture has glided on. The clock
                // is put back, so the grants that follow stay on the interval.
                if (grant == 0) {
                    const std::vector<uint8_t> at_tick = match_world_cpu_.rgb;
                    const uint32_t tick = tick_now();
                    frame_time_ns_ += interval / 2U;
                    render_match_surface();
                    require(
                        match_world_cpu_.rgb != at_tick,
                        "a frame half an interval after a slow tick matches the frame at it"
                    );
                    require(tick_now() == tick, "the half interval played a tick");
                    frame_time_ns_ -= interval / 2U;
                }
            }
            require(tick_now() == 50, "slow did not play 15 ticks");
            require(pace_same(pace, pace_of()), "the slow ticks wrote the pause or the speed");
            require(photo_run(), "run did not act");
            require(!photo_frozen() && photo_slow_speed() == 0, "run left the match held");
            require(match_clock_steps(), "the clock does not step after run");
            require(pace_same(pace, pace_of()), "run wrote the pause or the speed");
            play_until(70);
            require(tick_now() == 70, "the fight did not reach tick 70");
            leave_photo_mode();
            require(!photo_mode(), "leaving did not end photo mode");
            require(match_clock_steps(), "the clock does not step after leaving");
            require(pace_same(pace, pace_of()), "leaving wrote the pause or the speed");
            play_until(120);
            require(tick_now() == 120, "the entered pass did not reach tick 120");
            require(pace_same(pace, pace_of()), "the fight wrote the pause or the speed");
            DrawReadBack read = match_draw_read_back();
            end_fight();
            return read;
        };
        const auto play_stayed = [&] {
            start_fight();
            play_until(120);
            require(tick_now() == 120, "the other pass did not reach tick 120");
            require(!photo_mode(), "the other pass entered photo mode");
            DrawReadBack read = match_draw_read_back();
            end_fight();
            return read;
        };

        photo_extension_bits = 0;
        extension_.state = photo_extension_state;
        // The first fight never enters photo mode, and writes the engine's
        // own rows. Photo mode must add nothing after that.
        const DrawReadBack stayed = play_stayed();
        const std::vector<char> preferences_settled = file_bytes(options_.preferences_file);
        const DrawReadBack entered = play_entered();
        const char* difference = draw_read_back_difference(entered, stayed);
        require(difference == nullptr, difference == nullptr ? "" : std::string(difference));

        const auto hold_shared = [&](uint32_t bits, const char* who) {
            photo_extension_bits = bits;
            extension_.state = photo_extension_state;
            start_fight();
            const GamePace pace = pace_of();
            require(enter_photo_mode(), std::string(who) + " did not enter photo mode");
            require(!photo_frozen(), std::string(who) + " froze");
            require(match_clock_steps(), std::string(who) + " held the clock");
            require(
                !photo_freeze() && !photo_slow(1) && !photo_step() && !photo_between(4),
                std::string(who) + " accepted a time command"
            );
            require(!photo_frozen() && photo_slow_speed() == 0, std::string(who) + " changed time");
            require(pace_same(pace, pace_of()), std::string(who) + " wrote the pause or the speed");
            const std::string before_clock = clock_text();
            const uint32_t from = tick_now();
            play_until(from + 30);
            const std::string after_clock = clock_text();
            require(after_clock != before_clock, std::string(who) + " clock did not advance");
            const std::string running = photo_status_line();
            require(
                running.find(after_clock) != std::string::npos,
                std::string(who) + " line lost the clock"
            );
            require(
                running.find(oa::data::languages::interface_text("Frozen")) == std::string::npos,
                std::string(who) + " line says Frozen"
            );
            require(match_clock_steps(), std::string(who) + " clock stopped");
            render_match_surface();
            const std::vector<uint8_t> copied = match_world_cpu_.rgb;
            require(!copied.empty(), std::string(who) + " drew an empty battlefield");
            const int32_t camera_x = match_camera_x_;
            const int32_t camera_z = match_camera_z_;
            const float zoom = match_zoom_;
            const float zoom_target = match_zoom_target_;
            require(photo_hold_picture(), std::string(who) + " did not hold the picture");
            require(
                photo_picture_held() && photo_camera_locked(), std::string(who) + " is not held"
            );
            scroll_match_view(1, 0, kPanStep);
            handle_match_zoom(
                1.0F,
                static_cast<float>(match_layout_.left + 8),
                static_cast<float>(match_layout_.top + 8),
                false
            );
            bool keep_running = true;
            const auto dispatch = [&](SDL_Event& event) {
                dispatch_event(event, keep_running);
                require(keep_running, std::string(who) + " key ended the run");
            };
            const auto tap = [&](SDL_Scancode scancode) {
                require(sdl_.window != nullptr, std::string(who) + " has no window");
                SDL_Event event{};
                event.key.windowID = SDL_GetWindowID(sdl_.window);
                event.key.scancode = scancode;
                event.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, true);
                event.key.mod = SDL_KMOD_NONE;
                for (const bool down : {true, false}) {
                    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                    event.key.down = down;
                    event.key.repeat = false;
                    dispatch(event);
                }
            };
            // Home centres and T follows. Neither may move the held picture.
            tap(SDL_SCANCODE_HOME);
            tap(SDL_SCANCODE_T);
            require(
                match_camera_x_ == camera_x && match_camera_z_ == camera_z && match_zoom_ == zoom,
                std::string(who) + " reframed the held picture"
            );
            require(
                match_zoom_target_ == zoom_target, std::string(who) + " zoomed the held picture"
            );
            const std::string held_line = photo_status_line();
            require(
                held_line.find(oa::data::languages::interface_text("Held")) != std::string::npos,
                std::string(who) + " line does not say Held"
            );
            const uint32_t held_from = tick_now();
            while (tick_now() < held_from + 30) {
                step_tick();
                render_match_surface();
                require(
                    match_world_cpu_.rgb == copied,
                    std::string(who) + " held picture changed while the match ran"
                );
            }
            const std::string held_clock = clock_text();
            require(held_clock != after_clock, std::string(who) + " clock stopped while held");
            require(
                photo_status_line().find(held_clock) != std::string::npos,
                std::string(who) + " line clock stopped while held"
            );
            require(
                pace_same(pace, pace_of()), std::string(who) + " hold wrote the pause or the speed"
            );
            // Letting go of the picture puts the camera back, still in photo mode.
            require(photo_hold_picture(), std::string(who) + " did not let go of the picture");
            require(!photo_picture_held(), std::string(who) + " picture stayed held");
            require(
                match_camera_x_ == camera_x && match_camera_z_ == camera_z && match_zoom_ == zoom,
                std::string(who) + " camera was not put back"
            );
            leave_photo_mode();
            require(!photo_mode() && !photo_picture_held(), std::string(who) + " did not let go");
            // The picture is live again: a pan the hold refused now moves the
            // camera, and the battlefield drawn from there is not the copy.
            const int32_t released_x = match_camera_x_;
            scroll_match_view(1, 0, kPanStep);
            require(match_camera_x_ != released_x, std::string(who) + " camera stayed locked");
            render_match_surface();
            require(
                match_world_cpu_.rgb != copied, std::string(who) + " still shows the held picture"
            );
            require(
                pace_same(pace, pace_of()),
                std::string(who) + " leaving wrote the pause or the speed"
            );
            end_fight();
        };
        hold_shared(extension_state::multiplayer | extension_state::shared_match, "a shared match");
        hold_shared(
            extension_state::multiplayer | extension_state::shared_match |
                extension_state::local_watcher,
            "a watcher"
        );
        require(
            file_bytes(options_.preferences_file) == preferences_settled,
            "the preferences file changed"
        );
    }
    if (file_bytes(options_.preferences_file) != preferences_at_start)
        throw std::runtime_error("photo time check: the preferences file was not put back");
    std::printf("photo time: kept the battlefield\n");
    std::fflush(stdout);
}

void Runtime::check_photo_selection_box(bool turned) {
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("photo selection: " + what);
    };
    const auto require = [&](bool ok, const std::string& what) {
        if (!ok)
            fail(what);
    };
    const auto end_fight = [this] {
        if (photo_mode())
            leave_photo_mode();
        if (match_ != nullptr) {
            leave_match();
            load(Screen::main_menu);
        }
    };
    const auto cloud_of = [](const std::vector<uint8_t>& plain,
                             const std::vector<uint8_t>& boxed,
                             uint32_t width,
                             uint32_t height) {
        struct Cloud {
            int64_t x = 0;
            int64_t y = 0;
            int count = 0;
        } cloud;
        if (plain.size() != boxed.size() || width == 0 || height == 0 ||
            plain.size() != static_cast<std::size_t>(width) * height * 3U)
            return cloud;
        for (uint32_t row = 0; row < height; ++row) {
            for (uint32_t column = 0; column < width; ++column) {
                const std::size_t at = (static_cast<std::size_t>(row) * width + column) * 3U;
                if (plain[at] == boxed[at] && plain[at + 1U] == boxed[at + 1U] &&
                    plain[at + 2U] == boxed[at + 2U])
                    continue;
                cloud.x += column;
                cloud.y += row;
                ++cloud.count;
            }
        }
        return cloud;
    };
    const auto apart = [](double across, double down) {
        return std::sqrt(across * across + down * down);
    };

    start_benchmark_skirmish();
    match_layout_ = lay_out_match(options_.match_width, options_.match_height);
    match_zoom_ = kPlayZoom;
    match_zoom_target_ = kPlayZoom;
    spawn_combat_armies(kArmy);
    uint16_t commander_id = 0;
    for (const auto& slot : match_->world().slots) {
        if (slot.unit_index == 0 || slot.unit == nullptr ||
            slot.owner_index != match_local_player_ || slot.unit->type_index == 0)
            continue;
        commander_id = slot.unit_index;
        break;
    }
    require(commander_id != 0, "the fight has no local commander");
    oa::Unit* commander = oa::world_unit_at(&match_->state(), commander_id);
    require(commander != nullptr, "the commander is not in the world");
    commander->flags |= OA_UNIT_FLAG_SELECTED;
    selected_match_unit_ = commander_id;
    oa::Game& game = match_->state().game;
    console::set_console_flags(
        game,
        static_cast<uint16_t>(console::console_flags(game) | console::console_flag::selection_boxes)
    );
    require(enter_photo_mode(), "photo mode did not open");
    set_photo_show_health_bars(false);
    set_photo_show_selection(false);
    center_camera_on_unit(commander_id);
    const float zoomed = std::min(2.0F, photo_zoom_limits()[1]);
    match_zoom_ = zoomed;
    match_zoom_target_ = zoomed;
    if (turned) {
        bool did_turn = false;
        try {
            did_turn = turn_live_camera(20.0F, 12.0F);
        } catch (const std::exception&) {
            did_turn = false;
        }
        require(
            did_turn && live_renderer_active() && !live_renderer_failed(), "the view did not turn"
        );
    }
    const int32_t before_pan_x = match_camera_x_;
    const int32_t before_pan_z = match_camera_z_;
    scroll_match_view(1, 0, 200.0);
    render_match_surface();
    if (match_camera_x_ == before_pan_x && match_camera_z_ == before_pan_z)
        scroll_match_view(-1, 0, 200.0);
    render_match_surface();
    require(
        match_camera_x_ != static_cast<int32_t>(game.camera_x) ||
            match_camera_z_ != static_cast<int32_t>(game.camera_y),
        "the photo camera stayed on the player's camera"
    );
    // Full classic keeps the card's picture out of the world layer until the
    // window is presented. The presented frame is the screen, with the
    // panel's chrome left out of the comparison. The photo is the battlefield.
    const auto present_world = [this](renderer::Surface& shown) {
        const CaptureHold hold(capture_frame_, shown);
        present_match_layers();
    };
    renderer::Surface plain_shown;
    present_world(plain_shown);
    require(plain_shown.width > 0 && !plain_shown.rgb.empty(), "the window presented nothing");
    take_photo();
    wait_for_photos();
    const std::string plain_name = saved_photo_name(status_);
    require(!plain_name.empty(), "status is " + status_);
    const fs::path plain_file = photo_shots_folder() / oa::platform::path_from_utf8(plain_name);
    const DecodedPng plain_photo = decode_png(plain_file);

    set_photo_show_selection(true);
    render_match_surface();
    renderer::Surface boxed_shown;
    present_world(boxed_shown);
    take_photo();
    wait_for_photos();
    const std::string boxed_name = saved_photo_name(status_);
    require(!boxed_name.empty() && boxed_name != plain_name, "status is " + status_);
    const fs::path boxed_file = photo_shots_folder() / oa::platform::path_from_utf8(boxed_name);
    const DecodedPng boxed_photo = decode_png(boxed_file);

    const int origin_x = match_layout_.left;
    const int origin_y = match_layout_.top;
    const int layout_width = match_layout_.width;
    const int layout_height = match_layout_.height;

    struct Cloud {
        int64_t x = 0;
        int64_t y = 0;
        int count = 0;
    } screen_cloud;

    require(
        plain_shown.width == boxed_shown.width && plain_shown.height == boxed_shown.height &&
            plain_shown.rgb.size() == boxed_shown.rgb.size(),
        "the two frames are not the same size"
    );
    for (uint32_t row = 0; row < plain_shown.height; ++row) {
        for (uint32_t column = 0; column < plain_shown.width; ++column) {
            if (oa::ui::photo_panel::covers_chrome(
                    static_cast<int>(column), static_cast<int>(row), layout_width, layout_height
                ))
                continue;
            const std::size_t at =
                (static_cast<std::size_t>(row) * plain_shown.width + column) * 3U;
            if (plain_shown.rgb[at] == boxed_shown.rgb[at] &&
                plain_shown.rgb[at + 1U] == boxed_shown.rgb[at + 1U] &&
                plain_shown.rgb[at + 2U] == boxed_shown.rgb[at + 2U])
                continue;
            screen_cloud.x += static_cast<int64_t>(column) - origin_x;
            screen_cloud.y += static_cast<int64_t>(row) - origin_y;
            ++screen_cloud.count;
        }
    }
    require(
        plain_photo.width == boxed_photo.width && plain_photo.height == boxed_photo.height,
        "the two photos are not the same size"
    );
    const auto photo_cloud =
        cloud_of(plain_photo.rows, boxed_photo.rows, plain_photo.width, plain_photo.height);
    // Full classic reads the card's picture back into the window. A software
    // card can leave that read-back without the box while the photo, which
    // is the battlefield, still has it. Either picture is enough to place
    // the box, and both are required to agree when each has one.
    require(screen_cloud.count > 8 || photo_cloud.count > 8, "Show selection drew no box");
    const bool screen_holds = screen_cloud.count > 8;
    const double screen_x =
        screen_holds ? static_cast<double>(screen_cloud.x) / screen_cloud.count : 0.0;
    const double screen_y =
        screen_holds ? static_cast<double>(screen_cloud.y) / screen_cloud.count : 0.0;
    const double photo_x =
        photo_cloud.count > 0 ? static_cast<double>(photo_cloud.x) / photo_cloud.count : 0.0;
    const double photo_y =
        photo_cloud.count > 0 ? static_cast<double>(photo_cloud.y) / photo_cloud.count : 0.0;
    const double box_x = screen_holds ? screen_x : photo_x;
    const double box_y = screen_holds ? screen_y : photo_y;

    double anchor_x = 0;
    double anchor_y = 0;
    if (live_renderer_active()) {
        const auto bounds = live_unit_bounds(commander_id);
        require(bounds.has_value(), "the turned view has no box for the commander");
        anchor_x = (static_cast<double>(bounds->x1) + bounds->x2) * 0.5 - match_layout_.left;
        anchor_y = (static_cast<double>(bounds->y1) + bounds->y2) * 0.5 - match_layout_.top;
    } else {
        const std::array<uint32_t, 3> position{
            static_cast<uint32_t>(commander->position.x),
            static_cast<uint32_t>(commander->position.y),
            static_cast<uint32_t>(commander->position.z)
        };
        const auto seen =
            project_match_point(live_viewport(match_camera_x_, match_camera_z_), position);
        const auto stale = project_match_point(
            live_viewport(static_cast<int32_t>(game.camera_x), static_cast<int32_t>(game.camera_y)),
            position
        );
        anchor_x = static_cast<double>(seen.x) - match_layout_.left;
        anchor_y = static_cast<double>(seen.y) - match_layout_.top;
        const double stale_x = static_cast<double>(stale.x) - match_layout_.left;
        const double stale_y = static_cast<double>(stale.y) - match_layout_.top;
        require(
            apart(box_x - anchor_x, box_y - anchor_y) + 24.0 <
                apart(box_x - stale_x, box_y - stale_y),
            "the box sits on the player's camera"
        );
    }
    require(apart(box_x - anchor_x, box_y - anchor_y) < 96.0, "the box does not surround the unit");
    require(photo_cloud.count > 8, "the photo has no selection box");
    if (screen_holds) {
        require(
            apart(photo_x - screen_x, photo_y - screen_y) < 12.0,
            "the photo's box is not the one on screen"
        );
    } else {
        require(
            apart(photo_x - anchor_x, photo_y - anchor_y) < 96.0,
            "the photo's box does not surround the unit"
        );
    }

    std::error_code remove_error;
    fs::remove(plain_file, remove_error);
    fs::remove(boxed_file, remove_error);
    end_fight();
    std::printf("photo selection: on the unit\n");
    std::fflush(stdout);
}

void Runtime::check_photo_camera() {
    /// Puts the preferences file back to the bytes the check started with.
    struct PreferencesHold {
        Runtime& runtime;
        bool existed{false};
        std::vector<char> bytes;

        explicit PreferencesHold(Runtime& host) : runtime(host) {
            const auto& path = host.options_.preferences_file;
            if (path.has_value())
                existed = std::filesystem::exists(*path);
            bytes = file_bytes(path);
        }

        PreferencesHold(const PreferencesHold&) = delete;
        PreferencesHold& operator=(const PreferencesHold&) = delete;

        ~PreferencesHold() {
            const auto& path = runtime.options_.preferences_file;
            if (path.has_value()) {
                if (!existed) {
                    std::error_code error;
                    std::filesystem::remove(*path, error);
                } else {
                    std::ofstream out(*path, std::ios::binary | std::ios::trunc);
                    if (out)
                        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                }
            }
            runtime.preferences_dirty_ = false;
        }
    };

    /// Puts the model budget back. A forced budget is what the check reads
    /// through rendered_units_budget_override.
    struct BudgetHold {
        Runtime& runtime;

        explicit BudgetHold(Runtime& host, uint64_t bytes) : runtime(host) {
            runtime.force_rendered_units_budget(bytes);
        }

        BudgetHold(const BudgetHold&) = delete;
        BudgetHold& operator=(const BudgetHold&) = delete;

        ~BudgetHold() { runtime.force_rendered_units_budget(std::nullopt); }
    };

    const PreferencesHold preferences_hold(*this);
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("photo camera check: " + what);
    };
    const auto require = [&](bool ok, const std::string& what) {
        if (!ok)
            fail(what);
    };
    const auto step_tick = [this] {
        try {
            step_match_simulation();
        } catch (const std::exception& error) {
            report_match_tick_error(error.what());
        }
    };
    const auto same_layout = [](const layout::MatchLayout& left, const layout::MatchLayout& right) {
        return left.width == right.width && left.height == right.height &&
               left.left == right.left && left.top == right.top && left.bottom == right.bottom &&
               left.hud_width == right.hud_width && left.hud_height == right.hud_height &&
               left.scale == right.scale && left.column_scale == right.column_scale &&
               left.phone == right.phone && left.placed_count == right.placed_count;
    };
    const auto gap = [](int32_t left, int32_t right) {
        return left > right ? left - right : right - left;
    };
    // On Full, the photo battlefield is made small enough that the whole map
    // sits farther out than the card draws, whenever that size can still be
    // drawn. Both passes of one fight use it. The pure range covers a map
    // this window cannot shrink under the floor.
    const auto shrink_for_the_card = [&] {
        if (!full_presentation())
            return;
        const auto [map_wide, map_high] = shown_map_size();
        if (map_wide <= 0 || map_high <= 0)
            return;
        const int32_t now_wide =
            match_layout_.width > 0 ? match_layout_.width : options_.match_width;
        const int32_t now_high =
            match_layout_.height > 0 ? match_layout_.height : options_.match_height;
        if (now_wide < 32 || now_high < 32)
            return;
        const float now = least_battlefield_zoom(
            oa::ui::engine_settings::ZoomOutLimit::whole_map,
            kMinFullBattlefieldZoom,
            map_wide,
            map_high,
            now_wide,
            now_high
        );
        if (now < kMinFullBattlefieldZoom)
            return;
        int32_t fit_wide = static_cast<int32_t>(
            std::floor(static_cast<double>(kMinFullBattlefieldZoom) * map_wide - 1.0)
        );
        int32_t fit_high = static_cast<int32_t>(
            std::floor(static_cast<double>(kMinFullBattlefieldZoom) * map_high - 1.0)
        );
        fit_wide = std::clamp(fit_wide, int32_t{32}, now_wide);
        fit_high = std::clamp(fit_high, int32_t{32}, now_high);
        const float fit = least_battlefield_zoom(
            oa::ui::engine_settings::ZoomOutLimit::whole_map,
            kMinFullBattlefieldZoom,
            map_wide,
            map_high,
            fit_wide,
            fit_high
        );
        if (!(fit < kMinFullBattlefieldZoom))
            return;
        match_layout_ = layout::make_battlefield_layout(fit_wide, fit_high);
    };
    const auto start_fight = [&] {
        start_benchmark_skirmish();
        match_layout_ = lay_out_match(options_.match_width, options_.match_height);
        match_zoom_ = kPlayZoom;
        match_zoom_target_ = kPlayZoom;
        spawn_combat_armies(kArmy);
        shrink_for_the_card();
    };
    const auto end_fight = [this] {
        if (photo_mode())
            leave_photo_mode();
        if (match_ != nullptr) {
            leave_match();
            load(Screen::main_menu);
        }
    };
    // Models are refused only by the memory and the reach photo mode keeps.
    const auto models_refused = [this] {
        const double zoom = static_cast<double>(match_zoom_ <= 0.0F ? 1.0F : match_zoom_);
        const double shown_wide = static_cast<double>(match_layout_.battlefield_width()) / zoom;
        const double shown_high = static_cast<double>(match_layout_.battlefield_height()) / zoom;
        const bool memory =
            shown_wide * shown_high * static_cast<double>(kModelBridgeBytesPerMapPixel) >
            static_cast<double>(rendered_units_budget());
        const bool reach =
            far_view_frame() &&
            std::max(shown_wide, shown_high) + static_cast<double>(kFarCullMarginMapPixels) >
                static_cast<double>(kMostModelOffset);
        return memory || reach;
    };
    // Both passes pose every unit before the read-back. The match's own
    // piece reads rebuild the pose first, so the hash is the match's pose
    // and not which frame drew the unit last.
    const auto pose_every_unit = [this] {
        for (const auto& slot : match_->world().slots) {
            if (slot.unit_index == 0 || slot.unit == nullptr)
                continue;
            auto* unit_instance = match_->instance(slot.unit_index);
            if (unit_instance == nullptr)
                continue;
            const oa::Unit& record = slot.record;
            unit_instance->model().rebuild_transforms(
                {record.bank, static_cast<int16_t>(record.heading), record.pitch}
            );
        }
    };

    leave_photo_mode();
    require(!photo_mode(), "photo mode was open before the check");

    start_fight();
    end_fight();
    const std::vector<char> preferences_settled = file_bytes(options_.preferences_file);

    start_fight();
    const auto zoom_out = engine_settings().max_zoom_out;
    const auto zoom_in = engine_settings().max_zoom_in;
    const float play_least = least_match_zoom();
    const float play_most = most_match_zoom();
    const float saved_zoom = match_zoom_;
    const float saved_target = match_zoom_target_;
    const int32_t saved_camera_x = match_camera_x_;
    const int32_t saved_camera_z = match_camera_z_;
    const layout::MatchLayout saved_layout = match_layout_;
    const std::vector<char> preferences_at_visit = file_bytes(options_.preferences_file);
    const auto angles_before = live_camera_angles();
    auto& game = match_->state().game;
    game.cursor_unit_id = 1;
    game.cursor_feature = 1;
    hovered_match_unit_ = 1;
    require(enter_photo_mode(), "photo mode did not open");
    require(game.cursor_unit_id == 0, "opening left the pointer's unit");
    require(
        game.cursor_feature == oa::sim::spatial_state::no_feature,
        "opening left the pointer's feature"
    );
    require(hovered_match_unit_ == 0, "opening left the pointer's hover");
    const uint32_t entered_camera_x = game.camera_x;
    const uint32_t entered_camera_y = game.camera_y;
    // The offer is the photo battlefield, which is the window.
    require(
        least_match_zoom() == play_least && most_match_zoom() == play_most,
        "photo mode changed play's zoom"
    );
    require(
        engine_settings().max_zoom_out == zoom_out && engine_settings().max_zoom_in == zoom_in,
        "photo mode changed the zoom settings"
    );
    const auto [map_wide, map_high] = shown_map_size();
    const int32_t battlefield_wide = match_layout_.battlefield_width();
    const int32_t battlefield_high = match_layout_.battlefield_height();
    const float whole_map = least_battlefield_zoom(
        oa::ui::engine_settings::ZoomOutLimit::whole_map,
        full_presentation() ? kMinFullBattlefieldZoom : kMinBattlefieldZoom,
        map_wide,
        map_high,
        battlefield_wide,
        battlefield_high
    );
    const std::array<float, 2> offer = photo_zoom_limits();
    require(offer[1] == kPhotoClosestZoom, "the close end is not eight times");
    require(offer[0] <= offer[1], "the photo range is backwards");
    require(offer[1] > play_most, "photo mode does not zoom closer than play");
    const bool classic_card = full_presentation() && !live_renderer_active();
    if (classic_card && whole_map < kMinFullBattlefieldZoom) {
        require(offer[0] == kMinFullBattlefieldZoom, "Full offered a zoom it does not draw");
        require(!photo_zoom_reason().empty(), "Full gave no reason for stopping short");
    } else if (classic_card) {
        require(offer[0] == whole_map, "Full did not offer the whole map");
        require(photo_zoom_reason().empty(), "Full gave a reason without stopping");
    } else {
        require(offer[0] == whole_map, "the offer is not the whole map");
        require(photo_zoom_reason().empty(), "this picture gave a reason for stopping");
    }
    if (classic_card)
        require(
            offer[0] + 1.0e-6F >= kMinFullBattlefieldZoom,
            "Full offered a zoom below the zoom it draws"
        );

    // Zoomed out units is Dots, after 1/2, for this visit. Photo mode keeps
    // models, so the far picture still draws them; a frame that honoured the
    // setting would draw dots. The fields go back before the preferences
    // are compared.
    require(engine_settings_ != nullptr, "the check has no settings");
    const auto saved_units = engine_settings_->current.zoomed_out_units;
    const auto saved_after = engine_settings_->current.zoomed_out_after;
    engine_settings_->current.zoomed_out_units = oa::ui::engine_settings::ZoomedOutUnits::dots;
    engine_settings_->current.zoomed_out_after = oa::ui::engine_settings::ZoomedOutAfter::one_half;
    require(
        offer[0] <
            oa::ui::engine_settings::zoomed_out_zoom(engine_settings_->current.zoomed_out_after),
        "the far picture is not past After zoom 1/2"
    );

    const auto draw_at = [&](float zoom) {
        match_zoom_ = zoom;
        match_zoom_target_ = zoom;
        const uint32_t camera_x = match_->state().game.camera_x;
        const uint32_t camera_y = match_->state().game.camera_y;
        render_match_surface();
        require(
            match_->state().game.camera_x == camera_x && match_->state().game.camera_y == camera_y,
            "the photo camera entered the match"
        );
        const auto picture_wide = static_cast<uint32_t>(match_layout_.battlefield_width());
        const auto picture_high = static_cast<uint32_t>(match_layout_.battlefield_height());
        require(
            match_world_cpu_.width == picture_wide && match_world_cpu_.height == picture_high &&
                match_world_cpu_.rgb.size() ==
                    static_cast<std::size_t>(picture_wide) * picture_high * 3U,
            "the picture is not the battlefield's size"
        );
        if (full_presentation() && !live_renderer_active()) {
            require(full_frame_drawn(), "Full did not draw the frame");
            require(!far_view_frame(), "the processor's far view drew Full's frame");
            require(!live_renderer_active(), "the turned view drew Full's classic frame");
        } else if (!full_presentation()) {
            require(!full_frame_drawn(), "Full drew the processor's frame");
            require(more_than_one_colour(match_world_cpu_.rgb), "the picture is one colour");
        }
        require(
            frame_draws_.units_as_dots == models_refused(),
            "units were not drawn as the memory and the reach allow"
        );
        return match_world_cpu_.rgb;
    };
    const std::vector<uint8_t> far_picture = draw_at(offer[0]);
    const std::vector<uint8_t> near_picture = draw_at(offer[1]);
    if (!full_presentation())
        require(far_picture != near_picture, "the whole map and eight times drew the same picture");
    require(!frame_draws_.units_as_dots, "eight times drew dots before the budget was forced");
    {
        const BudgetHold budget_hold(*this, 1);
        render_match_surface();
        require(frame_draws_.units_as_dots, "a budget of one byte did not draw dots");
    }
    require(!models_refused(), "the budget stayed forced");

    match_zoom_ = offer[1];
    match_zoom_target_ = offer[1];
    const auto park = [&](int32_t way_x, int32_t way_z) {
        scroll_match_view(way_x, way_z, 1.0e7);
        const int32_t parked_x = match_camera_x_;
        const int32_t parked_z = match_camera_z_;
        scroll_match_view(way_x, way_z, 1.0e7);
        require(
            match_camera_x_ == parked_x && match_camera_z_ == parked_z,
            "the view did not stop at its limit"
        );
    };
    park(1, 0);
    const int32_t right_edge = match_camera_x_;
    park(-1, 0);
    require(match_camera_x_ != right_edge, "the view did not travel across the map");
    park(0, 1);
    park(0, -1);
    render_match_surface();
    const std::array<int32_t, 2> held = view_camera();
    require(
        match_camera_x_ == held[0] && match_camera_z_ == held[1],
        "the picture's camera is not the view's limit"
    );
    // The pan above moved the photo camera. The Game block still holds the
    // camera from when photo mode opened.
    require(
        game.camera_x == entered_camera_x && game.camera_y == entered_camera_y,
        "the photo camera entered the match"
    );
    require(
        static_cast<uint32_t>(match_camera_x_) != entered_camera_x ||
            static_cast<uint32_t>(match_camera_z_) != entered_camera_y,
        "the photo camera entered the match"
    );

    bool running = true;
    const auto dispatch = [&](SDL_Event& event) {
        dispatch_event(event, running);
        require(running, "a key ended the run");
    };
    const auto tap = [&](SDL_Scancode scancode) {
        require(sdl_.window != nullptr, "the check has no window");
        SDL_Event event{};
        event.key.windowID = SDL_GetWindowID(sdl_.window);
        event.key.scancode = scancode;
        event.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, true);
        event.key.mod = SDL_KMOD_NONE;
        for (const bool down : {true, false}) {
            event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.down = down;
            event.key.repeat = false;
            dispatch(event);
        }
    };
    tap(SDL_SCANCODE_T);
    require(match_tracking_ && match_->state().game.follow_unit != 0, "T did not start follow");
    move_match_camera();
    const int32_t centred_x = match_camera_x_;
    const int32_t centred_z = match_camera_z_;
    match_camera_x_ = centred_x + 400;
    move_match_camera();
    require(
        gap(match_camera_x_, centred_x) <= 2 && gap(match_camera_z_, centred_z) <= 2,
        "follow did not centre on the unit"
    );
    // A frame places the followed unit after that step (place_tracking_camera).
    // At eight times it is centred where it is drawn, inside the battlefield's
    // middle half.
    place_tracking_camera();
    const auto* followed = match_->world().slots[tracked_match_unit_].unit;
    require(followed != nullptr, "follow lost the unit");
    const auto pointed =
        project_match_point(live_viewport(match_camera_x_, match_camera_z_), followed->position);
    const int32_t field_left = match_layout_.left;
    const int32_t field_top = match_layout_.top;
    const int32_t field_wide = match_layout_.battlefield_width();
    const int32_t field_high = match_layout_.battlefield_height();
    require(
        pointed.x >= field_left + field_wide / 4 && pointed.x < field_left + field_wide * 3 / 4 &&
            pointed.y >= field_top + field_high / 4 && pointed.y < field_top + field_high * 3 / 4,
        "the followed unit is outside the battlefield's middle half"
    );
    scroll_match_view(1, 0, kPanStep);
    require(!match_tracking_ && match_->state().game.follow_unit == 0, "a pan left follow on");

    if (!full_presentation()) {
        require(
            !photo_turn(5.0F) && !photo_tilt(5.0F), "a turn was accepted outside the turned view"
        );
        require(
            photo_turn_reason() == oa::data::languages::interface_text("Needs Full acceleration"),
            "the refusal is not the one Settings gives"
        );
        const auto angles = live_camera_angles();
        require(
            angles[0] == angles_before[0] && angles[1] == angles_before[1],
            "a refused turn moved the camera"
        );
    } else {
        const auto angles = live_camera_angles();
        const bool turned = photo_turn(5.0F);
        if (!turned) {
            require(!photo_turn_reason().empty(), "a refused turn gave no reason");
            require(!photo_tilt(5.0F), "a tilt was accepted when the turn was refused");
            const auto stayed = live_camera_angles();
            require(
                stayed[0] == angles[0] && stayed[1] == angles[1], "a refused turn moved the camera"
            );
            render_match_surface();
            require(full_frame_drawn() && !live_renderer_active(), "Full did not keep the frame");
        } else {
            // A device opened. Classic view while the zoom is the whole map:
            // the offer's far end moves to Full's floor, and that frame is
            // taken there before it is drawn. Full draws it.
            const float offer_far = photo_zoom_limits()[0];
            require(offer_far < kMinFullBattlefieldZoom, "the whole map is not past Full's floor");
            match_zoom_ = offer_far;
            match_zoom_target_ = offer_far;
            reset_live_renderer();
            require(!live_renderer_active(), "reset left the turned view up");
            render_match_surface();
            require(full_frame_drawn(), "Full did not draw the frame under its floor");
            require(!far_view_frame(), "the processor drew Full's frame under its floor");
            require(!live_renderer_active(), "the turned view stayed up after Classic view");
            require(
                match_zoom_ == kMinFullBattlefieldZoom &&
                    match_zoom_target_ == kMinFullBattlefieldZoom,
                "the zoom did not reach Full's floor in that frame"
            );
        }
    }

    game.cursor_unit_id = 1;
    game.cursor_feature = 1;
    hovered_match_unit_ = 1;
    leave_photo_mode();
    require(!photo_mode(), "leaving did not end photo mode");
    require(game.cursor_unit_id == 0, "leaving left the pointer's unit");
    require(
        game.cursor_feature == oa::sim::spatial_state::no_feature,
        "leaving left the pointer's feature"
    );
    require(hovered_match_unit_ == 0, "leaving left the pointer's hover");
    require(
        match_zoom_ == saved_zoom && match_zoom_target_ == saved_target &&
            match_camera_x_ == saved_camera_x && match_camera_z_ == saved_camera_z &&
            same_layout(match_layout_, saved_layout),
        "leaving did not put the view back"
    );
    require(
        !photo_turn(5.0F) && photo_turn_reason().empty(),
        "a turn was offered with photo mode closed"
    );
    require(
        engine_settings().max_zoom_out == zoom_out && engine_settings().max_zoom_in == zoom_in,
        "leaving changed the zoom settings"
    );
    engine_settings_->current.zoomed_out_units = saved_units;
    engine_settings_->current.zoomed_out_after = saved_after;
    require(
        file_bytes(options_.preferences_file) == preferences_at_visit,
        "photo mode wrote the preferences"
    );
    end_fight();

    const auto play_pass = [&](bool enter) {
        start_fight();
        for (int tick = 1; tick <= kPassTicks; ++tick) {
            step_tick();
            if (enter && tick == kEnterTick)
                require(enter_photo_mode(), "photo mode did not open");
            if (enter && photo_mode()) {
                const std::array<float, 2> ends = photo_zoom_limits();
                const float zoom = tick < kEnterTick + 20 ? ends[0] : ends[1];
                match_zoom_ = zoom;
                match_zoom_target_ = zoom;
                scroll_match_view(1, 1, 1.0e7);
            }
            render_match_surface();
            if (enter && tick == kLeaveTick)
                leave_photo_mode();
        }
        require(!photo_mode(), "photo mode was still open at the last tick");
        pose_every_unit();
        DrawReadBack read = match_draw_read_back();
        end_fight();
        return read;
    };
    const DrawReadBack entered = play_pass(true);
    const DrawReadBack stayed = play_pass(false);
    const char* difference = draw_read_back_difference(entered, stayed);
    require(difference == nullptr, difference == nullptr ? "" : std::string(difference));
    require(
        file_bytes(options_.preferences_file) == preferences_settled, "the preferences file changed"
    );
    check_photo_selection_box(false);
    require(
        file_bytes(options_.preferences_file) == preferences_settled,
        "the selection check wrote the preferences"
    );
    std::printf("photo camera: framed the battlefield\n");
    std::fflush(stdout);
}

void Runtime::check_photo_capture() {
    /// Puts the preferences file back. The fights write their own rows.
    struct PreferencesHold {
        Runtime& runtime;
        bool existed{false};
        std::vector<char> bytes;

        explicit PreferencesHold(Runtime& host) : runtime(host) {
            const auto& preferences = host.options_.preferences_file;
            if (preferences.has_value())
                existed = fs::exists(*preferences);
            bytes = file_bytes(preferences);
        }

        PreferencesHold(const PreferencesHold&) = delete;
        PreferencesHold& operator=(const PreferencesHold&) = delete;

        ~PreferencesHold() {
            const auto& preferences = runtime.options_.preferences_file;
            if (preferences.has_value()) {
                if (!existed) {
                    std::error_code remove_error;
                    fs::remove(*preferences, remove_error);
                } else {
                    std::ofstream out(*preferences, std::ios::binary | std::ios::trunc);
                    if (out)
                        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                }
            }
            runtime.preferences_dirty_ = false;
        }
    };

    const PreferencesHold preferences_hold(*this);

    // The player's gamma for this check, put back afterwards. A match start
    // applies the saved gamma, so each fight sets this again.
    struct GammaHold {
        Runtime& runtime;
        float previous;

        explicit GammaHold(Runtime& host) : runtime(host), previous(host.display_gamma_) {
            runtime.set_display_gamma(kCheckGamma);
        }

        GammaHold(const GammaHold&) = delete;
        GammaHold& operator=(const GammaHold&) = delete;

        ~GammaHold() { runtime.set_display_gamma(previous); }
    };

    const GammaHold gamma_hold(*this);

    // Removes the photos this check wrote, including when a require throws.
    struct PictureSweep {
        Runtime& runtime;

        explicit PictureSweep(Runtime& host) : runtime(host) {}

        PictureSweep(const PictureSweep&) = delete;
        PictureSweep& operator=(const PictureSweep&) = delete;

        ~PictureSweep() {
            remove_saved_pictures(screenshots_folder(runtime.user_folder_, ""));
            remove_saved_pictures(screenshots_folder(runtime.user_folder_, "ridge"));
        }
    };

    const PictureSweep picture_sweep(*this);
    const ClipboardHookHold clipboard_hold;
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("photo capture check: " + what);
    };
    const auto require = [&](bool ok, const std::string& what) {
        if (!ok)
            fail(what);
    };
    require(!gamma_identity_, "the gamma is the identity");
    using HardwareAcceleration = oa::ui::engine_settings::HardwareAcceleration;
    const HardwareAcceleration asked =
        options_.hardware_acceleration.value_or(HardwareAcceleration::off);
    const auto step_tick = [this] {
        try {
            step_match_simulation();
        } catch (const std::exception& step_error) {
            report_match_tick_error(step_error.what());
        }
    };
    const auto hold_play_zoom = [&] {
        require(
            least_match_zoom() <= kPlayZoom && most_match_zoom() >= kPlayZoom,
            "zoom 1 is outside the match's play limits"
        );
        match_zoom_ = kPlayZoom;
        match_zoom_target_ = kPlayZoom;
    };
    const auto arm_and_select = [&] {
        oa::Game& game = match_->state().game;
        console::set_console_flags(
            game,
            static_cast<uint16_t>(
                console::console_flags(game) | console::console_flag::selection_boxes
            )
        );
        oa::Unit* commander = nullptr;
        for (const auto& slot : match_->world().slots) {
            if (slot.unit != nullptr && slot.record.type_index != 0 &&
                slot.record.owner_index == match_local_player_) {
                commander = oa::world_unit_at(&match_->state(), slot.unit_index);
                break;
            }
        }
        require(commander != nullptr, "the fight has no local commander");
        commander->flags |= OA_UNIT_FLAG_SELECTED;
        match_command_ = MatchCommand::attack;
        pending_build_type_ = 0;
    };
    const auto start_fight = [&] {
        start_benchmark_skirmish();
        match_layout_ = lay_out_match(options_.match_width, options_.match_height);
        hold_play_zoom();
        spawn_combat_armies(kArmy);
        arm_and_select();
        // Entering the match applies the saved gamma. Put the check's gamma
        // back before any frame of the fight is drawn.
        set_display_gamma(kCheckGamma);
        require(!gamma_identity_, "the gamma is the identity");
    };
    const auto end_fight = [this] {
        if (photo_mode())
            leave_photo_mode();
        if (match_ != nullptr) {
            leave_match();
            load(Screen::main_menu);
        }
    };
    const auto clear_pictures = [&] {
        remove_saved_pictures(screenshots_folder(user_folder_, ""));
        remove_saved_pictures(screenshots_folder(user_folder_, "ridge"));
        if (match_ != nullptr)
            remove_saved_pictures(photo_shots_folder());
    };
    const auto require_tier = [&] {
        if (asked == HardwareAcceleration::full)
            require(full_presentation(), "Full is not presenting");
        else if (asked == HardwareAcceleration::basic)
            require(accelerated_presentation() && !full_presentation(), "Basic is not presenting");
        else
            require(
                !accelerated_presentation() && !full_presentation(), "Off is presenting on the card"
            );
    };
    const auto require_battlefield = [&] {
        const int world_w = static_cast<int>(match_world_cpu_.width);
        const int world_h = static_cast<int>(match_world_cpu_.height);
        require(
            match_layout_.hud_width == 0 && world_w == match_layout_.battlefield_width() &&
                world_h == match_layout_.battlefield_height(),
            "world " + std::to_string(world_w) + "x" + std::to_string(world_h) + " layout " +
                std::to_string(match_layout_.width) + "x" + std::to_string(match_layout_.height) +
                " battlefield " + std::to_string(match_layout_.battlefield_width()) + "x" +
                std::to_string(match_layout_.battlefield_height())
        );
    };
    const auto saved_name = [&] {
        const std::string saved = saved_photo_name(status_);
        require(!saved.empty(), "status is " + status_);
        require(
            saved.find('/') == std::string::npos && saved.find('\\') == std::string::npos, saved
        );
        const std::string line = photo_status_line();
        require(line.find("Photo mode") != std::string::npos, line);
        require(line.find("Esc leaves") != std::string::npos, line);
        require(line.find(saved) != std::string::npos, line);
        return saved;
    };
    const auto picture_rows = [&](const std::string& saved,
                                  const fs::path& folder,
                                  const std::vector<uint8_t>& expected,
                                  uint32_t width,
                                  uint32_t height) {
        const fs::path file = folder / oa::platform::path_from_utf8(saved);
        std::error_code exists_error;
        require(fs::is_regular_file(file, exists_error), saved);
        const std::vector<uint8_t> rows = png_rgb(file, width, height);
        require(rows == expected, "the photo is not the battlefield");
        return rows;
    };
    const auto open_photo = [&] {
        start_fight();
        clear_pictures();
        for (int tick = 1; tick <= kEnterTick; ++tick) {
            step_tick();
            if (tick == kEnterTick)
                require(enter_photo_mode(), "photo mode did not open");
            if (photo_mode()) {
                set_photo_show_selection(false);
                set_photo_show_health_bars(false);
            }
            render_match_surface();
        }
        require(photo_mode(), "photo mode was not open");
        require_tier();
        require_battlefield();
    };

    const auto gamma_picture = [&] {
        std::vector<uint8_t> rgb = gamma_mapped(match_world_cpu_, gamma_table_);
        require(!rgb.empty(), "the battlefield could not be copied");
        return rgb;
    };
    const auto require_fresh_line = [&] {
        if (photo_mode())
            leave_photo_mode();
        require(enter_photo_mode(), "photo mode did not open again");
        const std::string line = photo_status_line();
        require(line == fresh_photo_line(photo_frozen(), match_->state().game.tick), line);
        leave_photo_mode();
    };
    const auto resize_window = [&](int width, int height) {
        require(
            sdl_.window != nullptr && SDL_SetWindowSize(sdl_.window, width, height) &&
                SDL_SyncWindow(sdl_.window),
            std::string("the window did not resize: ") + SDL_GetError()
        );
        options_.match_width = width;
        options_.match_height = height;
    };
    const auto hold_zoom = [&](float zoom) {
        require(
            least_match_zoom() <= zoom && most_match_zoom() >= zoom,
            "zoom " + std::to_string(zoom) + " is outside the match's play limits"
        );
        match_zoom_ = zoom;
        match_zoom_target_ = zoom;
    };
    // One window size: photo mode, then a photo at each zoom. Below zoom 1
    // the area pass has to be what the screen shows.
    const auto shoot_basic = [&](int width, int height) {
        resize_window(width, height);
        open_photo();
        // A check is an untested run, so the ladder starts with no zoomed-out
        // filtering. Basic on a machine that smooths uses the full scene
        // budget: below zoom 1 that is the area pass, which the photo keeps.
        accelerated_.rung.budget = render_policy::SceneBudget::full;
        accelerated_.rung.magnify = true;
        const float zooms[]{0.5F, 1.0F, 2.0F};
        for (const float zoom : zooms) {
            hold_zoom(zoom);
            set_display_gamma(kCheckGamma);
            require(!gamma_identity_, "the gamma is the identity");
            renderer::Surface presented;
            {
                const CaptureHold hold(capture_frame_, presented);
                render_match_surface();
                present_match_layers();
            }
            if (zoom < 1.0F) {
                require(
                    accelerated_.frame.method == SceneMethod::area,
                    "zoom " + std::to_string(zoom) + " at " + std::to_string(width) + "x" +
                        std::to_string(height) + " drew by method " +
                        std::to_string(static_cast<int>(accelerated_.frame.method)) + " budget " +
                        std::to_string(static_cast<int>(accelerated_.rung.budget))
                );
            }
            const std::vector<uint8_t> expected = gamma_picture();
            const uint32_t world_w = match_world_cpu_.width;
            const uint32_t world_h = match_world_cpu_.height;
            take_photo();
            wait_for_photos();
            const std::vector<uint8_t> rows =
                picture_rows(saved_name(), photo_shots_folder(), expected, world_w, world_h);
            require_presented_battlefield(
                rows,
                world_w,
                world_h,
                presented,
                match_layout_.left,
                match_layout_.top,
                match_layout_.width,
                match_layout_.height
            );
        }
        require_no_partials(photo_shots_folder());
        require_fresh_line();
        end_fight();
    };
    // A held picture in a shared match. The match runs on under it, and the
    // photo is the held battlefield with the player's gamma once, as the
    // screen shows it, not the battlefield the match moved on to. Letting
    // go, the next photo is that live battlefield.
    const auto shoot_held = [&] {
        const SharedMatchHold shared(extension_);
        open_photo();
        require(!photo_frozen(), "the shared match froze");
        set_display_gamma(kCheckGamma);
        require(!gamma_identity_, "the gamma is the identity");
        render_match_surface();
        present_match_layers();
        if (asked == HardwareAcceleration::full)
            require(full_ != nullptr && full_->drawn, "Full did not draw the frame");
        require(photo_hold_picture() && photo_picture_held(), "the picture was not held");
        const renderer::Surface held{photo_->held_width, photo_->held_height, photo_->held_rgb};
        const std::vector<uint8_t> expected = gamma_mapped(held, gamma_table_);
        require(!expected.empty(), "the held picture is empty");
        const uint32_t held_tick = match_->state().game.tick;
        for (int tick = 0; tick < kHeldTicks; ++tick) {
            step_tick();
            render_match_surface();
            present_match_layers();
        }
        require(
            match_->state().game.tick >= held_tick + static_cast<uint32_t>(kHeldTicks),
            "the match did not run under the held picture"
        );
        renderer::Surface presented;
        {
            const CaptureHold hold(capture_frame_, presented);
            render_match_surface();
            present_match_layers();
        }
        take_photo();
        wait_for_photos();
        const fs::path shots = photo_shots_folder();
        const fs::path held_file = shots / oa::platform::path_from_utf8(saved_name());
        const std::vector<uint8_t> rows = png_rgb(held_file, held.width, held.height);
        require_held_moment(held_file, held_tick);
        // At native density the window scales the held copy up and the photo
        // keeps the copy's size, so the window is compared only where it
        // shows one pixel for each.
        if (window_one_to_one(sdl_.window))
            require_presented_battlefield(
                rows,
                held.width,
                held.height,
                presented,
                match_layout_.left,
                match_layout_.top,
                match_layout_.width,
                match_layout_.height
            );
        require(rows == expected, "the photo is not the held picture");
        require(photo_hold_picture() && !photo_picture_held(), "the picture was not let go");
        render_match_surface();
        present_match_layers();
        take_photo();
        wait_for_photos();
        const DecodedPng live = decode_png(shots / oa::platform::path_from_utf8(saved_name()));
        require(live.rows != rows, "the held photo is the live battlefield");
        require_no_partials(shots);
        end_fight();
        std::printf("photo capture held: %ux%u\n", held.width, held.height);
        std::fflush(stdout);
    };

    // F9 and Enter in one batch, Full. The photo is the window, not the play
    // battlefield the selection box was drawn on, and it matches the next
    // frozen frame.
    const auto shoot_entering = [&] {
        start_fight();
        clear_pictures();
        // The window the review measured. The play frame keeps the selection
        // box; the photo is the window, once a photo-mode frame is presented.
        // The size goes back afterwards, so the pinned Full photo is unchanged.
        int was_w = 0;
        int was_h = 0;
        require(
            sdl_.window != nullptr && SDL_GetWindowSize(sdl_.window, &was_w, &was_h) &&
                SDL_SetWindowSize(sdl_.window, 1280, 720) && SDL_SyncWindow(sdl_.window),
            std::string("the window did not resize: ") + SDL_GetError()
        );
        SDL_Event resized{};
        resized.type = SDL_EVENT_WINDOW_RESIZED;
        resized.window.windowID = SDL_GetWindowID(sdl_.window);
        resized.window.data1 = 1280;
        resized.window.data2 = 720;
        bool running = true;
        dispatch_event(resized, running);
        require(running, "a resize ended the run");
        const uint32_t play_w = match_world_cpu_.width;
        const uint32_t play_h = match_world_cpu_.height;
        const auto tap_key = [&](SDL_Scancode scancode) {
            SDL_Event event{};
            event.key.windowID = SDL_GetWindowID(sdl_.window);
            event.key.scancode = scancode;
            event.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, true);
            event.key.mod = SDL_KMOD_NONE;
            for (const bool down : {true, false}) {
                event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                event.key.down = down;
                event.key.repeat = false;
                dispatch_event(event, running);
            }
            require(running, "a key ended the run");
        };
        const auto pngs = [&] {
            int count = 0;
            const fs::path folder = photo_shots_folder();
            std::error_code error;
            if (!fs::exists(folder, error))
                return 0;
            for (const fs::directory_entry& entry : fs::directory_iterator(folder, error)) {
                const fs::path ext = entry.path().extension();
                if (ext == ".png" || ext == ".PNG")
                    ++count;
            }
            return count;
        };
        const int before = pngs();
        tap_key(SDL_SCANCODE_F9);
        tap_key(SDL_SCANCODE_RETURN);
        require(photo_mode() && photo_frozen(), "F9 and Enter did not open photo mode frozen");
        require(pngs() == before, "a photo was written before a photo-mode frame");
        render_match_surface();
        present_match_layers();
        wait_for_photos();
        require(pngs() == before + 1, "entering wrote no photo");
        const std::string entered_name = saved_photo_name(status_);
        require(!entered_name.empty(), "status is " + status_);
        const DecodedPng entered =
            decode_png(photo_shots_folder() / oa::platform::path_from_utf8(entered_name));
        int window_w = 0;
        int window_h = 0;
        require(
            sdl_.window != nullptr && SDL_GetWindowSize(sdl_.window, &window_w, &window_h),
            "the window's size could not be read"
        );
        int output_w = 0;
        int output_h = 0;
        if (sdl_.renderer != nullptr)
            std::ignore = SDL_GetRenderOutputSize(sdl_.renderer, &output_w, &output_h);
        const bool window_sized =
            (entered.width == static_cast<uint32_t>(window_w) &&
             entered.height == static_cast<uint32_t>(window_h)) ||
            (output_w > 0 && entered.width == static_cast<uint32_t>(output_w) &&
             entered.height == static_cast<uint32_t>(output_h));
        require(
            window_sized,
            "photo " + std::to_string(entered.width) + "x" + std::to_string(entered.height) +
                " window " + std::to_string(window_w) + "x" + std::to_string(window_h) +
                " output " + std::to_string(output_w) + "x" + std::to_string(output_h) + " play " +
                std::to_string(play_w) + "x" + std::to_string(play_h)
        );
        require(
            entered.width != play_w || entered.height != play_h, "the photo is the play battlefield"
        );
        render_match_surface();
        present_match_layers();
        take_photo();
        wait_for_photos();
        const std::string later_name = saved_photo_name(status_);
        require(!later_name.empty(), "status is " + status_);
        const DecodedPng later =
            decode_png(photo_shots_folder() / oa::platform::path_from_utf8(later_name));
        require(
            entered.rows == later.rows, "a photo on entering differs from the next frozen frame"
        );
        end_fight();
        clear_pictures();
        require(
            sdl_.window != nullptr && SDL_SetWindowSize(sdl_.window, was_w, was_h) &&
                SDL_SyncWindow(sdl_.window),
            std::string("the window did not resize: ") + SDL_GetError()
        );
        SDL_Event restored{};
        restored.type = SDL_EVENT_WINDOW_RESIZED;
        restored.window.windowID = SDL_GetWindowID(sdl_.window);
        restored.window.data1 = was_w;
        restored.window.data2 = was_h;
        dispatch_event(restored, running);
        require(running, "restoring the window ended the run");
    };

    clear_pictures();
    if (asked == HardwareAcceleration::full) {
        shoot_entering();
        open_photo();
        set_display_gamma(kCheckGamma);
        require(!gamma_identity_, "the gamma is the identity");
        renderer::Surface presented;
        {
            const CaptureHold hold(capture_frame_, presented);
            render_match_surface();
            present_match_layers();
        }
        require(full_ != nullptr && full_->drawn, "Full did not draw the frame");
        take_photo();
        wait_for_photos();
        require(full_ != nullptr && full_->drawn, "Full did not draw the frame");
        const fs::path shots = photo_shots_folder();
        const std::string saved = saved_name();
        const fs::path file = shots / oa::platform::path_from_utf8(saved);
        std::error_code exists_error;
        require(fs::is_regular_file(file, exists_error), saved);
        require(presented.width > 0 && presented.height > 0, "the window presented nothing");
        const DecodedPng image = decode_png(file);
        require_presented_battlefield(
            image.rows,
            image.width,
            image.height,
            presented,
            match_layout_.left,
            match_layout_.top,
            match_layout_.width,
            match_layout_.height
        );
        require_no_partials(shots);
        require_fresh_line();
        end_fight();
        shoot_held();
        clear_pictures();
        std::printf("photo capture full: drawn\n");
        std::fflush(stdout);
        return;
    }
    if (asked == HardwareAcceleration::basic) {
        shoot_basic(640, 480);
        shoot_basic(1280, 720);
        shoot_held();
        clear_pictures();
        check_photo_selection_box(false);
        std::printf("photo capture basic: equal\n");
        std::fflush(stdout);
        return;
    }

    std::string photo_sha;
    std::vector<char> prefs_before;
    const auto play_pass = [&](bool shoot) {
        start_fight();
        clear_pictures();
        for (int tick = 1; tick <= kPassTicks; ++tick) {
            step_tick();
            if (tick == kEnterTick)
                require(enter_photo_mode(), "photo mode did not open");
            if (photo_mode()) {
                set_photo_show_selection(false);
                set_photo_show_health_bars(false);
            }
            renderer::Surface presented;
            if (shoot && tick == kEnterTick) {
                set_display_gamma(kCheckGamma);
                require(!gamma_identity_, "the gamma is the identity");
                const CaptureHold hold(capture_frame_, presented);
                render_match_surface();
                present_match_layers();
            } else {
                render_match_surface();
            }
            if (tick == kEnterTick) {
                require_tier();
                require_battlefield();
            }
            if (shoot && tick == kEnterTick) {
                const std::vector<uint8_t> expected = gamma_picture();
                const uint32_t width = match_world_cpu_.width;
                const uint32_t height = match_world_cpu_.height;
                prefs_before = file_bytes(options_.preferences_file);
                const fs::path shots = photo_shots_folder();
                const std::string shots_text = oa::platform::path_to_generic_utf8(shots);
                require(shots_text.ends_with("Screenshots/default"), shots_text);
                std::string first_name;
                std::string second_name;
                bool paired = false;
                for (int attempt = 0; attempt < 4 && !paired; ++attempt) {
                    take_photo();
                    wait_for_photos();
                    first_name = saved_name();
                    take_photo();
                    wait_for_photos();
                    second_name = saved_name();
                    constexpr std::size_t kPrefixCharacters = 25;
                    if (first_name.size() >= kPrefixCharacters &&
                        second_name.size() >= kPrefixCharacters &&
                        first_name.compare(
                            0, kPrefixCharacters, second_name, 0, kPrefixCharacters
                        ) == 0) {
                        paired = true;
                        break;
                    }
                    std::error_code remove_error;
                    fs::remove(shots / oa::platform::path_from_utf8(first_name), remove_error);
                    fs::remove(shots / oa::platform::path_from_utf8(second_name), remove_error);
                }
                require(paired, "the two photos did not share a second");
                require(first_name.ends_with(".png"), first_name);
                require(
                    second_name == first_name.substr(0, first_name.size() - 4) + " 2.png",
                    second_name
                );
                const std::vector<uint8_t> rows =
                    picture_rows(first_name, shots, expected, width, height);
                std::ignore = picture_rows(second_name, shots, expected, width, height);
                require_presented_battlefield(
                    rows,
                    width,
                    height,
                    presented,
                    match_layout_.left,
                    match_layout_.top,
                    match_layout_.width,
                    match_layout_.height
                );
                photo_sha = sha256_hex(rows);
                require(photo_player_names(), "player names were off");
                require(
                    preference_values_.find("open-annihilation.photo.player-names") ==
                        preference_values_.end(),
                    "player names were stored before they were changed"
                );
                const std::vector<RosterEntry> roster = photo_roster(match_->state());
                const uint32_t photo_tick = match_->state().game.tick;
                require_photo_chunks(
                    shots / oa::platform::path_from_utf8(first_name),
                    width,
                    height,
                    photo_tick,
                    selected_map_name_runtime_,
                    true,
                    roster,
                    shots_text
                );
                require_photo_chunks(
                    shots / oa::platform::path_from_utf8(second_name),
                    width,
                    height,
                    photo_tick,
                    selected_map_name_runtime_,
                    true,
                    roster,
                    shots_text
                );
                // One photo with the played profile id set to ridge. The
                // profile is put back before the next frame is drawn.
                {
                    struct ProfileHold {
                        std::shared_ptr<const oa::data::mod_profile::ModProfile>& slot;
                        std::shared_ptr<const oa::data::mod_profile::ModProfile> played;

                        explicit ProfileHold(
                            std::shared_ptr<const oa::data::mod_profile::ModProfile>& held
                        )
                            : slot(held), played(held) {}

                        ProfileHold(const ProfileHold&) = delete;
                        ProfileHold& operator=(const ProfileHold&) = delete;
                        ~ProfileHold() { slot = played; }
                    };
                    auto ridge = std::make_shared<oa::data::mod_profile::ModProfile>();
                    ridge->id = "ridge";
                    const ProfileHold profile(options_.mod_profile);
                    options_.mod_profile = std::move(ridge);
                    const fs::path ridge_folder = photo_shots_folder();
                    const std::string ridge_text = oa::platform::path_to_generic_utf8(ridge_folder);
                    require(ridge_text.ends_with("Screenshots/ridge"), ridge_text);
                    take_photo();
                    wait_for_photos();
                    std::ignore = picture_rows(saved_name(), ridge_folder, expected, width, height);
                    require_no_partials(ridge_folder);
                }
                const auto kept_values = preference_values_;
                set_photo_player_names(false);
                require(!photo_player_names(), "player names stayed on");
                take_photo();
                wait_for_photos();
                require_photo_chunks(
                    shots / oa::platform::path_from_utf8(saved_name()),
                    width,
                    height,
                    match_->state().game.tick,
                    selected_map_name_runtime_,
                    false,
                    roster,
                    shots_text
                );
                set_photo_player_names(true);
                preference_values_ = kept_values;
                preferences_dirty_ = false;
                if (options_.preferences_file.has_value()) {
                    if (prefs_before.empty()) {
                        std::error_code remove_error;
                        fs::remove(*options_.preferences_file, remove_error);
                    } else {
                        std::ofstream restored(
                            *options_.preferences_file, std::ios::binary | std::ios::trunc
                        );
                        require(
                            static_cast<bool>(restored),
                            "the preferences file could not be restored"
                        );
                        restored.write(
                            prefs_before.data(), static_cast<std::streamsize>(prefs_before.size())
                        );
                        restored.close();
                        require(
                            static_cast<bool>(restored),
                            "the preferences file could not be restored"
                        );
                    }
                }
                require(
                    file_bytes(options_.preferences_file) == prefs_before,
                    "the preferences file changed"
                );
                // One photo is offered once, and none after the switch is
                // turned off. The switch and the preferences file are put back.
                const char* expected_mime = photo_clipboard_mime(this_photo_clipboard_system());
                require(expected_mime != nullptr, "this system has no image clipboard");
                const int offered_before = clipboard_catch.offers;
                take_photo();
                wait_for_photos();
                require(
                    clipboard_catch.offers == offered_before + 1, "the photo was not offered once"
                );
                require(clipboard_catch.mime == expected_mime, clipboard_catch.mime);
                {
                    const auto values = preference_values_;
                    const bool dirty = preferences_dirty_;
                    const std::vector<char> bytes = file_bytes(options_.preferences_file);
                    set_photo_copy(false);
                    require(!photo_copy(), "the switch stayed on");
                    const int offered_held = clipboard_catch.offers;
                    take_photo();
                    wait_for_photos();
                    require(
                        clipboard_catch.offers == offered_held,
                        "a photo was offered with the switch off"
                    );
                    // An explicit copy copies whatever the switch says.
                    copy_last_photo();
                    require(
                        clipboard_catch.offers == offered_held + 1,
                        "an explicit copy was not offered with the switch off"
                    );
                    require(status_.ends_with("  Copied"), status_);
                    preference_values_ = values;
                    if (options_.preferences_file.has_value()) {
                        std::ofstream restored(
                            *options_.preferences_file, std::ios::binary | std::ios::trunc
                        );
                        require(
                            static_cast<bool>(restored), "the preferences file was not put back"
                        );
                        restored.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                        require(
                            static_cast<bool>(restored), "the preferences file was not put back"
                        );
                    }
                    preferences_dirty_ = dirty;
                    require(photo_copy(), "the switch was not put back");
                }
                require(
                    file_bytes(options_.preferences_file) == prefs_before,
                    "the preferences file changed"
                );
                require_no_partials(shots);
                std::printf("photo clipboard: %s once\n", expected_mime);
                std::fflush(stdout);
            }
            if (tick == kLeaveTick)
                leave_photo_mode();
        }
        require(!photo_mode(), "photo mode was still open at the last tick");
        DrawReadBack read = match_draw_read_back();
        if (shoot)
            require_fresh_line();
        end_fight();
        return read;
    };
    const DrawReadBack shot = play_pass(true);
    const DrawReadBack quiet = play_pass(false);
    const char* difference = draw_read_back_difference(shot, quiet);
    require(difference == nullptr, difference == nullptr ? std::string() : std::string(difference));
    require(file_bytes(options_.preferences_file) == prefs_before, "the preferences file changed");
    require(photo_sha == kPhotoCaptureSha256, "sha " + photo_sha);
    shoot_held();
    clear_pictures();
    std::printf("photo capture: %s\n", photo_sha.c_str());
    std::fflush(stdout);
}

int Runtime::check_photo_live() {
    struct PreferencesHold {
        Runtime& runtime;
        bool existed{false};
        std::vector<char> bytes;

        explicit PreferencesHold(Runtime& host) : runtime(host) {
            const auto& preferences = host.options_.preferences_file;
            if (preferences.has_value())
                existed = fs::exists(*preferences);
            bytes = file_bytes(preferences);
        }

        PreferencesHold(const PreferencesHold&) = delete;
        PreferencesHold& operator=(const PreferencesHold&) = delete;

        ~PreferencesHold() {
            const auto& preferences = runtime.options_.preferences_file;
            if (preferences.has_value()) {
                if (!existed) {
                    std::error_code remove_error;
                    fs::remove(*preferences, remove_error);
                } else {
                    std::ofstream out(*preferences, std::ios::binary | std::ios::trunc);
                    if (out)
                        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                }
            }
            runtime.preferences_dirty_ = false;
        }
    };

    struct FlagHold {
        bool& flag;
        bool previous;

        explicit FlagHold(bool& held, bool value) : flag(held), previous(held) { flag = value; }

        FlagHold(const FlagHold&) = delete;
        FlagHold& operator=(const FlagHold&) = delete;

        ~FlagHold() { flag = previous; }
    };

    const PreferencesHold preferences_hold(*this);

    struct GammaHold {
        Runtime& runtime;
        float previous;

        explicit GammaHold(Runtime& host) : runtime(host), previous(host.display_gamma_) {
            runtime.set_display_gamma(kCheckGamma);
        }

        GammaHold(const GammaHold&) = delete;
        GammaHold& operator=(const GammaHold&) = delete;

        ~GammaHold() { runtime.set_display_gamma(previous); }
    };

    const GammaHold gamma_hold(*this);

    struct PictureSweep {
        Runtime& runtime;

        explicit PictureSweep(Runtime& host) : runtime(host) {}

        PictureSweep(const PictureSweep&) = delete;
        PictureSweep& operator=(const PictureSweep&) = delete;

        ~PictureSweep() {
            remove_saved_pictures(screenshots_folder(runtime.user_folder_, ""));
            remove_saved_pictures(screenshots_folder(runtime.user_folder_, "ridge"));
        }
    };

    const PictureSweep picture_sweep(*this);
    const ClipboardHookHold clipboard_hold;
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("photo capture check: " + what);
    };
    const auto require = [&](bool ok, const std::string& what) {
        if (!ok)
            fail(what);
    };
    require(!gamma_identity_, "the gamma is the identity");
    const auto end_fight = [this] {
        if (photo_mode())
            leave_photo_mode();
        if (match_ != nullptr) {
            leave_match();
            load(Screen::main_menu);
        }
    };
    const auto sweep_pictures = [&] {
        remove_saved_pictures(screenshots_folder(user_folder_, ""));
        remove_saved_pictures(screenshots_folder(user_folder_, "ridge"));
    };
    oa::ui::engine_settings::EngineSettings chosen = engine_settings();
    chosen.allow_full_3d = true;
    apply_engine_settings(chosen);

    start_benchmark_skirmish();
    set_display_gamma(kCheckGamma);
    require(!gamma_identity_, "the gamma is the identity");
    match_layout_ = lay_out_match(options_.match_width, options_.match_height);
    match_zoom_ = kPlayZoom;
    match_zoom_target_ = kPlayZoom;
    spawn_combat_armies(kArmy);
    for (int tick = 1; tick <= kEnterTick; ++tick) {
        try {
            step_match_simulation();
        } catch (const std::exception& step_error) {
            report_match_tick_error(step_error.what());
        }
        if (tick == kEnterTick)
            require(enter_photo_mode(), "photo mode did not open");
        if (photo_mode()) {
            set_photo_show_selection(false);
            set_photo_show_health_bars(false);
        }
        render_match_surface();
    }
    require(photo_mode(), "photo mode was not open");
    bool turned = false;
    try {
        turned = turn_live_camera(20.0F, 12.0F);
    } catch (const std::exception&) {
        turned = false;
    }

    bool skip = false;
    {
        const FlagHold presenting(presenting_frame_, true);
        const auto draw_live = [&] {
            try {
                const FlagHold capturing(photo_capture_live_, true);
                return render_live_match_surface();
            } catch (const std::exception&) {
                return false;
            }
        };
        const bool warmed = draw_live();
        if (!turned || live_renderer_unavailable() || !live_renderer_active())
            skip = true;
        else {
            require(warmed && !live_renderer_failed(), "the turned view did not draw");
            const bool drew = draw_live();
            require(
                drew && !live_renderer_failed() && live_renderer_active(),
                "the turned view did not draw"
            );
            require(!match_world_cpu_.rgb.empty(), "the turned view produced an empty picture");
            const auto key = full_overlay_key();
            bool keyed = true;
            for (std::size_t index = 0; keyed && index + 2 < match_world_cpu_.rgb.size();
                 index += 3) {
                keyed = match_world_cpu_.rgb[index] == key[0] &&
                        match_world_cpu_.rgb[index + 1] == key[1] &&
                        match_world_cpu_.rgb[index + 2] == key[2];
            }
            require(!keyed, "the picture is the overlay key");
        }
    }
    // The window presents after the draw, with presenting_frame_ clear, as a
    // played frame does. The card then shows the picture just read back.
    if (!skip) {
        set_display_gamma(kCheckGamma);
        require(!gamma_identity_, "the gamma is the identity");
        const std::vector<uint8_t> expected = gamma_mapped(match_world_cpu_, gamma_table_);
        const uint32_t width = match_world_cpu_.width;
        const uint32_t height = match_world_cpu_.height;
        require(!expected.empty(), "the battlefield could not be copied");
        renderer::Surface presented;
        {
            const CaptureHold hold(capture_frame_, presented);
            present_match_layers();
        }
        take_photo();
        wait_for_photos();
        const std::string saved = saved_photo_name(status_);
        require(!saved.empty(), "status is " + status_);
        const fs::path file = photo_shots_folder() / oa::platform::path_from_utf8(saved);
        std::error_code exists_error;
        require(fs::is_regular_file(file, exists_error), saved);
        const std::vector<uint8_t> rows = png_rgb(file, width, height);
        require(rows == expected, "the photo is not the live picture");
        require_presented_battlefield(
            rows,
            width,
            height,
            presented,
            match_layout_.left,
            match_layout_.top,
            match_layout_.width,
            match_layout_.height
        );
    }
    if (!skip && match_ != nullptr && screen_ == Screen::match) {
        if (photo_mode())
            leave_photo_mode();
        require(enter_photo_mode(), "photo mode did not open again");
        const std::string line = photo_status_line();
        require(line == fresh_photo_line(photo_frozen(), match_->state().game.tick), line);
        leave_photo_mode();
    }
    // A held picture of the turned view in a shared match, the frames drawn
    // as played frames are. The match runs on under it, and the photo is the
    // held battlefield with the player's gamma once, not the live one.
    if (!skip && match_ != nullptr && screen_ == Screen::match) {
        const SharedMatchHold shared(extension_);
        require(enter_photo_mode(), "photo mode did not open in the shared match");
        require(!photo_frozen(), "the shared match froze");
        set_photo_show_selection(false);
        set_photo_show_health_bars(false);
        bool turned_again = false;
        try {
            turned_again = turn_live_camera(20.0F, 12.0F);
        } catch (const std::exception&) {
            turned_again = false;
        }
        require(turned_again, "the turned view did not turn again");
        set_display_gamma(kCheckGamma);
        require(!gamma_identity_, "the gamma is the identity");
        render();
        render();
        require(live_renderer_active() && !live_renderer_failed(), "the turned view did not draw");
        const bool on_card = turned_overlay_painted();
        require(photo_hold_picture() && photo_picture_held(), "the picture was not held");
        const renderer::Surface held{photo_->held_width, photo_->held_height, photo_->held_rgb};
        const std::vector<uint8_t> expected = gamma_mapped(held, gamma_table_);
        require(!expected.empty(), "the held picture is empty");
        const uint32_t held_tick = match_->state().game.tick;
        for (int tick = 0; tick < kHeldTicks; ++tick) {
            try {
                step_match_simulation();
            } catch (const std::exception& step_error) {
                report_match_tick_error(step_error.what());
            }
            render();
        }
        require(
            match_->state().game.tick >= held_tick + static_cast<uint32_t>(kHeldTicks),
            "the match did not run under the held picture"
        );
        renderer::Surface presented;
        {
            const CaptureHold hold(capture_frame_, presented);
            render();
        }
        const auto saved_file = [&] {
            const std::string saved = saved_photo_name(status_);
            require(!saved.empty(), "status is " + status_);
            const fs::path file = photo_shots_folder() / oa::platform::path_from_utf8(saved);
            std::error_code exists_error;
            require(fs::is_regular_file(file, exists_error), status_);
            return file;
        };
        take_photo();
        wait_for_photos();
        const fs::path held_file = saved_file();
        const std::vector<uint8_t> rows = png_rgb(held_file, held.width, held.height);
        require_held_moment(held_file, held_tick);
        if (window_one_to_one(sdl_.window))
            require_presented_battlefield(
                rows,
                held.width,
                held.height,
                presented,
                match_layout_.left,
                match_layout_.top,
                match_layout_.width,
                match_layout_.height
            );
        require(rows == expected, "the photo is not the held picture");
        require(photo_hold_picture() && !photo_picture_held(), "the picture was not let go");
        render();
        take_photo();
        wait_for_photos();
        require(decode_png(saved_file()).rows != rows, "the held photo is the live picture");
        leave_photo_mode();
        std::printf(
            "photo capture live held: %s %ux%u\n",
            on_card ? "card" : "processor",
            held.width,
            held.height
        );
        std::fflush(stdout);
    }
    end_fight();
    sweep_pictures();
    if (skip)
        return 77;
    check_photo_selection_box(true);
    std::printf("photo capture live: equal\n");
    std::fflush(stdout);
    return 0;
}

int Runtime::check_photo_depth_live() {
    struct PreferencesHold {
        Runtime& runtime;
        bool existed{false};
        std::vector<char> bytes;

        explicit PreferencesHold(Runtime& host) : runtime(host) {
            const auto& preferences = host.options_.preferences_file;
            if (preferences.has_value())
                existed = fs::exists(*preferences);
            bytes = file_bytes(preferences);
        }

        PreferencesHold(const PreferencesHold&) = delete;
        PreferencesHold& operator=(const PreferencesHold&) = delete;

        ~PreferencesHold() {
            const auto& preferences = runtime.options_.preferences_file;
            if (preferences.has_value()) {
                if (!existed) {
                    std::error_code remove_error;
                    fs::remove(*preferences, remove_error);
                } else {
                    std::ofstream out(*preferences, std::ios::binary | std::ios::trunc);
                    if (out)
                        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                }
            }
            runtime.preferences_dirty_ = false;
        }
    };

    struct FlagHold {
        bool& flag;
        bool previous;

        explicit FlagHold(bool& held, bool value) : flag(held), previous(held) { flag = value; }

        FlagHold(const FlagHold&) = delete;
        FlagHold& operator=(const FlagHold&) = delete;

        ~FlagHold() { flag = previous; }
    };

    const PreferencesHold preferences_hold(*this);

    struct GammaHold {
        Runtime& runtime;
        float previous;

        explicit GammaHold(Runtime& host) : runtime(host), previous(host.display_gamma_) {
            runtime.set_display_gamma(kCheckGamma);
        }

        GammaHold(const GammaHold&) = delete;
        GammaHold& operator=(const GammaHold&) = delete;

        ~GammaHold() { runtime.set_display_gamma(previous); }
    };

    const GammaHold gamma_hold(*this);

    struct PictureSweep {
        Runtime& runtime;

        explicit PictureSweep(Runtime& host) : runtime(host) {}

        PictureSweep(const PictureSweep&) = delete;
        PictureSweep& operator=(const PictureSweep&) = delete;

        ~PictureSweep() {
            remove_saved_pictures(screenshots_folder(runtime.user_folder_, ""));
            remove_saved_pictures(screenshots_folder(runtime.user_folder_, "ridge"));
        }
    };

    const PictureSweep picture_sweep(*this);
    const ClipboardHookHold clipboard_hold;
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("photo depth live check: " + what);
    };
    const auto require = [&](bool ok, const std::string& what) {
        if (!ok)
            fail(what);
    };
    const auto end_fight = [this] {
        if (photo_mode())
            leave_photo_mode();
        if (match_ != nullptr) {
            leave_match();
            load(Screen::main_menu);
        }
    };
    const auto sweep_pictures = [&] {
        remove_saved_pictures(screenshots_folder(user_folder_, ""));
        remove_saved_pictures(screenshots_folder(user_folder_, "ridge"));
    };
    oa::ui::engine_settings::EngineSettings chosen = engine_settings();
    chosen.allow_full_3d = true;
    apply_engine_settings(chosen);

    start_benchmark_skirmish();
    set_display_gamma(kCheckGamma);
    match_layout_ = lay_out_match(options_.match_width, options_.match_height);
    match_zoom_ = kPlayZoom;
    match_zoom_target_ = kPlayZoom;
    spawn_combat_armies(kArmy);
    for (int tick = 1; tick <= kEnterTick; ++tick) {
        try {
            step_match_simulation();
        } catch (const std::exception& step_error) {
            report_match_tick_error(step_error.what());
        }
        if (tick == kEnterTick)
            require(enter_photo_mode(), "photo mode did not open");
        if (photo_mode()) {
            set_photo_show_selection(false);
            set_photo_show_health_bars(false);
        }
        render_match_surface();
    }
    require(photo_mode() && photo_ != nullptr, "photo mode was not open");
    require(photo_frozen(), "the battle was not frozen");
    // A low look across the map, so ground far from the focus is outside the
    // sharp band. The unlocked range holds this pitch.
    bool turned = false;
    try {
        turned = turn_live_camera(30.0F, -40.0F);
    } catch (const std::exception&) {
        turned = false;
    }

    bool skip = false;
    uint32_t width = 0;
    uint32_t height = 0;
    {
        const FlagHold presenting(presenting_frame_, true);
        const auto draw_live = [&] {
            try {
                const FlagHold capturing(photo_capture_live_, true);
                return render_live_match_surface();
            } catch (const std::exception&) {
                return false;
            }
        };
        const bool warmed = draw_live();
        if (!turned || live_renderer_unavailable() || !live_renderer_active())
            skip = true;
        else {
            require(warmed && !live_renderer_failed(), "the turned view did not draw");
            const bool drew = draw_live();
            require(
                drew && !live_renderer_failed() && live_renderer_active(),
                "the turned view did not draw"
            );
            // No float depth means the cues are not offered. The processor
            // does not draw them instead.
            if (!photo_card_keeps_depth())
                skip = true;
            else {
                // A photo's own frame does not pick the focus. This frame is
                // not a photo, so the ground under the window is kept for the
                // four photos of it.
                bool focused = false;
                try {
                    focused = render_live_match_surface();
                } catch (const std::exception&) {
                    focused = false;
                }
                require(
                    focused && !live_renderer_failed() && live_renderer_active(),
                    "the turned view did not draw"
                );
                width = match_world_cpu_.width;
                height = match_world_cpu_.height;
            }
        }
    }
    if (!skip) {
        require(width > 0 && height > 0, "the turned view produced an empty picture");
        photo_->look_preset = 0;
        photo_->look_vignette = 0;
        photo_->look_grain = 0;
        photo_->look_frame = 0;
        photo_->look_tilt_radius = 0;
        photo_->look_haze = 0;
        photo_->look_depth_of_field = 0;
        photo_->size_factor = 1;
        photo_->smooth_factor = 1;
        require(!photo_look_active(), "a processor look is on");
        const uint32_t frozen_tick = match_->state().game.tick;
        const auto shoot = [&] {
            require(!photo_look_active(), "a processor look is on");
            require(live_renderer_active(), "the turned view stopped");
            take_photo();
            wait_for_photos();
            const std::string saved = saved_photo_name(status_);
            require(!saved.empty(), "status is " + status_);
            const fs::path file = photo_shots_folder() / oa::platform::path_from_utf8(saved);
            std::error_code exists_error;
            require(fs::is_regular_file(file, exists_error), saved);
            require(match_->state().game.tick == frozen_tick, "the battle stepped");
            return png_rgb(file, width, height);
        };
        const std::vector<uint8_t> plain = shoot();
        photo_->look_depth_of_field = 24;
        const std::vector<uint8_t> blurred = shoot();
        require(
            photo_->card_dof_pixels == oa::present::photo::photo_tilt_radius(24, 1),
            "depth of field was not drawn"
        );
        require(photo_->card_haze == 0, "haze was drawn with depth of field");
        require(blurred != plain, "depth of field left the picture unchanged");
        photo_->look_depth_of_field = 0;
        photo_->look_haze = 80;
        const std::vector<uint8_t> hazed = shoot();
        require(photo_->card_haze == 80, "haze was not drawn");
        require(photo_->card_dof_pixels == 0, "depth of field was drawn with haze");
        require(hazed != plain, "haze left the picture unchanged");
        // Photo mode keeps the fog: ground the player has never seen, and
        // the void past the map, stay black under both cues. This frame
        // looks across ground never seen, so most of its black is that; the
        // few black pixels a cue may lighten are the fog's soft edge, which
        // the player has seen a little of.
        const auto lit_black = [&](const std::vector<uint8_t>& cued) {
            std::size_t black = 0;
            std::size_t lit = 0;
            for (std::size_t at = 0; at + 2U < plain.size() && at + 2U < cued.size(); at += 3U) {
                if (plain[at] > 3 || plain[at + 1U] > 3 || plain[at + 2U] > 3)
                    continue;
                ++black;
                if (cued[at] > 8 || cued[at + 1U] > 8 || cued[at + 2U] > 8)
                    ++lit;
            }
            return std::pair<std::size_t, std::size_t>{black, lit};
        };
        const auto [black_pixels, hazed_black] = lit_black(hazed);
        const auto [black_again, blurred_black] = lit_black(blurred);
        std::printf(
            "photo depth live: of %zu black pixels, haze lightens %zu and depth of field %zu\n",
            black_pixels,
            hazed_black,
            blurred_black
        );
        require(
            black_pixels == black_again && black_pixels >= plain.size() / 30U,
            "the frame shows no ground never seen"
        );
        require(hazed_black * 20U <= black_pixels, "haze lightens the fog");
        require(blurred_black * 20U <= black_pixels, "depth of field lightens the fog");
        photo_->look_haze = 0;
        const std::vector<uint8_t> again = shoot();
        require(
            photo_->card_dof_pixels == 0 && photo_->card_haze == 0, "a cue was drawn with both off"
        );
        require(again == plain, "the photo with both off differs from the first");

        // A photo drawn larger blurs as many times more pixels, smooth
        // edges' draw included, so that it matches the preview: Smooth edges
        // 2x with depth of field is the 2x photo with depth of field
        // box-reduced, as it is with no look. The second keeps the focus
        // the preview picked, though the first put the window back. The
        // records name the blur each saved picture has.
        const auto saved_record = [&] {
            const std::string saved = saved_photo_name(status_);
            const fs::path file = photo_shots_folder() / oa::platform::path_from_utf8(saved);
            std::ifstream in(file, std::ios::binary);
            require(static_cast<bool>(in), "the photo could not be read");
            return std::string(
                (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()
            );
        };
        require(
            saved_record().find("\"look\":") == std::string::npos,
            "a photo with both cues off records a look"
        );
        const auto shoot_at = [&](int size_factor, int smooth_step, uint32_t times) {
            set_photo_size_factor(size_factor);
            set_photo_smooth_factor(smooth_step);
            set_photo_size_factor(size_factor);
            set_photo_smooth_factor(smooth_step);
            require(
                photo_->size_factor == size_factor && photo_->smooth_factor == smooth_step,
                "size " + std::to_string(size_factor) + " smooth " + std::to_string(smooth_step) +
                    " was not offered"
            );
            take_photo();
            wait_for_photos();
            const std::string saved = saved_photo_name(status_);
            require(!saved.empty(), "status is " + status_);
            require(match_->state().game.tick == frozen_tick, "the battle stepped");
            return png_rgb(
                photo_shots_folder() / oa::platform::path_from_utf8(saved),
                width * times,
                height * times
            );
        };
        photo_->look_depth_of_field = 24;
        const std::vector<uint8_t> two = shoot_at(2, 1, 2U);
        require(
            saved_record().find("\"depth_of_field\":{\"radius\":24,\"radius_pixels\":48}") !=
                std::string::npos,
            "a 2x photo's record does not name twice the blur"
        );
        const std::vector<uint8_t> smooth = shoot_at(1, 2, 1U);
        require(
            saved_record().find("\"depth_of_field\":{\"radius\":24,\"radius_pixels\":24}") !=
                std::string::npos,
            "smooth edges' record does not name the Screen blur"
        );
        std::vector<uint8_t> reduced;
        int32_t reduced_w = 0;
        int32_t reduced_h = 0;
        require(
            oa::present::photo::box_reduce(
                two.data(),
                two.size(),
                static_cast<int32_t>(width * 2U),
                static_cast<int32_t>(height * 2U),
                2,
                reduced,
                reduced_w,
                reduced_h
            ),
            "the 2x photo did not reduce"
        );
        require(
            reduced_w == static_cast<int32_t>(width) && reduced_h == static_cast<int32_t>(height) &&
                reduced == smooth,
            "smooth edges 2x with depth of field is not the 2x photo box-reduced"
        );
        require(smooth != blurred, "smooth edges 2x with depth of field is the Screen photo");
        std::printf(
            "photo depth live: smooth edges 2x with depth of field is the 2x photo box-reduced\n"
        );
        set_photo_smooth_factor(1);
        set_photo_size_factor(1);
        photo_->look_depth_of_field = 0;
    }
    end_fight();
    sweep_pictures();
    if (skip)
        return 77;
    std::printf("photo depth live: ok\n");
    std::fflush(stdout);
    return 0;
}

int Runtime::check_photo_sizes() {
    struct PreferencesHold {
        Runtime& runtime;
        bool existed{false};
        std::vector<char> bytes;

        explicit PreferencesHold(Runtime& host) : runtime(host) {
            const auto& preferences = host.options_.preferences_file;
            if (preferences.has_value())
                existed = fs::exists(*preferences);
            bytes = file_bytes(preferences);
        }

        PreferencesHold(const PreferencesHold&) = delete;
        PreferencesHold& operator=(const PreferencesHold&) = delete;

        ~PreferencesHold() {
            const auto& preferences = runtime.options_.preferences_file;
            if (preferences.has_value()) {
                if (!existed) {
                    std::error_code remove_error;
                    fs::remove(*preferences, remove_error);
                } else {
                    std::ofstream out(*preferences, std::ios::binary | std::ios::trunc);
                    if (out)
                        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                }
            }
            runtime.preferences_dirty_ = false;
        }
    };

    struct GammaHold {
        Runtime& runtime;
        float previous;

        explicit GammaHold(Runtime& host) : runtime(host), previous(host.display_gamma_) {
            runtime.set_display_gamma(kCheckGamma);
        }

        GammaHold(const GammaHold&) = delete;
        GammaHold& operator=(const GammaHold&) = delete;

        ~GammaHold() { runtime.set_display_gamma(previous); }
    };

    struct PictureSweep {
        Runtime& runtime;

        explicit PictureSweep(Runtime& host) : runtime(host) {}

        PictureSweep(const PictureSweep&) = delete;
        PictureSweep& operator=(const PictureSweep&) = delete;

        ~PictureSweep() {
            remove_saved_pictures(screenshots_folder(runtime.user_folder_, ""));
            remove_saved_pictures(screenshots_folder(runtime.user_folder_, "ridge"));
        }
    };

    struct FlagHold {
        bool& flag;
        bool previous;

        explicit FlagHold(bool& held, bool value) : flag(held), previous(held) { flag = value; }

        FlagHold(const FlagHold&) = delete;
        FlagHold& operator=(const FlagHold&) = delete;

        ~FlagHold() { flag = previous; }
    };

    namespace photo = oa::present::photo;

    const PreferencesHold preferences_hold(*this);
    const GammaHold gamma_hold(*this);
    const PictureSweep picture_sweep(*this);
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("photo sizes failed: " + what);
    };
    const auto require = [&](bool ok, const std::string& what) {
        if (!ok)
            fail(what);
    };
    const auto same_world = [&](const DrawReadBack& earlier, const DrawReadBack& later) {
        const char* difference = draw_read_back_difference(earlier, later);
        require(
            difference == nullptr, difference == nullptr ? std::string{} : std::string(difference)
        );
    };
    // The file name carries the clock's second. A photo that crosses that
    // second may change those pixels and no others.
    const auto frames_equal = [&](const renderer::Surface& earlier,
                                  const renderer::Surface& later) {
        require(
            earlier.width == later.width && earlier.height == later.height &&
                !earlier.rgb.empty() && earlier.rgb.size() == later.rgb.size(),
            "the window frames differ in size"
        );
        oa::ui::kit::Rect file_box{};
        bool have_file = false;
        if (LayerScreen* open = oa_layer().find("photo-mode")) {
            const oa::ui::kit::DisplayList listed = open->display_list();
            for (const oa::ui::kit::Control& control : listed.controls) {
                if (control.name == "photo.file") {
                    file_box = control.rect;
                    have_file = true;
                }
            }
        }
        const double density = sdl_.window == nullptr
                                   ? 1.0
                                   : static_cast<double>(SDL_GetWindowPixelDensity(sdl_.window));
        const auto to_px = [&](int32_t points) {
            return static_cast<int32_t>(std::lround(static_cast<double>(points) * density));
        };
        const int32_t file_x = have_file ? to_px(file_box.x) : 0;
        const int32_t file_y = have_file ? to_px(file_box.y) : 0;
        const int32_t file_right = have_file ? to_px(file_box.x + file_box.width) : 0;
        const int32_t file_bottom = have_file ? to_px(file_box.y + file_box.height) : 0;
        for (uint32_t row = 0; row < earlier.height; ++row) {
            for (uint32_t column = 0; column < earlier.width; ++column) {
                const std::size_t at =
                    (static_cast<std::size_t>(row) * earlier.width + column) * 3U;
                if (earlier.rgb[at] == later.rgb[at] &&
                    earlier.rgb[at + 1U] == later.rgb[at + 1U] &&
                    earlier.rgb[at + 2U] == later.rgb[at + 2U])
                    continue;
                const bool in_file = have_file && static_cast<int32_t>(column) >= file_x &&
                                     static_cast<int32_t>(column) < file_right &&
                                     static_cast<int32_t>(row) >= file_y &&
                                     static_cast<int32_t>(row) < file_bottom;
                if (!in_file) {
                    fail(
                        "the window changed at " + std::to_string(column) + "," +
                        std::to_string(row)
                    );
                }
            }
        }
    };
    const auto settle_panel = [&] {
        if (photo_ != nullptr)
            photo_->toast_on = false;
        set_photo_smooth_factor(1);
        set_photo_size_factor(1);
        wait_for_photos();
        if (match_ != nullptr)
            remove_saved_pictures(photo_shots_folder());
    };
    const auto choose_photo = [&](int size_factor, int smooth) {
        set_photo_size_factor(size_factor);
        set_photo_smooth_factor(smooth);
        set_photo_size_factor(size_factor);
        set_photo_smooth_factor(smooth);
        require(
            photo_ != nullptr && photo_->size_factor == size_factor &&
                photo_->smooth_factor == smooth,
            "size " + std::to_string(size_factor) + " smooth " + std::to_string(smooth) +
                " was not offered"
        );
    };
    const auto take_sized = [&](int size_factor, int smooth) {
        choose_photo(size_factor, smooth);
        take_photo();
        wait_for_photos();
        const std::string saved = saved_photo_name(status_);
        require(!saved.empty(), "status is " + status_);
        const fs::path file = photo_shots_folder() / oa::platform::path_from_utf8(saved);
        std::error_code exists_error;
        require(fs::is_regular_file(file, exists_error), saved);
        return decode_png(file);
    };
    const auto capture_window = [&] {
        renderer::Surface frame;
        const CaptureHold hold(capture_frame_, frame);
        render_match_surface();
        present_match_layers();
        return frame;
    };
    const auto step_tick = [this] {
        try {
            step_match_simulation();
        } catch (const std::exception& step_error) {
            report_match_tick_error(step_error.what());
        }
    };
    const auto hold_play_zoom = [&] {
        require(
            least_match_zoom() <= kPlayZoom && most_match_zoom() >= kPlayZoom,
            "zoom 1 is outside the match's play limits"
        );
        match_zoom_ = kPlayZoom;
        match_zoom_target_ = kPlayZoom;
    };
    const auto arm_and_select = [&] {
        oa::Game& game = match_->state().game;
        console::set_console_flags(
            game,
            static_cast<uint16_t>(
                console::console_flags(game) | console::console_flag::selection_boxes
            )
        );
        oa::Unit* commander = nullptr;
        for (const auto& slot : match_->world().slots) {
            if (slot.unit != nullptr && slot.record.type_index != 0 &&
                slot.record.owner_index == match_local_player_) {
                commander = oa::world_unit_at(&match_->state(), slot.unit_index);
                break;
            }
        }
        require(commander != nullptr, "the fight has no local commander");
        commander->flags |= OA_UNIT_FLAG_SELECTED;
        match_command_ = MatchCommand::attack;
        pending_build_type_ = 0;
    };
    const auto start_fight = [&] {
        start_benchmark_skirmish();
        match_layout_ = lay_out_match(options_.match_width, options_.match_height);
        hold_play_zoom();
        spawn_combat_armies(kArmy);
        arm_and_select();
        set_display_gamma(kCheckGamma);
        require(!gamma_identity_, "the gamma is the identity");
    };
    const auto end_fight = [this] {
        if (photo_mode())
            leave_photo_mode();
        if (match_ != nullptr) {
            leave_match();
            load(Screen::main_menu);
        }
    };
    const auto clear_pictures = [&] {
        remove_saved_pictures(screenshots_folder(user_folder_, ""));
        remove_saved_pictures(screenshots_folder(user_folder_, "ridge"));
        if (match_ != nullptr)
            remove_saved_pictures(photo_shots_folder());
    };
    const auto open_photo = [&] {
        start_fight();
        clear_pictures();
        for (int tick = 1; tick <= kEnterTick; ++tick) {
            step_tick();
            if (tick == kEnterTick)
                require(enter_photo_mode(), "photo mode did not open");
            if (photo_mode()) {
                set_photo_show_selection(false);
                set_photo_show_health_bars(false);
            }
            render_match_surface();
        }
        require(photo_mode() && photo_ != nullptr, "photo mode was not open");
        set_display_gamma(kCheckGamma);
        require(!gamma_identity_, "the gamma is the identity");
    };

    // Grain size is in pixels, so a photo twice the battlefield draws each
    // grain twice as large, and carries the grain the preview shows. The
    // criterion: box-reduced by 2, the grain the 2× photo carries (the 2×
    // photo with grain less the 2× photo without) is the grain the Screen
    // photo carries (Screen with grain less Screen without), the same value
    // in every colour channel where neither photo is at 0 or 255, which the
    // grain may have clipped, and those are at least half the channels. The
    // grain is the same hash of the same cells, so no tolerance is needed.
    // A photo drawn in tiles may differ from the whole draw in a few pixels
    // (the ruling of 2026-11-10, the same bound and seam rule as
    // `--check-photo tiles`). Those pixels' channels are not read, and every
    // other channel is held to the criterion exactly.
    // The draw's own pixels cancel out, so it
    // holds in every render mode. Grain left at the panel's size must fail it,
    // so the check tells the two apart. Each photo is also the look on its
    // picture without grain, and the records keep both grain sizes.
    const auto check_grain = [&](const DecodedPng& screen_plain,
                                 const DecodedPng& two_plain,
                                 const char* where,
                                 const photo::TilePlan* tiles = nullptr) {
        require(photo_ != nullptr, "photo mode was not open");
        const uint8_t preset = photo_->look_preset;
        const int32_t vignette = photo_->look_vignette;
        const int32_t grain = photo_->look_grain;
        const int32_t grain_size = photo_->look_grain_size;
        const uint8_t frame = photo_->look_frame;
        photo_->look_preset = 0;
        photo_->look_vignette = 0;
        photo_->look_frame = 0;
        photo_->look_grain = 60;
        photo_->look_grain_size = 3;
        const auto take_grained = [&](int size_factor) {
            choose_photo(size_factor, 1);
            take_photo();
            wait_for_photos();
            const std::string saved = saved_photo_name(status_);
            require(!saved.empty(), "status is " + status_);
            const fs::path file = photo_shots_folder() / oa::platform::path_from_utf8(saved);
            std::ifstream in(file, std::ios::binary);
            require(static_cast<bool>(in), "the grain photo could not be read");
            std::string bytes(
                (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()
            );
            return std::pair<DecodedPng, std::string>{decode_png(file), std::move(bytes)};
        };
        const auto [screen_grain, screen_file] = take_grained(1);
        const auto [two_grain, two_file] = take_grained(2);
        require(
            screen_grain.width == screen_plain.width &&
                screen_grain.height == screen_plain.height && two_grain.width == two_plain.width &&
                two_grain.height == two_plain.height &&
                two_plain.width == screen_plain.width * 2U &&
                two_plain.height == screen_plain.height * 2U,
            "the grain photos are not Screen and twice it"
        );
        std::vector<uint8_t> screen_look = screen_plain.rows;
        apply_photo_look_rgb(screen_look, screen_plain.width, screen_plain.height, 1, 0, 0);
        require(screen_grain.rows == screen_look, "the Screen photo is not its look");
        std::vector<uint8_t> two_look = two_plain.rows;
        apply_photo_look_rgb(two_look, two_plain.width, two_plain.height, 2, 0, 0);
        if (tiles == nullptr) {
            require(
                two_grain.rows == two_look, "the 2× photo is not the look at twice the grain size"
            );
        } else {
            // Drawn in tiles, the 2× photo is the whole draw's look within the
            // ruling, and the look is painted across the tile edges.
            const TileGap gap = measure_tile_gap(
                two_look, two_grain.rows, two_plain.width, two_plain.height, *tiles
            );
            if (gap.different != 0) {
                std::printf("photo sizes%s grain: %s\n", where, tile_gap_words(gap).c_str());
                std::fflush(stdout);
            }
            require(
                gap.within(),
                "the tiled 2× photo is not the look at twice the grain size: " + tile_gap_words(gap)
            );
        }
        std::vector<uint8_t> panel_sized = two_plain.rows;
        apply_photo_look_rgb(panel_sized, two_plain.width, two_plain.height, 1, 0, 0);
        const auto reduced = [&](const std::vector<uint8_t>& rows) {
            std::vector<uint8_t> out;
            int32_t out_w = 0;
            int32_t out_h = 0;
            require(
                photo::box_reduce(
                    rows.data(),
                    rows.size(),
                    static_cast<int32_t>(two_plain.width),
                    static_cast<int32_t>(two_plain.height),
                    2,
                    out,
                    out_w,
                    out_h
                ) && out_w == static_cast<int32_t>(screen_plain.width) &&
                    out_h == static_cast<int32_t>(screen_plain.height),
                "the 2× photo did not reduce to Screen's size"
            );
            return out;
        };
        const std::vector<uint8_t> two_reduced = reduced(two_grain.rows);
        const std::vector<uint8_t> plain_reduced = reduced(two_plain.rows);
        const std::vector<uint8_t> panel_reduced = reduced(panel_sized);
        const auto at_end = [](uint8_t value) { return value == 0 || value == 255; };
        std::size_t kept = 0;
        std::size_t matched = 0;
        std::size_t panel_matched = 0;
        const std::size_t wide = static_cast<std::size_t>(two_plain.width) * 3U;
        for (uint32_t row = 0; row < screen_plain.height; ++row) {
            for (uint32_t column = 0; column < screen_plain.width; ++column) {
                for (uint32_t channel = 0; channel < 3U; ++channel) {
                    const std::size_t at =
                        (static_cast<std::size_t>(row) * screen_plain.width + column) * 3U +
                        channel;
                    const std::size_t corner = static_cast<std::size_t>(row) * 2U * wide +
                                               static_cast<std::size_t>(column) * 6U + channel;
                    if (at_end(screen_grain.rows[at]) || at_end(two_grain.rows[corner]) ||
                        at_end(two_grain.rows[corner + 3U]) ||
                        at_end(two_grain.rows[corner + wide]) ||
                        at_end(two_grain.rows[corner + wide + 3U]))
                        continue;
                    // A tile's draw may differ from the whole draw here, and
                    // that is not the grain's to answer for. A whole photo
                    // never skips.
                    if (two_grain.rows[corner] != two_look[corner] ||
                        two_grain.rows[corner + 3U] != two_look[corner + 3U] ||
                        two_grain.rows[corner + wide] != two_look[corner + wide] ||
                        two_grain.rows[corner + wide + 3U] != two_look[corner + wide + 3U])
                        continue;
                    ++kept;
                    const int preview = static_cast<int>(screen_grain.rows[at]) -
                                        static_cast<int>(screen_plain.rows[at]);
                    const int photo_grain =
                        static_cast<int>(two_reduced[at]) - static_cast<int>(plain_reduced[at]);
                    const int panel_grain =
                        static_cast<int>(panel_reduced[at]) - static_cast<int>(plain_reduced[at]);
                    if (photo_grain == preview)
                        ++matched;
                    if (panel_grain == preview)
                        ++panel_matched;
                }
            }
        }
        const std::size_t channels = screen_plain.rows.size();
        std::size_t plain_ends = 0;
        for (const uint8_t value : screen_plain.rows)
            plain_ends += at_end(value) ? 1U : 0U;
        require(kept * 2U >= channels, "the grain could be read in under half the channels");
        require(matched == kept, "box-reduced, the 2× photo's grain is not the Screen photo's");
        require(panel_matched * 2U < kept, "grain at the panel's size passes as the preview's");
        require(
            screen_file.find("\"grain_size\":3,\"grain_pixels\":3,") != std::string::npos,
            "the Screen photo's record does not keep its grain sizes"
        );
        require(
            two_file.find("\"grain_size\":3,\"grain_pixels\":6,") != std::string::npos,
            "the 2× photo's record does not keep its grain sizes"
        );
        std::printf(
            "photo sizes%s grain: box-reduced 2x carries the Screen grain in %zu of %zu channels "
            "read (%zu in all, %zu already at 0 or 255), grain at the panel's size in %zu\n",
            where,
            matched,
            kept,
            channels,
            plain_ends,
            panel_matched
        );
        std::fflush(stdout);
        photo_->look_preset = preset;
        photo_->look_vignette = vignette;
        photo_->look_grain = grain;
        photo_->look_grain_size = grain_size;
        photo_->look_frame = frame;
    };
    // Off only. A haze photo twice the battlefield box-reduces to the
    // Screen haze photo in every pixel. The amount does not scale.
    const auto check_haze_scale = [&](const char* where) {
        require(photo_ != nullptr, "photo mode was not open");
        const uint8_t preset = photo_->look_preset;
        const int32_t vignette = photo_->look_vignette;
        const int32_t grain = photo_->look_grain;
        const uint8_t frame = photo_->look_frame;
        const int32_t tilt_radius = photo_->look_tilt_radius;
        const int32_t haze = photo_->look_haze;
        photo_->look_preset = 0;
        photo_->look_vignette = 0;
        photo_->look_grain = 0;
        photo_->look_frame = 0;
        photo_->look_tilt_radius = 0;
        photo_->look_haze = 100;
        const DecodedPng screen_haze = take_sized(1, 1);
        const DecodedPng two_haze = take_sized(2, 1);
        require(
            two_haze.width == screen_haze.width * 2U && two_haze.height == screen_haze.height * 2U,
            "the haze photos are not Screen and twice it"
        );
        std::vector<uint8_t> reduced;
        int32_t out_w = 0;
        int32_t out_h = 0;
        require(
            photo::box_reduce(
                two_haze.rows.data(),
                two_haze.rows.size(),
                static_cast<int32_t>(two_haze.width),
                static_cast<int32_t>(two_haze.height),
                2,
                reduced,
                out_w,
                out_h
            ) && out_w == static_cast<int32_t>(screen_haze.width) &&
                out_h == static_cast<int32_t>(screen_haze.height) && reduced == screen_haze.rows,
            "box-reduced, the 2× haze photo is not the Screen haze photo"
        );
        std::printf(
            "photo sizes%s haze: box-reduced 2x matches Screen, %ux%u\n",
            where,
            screen_haze.width,
            screen_haze.height
        );
        std::fflush(stdout);
        photo_->look_preset = preset;
        photo_->look_vignette = vignette;
        photo_->look_grain = grain;
        photo_->look_frame = frame;
        photo_->look_tilt_radius = tilt_radius;
        photo_->look_haze = haze;
    };

    using HardwareAcceleration = oa::ui::engine_settings::HardwareAcceleration;
    const HardwareAcceleration asked =
        options_.hardware_acceleration.value_or(HardwareAcceleration::off);

    // Full without --force-capable is the turned view on the system's video
    // driver. Where no graphics device can draw it, the dummy driver
    // included, the check skips with 77 rather than taking Full's branch.
    if (asked == HardwareAcceleration::full && !options_.force_capable) {
        oa::ui::engine_settings::EngineSettings chosen = engine_settings();
        chosen.allow_full_3d = true;
        chosen.anti_aliasing = oa::ui::engine_settings::AntiAliasing::x4;
        apply_engine_settings(chosen);
        const auto aliasing = engine_settings().anti_aliasing;
        open_photo();
        bool turned = false;
        try {
            turned = turn_live_camera(20.0F, 12.0F);
        } catch (const std::exception&) {
            turned = false;
        }
        bool skip = false;
        {
            const FlagHold presenting(presenting_frame_, true);
            const auto draw_live = [&] {
                try {
                    const FlagHold capturing(photo_capture_live_, true);
                    return render_live_match_surface();
                } catch (const std::exception&) {
                    return false;
                }
            };
            const bool warmed = draw_live();
            if (!turned || live_renderer_unavailable() || !live_renderer_active())
                skip = true;
            else {
                require(warmed && !live_renderer_failed(), "the turned view did not draw");
            }
        }
        if (skip) {
            end_fight();
            return 77;
        }
        render_match_surface();
        const int32_t field_w = match_layout_.battlefield_width();
        const int32_t field_h = match_layout_.battlefield_height();
        require(field_w > 0 && field_h > 0, "the battlefield is empty");
        require(
            engine_settings().anti_aliasing == oa::ui::engine_settings::AntiAliasing::x4,
            "anti-aliasing is not 4×"
        );
        const photo::Machine machine = photo_capture_machine();
        require(
            machine.kind == photo::CaptureKind::turned, "the turned view is not drawing the photo"
        );
        require(
            machine.edge_limit > 0 && machine.edge_limit <= photo::kTurnedEdge,
            "the turned view offers more than 4096 pixels a side"
        );
        const TurnedFrameSampling play = turned_frame_sampling();
        uint32_t requested = static_cast<uint32_t>(engine_settings().anti_aliasing);
        if (requested == 0)
            requested = 1;
        const renderer_live_gpu::Sampling expect_sampling = renderer_live_gpu::fitted_sampling(
            static_cast<uint32_t>(field_w),
            static_cast<uint32_t>(field_h),
            requested,
            play.device_multisamples
        );
        require(
            play.supersample == expect_sampling.supersample &&
                play.multisample == expect_sampling.multisample && play.supersample != 0,
            "the window frame lost samples"
        );
        const photo::Plan planned = photo_size_plan();
        int first_locked = 0;
        for (const photo::Offer& offer : planned.sizes) {
            if (offer.offered) {
                const int32_t draw_edge = std::max(offer.width, offer.height);
                if (draw_edge > machine.edge_limit) {
                    photo::TurnedTileRequest request;
                    request.width = offer.width;
                    request.height = offer.height;
                    request.zoom = (match_zoom_ > 0.0F ? match_zoom_ : 1.0F) *
                                   static_cast<float>(offer.factor);
                    request.draw_limit = photo::turned_tile_draw_limit(machine);
                    require(
                        draw_edge <= photo::kProcessorEdge &&
                            photo::plan_turned_tiles(request).fits,
                        "a size past one frame was offered without tiles"
                    );
                }
                continue;
            }
            if (first_locked != 0 || offer.lock != photo::SizeLock::edge)
                continue;
            first_locked = offer.factor;
            const std::string reason = photo_offer_reason(planned.kind, offer);
            require(reason.find(std::to_string(offer.limit)) != std::string::npos, reason);
            require(reason.find("anti-aliasing") != std::string::npos, reason);
        }
        if (first_locked == 0) {
            std::printf("photo sizes live: every size is drawn in tiles or one frame\n");
            std::fflush(stdout);
        }
        // The fight has written the engine's own rows. The photos must add none.
        const std::vector<char> preferences_settled = file_bytes(options_.preferences_file);
        const auto preferences_values = preference_values_;
        settle_panel();
        const renderer::Surface before = capture_window();
        const DrawReadBack read_before = match_draw_read_back();
        for (const photo::Offer& offer : planned.sizes) {
            if (!offer.offered || offer.factor <= 1)
                continue;
            const DecodedPng shot = take_sized(offer.factor, 1);
            require(
                photo_ != nullptr && photo_->sized_by_live && !photo_->sized_by_full,
                "an offered size was not drawn live"
            );
            require(
                shot.width ==
                        static_cast<uint32_t>(field_w) * static_cast<uint32_t>(offer.factor) &&
                    shot.height ==
                        static_cast<uint32_t>(field_h) * static_cast<uint32_t>(offer.factor),
                "an offered photo is not that many times the battlefield"
            );
        }
        // Smooth edges 2× draws the 2× picture and box-reduces it. A size past
        // the sampling edge is drawn in tiles, and the reduction is still the
        // 2× picture. Where 2× is not offered the comparison runs at Enhanced
        // 2×. The window's own frames stay at 4×.
        DecodedPng two;
        {
            struct AliasingHold {
                Runtime& runtime;
                oa::ui::engine_settings::AntiAliasing level;

                AliasingHold(Runtime& host, oa::ui::engine_settings::AntiAliasing kept)
                    : runtime(host), level(kept) {}

                AliasingHold(const AliasingHold&) = delete;
                AliasingHold& operator=(const AliasingHold&) = delete;

                ~AliasingHold() {
                    oa::ui::engine_settings::EngineSettings chosen = runtime.engine_settings();
                    chosen.anti_aliasing = level;
                    runtime.apply_engine_settings(chosen);
                }
            } aliasing_hold{*this, aliasing};

            bool size_two = false;
            bool smooth_two = false;
            for (const photo::Offer& offer : planned.sizes) {
                if (offer.factor == 2 && offer.offered)
                    size_two = true;
            }
            for (const photo::Offer& offer : planned.smooth) {
                if (offer.factor == 2 && offer.offered)
                    smooth_two = true;
            }
            if (!size_two || !smooth_two) {
                oa::ui::engine_settings::EngineSettings two_samples = engine_settings();
                two_samples.anti_aliasing = oa::ui::engine_settings::AntiAliasing::x2;
                apply_engine_settings(two_samples);
                const photo::Plan opened = photo_size_plan();
                size_two = false;
                smooth_two = false;
                int drawn_larger = 0;
                for (const photo::Offer& offer : opened.sizes) {
                    if (offer.factor == 2 && offer.offered)
                        size_two = true;
                    if (!offer.offered || offer.factor <= 1)
                        continue;
                    // A turned photo that lost samples is refused, so a
                    // photo saved here kept the window's.
                    const DecodedPng shot = take_sized(offer.factor, 1);
                    require(
                        photo_ != nullptr && photo_->sized_by_live && !photo_->sized_by_full,
                        "a size offered at Enhanced 2× was not drawn live"
                    );
                    require(
                        shot.width == static_cast<uint32_t>(field_w) *
                                          static_cast<uint32_t>(offer.factor) &&
                            shot.height == static_cast<uint32_t>(field_h) *
                                               static_cast<uint32_t>(offer.factor),
                        "a photo offered at Enhanced 2× is not that many times the battlefield"
                    );
                    ++drawn_larger;
                }
                for (const photo::Offer& offer : opened.smooth) {
                    if (offer.factor == 2 && offer.offered)
                        smooth_two = true;
                }
                const renderer_live_gpu::Sampling at_two = renderer_live_gpu::fitted_sampling(
                    static_cast<uint32_t>(field_w),
                    static_cast<uint32_t>(field_h),
                    2,
                    play.device_multisamples
                );
                std::printf(
                    "photo sizes live: at Enhanced 2x, sampling %u/%u, %d larger sizes drawn\n",
                    at_two.supersample,
                    at_two.multisample,
                    drawn_larger
                );
            }
            require(size_two && smooth_two, "2× was not offered where the samples stay");
            const DecodedPng screen = take_sized(1, 1);
            two = take_sized(2, 1);
            const DecodedPng smooth = take_sized(1, 2);
            require(
                photo_ != nullptr && photo_->sized_by_live && !photo_->sized_by_full,
                "the turned photo was not drawn live"
            );
            require(
                two.width == static_cast<uint32_t>(field_w) * 2U &&
                    two.height == static_cast<uint32_t>(field_h) * 2U,
                "the 2× photo is not twice the battlefield"
            );
            std::vector<uint8_t> reduced;
            int32_t reduced_w = 0;
            int32_t reduced_h = 0;
            require(
                photo::box_reduce(
                    two.rows.data(),
                    two.rows.size(),
                    static_cast<int32_t>(two.width),
                    static_cast<int32_t>(two.height),
                    2,
                    reduced,
                    reduced_w,
                    reduced_h
                ),
                "the 2× draw did not reduce"
            );
            require(
                reduced_w == static_cast<int32_t>(smooth.width) &&
                    reduced_h == static_cast<int32_t>(smooth.height) && reduced == smooth.rows,
                "Smooth 2× is not the 2× draw box-reduced"
            );
            require(smooth.rows != screen.rows, "Smooth 2× is the Screen photo");
            std::printf("photo sizes live: smooth 2x matches box-reduced 2x\n");
            check_grain(screen, two, " live");
            {
                struct TileEdgeHold {
                    int32_t& edge;

                    TileEdgeHold(int32_t& held, int32_t value) : edge(held) { edge = value; }

                    TileEdgeHold(const TileEdgeHold&) = delete;
                    TileEdgeHold& operator=(const TileEdgeHold&) = delete;

                    ~TileEdgeHold() { edge = 0; }
                };

                const photo::Machine grain_machine = photo_capture_machine();
                const int32_t grain_edge =
                    static_cast<int32_t>(std::max(two.width, two.height) / 4U);
                photo::TurnedTileRequest grain_tiles;
                grain_tiles.width = static_cast<int32_t>(two.width);
                grain_tiles.height = static_cast<int32_t>(two.height);
                grain_tiles.zoom = (match_zoom_ > 0.0F ? match_zoom_ : 1.0F) * 2.0F;
                grain_tiles.draw_limit = photo::turned_tile_draw_limit(grain_machine);
                grain_tiles.tile_edge = grain_edge;
                grain_tiles.blur =
                    photo::photo_tilt_radius(std::clamp(grain_machine.depth_of_field, 0, 32), 2);
                const photo::TilePlan grain_plan = photo::plan_turned_tiles(grain_tiles);
                require(
                    grain_plan.fits && grain_plan.tiles.size() > 1,
                    "the grain photo would not be drawn in tiles"
                );
                const TileEdgeHold grain_edge_hold(photo_check_tile_edge_, grain_edge);
                check_grain(screen, two, " live tiled", &grain_plan);
            }
        }
        require(engine_settings().anti_aliasing == aliasing, "anti-aliasing changed");
        settle_panel();
        const renderer::Surface after = capture_window();
        frames_equal(before, after);
        same_world(read_before, match_draw_read_back());
        require(engine_settings().anti_aliasing == aliasing, "anti-aliasing changed");
        require(
            file_bytes(options_.preferences_file) == preferences_settled &&
                preference_values_ == preferences_values,
            "the preferences file changed"
        );
        end_fight();
        std::printf(
            "photo sizes live: 2x %ux%u edge %d sampling %u/%u\n",
            two.width,
            two.height,
            machine.edge_limit,
            play.supersample,
            play.multisample
        );
        std::printf("photo sizes live: ok\n");
        std::fflush(stdout);
        return 0;
    }

    if (asked == HardwareAcceleration::full) {
        // Enhanced anti-aliasing at 4x, so that the window's world target is
        // supersampled and a photo at a lower factor would show.
        oa::ui::engine_settings::EngineSettings chosen = engine_settings();
        chosen.anti_aliasing = oa::ui::engine_settings::AntiAliasing::x4;
        apply_engine_settings(chosen);
        open_photo();
        require(full_presentation() && full_ != nullptr, "Full is not presenting");
        // Full fits its world target to the factor as it presents a frame.
        std::ignore = capture_window();
        require(full_ != nullptr && full_->supersample > 1, "Full is not supersampling");
        const uint32_t factor = full_->supersample;
        const auto aliasing = engine_settings().anti_aliasing;
        const int32_t field_w = match_layout_.battlefield_width();
        const int32_t field_h = match_layout_.battlefield_height();
        require(field_w > 0 && field_h > 0, "the battlefield is empty");
        // The fight has written the engine's own rows. The photos must add none.
        const std::vector<char> preferences_settled = file_bytes(options_.preferences_file);
        const auto preferences_values = preference_values_;
        settle_panel();
        const renderer::Surface before = capture_window();
        const DrawReadBack read_before = match_draw_read_back();
        require(full_ != nullptr && full_->supersample == factor, "the window's factor changed");
        const DecodedPng two = take_sized(2, 1);
        require(photo_->sized_by_full && !photo_->sized_by_live, "Full did not draw the 2× photo");
        require(photo_->sized_supersample == factor, "Full lowered the anti-aliasing factor");
        require(full_ != nullptr && full_->supersample == factor, "the window's factor changed");
        require(
            two.width == static_cast<uint32_t>(field_w) * 2U &&
                two.height == static_cast<uint32_t>(field_h) * 2U,
            "the 2× photo is not twice the battlefield"
        );
        std::vector<uint8_t> processor;
        const double shot_zoom = static_cast<double>(match_zoom()) * 2.0;
        require(
            draw_processor_photo(
                processor,
                static_cast<int32_t>(two.width),
                static_cast<int32_t>(two.height),
                shot_zoom
            ),
            "the processor frame was not drawn"
        );
        require(processor != two.rows, "Full drew the processor's picture");
        std::printf("photo sizes full: drawn factor %u differs\n", factor);
        // In a replay, photo mode keeps the framed view while a seek is in
        // flight. A photo taken then is the same picture, and the window's
        // next frame still finds that view to put back.
        arm_photo_replay_camera();
        const DecodedPng guarded = take_sized(2, 1);
        const bool still_armed =
            photo_ != nullptr && photo_->guard_camera && photo_->guard_awaiting;
        if (photo_ != nullptr) {
            photo_->guard_camera = false;
            photo_->guard_awaiting = false;
        }
        require(still_armed, "a photo used up the replay seek's framed view");
        require(guarded.rows == two.rows, "a photo during a replay seek drew another view");
        std::printf("photo sizes full: a replay seek's framed view leaves the photo alone\n");
        const DecodedPng screen = take_sized(1, 1);
        const DecodedPng smooth = take_sized(1, 2);
        require(
            photo_ != nullptr && photo_->sized_by_full && !photo_->sized_by_live,
            "Full did not draw Smooth edges 2×"
        );
        require(photo_->sized_supersample == factor, "Full lowered the anti-aliasing factor");
        std::vector<uint8_t> reduced;
        int32_t reduced_w = 0;
        int32_t reduced_h = 0;
        require(
            photo::box_reduce(
                two.rows.data(),
                two.rows.size(),
                static_cast<int32_t>(two.width),
                static_cast<int32_t>(two.height),
                2,
                reduced,
                reduced_w,
                reduced_h
            ),
            "the 2× draw did not reduce"
        );
        require(
            reduced_w == static_cast<int32_t>(smooth.width) &&
                reduced_h == static_cast<int32_t>(smooth.height) && reduced == smooth.rows,
            "Smooth 2× is not the 2× draw box-reduced"
        );
        require(smooth.rows != screen.rows, "Smooth 2× is the Screen photo");
        std::printf("photo sizes full: smooth 2x matches box-reduced 2x\n");
        check_grain(screen, two, " full");
        // The same criterion with the 2× grain photo drawn in tiles: the look
        // is painted once on the joined picture, so its grain cells run on
        // across every tile edge.
        {
            struct TileEdgeHold {
                int32_t& edge;

                TileEdgeHold(int32_t& held, int32_t value) : edge(held) { edge = value; }

                TileEdgeHold(const TileEdgeHold&) = delete;
                TileEdgeHold& operator=(const TileEdgeHold&) = delete;

                ~TileEdgeHold() { edge = 0; }
            };

            const int32_t grain_edge = static_cast<int32_t>(std::max(two.width, two.height) / 4U);
            photo::TileRequest grain_tiles;
            grain_tiles.width = static_cast<int32_t>(two.width);
            grain_tiles.height = static_cast<int32_t>(two.height);
            grain_tiles.zoom = (match_zoom_ > 0.0F ? match_zoom_ : 1.0F) * 2.0F;
            grain_tiles.supersample = static_cast<int>(factor);
            grain_tiles.grain = static_cast<int>(full_supersampling::target_grain);
            grain_tiles.texture_limit = static_cast<int32_t>(std::min(
                render_texture_limit(), static_cast<uint32_t>(std::numeric_limits<int32_t>::max())
            ));
            grain_tiles.tile_edge = grain_edge;
            grain_tiles.reach = photo::full_tile_reach(grain_tiles.zoom);
            const photo::TilePlan grain_plan = photo::plan_tiles(grain_tiles);
            require(
                grain_plan.fits && grain_plan.tiles.size() > 1,
                "the grain photo would not be drawn in tiles"
            );
            const TileEdgeHold grain_edge_hold(photo_check_tile_edge_, grain_edge);
            check_grain(screen, two, " full tiled", &grain_plan);
        }
        require(
            render_run_ != nullptr && render_run_->host != nullptr, "Full has no renderer host"
        );
        // The size row is judged at the smooth step in use. Smooth edges 2×
        // is still selected, and at that step 2× draws four times the
        // battlefield. Read the row at Smooth edges off.
        require(photo_ != nullptr, "photo mode was not open");
        photo_->size_factor = 1;
        photo_->smooth_factor = 1;
        uint32_t limit = 0;
        if (factor <= 1) {
            limit = static_cast<uint32_t>(std::max(field_w, field_h) * 2);
        } else {
            const uint32_t grain = full_supersampling::target_grain;
            const uint32_t across =
                full_supersampling::rounded_up(static_cast<uint32_t>(field_w) * 2U, grain);
            const uint32_t down =
                full_supersampling::rounded_up(static_cast<uint32_t>(field_h) * 2U, grain);
            limit = std::max(across, down) * factor;
        }
        render_run_->host->faults().texture_limit = limit;
        const photo::Plan planned = photo_size_plan();
        const photo::Offer* two_offer = nullptr;
        const photo::Offer* four_offer = nullptr;
        for (const photo::Offer& offer : planned.sizes) {
            if (offer.factor == 2)
                two_offer = &offer;
            if (offer.factor == 4)
                four_offer = &offer;
        }
        require(two_offer != nullptr && two_offer->offered, "2× was not offered under the limit");
        // 4× is past the card in one piece. Full draws it in tiles, so it is
        // offered. The edge sentence is read at a limit no bordered tile fits.
        require(four_offer != nullptr && four_offer->offered, "4× was not offered in tiles");
        const int32_t grain = static_cast<int32_t>(full_supersampling::target_grain);
        render_run_->host->faults().texture_limit =
            static_cast<uint32_t>(grain) * std::max(factor, uint32_t{1});
        const photo::Plan tight = photo_size_plan();
        const photo::Offer* tight_four = nullptr;
        for (const photo::Offer& offer : tight.sizes) {
            if (offer.factor == 4)
                tight_four = &offer;
        }
        require(
            tight_four != nullptr && !tight_four->offered &&
                tight_four->lock == photo::SizeLock::edge,
            "4× was offered where a tile cannot fit"
        );
        const std::string reason = photo_offer_reason(tight.kind, *tight_four);
        require(reason.find(std::to_string(tight_four->limit)) != std::string::npos, reason);
        const int32_t raw = static_cast<int32_t>(grain) * static_cast<int32_t>(factor);
        const int32_t fitted = (raw / static_cast<int32_t>(factor) / grain) * grain;
        require(tight_four->limit == fitted, "the limit is not the picture that fits");
        require(reason.find(std::to_string(fitted)) != std::string::npos, reason);
        if (fitted != raw)
            require(reason.find(std::to_string(raw)) == std::string::npos, reason);
        std::printf("photo sizes full: limit %s\n", reason.c_str());
        render_run_->host->faults().texture_limit = 0;
        require(full_ != nullptr && full_->supersample == factor, "the window's factor changed");
        require(engine_settings().anti_aliasing == aliasing, "anti-aliasing changed");
        settle_panel();
        const renderer::Surface after = capture_window();
        frames_equal(before, after);
        same_world(read_before, match_draw_read_back());
        require(full_ != nullptr && full_->supersample == factor, "the window's factor changed");
        std::printf("photo sizes full: frames equal\n");
        // A held picture is the Screen copy, whatever Size and Smooth edges
        // were. The rows say so. This does not draw the window.
        require(photo_ != nullptr, "photo mode was not open");
        photo_->size_factor = 2;
        photo_->smooth_factor = 2;
        photo_->picture_held = true;
        bool held_reason = false;
        bool held_screen = false;
        bool held_off = false;
        const std::string held_sentence(oa::data::languages::interface_text("Held picture"));
        const std::string screen_word(oa::data::languages::interface_text("Screen"));
        const std::string off_word(oa::data::languages::interface_text("Off"));
        if (LayerScreen* open = oa_layer().find("photo-mode")) {
            const oa::ui::kit::DisplayList listed = open->display_list();
            for (const oa::ui::kit::Control& control : listed.controls) {
                if (control.name == "photo.size.reason")
                    held_reason = control.text == held_sentence;
                if (control.name == "photo.size")
                    held_screen = control.text.find(screen_word) != std::string::npos;
                if (control.name == "photo.smooth")
                    held_off = control.text.find(off_word) != std::string::npos;
            }
        }
        photo_->picture_held = false;
        photo_->size_factor = 1;
        photo_->smooth_factor = 1;
        require(
            held_reason && held_screen && held_off, "a held picture does not say what it saves"
        );
        require(
            file_bytes(options_.preferences_file) == preferences_settled &&
                preference_values_ == preferences_values,
            "the preferences file changed"
        );
        end_fight();
        std::printf("photo sizes full: ok\n");
        std::fflush(stdout);
        return 0;
    }

    const auto play_pass = [&](bool shoot) {
        start_fight();
        clear_pictures();
        for (int tick = 1; tick <= kPassTicks; ++tick) {
            step_tick();
            if (tick == kEnterTick)
                require(enter_photo_mode(), "photo mode did not open");
            if (photo_mode()) {
                set_photo_show_selection(false);
                set_photo_show_health_bars(false);
            }
            render_match_surface();
            if (shoot && tick == kEnterTick) {
                set_display_gamma(kCheckGamma);
                require(!gamma_identity_, "the gamma is the identity");
                const int32_t field_w = match_layout_.battlefield_width();
                const int32_t field_h = match_layout_.battlefield_height();
                require(field_w > 0 && field_h > 0, "the battlefield is empty");
                settle_panel();
                const photo::Plan open_plan = photo_size_plan();
                const std::string pixels_reason(
                    oa::data::languages::interface_text("Off and Basic draw whole pixels")
                );
                for (const photo::Offer& offer : open_plan.smooth) {
                    if (offer.factor == 1)
                        continue;
                    require(
                        !offer.offered && offer.lock == photo::SizeLock::pixels,
                        "Smooth edges is offered where the picture is whole pixels"
                    );
                    require(
                        photo_offer_reason(open_plan.kind, offer) == pixels_reason, pixels_reason
                    );
                }
                for (const photo::Offer& offer : open_plan.sizes) {
                    if (offer.factor <= 1)
                        continue;
                    require(offer.offered, "a larger size was not offered");
                    if (offer.factor == 2) {
                        require(
                            offer.width == field_w * 2 && offer.height == field_h * 2,
                            "2× is not twice the battlefield"
                        );
                    }
                }
                bool saw_pixels = false;
                if (LayerScreen* open = oa_layer().find("photo-mode")) {
                    const oa::ui::kit::DisplayList listed = open->display_list();
                    for (const oa::ui::kit::Control& control : listed.controls) {
                        if (control.name == "photo.size.reason" &&
                            control.text.find(pixels_reason) != std::string::npos)
                            saw_pixels = true;
                    }
                }
                require(saw_pixels, pixels_reason);
                const renderer::Surface before = capture_window();
                const DrawReadBack read_before = match_draw_read_back();
                require(photo_look_scale(1, 100, 100) == 1, "Screen at density 1 grew its grain");
                require(photo_look_scale(2, 200, 100) == 2, "a 2× photo on the map grew its grain");
                require(photo_look_scale(4, 100, 100) == 4, "a wider panel factor was narrowed");
                require(
                    photo_look_scale(1, 200, 100) == 2,
                    "a Screen photo twice the battlefield kept the panel's grain"
                );
                require(
                    photo_look_scale(2, 400, 100) == 4,
                    "a 2× photo at density 2 kept the panel's grain"
                );
                require(
                    photo_haze_step(1, 1.0F, 100, 100) == 256,
                    "haze at density 1 is not one map pixel a picture pixel"
                );
                require(
                    photo_haze_step(1, 1.0F, 200, 100) == 128,
                    "haze at density 2 did not take the photo's width"
                );
                require(
                    photo_haze_step(2, 1.0F, 200, 100) == 128,
                    "haze on a 2× photo is not half a map pixel"
                );
                std::printf("photo sizes: look scale follows the photo's width\n");
                const DecodedPng screen = take_sized(1, 1);
                require(
                    photo_ != nullptr && !photo_->sized_by_full && !photo_->sized_by_live,
                    "Screen was drawn again"
                );
                require(
                    screen.width == static_cast<uint32_t>(field_w) &&
                        screen.height == static_cast<uint32_t>(field_h),
                    "Screen is not the battlefield"
                );
                const DecodedPng wide = take_sized(2, 1);
                require(
                    photo_ != nullptr && !photo_->sized_by_full && !photo_->sized_by_live,
                    "the 2× photo was not drawn by the processor"
                );
                require(
                    wide.width == static_cast<uint32_t>(field_w) * 2U &&
                        wide.height == static_cast<uint32_t>(field_h) * 2U,
                    "the 2× photo is not twice the battlefield"
                );
                {
                    std::vector<uint8_t> reduced_view;
                    int32_t reduced_w = 0;
                    int32_t reduced_h = 0;
                    require(
                        photo::box_reduce(
                            wide.rows.data(),
                            wide.rows.size(),
                            static_cast<int32_t>(wide.width),
                            static_cast<int32_t>(wide.height),
                            2,
                            reduced_view,
                            reduced_w,
                            reduced_h
                        ) && reduced_w == static_cast<int32_t>(screen.width) &&
                            reduced_h == static_cast<int32_t>(screen.height) &&
                            reduced_view == screen.rows,
                        "box-reduced, the 2× photo is not the Screen photo"
                    );
                    std::printf("photo sizes: 2x box-reduced equals Screen\n");
                }
                {
                    struct MemoryHold {
                        Runtime& runtime;
                        std::optional<oa::platform::SystemMemorySample> previous;

                        explicit MemoryHold(Runtime& host) : runtime(host) {
                            if (runtime.render_run_ != nullptr)
                                previous = runtime.render_run_->forced_memory;
                        }

                        MemoryHold(const MemoryHold&) = delete;
                        MemoryHold& operator=(const MemoryHold&) = delete;

                        ~MemoryHold() {
                            if (runtime.render_run_ != nullptr)
                                runtime.render_run_->forced_memory = std::move(previous);
                            clear_remembered_photo();
                        }
                    } memory_hold{*this};
                    require(render_run_ != nullptr, "the run has no renderer");
                    clear_remembered_photo();
                    const auto bytes_at = [&](int factor) {
                        return static_cast<uint64_t>(field_w) * static_cast<uint64_t>(factor) *
                               static_cast<uint64_t>(field_h) * static_cast<uint64_t>(factor) *
                               uint64_t{6};
                    };
                    uint64_t physical = uint64_t{64} << 20;
                    uint64_t floor = physical / render_policy::memory_guard_free_divisor;
                    if (render_run_->watch && render_run_->watch->memory.thresholds.physical != 0) {
                        floor = render_run_->watch->memory.thresholds.free_floor;
                        physical = render_run_->watch->memory.thresholds.physical;
                    }
                    const uint64_t two_bytes = bytes_at(2);
                    const uint64_t three_bytes = bytes_at(3);
                    const uint64_t headroom = two_bytes + (three_bytes - two_bytes) / 2U;
                    oa::platform::SystemMemorySample sample;
                    sample.available = floor + headroom;
                    sample.available_known = true;
                    sample.physical = physical;
                    sample.committed_known = false;
                    render_run_->forced_memory = sample;
                    const photo::Plan tight = photo_size_plan();
                    const photo::Offer* screen_offer = nullptr;
                    const photo::Offer* two_offer = nullptr;
                    const photo::Offer* four_offer = nullptr;
                    for (const photo::Offer& offer : tight.sizes) {
                        if (offer.factor == 1)
                            screen_offer = &offer;
                        if (offer.factor == 2)
                            two_offer = &offer;
                        if (offer.factor == 4)
                            four_offer = &offer;
                    }
                    require(screen_offer != nullptr && screen_offer->offered, "Screen was locked");
                    require(
                        two_offer != nullptr && two_offer->offered,
                        "2× was not offered before the held picture"
                    );
                    require(
                        four_offer != nullptr && !four_offer->offered &&
                            four_offer->lock == photo::SizeLock::memory,
                        "4× was not locked for memory"
                    );
                    require(
                        four_offer->width == field_w * 4 && four_offer->height == field_h * 4,
                        "4× is not four times the battlefield"
                    );
                    const std::string memory_reason(
                        oa::data::languages::interface_text("Not enough memory")
                    );
                    require(
                        photo_offer_reason(tight.kind, *four_offer) == memory_reason, memory_reason
                    );
                    const uint64_t held_bytes = (headroom - two_bytes) + 1U;
                    PhotoClipboardImage image;
                    image.bytes.assign(static_cast<std::size_t>(held_bytes), uint8_t{1});
                    image.mime = "image/png";
                    const PhotoCopyReport held_offer =
                        offer_photo_clipboard("Held.png", std::move(image), false, false, true);
                    require(
                        held_offer.result == PhotoCopyResult::skipped,
                        "the held picture was offered"
                    );
                    require(
                        remembered_photo_bytes() == static_cast<std::size_t>(held_bytes),
                        "the held picture was not counted"
                    );
                    const photo::Plan held_plan = photo_size_plan();
                    const photo::Offer* held_two = nullptr;
                    const photo::Offer* held_screen = nullptr;
                    for (const photo::Offer& offer : held_plan.sizes) {
                        if (offer.factor == 1)
                            held_screen = &offer;
                        if (offer.factor == 2)
                            held_two = &offer;
                    }
                    require(
                        held_screen != nullptr && held_screen->offered,
                        "Screen was locked by the held picture"
                    );
                    require(
                        held_two != nullptr && !held_two->offered &&
                            held_two->lock == photo::SizeLock::memory,
                        "the held picture did not lock 2×"
                    );
                    require(
                        photo_offer_reason(held_plan.kind, *held_two) == memory_reason,
                        memory_reason
                    );
                }
                // With the memory back, 4× is drawn: the largest photo the
                // fight offers, four times the battlefield.
                const DecodedPng four = take_sized(4, 1);
                require(
                    photo_ != nullptr && !photo_->sized_by_full && !photo_->sized_by_live,
                    "the 4× photo was not drawn by the processor"
                );
                require(
                    four.width == static_cast<uint32_t>(field_w) * 4U &&
                        four.height == static_cast<uint32_t>(field_h) * 4U,
                    "the 4× photo is not four times the battlefield"
                );
                check_grain(screen, wide, "");
                check_haze_scale("");
                settle_panel();
                const renderer::Surface after = capture_window();
                frames_equal(before, after);
                same_world(read_before, match_draw_read_back());
                const std::string screen_sha = sha256_hex(screen.rows);
                const std::string wide_sha = sha256_hex(wide.rows);
                const std::string four_sha = sha256_hex(four.rows);
                std::printf(
                    "photo sizes screen: %s %ux%u\n",
                    screen_sha.c_str(),
                    screen.width,
                    screen.height
                );
                std::printf(
                    "photo sizes two: %s %ux%u\n", wide_sha.c_str(), wide.width, wide.height
                );
                std::printf(
                    "photo sizes four: %s %ux%u\n", four_sha.c_str(), four.width, four.height
                );
                std::printf(
                    "photo sizes pinned: %s %s %s\n",
                    screen_sha.c_str(),
                    wide_sha.c_str(),
                    four_sha.c_str()
                );
                std::fflush(stdout);
                // The camera past the map's edges, as far as the view goes:
                // the larger photo shows the preview's view, margin and all,
                // and haze lands on the same ground, so box-reduced it is the
                // Screen photo. The whole map is drawn, not the fog, so the
                // ground on either side of an edge tells the views apart.
                // Everything is put back before the next tick.
                {
                    struct ViewKeep {
                        Runtime& runtime;
                        int32_t camera_x;
                        int32_t camera_z;
                        uint32_t flags;
                        ExactView exact;
                        ViewHold hold;
                        uint8_t sight;
                        int32_t haze;

                        explicit ViewKeep(Runtime& host)
                            : runtime(host), camera_x(host.match_camera_x_),
                              camera_z(host.match_camera_z_), flags(host.match_camera_flags_),
                              exact(host.exact_view_), hold(host.view_hold_),
                              sight(host.match_->state().game.visibility_flags),
                              haze(host.photo_ != nullptr ? host.photo_->look_haze : 0) {}

                        ViewKeep(const ViewKeep&) = delete;
                        ViewKeep& operator=(const ViewKeep&) = delete;

                        ~ViewKeep() {
                            runtime.match_camera_x_ = camera_x;
                            runtime.match_camera_z_ = camera_z;
                            runtime.match_camera_flags_ = flags;
                            runtime.exact_view_ = exact;
                            runtime.view_hold_ = hold;
                            if (runtime.match_ != nullptr)
                                runtime.match_->state().game.visibility_flags = sight;
                            if (runtime.photo_ != nullptr)
                                runtime.photo_->look_haze = haze;
                        }
                    } view_keep{*this};
                    match_->state().game.visibility_flags = static_cast<uint8_t>(
                        view_keep.sight & ~(console::visibility_flag::mapping |
                                            console::visibility_flag::line_of_sight)
                    );
                    const auto [map_width, map_height] = shown_map_size();
                    struct Edge {
                        const char* name;
                        int32_t x;
                        int32_t z;
                    };
                    const Edge edges[] = {
                        {"left and top", -field_w, -field_h},
                        {"right and bottom", map_width, map_height}
                    };
                    const auto reduced_two = [&](const DecodedPng& two) {
                        std::vector<uint8_t> out;
                        int32_t out_w = 0;
                        int32_t out_h = 0;
                        require(
                            photo::box_reduce(
                                two.rows.data(),
                                two.rows.size(),
                                static_cast<int32_t>(two.width),
                                static_cast<int32_t>(two.height),
                                2,
                                out,
                                out_w,
                                out_h
                            ) && out_w == field_w &&
                                out_h == field_h,
                            "the 2× photo did not reduce to Screen's size"
                        );
                        return out;
                    };
                    for (const Edge& edge : edges) {
                        photo_->look_haze = 0;
                        set_camera_position(edge.x, edge.z, 0);
                        (void)capture_window();
                        const bool past = edge.x < 0 ? match_camera_x_ < 0 && match_camera_z_ < 0
                                                     : match_camera_x_ + field_w > map_width &&
                                                           match_camera_z_ + field_h > map_height;
                        require(past, std::string("the camera is not past the ") + edge.name);
                        const DecodedPng edge_screen = take_sized(1, 1);
                        const DecodedPng edge_two = take_sized(2, 1);
                        require(
                            reduced_two(edge_two) == edge_screen.rows,
                            std::string("past the ") + edge.name +
                                ", box-reduced, the 2× photo is not the Screen photo"
                        );
                        photo_->look_haze = 60;
                        (void)capture_window();
                        const DecodedPng hazed_screen = take_sized(1, 1);
                        const DecodedPng hazed_two = take_sized(2, 1);
                        require(
                            hazed_screen.rows != edge_screen.rows &&
                                reduced_two(hazed_two) == hazed_screen.rows,
                            std::string("past the ") + edge.name +
                                ", box-reduced, the 2× haze photo is not the Screen one"
                        );
                        std::printf(
                            "photo sizes: past the %s (camera %d,%d), 2x box-reduced equals "
                            "Screen, haze too\n",
                            edge.name,
                            match_camera_x_,
                            match_camera_z_
                        );
                    }
                    std::fflush(stdout);
                }
                settle_panel();
                (void)capture_window();
            }
            if (tick == kLeaveTick)
                leave_photo_mode();
        }
        require(!photo_mode(), "photo mode was still open at the last tick");
        DrawReadBack read = match_draw_read_back();
        end_fight();
        return read;
    };
    // A fight writes the engine's own rows. The pass that takes no photo
    // settles those. The pass that takes photos must add nothing.
    const DrawReadBack quiet = play_pass(false);
    const std::vector<char> preferences_settled = file_bytes(options_.preferences_file);
    const auto preferences_values = preference_values_;
    const DrawReadBack shot = play_pass(true);
    same_world(shot, quiet);
    require(
        file_bytes(options_.preferences_file) == preferences_settled &&
            preference_values_ == preferences_values,
        "the preferences file changed"
    );
    std::printf("photo sizes: ok\n");
    std::fflush(stdout);
    return 0;
}

int Runtime::check_photo_poster() {
    struct PreferencesHold {
        Runtime& runtime;
        bool existed{false};
        std::vector<char> bytes;

        explicit PreferencesHold(Runtime& host) : runtime(host) {
            const auto& preferences = host.options_.preferences_file;
            if (preferences.has_value())
                existed = fs::exists(*preferences);
            bytes = file_bytes(preferences);
        }

        PreferencesHold(const PreferencesHold&) = delete;
        PreferencesHold& operator=(const PreferencesHold&) = delete;

        ~PreferencesHold() {
            const auto& preferences = runtime.options_.preferences_file;
            if (preferences.has_value()) {
                if (!existed) {
                    std::error_code remove_error;
                    fs::remove(*preferences, remove_error);
                } else {
                    std::ofstream out(*preferences, std::ios::binary | std::ios::trunc);
                    if (out)
                        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
                }
            }
            runtime.preferences_dirty_ = false;
        }
    };

    struct GammaHold {
        Runtime& runtime;
        float previous;

        explicit GammaHold(Runtime& host) : runtime(host), previous(host.display_gamma_) {
            runtime.set_display_gamma(kCheckGamma);
        }

        GammaHold(const GammaHold&) = delete;
        GammaHold& operator=(const GammaHold&) = delete;

        ~GammaHold() { runtime.set_display_gamma(previous); }
    };

    struct PictureSweep {
        Runtime& runtime;

        explicit PictureSweep(Runtime& host) : runtime(host) {}

        PictureSweep(const PictureSweep&) = delete;
        PictureSweep& operator=(const PictureSweep&) = delete;

        ~PictureSweep() {
            remove_saved_pictures(screenshots_folder(runtime.user_folder_, ""));
            remove_saved_pictures(screenshots_folder(runtime.user_folder_, "ridge"));
        }
    };

    namespace photo = oa::present::photo;

    const PreferencesHold preferences_hold(*this);
    const GammaHold gamma_hold(*this);
    const PictureSweep picture_sweep(*this);
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("photo poster failed: " + what);
    };
    const auto require = [&](bool ok, const std::string& what) {
        if (!ok)
            fail(what);
    };

    const auto step_tick = [this] {
        try {
            step_match_simulation();
        } catch (const std::exception& step_error) {
            report_match_tick_error(step_error.what());
        }
    };
    const auto end_fight = [this] {
        if (photo_mode())
            leave_photo_mode();
        if (match_ != nullptr) {
            leave_match();
            load(Screen::main_menu);
        }
    };
    const auto take_one = [&] {
        take_photo();
        wait_for_photos();
        const std::string saved = saved_photo_name(status_);
        require(!saved.empty(), "status is " + status_);
        const fs::path file = photo_shots_folder() / oa::platform::path_from_utf8(saved);
        std::error_code exists_error;
        require(fs::is_regular_file(file, exists_error), saved);
        DecodedPng decoded = decode_png(file);
        remove_saved_pictures(photo_shots_folder());
        return std::pair<DecodedPng, fs::path>{std::move(decoded), file};
    };

    start_benchmark_skirmish();
    match_layout_ = lay_out_match(options_.match_width, options_.match_height);
    require(
        least_match_zoom() <= kPlayZoom && most_match_zoom() >= kPlayZoom,
        "zoom 1 is outside the match's play limits"
    );
    match_zoom_ = kPlayZoom;
    match_zoom_target_ = kPlayZoom;
    spawn_combat_armies(kArmy);
    // The armies' camera frames the fight: the ground the player sees.
    const int32_t fight_x = match_camera_x_ + match_layout_.battlefield_width() / 2;
    const int32_t fight_z = match_camera_z_ + match_layout_.battlefield_height() / 2;
    set_display_gamma(kCheckGamma);
    for (int tick = 1; tick <= kEnterTick; ++tick) {
        step_tick();
        if (tick == kEnterTick)
            require(enter_photo_mode(), "photo mode did not open");
    }
    require(photo_frozen(), "entering a local match did not freeze the picture");
    set_photo_show_selection(false);
    set_photo_show_health_bars(false);
    if (photo_ != nullptr)
        photo_->guide = 0;
    set_photo_smooth_factor(1);
    set_photo_size_factor(1);
    match_tracking_ = false;
    exact_view_.held = false;

    const oa::Game& game = match_->state().game;
    const int32_t map_w = game.map_pixel_width;
    const int32_t map_h = game.map_pixel_height;
    const int32_t view_w = match_layout_.battlefield_width();
    const int32_t view_h = match_layout_.battlefield_height();
    require(
        view_w > 0 && view_h > 0 && map_w >= view_w * 2 && map_h >= view_h,
        "the map has no room for two views"
    );
    // The join between the two views runs through the fight, so both sides
    // hold drawn ground and units, not only ground the player never saw.
    // The left view may reach half a view past the map's edge, as the
    // window's may; the rows start on the 16-pixel grid.
    const int32_t origin_x = std::clamp(fight_x - view_w, -(view_w / 2), map_w - view_w * 2);
    const int32_t origin_z = (std::clamp(fight_z - view_h / 2, 0, map_h - view_h) / 16) * 16;
    require(
        origin_x + view_w * 2 <= map_w && origin_z + view_h <= map_h,
        "two views do not fit on the map"
    );

    const auto show = [&](int32_t camera_x, int32_t camera_z) {
        match_camera_x_ = camera_x;
        match_camera_z_ = camera_z;
        match_zoom_ = kPlayZoom;
        match_zoom_target_ = kPlayZoom;
        exact_view_.held = false;
        render_match_surface();
        present_match_layers();
    };

    struct Kept {
        uint16_t console{0};
        decltype(game.visibility_flags) visibility{};
        decltype(game.sim_run_flags) sim_run{};
        int32_t lines{0};
        std::vector<uint8_t> chat;
        std::vector<std::string> log;
    };

    const auto keep = [&] {
        const oa::Game& now = match_->state().game;
        Kept kept;
        kept.console = console::console_flags(now);
        kept.visibility = now.visibility_flags;
        kept.sim_run = now.sim_run_flags;
        kept.lines = oa::ui::hud::message_lines(now);
        kept.chat.resize(static_cast<std::size_t>(OA_CHAT_LINE_COUNT) * OA_CHAT_LINE_BYTES);
        std::memcpy(kept.chat.data(), now.chat_lines, kept.chat.size());
        kept.log = match_message_lines();
        return kept;
    };
    show(origin_x, origin_z);
    require(
        view_camera() == std::array<int32_t, 2>{origin_x, origin_z},
        "the view was held away from " + std::to_string(origin_x) + "," + std::to_string(origin_z)
    );
    const DecodedPng left = take_one().first;
    show(origin_x + view_w, origin_z);
    const DecodedPng right = take_one().first;
    show(origin_x, origin_z);
    const Kept before = keep();
    const std::vector<char> preferences_before = file_bytes(options_.preferences_file);
    const auto preference_values_before = preference_values_;

    require(
        left.width == right.width && left.height == right.height, "the two views differ in size"
    );
    require(
        static_cast<int32_t>(left.width) == view_w && static_cast<int32_t>(left.height) == view_h,
        "a 1:1 photo is not the view, " + std::to_string(left.width) + "x" +
            std::to_string(left.height)
    );

    // Full is on only once the match is presenting. Sampling it before the
    // fight starts reports the processor, then rejects the card's poster.
    const bool full = full_presentation();
    if (full)
        photo_check_tile_edge_ = view_w;
    set_photo_poster_rect(origin_x, origin_z, view_w * 2, view_h);
    require(photo_ != nullptr && photo_->poster, "the two-view poster was not offered");
    take_photo();
    const bool drew_full = photo_ != nullptr && photo_->sized_by_full;
    const bool drew_live = photo_ != nullptr && photo_->sized_by_live;
    wait_for_photos();
    const std::string poster_name = saved_photo_name(status_);
    require(!poster_name.empty(), "status is " + status_);
    const fs::path poster_file = photo_shots_folder() / oa::platform::path_from_utf8(poster_name);
    const DecodedPng poster = decode_png(poster_file);
    {
        std::ifstream poster_in(poster_file, std::ios::binary);
        require(static_cast<bool>(poster_in), "the poster could not be read");
        const std::vector<char> stored{
            std::istreambuf_iterator<char>(poster_in), std::istreambuf_iterator<char>()
        };
        const std::span<const uint8_t> file_bytes_view(
            reinterpret_cast<const uint8_t*>(stored.data()), stored.size()
        );
        std::vector<oa::formats::png::TextChunk> texts;
        require(
            oa::formats::png::read_texts(file_bytes_view, &texts),
            "the poster's text could not be read"
        );
        std::string record_text;
        for (const oa::formats::png::TextChunk& chunk : texts) {
            if (chunk.keyword == "OpenAnnihilation")
                record_text = std::string(chunk.text);
        }
        oa::formats::json::JsonError json_error;
        const std::optional<oa::formats::json::Json> record =
            oa::formats::json::parse_json(record_text, json_error);
        require(record.has_value(), "the poster's record is not JSON");
        const oa::formats::json::Json* size = record->find("size");
        const oa::formats::json::Json* size_width = size == nullptr ? nullptr : size->find("width");
        const oa::formats::json::Json* size_height =
            size == nullptr ? nullptr : size->find("height");
        const oa::formats::json::Json* smooth = size == nullptr ? nullptr : size->find("smooth");
        const oa::formats::json::Json* camera = record->find("camera");
        const oa::formats::json::Json* camera_x = camera == nullptr ? nullptr : camera->find("x");
        const oa::formats::json::Json* camera_z = camera == nullptr ? nullptr : camera->find("z");
        const oa::formats::json::Json* camera_zoom =
            camera == nullptr ? nullptr : camera->find("zoom");
        require(
            size_width != nullptr && size_width->integer() == static_cast<int64_t>(poster.width) &&
                size_height != nullptr &&
                size_height->integer() == static_cast<int64_t>(poster.height) &&
                smooth != nullptr && smooth->integer() == 1,
            "the poster's record does not name its pixels"
        );
        require(
            camera_x != nullptr && camera_x->integer() == origin_x && camera_z != nullptr &&
                camera_z->integer() == origin_z && camera_zoom != nullptr &&
                camera_zoom->integer() == 1,
            "the poster's record does not name zoom 1 at the poster's origin"
        );
    }
    if (full)
        require(drew_full && !drew_live, "Full did not draw the poster");
    else
        require(!drew_full && !drew_live, "the processor poster was drawn by the card");
    remove_saved_pictures(photo_shots_folder());
    photo_check_tile_edge_ = 0;

    require(
        poster.width == left.width + right.width && poster.height == left.height,
        "the poster is " + std::to_string(poster.width) + "x" + std::to_string(poster.height)
    );
    std::vector<uint8_t> beside(poster.rows.size());
    for (uint32_t row = 0; row < left.height; ++row) {
        const std::size_t left_from = static_cast<std::size_t>(row) * left.width * 3U;
        const std::size_t to = static_cast<std::size_t>(row) * poster.width * 3U;
        std::memcpy(
            beside.data() + to,
            left.rows.data() + left_from,
            static_cast<std::size_t>(left.width) * 3U
        );
        std::memcpy(
            beside.data() + to + static_cast<std::size_t>(left.width) * 3U,
            right.rows.data() + left_from,
            static_cast<std::size_t>(right.width) * 3U
        );
    }
    uint64_t black = 0;
    for (std::size_t at = 0; at + 2 < poster.rows.size(); at += 3) {
        if (poster.rows[at] == 0 && poster.rows[at + 1] == 0 && poster.rows[at + 2] == 0)
            ++black;
    }
    require(black > 0, "the poster shows no unseen ground");
    // Drawn ground on both sides of the join, so the join is compared on
    // pictures, not on black.
    uint64_t seen_left = 0;
    uint64_t seen_right = 0;
    for (uint32_t row = 0; row < poster.height; ++row) {
        for (uint32_t column = left.width - 8U; column < left.width + 8U; ++column) {
            const std::size_t at = (static_cast<std::size_t>(row) * poster.width + column) * 3U;
            const bool seen =
                poster.rows[at] != 0 || poster.rows[at + 1] != 0 || poster.rows[at + 2] != 0;
            if (seen && column < left.width)
                ++seen_left;
            else if (seen)
                ++seen_right;
        }
    }
    require(
        seen_left > poster.height && seen_right > poster.height,
        "the join does not run through seen ground: " + std::to_string(seen_left) + " and " +
            std::to_string(seen_right) + " pixels"
    );

    if (!full) {
        require(beside == poster.rows, "the poster differs from the two photos");
    } else {
        photo::TileRequest request;
        request.width = static_cast<int32_t>(poster.width);
        request.height = static_cast<int32_t>(poster.height);
        request.zoom = 1.0F;
        request.supersample = std::max(photo_capture_machine().supersample, 1);
        request.grain = std::max(photo_capture_machine().grain, 1);
        request.texture_limit = photo_capture_machine().edge_limit;
        request.tile_edge = view_w;
        request.reach = photo::full_tile_reach(1.0F);
        const photo::TilePlan tiles = photo::plan_tiles(request);
        require(tiles.fits && tiles.tiles.size() > 1U, "the Full poster was not drawn in tiles");
        const TileGap gap =
            measure_tile_gap(beside, poster.rows, poster.width, poster.height, tiles);
        require(gap.within(), "the Full poster: " + tile_gap_words(gap));
    }

    // Two views tall, beside the fight: the strips stacked down the map join
    // as two photos one above the other.
    set_photo_poster(false);
    if (origin_z + view_h * 2 <= map_h && view_h % 16 == 0) {
        show(origin_x + view_w, origin_z + view_h);
        const DecodedPng below = take_one().first;
        show(origin_x, origin_z);
        photo_check_tile_edge_ = full ? view_w : 0;
        set_photo_poster_rect(origin_x + view_w, origin_z, view_w, view_h * 2);
        require(photo_ != nullptr && photo_->poster, "the two-view-tall poster was not offered");
        const DecodedPng tall = take_one().first;
        photo_check_tile_edge_ = 0;
        set_photo_poster(false);
        require(
            tall.width == right.width && tall.height == right.height * 2,
            "the tall poster is " + std::to_string(tall.width) + "x" + std::to_string(tall.height)
        );
        std::vector<uint8_t> stacked = right.rows;
        stacked.insert(stacked.end(), below.rows.begin(), below.rows.end());
        if (!full) {
            require(stacked == tall.rows, "the tall poster differs from the two photos");
        } else {
            photo::TileRequest request;
            request.width = static_cast<int32_t>(tall.width);
            request.height = static_cast<int32_t>(tall.height);
            request.zoom = 1.0F;
            request.supersample = std::max(photo_capture_machine().supersample, 1);
            request.grain = std::max(photo_capture_machine().grain, 1);
            request.texture_limit = photo_capture_machine().edge_limit;
            request.tile_edge = view_w;
            request.reach = photo::full_tile_reach(1.0F);
            const TileGap gap = measure_tile_gap(
                stacked, tall.rows, tall.width, tall.height, photo::plan_tiles(request)
            );
            require(gap.within(), "the tall Full poster: " + tile_gap_words(gap));
        }
    }

    // Haze is painted once on the poster, from the poster's own corner at
    // zoom 1, wherever the window looks.
    if (photo_ != nullptr) {
        photo_->look_haze = 60;
        show(
            std::min(origin_x + view_w * 3 / 2, map_w - view_w),
            std::min(origin_z + view_h, map_h - view_h)
        );
        photo_check_tile_edge_ = full ? view_w : 0;
        set_photo_poster_rect(origin_x, origin_z, view_w * 2, view_h);
        require(photo_->poster, "the hazed poster was not offered");
        const DecodedPng hazed = take_one().first;
        photo_check_tile_edge_ = 0;
        set_photo_poster(false);
        std::vector<uint8_t> expected = poster.rows;
        apply_photo_look_rgb(
            expected,
            poster.width,
            poster.height,
            1,
            0,
            0,
            std::array<int32_t, 2>{origin_x, origin_z}
        );
        photo_->look_haze = 0;
        show(origin_x, origin_z);
        require(expected != poster.rows, "the haze changed nothing on the poster");
        require(
            hazed.width == poster.width && hazed.height == poster.height && hazed.rows == expected,
            "the poster's haze is not painted from the poster's corner"
        );
    }

    // Grain and the tilt band keep the size against the ground the preview
    // shows. A poster's pixel is a map pixel, so they grow by one over the
    // window's zoom, not by the poster's width: a poster two views wide
    // keeps the panel's sizes at zoom 1 and doubles them at zoom 0.5. Haze
    // and depth of field stay off, so this picture is only that scale.
    if (photo_ != nullptr) {
        const uint8_t preset = photo_->look_preset;
        const int32_t vignette = photo_->look_vignette;
        const int32_t grain = photo_->look_grain;
        const int32_t grain_size = photo_->look_grain_size;
        const uint8_t frame = photo_->look_frame;
        const int32_t haze = photo_->look_haze;
        const int32_t tilt_centre = photo_->look_tilt_centre;
        const int32_t tilt_width = photo_->look_tilt_width;
        const int32_t tilt_radius = photo_->look_tilt_radius;
        const int32_t depth = photo_->look_depth_of_field;
        photo_->look_preset = 0;
        photo_->look_vignette = 0;
        photo_->look_frame = 0;
        photo_->look_haze = 0;
        photo_->look_depth_of_field = 0;
        photo_->look_grain = 50;
        photo_->look_grain_size = 3;
        photo_->look_tilt_centre = 50;
        photo_->look_tilt_width = 20;
        photo_->look_tilt_radius = 4;
        const auto grain_poster_at = [&](float zoom, int factor, const char* where) {
            show(origin_x, origin_z);
            match_zoom_ = zoom;
            match_zoom_target_ = zoom;
            set_photo_poster_rect(origin_x, origin_z, view_w * 2, view_h);
            require(photo_->poster, std::string("the grain poster was not offered ") + where);
            take_photo();
            wait_for_photos();
            const std::string saved = saved_photo_name(status_);
            require(
                !saved.empty(),
                std::string("the grain poster was not saved ") + where + " (" + status_ + ")"
            );
            const fs::path file = photo_shots_folder() / oa::platform::path_from_utf8(saved);
            std::ifstream grain_in(file, std::ios::binary);
            require(static_cast<bool>(grain_in), "the grain poster could not be read");
            const std::string grain_bytes(
                (std::istreambuf_iterator<char>(grain_in)), std::istreambuf_iterator<char>()
            );
            grain_in.close();
            const DecodedPng grained = decode_png(file);
            remove_saved_pictures(photo_shots_folder());
            require(
                static_cast<int32_t>(grained.width) == view_w * 2,
                std::string("the grain poster is not two views wide ") + where
            );
            const std::string grain_record =
                "\"grain_size\":3,\"grain_pixels\":" + std::to_string(3 * factor) + ",";
            require(
                grain_bytes.find(grain_record) != std::string::npos,
                std::string("the poster record does not keep its grain sizes ") + where
            );
            const std::string tilt_record =
                "\"width\":20,\"width_pixels\":" + std::to_string(20 * factor) +
                ",\"radius\":4,\"radius_pixels\":" + std::to_string(4 * factor);
            require(
                grain_bytes.find(tilt_record) != std::string::npos,
                std::string("the poster record does not keep its tilt band ") + where
            );
            set_photo_poster(false);
        };
        grain_poster_at(kPlayZoom, 1, "at zoom 1");
        if (least_match_zoom() <= 0.5F)
            grain_poster_at(0.5F, 2, "at zoom 0.5");
        photo_->look_preset = preset;
        photo_->look_vignette = vignette;
        photo_->look_grain = grain;
        photo_->look_grain_size = grain_size;
        photo_->look_frame = frame;
        photo_->look_haze = haze;
        photo_->look_tilt_centre = tilt_centre;
        photo_->look_tilt_width = tilt_width;
        photo_->look_tilt_radius = tilt_radius;
        photo_->look_depth_of_field = depth;
        show(origin_x, origin_z);
    }

    const Kept after = keep();
    require(
        after.console == before.console && after.visibility == before.visibility &&
            after.sim_run == before.sim_run && after.lines == before.lines &&
            after.chat == before.chat && after.log == before.log,
        "the poster wrote the battle"
    );
    require(
        file_bytes(options_.preferences_file) == preferences_before &&
            preference_values_ == preference_values_before,
        "the preferences file changed"
    );

    // A drag marks the area it covers, not the view's size, and is at least
    // 64 map pixels on a side. A click keeps the mark. Left at the map's
    // left edge leaves the mark as it is.
    if (photo_ != nullptr) {
        show(origin_x + view_w, origin_z);
        set_photo_poster_extent(static_cast<int>(photo::PosterExtent::marked));
        require(photo_->poster && photo_poster_takes_drag(), "Area was not chosen for a drag");
        const auto window_at = [&](float across, float down) {
            return std::array<float, 2>{
                static_cast<float>(match_layout_.left) + static_cast<float>(view_w) * across,
                static_cast<float>(match_layout_.top) + static_cast<float>(view_h) * down
            };
        };
        const auto drag = [&](std::array<float, 2> from, std::array<float, 2> to) {
            photo_poster_drag_begin(from[0], from[1]);
            for (int part = 1; part <= 4; ++part) {
                const float share = static_cast<float>(part) / 4.0F;
                photo_poster_drag_move(
                    from[0] + (to[0] - from[0]) * share, from[1] + (to[1] - from[1]) * share
                );
            }
            photo_poster_drag_end();
        };
        const std::array<float, 2> from = window_at(0.25F, 0.25F);
        const std::array<float, 2> to = window_at(0.75F, 0.625F);
        const std::optional<std::array<int32_t, 2>> map_from = poster_map_at(from[0], from[1]);
        const std::optional<std::array<int32_t, 2>> map_to = poster_map_at(to[0], to[1]);
        require(map_from.has_value() && map_to.has_value(), "the drag's ends are not on the map");
        drag(from, to);
        require(
            photo_->poster_mark_x == (*map_from)[0] && photo_->poster_mark_z == (*map_from)[1] &&
                photo_->poster_mark_w == (*map_to)[0] - (*map_from)[0] + 1 &&
                photo_->poster_mark_h == (*map_to)[1] - (*map_from)[1] + 1,
            "a drag did not mark the area it covers"
        );
        require(
            photo_->poster_mark_w < view_w && photo_->poster_mark_h < view_h,
            "a drag inside the view marked the view's size"
        );
        const int32_t dragged_x = photo_->poster_mark_x;
        const int32_t dragged_w = photo_->poster_mark_w;
        drag(window_at(0.5F, 0.5F), window_at(0.5F, 0.5F));
        require(
            photo_->poster_mark_x == dragged_x && photo_->poster_mark_w == dragged_w,
            "a click replaced the mark"
        );
        drag(window_at(0.5F, 0.5F), window_at(0.5F + 16.0F / static_cast<float>(view_w), 0.55F));
        require(
            photo_->poster_mark_w == 64 && photo_->poster_mark_h == 64,
            "a small drag did not mark 64 map pixels a side"
        );
        set_photo_poster_rect(0, origin_z, 128, 128);
        photo_poster_grow(-1, 0);
        require(
            photo_->poster_mark_x == 0 && photo_->poster_mark_w == 128,
            "Left at the map's left edge moved the mark's other edge"
        );
        photo_poster_grow(1, 0);
        require(
            photo_->poster_mark_x == 0 && photo_->poster_mark_w > 128,
            "Right did not grow the mark's right edge"
        );
        set_photo_poster(false);
        show(origin_x, origin_z);
    }

    require(photo_run(), "the picture did not run on");
    const photo::PosterPlan running = photo_poster_plan(photo_capture_machine());
    require(running.button == photo::SizeLock::moving, "a moving picture still offers a poster");
    require(
        photo_poster_reason(running.button, running.limit) == "Freeze the picture",
        "the moving lock does not say to freeze the picture"
    );
    const std::string classic_key = bound_chord_text("camera.classic-view");
    const std::string above = photo_poster_reason(photo::SizeLock::above, 0);
    require(
        above.find(classic_key.empty() ? std::string("Alt+R") : classic_key) != std::string::npos,
        "the turned view does not name Classic view's key (" + above + ")"
    );
    require(photo_freeze(), "the picture did not freeze again");
    const photo::PosterPlan frozen = photo_poster_plan(photo_capture_machine());
    require(
        frozen.button == photo::SizeLock::open && frozen.view.offered,
        "a frozen picture does not offer a poster"
    );

    end_fight();
    std::printf("%s\n", full ? "photo poster full: ok" : "photo poster: equal");
    std::fflush(stdout);
    return 0;
}

} // namespace oa::app

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Multiplayer screens: resources, input routing, dialogs and drawing.
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/frontend_multiplayer/get_map.hpp"
#include "oa/ui/frontend_multiplayer/map_list.hpp"
#include "oa/ui/frontend_multiplayer/presence_card.hpp"
#include "oa/ui/decoded.hpp"

#include "oa/base/game_loop.hpp"
#include "oa/data/campaign/campaign_assets.hpp"
#include "oa/data/map_pack/map_name.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/defs/unit_header.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/languages/unit_texts.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_renderer/game_text.hpp"
#include "oa/ui/frontend_renderer/scroll_bars.hpp"
#include "oa/ui/gui_input/scroll_bar.hpp"
#include "oa/ui/gui_input.hpp"
#include "oa/ui/gui_layout/gui_gadget.hpp"
#include "oa/data/defs/asset_files.hpp"
#include "oa/data/defs/unit_header.hpp"
#include "oa/data/campaign/map_catalog.hpp"
#include "oa/netgame/sync/map_hash.hpp"
#include "oa/netgame/sync/unit_checksum.hpp"
#include "oa/formats/ota.hpp"
#include "oa/data/unit_definitions.hpp"
#include "oa/present/model/mesh_raster.hpp"
#include "oa/present/palette_tables.hpp"
#include "oa/present/surface.hpp"
#include "oa/present/game_text.hpp"
#include "oa/present/typed_text.hpp"
#include "oa/platform/system.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oa::ui::frontend_multiplayer {

namespace {

namespace renderer = oa::ui::frontend_renderer;
using app::ScreenContext;
using app::ScreenInput;
using app::ScreenInputKind;

constexpr const char* kGuiPalette = "palettes/guipal.pal";
constexpr const char* kCommonGaf = "anims/commongui.gaf";
constexpr int32_t kCanvasWidth = 640;
constexpr int32_t kCanvasHeight = 480;
constexpr uint32_t kTicksPerSecond = 30;
constexpr uint32_t kDoubleClickMs = 400;

/// Tells whether a Player.reject_reason is one a host refuses a joiner with
/// as it arrives: the game closed (3), the wrong password (4), the game full
/// (5), a unit or version the joiner lacks (7, 8) or no watching (9).
///
/// @param reason the reason
/// @return true for those reasons
constexpr bool refuses_arrival(uint8_t reason) noexcept {
    return (reason >= 3 && reason <= 5) || (reason >= 7 && reason <= 9);
}

constexpr uint16_t kDefaultMaxUnits = 250;
constexpr uint16_t kMachineMemoryMb = 256; // stand-in for the physical memory probe
constexpr uint32_t kSdlKeyReturn = 0x0d;
constexpr uint32_t kSdlKeyEscape = 0x1b;
constexpr uint32_t kSdlKeyBackspace = 0x08;
constexpr uint32_t kSdlKeyTab = 0x09;
constexpr uint32_t kSdlKeyRight = 0x4000004F;
constexpr uint32_t kSdlKeyLeft = 0x40000050;
constexpr uint32_t kSdlKeyDown = 0x40000051;
constexpr uint32_t kSdlKeyUp = 0x40000052;
/// Either Shift key, as the platform reports it on a key.
constexpr uint16_t kSdlModifierShift = 0x0003;
constexpr uint8_t kSdlRightButton = 3;
constexpr uint8_t kMenuTickFlag = 1;
// The frontend signal a state starts with.
constexpr uint8_t kSignalInitialize = 0;

struct Resources {
    renderer::ScreenResources screen;
    ui::gui_layout::Layout source;
    Panel panel;
    int16_t offset_x = 0;
    int16_t offset_y = 0;
    bool loaded = false;
    int32_t hovered = kNoControl;
    int32_t pressed = kNoControl;
    /// The slider or slider arrow the pointer holds down.
    oa::ui::gui_input::ScrollHold hold;
    /// The game palette's gray table, built the first time a grayed control is drawn.
    std::vector<uint8_t> gray_table;
    /// The dialog names no picture, so its face is the common panel tiles and
    /// its text fields and lists take the common list-box frame.
    bool plain_panel = false;
};

struct MapInfo {
    std::string name, description, size;
    int32_t memory = 0;
    std::string title;      ///< the pack index's name; empty for a base map
    std::string file_title; ///< the map file's title, when the source held one
    std::string pack_name;  ///< the pack's name; empty for a base map
    bool pack = false;
    /// The short English reason the row is greyed with; empty when it can be played.
    std::string blocked_reason;
};

struct UnitStore {
    std::vector<std::string> names, sides, unit_names;
    std::vector<LobbyUnit> units;
    const oa::AssetStore* assets = nullptr; // the install the table was read from
    bool loaded = false;
};

struct Ui {
    std::unique_ptr<Game> game = std::make_unique<Game>();
    Lobby lobby{};
    LoopbackNet loopback{};
    bool custom_net = false;
    LobbyNet bound_net{};
    StartHandler start_handler = nullptr;
    void* start_context = nullptr;
    // The launch the lobby reads; it survives multiplayer_reset.
    LaunchLink launch_link{};
    // The mod profile's rules the battle room keeps; null for 3.1c's. It
    // survives multiplayer_reset.
    const data::match_rules::MatchRules* rules{};
    // The game's translation of interface texts; it survives multiplayer_reset.
    TextTranslation translation{};
    // The sides the game's SIDEDATA loaded (multiplayer_bind_side_count); it
    // survives multiplayer_reset. 0 until bound: the battle room then assumes
    // 3.1c's two.
    int32_t side_count{};
    // Those sides' SIDEDATA names, by side number (multiplayer_bind_side_names).
    // It survives multiplayer_reset.
    std::vector<std::string> side_names;
    // The line this machine says of its engine in the battle room; it
    // survives multiplayer_reset.
    EngineBanner engine_banner{};
    // The engine's line said last since the battle room was entered; empty
    // until it is said.
    std::string banner_said;
    // The last text LobbyServices::translate gave.
    std::string translated;
    ConnectState connect{};
    RestrictPanel restrict {};
    MapSelect mapselect;
    Resources base;
    Resources modal;
    ModalKind modal_kind = ModalKind::none;
    /// The dialog TIMEOUT.GUI opened over, back in front once it closes.
    Resources covered;
    /// Which dialog TIMEOUT.GUI covers; none when it opened over the battle room.
    ModalKind covered_kind = ModalKind::none;
    /// The player timeout the battle room's game takes, in seconds.
    int32_t player_timeout_seconds = kDefaultPlayerTimeoutSeconds;
    /// The battle room buttons a mod's display rules add (lobby_button bits).
    uint8_t lobby_buttons = 0;
    /// The network rules the battle room plays by; 3.1c's until bound.
    netgame::WireRules wire_rules{};
    /// This machine sends and reads chat as UTF-8; off until bound.
    bool unicode_chat{};
    /// How the chat list measures a line; unbound until the engine binds it.
    /// It survives multiplayer_reset.
    ChatMeasure chat_measure{};
    /// The presence record this machine sends, without a map pack; empty
    /// until one is bound. It survives multiplayer_reset.
    std::vector<uint8_t> presence_record;
    /// The map packs' answer to which pack the selected map comes from
    /// (LobbyMaps::pack), and its context; null until bound. It survives
    /// multiplayer_reset.
    void* pack_context = nullptr;
    bool (*pack)(void* context, netgame::PresenceMapPack* out) = nullptr;
    /// The line this machine's recorder answers .report with.
    std::string program_line;
    std::string message;
    std::vector<MapInfo> maps;
    int32_t map = -1;
    bool maps_loaded = false;
    // The pack map whose choice was refused, and why. Empty when the last
    // choice was accepted.
    std::string pack_refused_name;
    std::string pack_refusal;
    // A pack map's files are mounted for the battle room.
    bool pack_prepared = false;
    // The map context (Game.game_options) the content hash is computed from.
    data::campaign::CampaignFile map_context{};
    data::campaign::MapList map_list{};
    oa::netgame::sync::MapHashCache map_hashes{};
    UnitStore units;
    // Pictures the restriction panel loaded (handle = index + 1) and the
    // file services its picture paths use.
    std::vector<std::unique_ptr<Image>> pictures;
    data::campaign::CampaignFiles picture_files{};
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ScreenContext* ctx = nullptr;
    /// The install the last live screen context named. The context does not
    /// outlive its callback; a map installed while the battle room shows is
    /// listed after that callback returns, and the list reads this install.
    const oa::AssetStore* assets = nullptr;
    /// The host and services the last live screen context named. Like the
    /// install, they live on with the runtime; a sound played or a setting
    /// read after the callback returns uses these, never the context.
    void* host = nullptr;
    const app::ScreenServices* services = nullptr;
    uint32_t last_click_ms = 0;
    int32_t last_click_index = kNoControl;
    bool in_lobby = false;
    app::ScreenId screen = 0;
    // One of these screens is on screen: set as each is entered, cleared as
    // it is left and as the battle room hands over to the match loader.
    bool showing = false;
    // A request to close the window asked for the exit confirmation, which
    // the screen's next tick opens.
    bool exit_confirm_requested = false;
    // The quick key, lowercase, that answered a yes-or-no question at the
    // last key press; zero for none. The character it types is dropped, so
    // it never reaches the text box focused under the question.
    int answered_key = 0;
    // The input method's composition shown at the end of the battle room's
    // chat line, as game text: as much of it as fits. It is no part of the
    // line that is sent.
    std::string composition;

    Ui() {
        data::campaign::campaign_file_init(&map_context);
        map_context.kind = data::campaign::SessionKind::multiplayer;
    }

    ~Ui() {
        data::campaign::map_clear_list_cache(map_list, &map_context);
        oa::netgame::sync::map_destroy_hash_cache(map_hashes);
    }

    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;
};

Ui& ui() {
    static auto state = std::make_unique<Ui>();
    return *state;
}

/// Remembers the screen for this callback, and the install it names.
///
/// The context belongs to the caller and does not outlive the callback. A
/// map got while the battle room shows is listed once the callback has
/// returned, so the list keeps the install, which lives on with the runtime.
///
/// @param state the screens
/// @param ctx the callback's context; null clears the remembered install
void remember_screen(Ui& state, ScreenContext* ctx) {
    state.ctx = ctx;
    state.assets = ctx != nullptr ? ctx->assets : nullptr;
    state.host = ctx != nullptr ? ctx->host : nullptr;
    state.services = ctx != nullptr ? ctx->services : nullptr;
}

// The clock multiplayer_bind_clock bound; it survives multiplayer_reset.
LobbyClock g_clock{};

// The screen sizes multiplayer_bind_display_modes bound; they survive
// multiplayer_reset.
LobbyDisplayModes g_display_modes{};

// The screen whose entry does the direct game's next part.
enum class DirectStage : uint8_t {
    none,      // nothing left to do
    tcp,       // TCP.GUI accepts the address
    game_list, // the game list types the names and goes on
    new_game,  // NEWMULTI types the game's name and password and takes OK
    joining,   // the game list waits for the game and joins it
    entering,  // the battle room opens
};

// The game multiplayer_bind_direct_game asked for and how far it has gone;
// it survives multiplayer_reset.
struct DirectGameState {
    DirectGame game{};
    DirectStage stage = DirectStage::none;
    DirectGameProgress progress{};
};

DirectGameState g_direct{};

// The pack maps the battle room lists beside the base maps. A null member
// does nothing. The binding survives multiplayer_reset.
LobbyMapSource g_map_source{};

// The OA layer's answer for whether it draws the badges at the window's own
// pixels this frame. A null answer keeps the frame's pixel badge. The
// binding survives multiplayer_reset.
void* g_layer_badge_context = nullptr;
bool (*g_layer_draws_badges)(void*) = nullptr;

/// Tells whether the bound source lists a map by this exact name.
///
/// @param name the map's name
/// @return true when the source lists it
bool source_lists(const char* name) {
    if (name == nullptr || g_map_source.count == nullptr || g_map_source.at == nullptr)
        return false;
    const int32_t count = g_map_source.count(g_map_source.context);
    for (int32_t index = 0; index < count; ++index) {
        LobbyPackMap pack{};
        if (!g_map_source.at(g_map_source.context, index, &pack) || pack.name == nullptr)
            continue;
        if (std::strcmp(pack.name, name) == 0)
            return true;
    }
    return false;
}

/// Forgets the last refused pack map.
void clear_pack_refusal() {
    auto& state = ui();
    state.pack_refused_name.clear();
    state.pack_refusal.clear();
}

/// Remembers why a pack map was refused.
///
/// @param name the map's name
/// @param reason the reason; null is an empty reason
void remember_pack_refusal(const std::string& name, const char* reason) {
    auto& state = ui();
    state.pack_refused_name = name;
    state.pack_refusal = reason != nullptr ? reason : "";
}

/// Unmounts the pack map whose files are mounted. A second call does nothing.
void release_pack_source() {
    auto& state = ui();
    if (!state.pack_prepared)
        return;
    state.pack_prepared = false;
    if (g_map_source.release != nullptr)
        g_map_source.release(g_map_source.context);
}

/// Mounts a listed pack map's files.
///
/// A source with no prepare leaves the files as they are and accepts the map.
/// A refusal keeps the reason and mounts nothing.
///
/// @param name the map's name
/// @return true when the map may be chosen
bool prepare_listed(const std::string& name) {
    auto& state = ui();
    if (g_map_source.prepare == nullptr) {
        clear_pack_refusal();
        state.pack_prepared = false;
        return true;
    }
    char reason[1024]{};
    if (!g_map_source.prepare(g_map_source.context, name.c_str(), reason, sizeof reason)) {
        state.pack_prepared = false;
        remember_pack_refusal(name, reason);
        return false;
    }
    clear_pack_refusal();
    // Mounted again after it was not: the files may be another release's
    // now, as after GET MAP installs the host's. A map context bound to the
    // map by this name reads them again, its hashes among them.
    if (!state.pack_prepared &&
        oa::formats::tdf::compare_nocase(
            data::campaign::campaign_mission_name(&state.map_context), name.c_str()
        ) == 0)
        state.map_context.mission_name[0] = '\0';
    state.pack_prepared = true;
    return true;
}

uint32_t elapsed_ms() {
    if (g_clock.now_ms != nullptr)
        return g_clock.now_ms(g_clock.context);
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - ui().start
    )
                                     .count());
}

// ---------------------------------------------------------------------------
// Boundaries bound to the application

void service_play_sound(void*, const char* name) {
    auto& state = ui();
    // The host and services, not the callback's context, which may be gone
    // by the time the battle room plays a sound.
    if (state.services != nullptr && state.services->play_sound != nullptr)
        state.services->play_sound(state.host, name);
}

void service_message(void*, const char* text) {
    ui().message = text != nullptr ? text : "";
}

uint32_t service_tick(void*) {
    return base::game_loop::scaled_clock(elapsed_ms(), kTicksPerSecond);
}

uint32_t service_milliseconds(void*) {
    return elapsed_ms();
}

/// Translates interface text through the bound translation (LobbyServices::translate).
///
/// @param text text to translate
/// @return its translation, valid until the next call, or null without a
///         translation or when it fails
const char* service_translate(void*, const char* text) {
    auto& state = ui();
    if (text == nullptr || state.translation.translate == nullptr)
        return nullptr;
    try {
        state.translated = state.translation.translate(state.translation.context, text);
    } catch (const std::exception&) {
        return nullptr;
    }
    return state.translated.c_str();
}

/// Draws a value below a bound for the team deal (LobbyServices::random_below).
///
/// @param bound exclusive upper limit
/// @return a value from 0 to bound - 1; 0 for a bound below 2
uint32_t service_random_below(void*, uint32_t bound) {
    if (bound < 2)
        return 0;
    return static_cast<uint32_t>(std::rand()) % bound;
}

oa_ref32 service_load_picture(void*, const char* path, int32_t* width, int32_t* height) {
    auto& state = ui();
    if (state.assets == nullptr)
        return 0;
    std::unique_ptr<Image> image;
    try {
        image = std::make_unique<Image>(
            ui::decoded::require(decode_pcx(state.assets->read(path).bytes), path)
        );
    } catch (const std::exception&) {
        return 0;
    }
    *width = static_cast<int32_t>(image->width);
    *height = static_cast<int32_t>(image->height);
    auto free = std::find(state.pictures.begin(), state.pictures.end(), nullptr);
    if (free == state.pictures.end())
        free = state.pictures.insert(state.pictures.end(), nullptr);
    *free = std::move(image);
    return static_cast<oa_ref32>(free - state.pictures.begin() + 1);
}

void service_free_picture(void*, oa_ref32 picture) {
    auto& pictures = ui().pictures;
    if (picture != 0 && picture <= pictures.size())
        pictures[picture - 1].reset();
}

int32_t service_display_modes(void*, DisplayMode* out, int32_t capacity) {
    if (g_display_modes.modes == nullptr || out == nullptr || capacity <= 0)
        return 0;
    return std::clamp(g_display_modes.modes(g_display_modes.context, out, capacity), 0, capacity);
}

data::campaign::CampaignFiles map_files() {
    if (ui().assets == nullptr)
        return {};
    return data::campaign::campaign_asset_files(*ui().assets);
}

/// The row the battle room starts on: a map it can read, else one that can
/// be played, else the first row.
///
/// @return that row; 0 when the list is empty
int32_t starting_map_index();

/// Orders the battle room's maps by the name each row shows.
///
/// A list past the sorting limits stays in the order it was gathered.
///
/// @param[in,out] maps the maps
void order_listed_maps(std::vector<MapInfo>& maps) {
    std::vector<MapListFact> facts;
    facts.reserve(maps.size());
    for (const MapInfo& map : maps) {
        MapListFact fact;
        fact.key = map.name;
        fact.title = map.title;
        fact.file_title = map.file_title;
        fact.pack_name = map.pack_name;
        fact.pack = map.pack;
        fact.blocked_reason = map.blocked_reason;
        facts.push_back(fact);
    }
    std::vector<std::size_t> order;
    try {
        order = map_list_order(facts);
    } catch (const std::exception&) {
        return;
    }
    if (order.size() != maps.size())
        return;
    for (const std::size_t index : order) {
        if (index >= maps.size())
            return;
    }
    std::vector<MapInfo> sorted;
    sorted.reserve(maps.size());
    for (const std::size_t index : order)
        sorted.push_back(std::move(maps[index]));
    maps = std::move(sorted);
}

void load_maps() {
    auto& state = ui();
    if (state.maps_loaded)
        return;
    // A source that supplies both the base maps and the pack maps can build
    // the list before a multiplayer screen has an install. The fallback scan
    // of the installed maps still needs that install.
    const bool sourced = g_map_source.base_count != nullptr && g_map_source.base_at != nullptr &&
                         g_map_source.count != nullptr && g_map_source.at != nullptr;
    if (!sourced && state.assets == nullptr)
        return;
    state.maps_loaded = true;

    // Pointers are copied before this returns. A base map keeps an empty
    // title, so its row stays the file name.
    struct Listed {
        const char* name{};
        const char* description{};
        const char* size{};
        int32_t memory = 0;
        const char* title{};
        const char* file_title{};
        const char* pack_name{};
        const char* blocked_reason{};
        bool pack = false;
    };

    const auto append = [&](const Listed& map) {
        if (map.name == nullptr || map.name[0] == '\0')
            return;
        const bool listed =
            std::any_of(state.maps.begin(), state.maps.end(), [&](const MapInfo& info) {
                return info.name == map.name;
            });
        if (listed)
            return;
        MapInfo info;
        info.name = map.name;
        info.description = map.description != nullptr ? map.description : "";
        info.size = map.size != nullptr ? map.size : "";
        info.memory = map.memory;
        if (map.pack) {
            info.title = map.title != nullptr ? map.title : "";
            info.file_title = map.file_title != nullptr ? map.file_title : "";
            info.pack_name = map.pack_name != nullptr ? map.pack_name : "";
            info.blocked_reason = map.blocked_reason != nullptr ? map.blocked_reason : "";
            info.pack = true;
        }
        state.maps.push_back(std::move(info));
    };
    // A bound source already gathered the base maps in the one start scan.
    // With no binding, the list scans the installed maps as before.
    if (g_map_source.base_count != nullptr && g_map_source.base_at != nullptr) {
        const int32_t base = g_map_source.base_count(g_map_source.context);
        for (int32_t index = 0; index < base; ++index) {
            LobbyPackMap map{};
            if (g_map_source.base_at(g_map_source.context, index, &map))
                append({map.name, map.description, map.size, map.memory_mb});
        }
    } else {
        auto& assets = *state.assets;
        char* names = nullptr;
        // The list keeps the maps' file names, which the screens find the maps
        // by; a chosen map shows its translated name (campaign_localized_name).
        auto files = map_files();
        files.translate = nullptr;
        const auto count = data::campaign::map_build_multiplayer_list(
            state.map_list, files, {}, &names, false, false
        );
        const char* name = names;
        for (int32_t index = 0; name != nullptr && index < count;
             ++index, name += std::strlen(name) + 1) {
            try {
                const auto bytes = assets.read(std::string("maps/") + name + ".ota").bytes;
                const auto parsed = oa::formats::ota::parse(
                    std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())
                );
                if (!parsed.ok())
                    continue;
                append(
                    {name,
                     parsed.metadata->mission_description.c_str(),
                     parsed.metadata->map_size.c_str(),
                     map_memory_mb(
                         static_cast<int32_t>(
                             assets.file_size(std::string("maps/") + name + ".tnt")
                         )
                     )}
                );
            } catch (const std::exception&) {
            }
        }
        std::free(names);
    }
    // Installed pack maps join the list from the source's own fields. Their
    // map files are read when one is chosen, not while the list is built.
    if (g_map_source.count != nullptr && g_map_source.at != nullptr) {
        const int32_t extra = g_map_source.count(g_map_source.context);
        for (int32_t index = 0; index < extra; ++index) {
            LobbyPackMap pack{};
            if (g_map_source.at(g_map_source.context, index, &pack))
                append(
                    {pack.name,
                     pack.description,
                     pack.size,
                     pack.memory_mb,
                     pack.title,
                     pack.file_title,
                     pack.pack_name,
                     pack.blocked_reason,
                     true}
                );
        }
    }
    order_listed_maps(state.maps);
    // The room starts on a map it can read, so Select Map can open. A pack
    // map that sorts first and has no file yet is not that map. A grey map
    // is not either. When none can be read, the first map that can be played
    // stays selected, and a list of only grey maps stays on the first row.
    if (!state.maps.empty() && state.map < 0)
        state.map = starting_map_index();
}

const MapInfo* current_map() {
    auto& state = ui();
    load_maps();
    if (state.map < 0 || static_cast<std::size_t>(state.map) >= state.maps.size())
        return nullptr;
    return &state.maps[static_cast<std::size_t>(state.map)];
}

// Binds the map context to the selected map; null when none is selected.
data::campaign::CampaignFile* bound_map_context() {
    auto& state = ui();
    const auto* map = current_map();
    if (map == nullptr)
        return nullptr;
    const char* bound = data::campaign::campaign_mission_name(&state.map_context);
    if (oa::formats::tdf::compare_nocase(bound, map->name.c_str()) != 0) {
        const auto files = map_files();
        data::campaign::CampaignEnv env{&files, nullptr, 0, 0};
        // The list's row carries the pack index's name. A base map's row
        // has none, and a map the source does not list has none.
        env.pack_index_name = [](void*, const char* name) -> const char* {
            if (name == nullptr || g_map_source.count == nullptr || g_map_source.at == nullptr)
                return nullptr;
            const int32_t count = g_map_source.count(g_map_source.context);
            for (int32_t row = 0; row < count; ++row) {
                LobbyPackMap pack{};
                if (!g_map_source.at(g_map_source.context, row, &pack) || pack.name == nullptr)
                    continue;
                if (std::strcmp(pack.name, name) == 0)
                    return pack.title;
            }
            return nullptr;
        };
        data::campaign::campaign_select_mission(&state.map_context, &env, map->name.c_str());
    }
    return &state.map_context;
}

// A map counts as selected once the map context holds its map file.
bool maps_selected(void*) {
    const auto* context = bound_map_context();
    return context != nullptr && data::campaign::campaign_mission_file_size(context) != 0;
}

int32_t starting_map_index() {
    auto& state = ui();
    const bool can_read = state.assets != nullptr;
    int32_t playable = -1;
    for (std::size_t index = 0; index < state.maps.size(); ++index) {
        if (!state.maps[index].blocked_reason.empty())
            continue;
        if (playable < 0)
            playable = static_cast<int32_t>(index);
        if (!can_read)
            return playable;
        state.map = static_cast<int32_t>(index);
        const auto* held = bound_map_context();
        if (held != nullptr && data::campaign::campaign_mission_file_size(held) != 0)
            return state.map;
    }
    if (playable >= 0)
        return playable;
    return 0;
}

const char* maps_name(void*) {
    const auto* map = current_map();
    return map != nullptr ? map->name.c_str() : "";
}

/// Lists a pack map the source lists now and the battle room's list does not
/// yet: one installed since the list was read, as GET MAP installs the host's.
///
/// @param name the map's name
void list_new_pack_map(const char* name) {
    auto& state = ui();
    const bool listed = std::any_of(state.maps.begin(), state.maps.end(), [&](const MapInfo& info) {
        return info.name == name;
    });
    if (listed || g_map_source.count == nullptr || g_map_source.at == nullptr)
        return;
    const int32_t count = g_map_source.count(g_map_source.context);
    for (int32_t index = 0; index < count; ++index) {
        LobbyPackMap pack{};
        if (!g_map_source.at(g_map_source.context, index, &pack) || pack.name == nullptr ||
            std::strcmp(pack.name, name) != 0)
            continue;
        // The selected map keeps its place by name as the list is sorted again.
        const std::string selected =
            state.map >= 0 && static_cast<std::size_t>(state.map) < state.maps.size()
                ? state.maps[static_cast<std::size_t>(state.map)].name
                : std::string();
        MapInfo info;
        info.name = pack.name;
        info.description = pack.description != nullptr ? pack.description : "";
        info.size = pack.size != nullptr ? pack.size : "";
        info.memory = pack.memory_mb;
        info.title = pack.title != nullptr ? pack.title : "";
        info.file_title = pack.file_title != nullptr ? pack.file_title : "";
        info.pack_name = pack.pack_name != nullptr ? pack.pack_name : "";
        info.blocked_reason = pack.blocked_reason != nullptr ? pack.blocked_reason : "";
        info.pack = true;
        state.maps.push_back(std::move(info));
        order_listed_maps(state.maps);
        for (std::size_t at = 0; at < state.maps.size() && !selected.empty(); ++at)
            if (state.maps[at].name == selected)
                state.map = static_cast<int32_t>(at);
        return;
    }
}

bool maps_select(void*, const char* name) {
    auto& state = ui();
    load_maps();
    if (name == nullptr)
        return false;
    list_new_pack_map(name);
    for (std::size_t index = 0; index < state.maps.size(); ++index) {
        if (state.maps[index].name != name)
            continue;
        if (source_lists(name)) {
            if (!prepare_listed(state.maps[index].name))
                return false;
        } else if (state.pack_prepared) {
            release_pack_source();
            clear_pack_refusal();
        }
        state.map = static_cast<int32_t>(index);
        return true;
    }
    return false;
}

/// Returns why the named map was refused the last time it was chosen.
///
/// @param name the map's name
/// @return the reason, valid until the next choice; null when there is none
const char* maps_refusal(void*, const char* name) {
    const auto& state = ui();
    if (name == nullptr || state.pack_refusal.empty() || state.pack_refused_name != name)
        return nullptr;
    return state.pack_refusal.c_str();
}

uint32_t maps_hash(void*) {
    auto* context = bound_map_context();
    if (context == nullptr)
        return 0;
    const auto files = map_files();
    return oa::netgame::sync::map_compute_content_hash(*context, files, ui().map_hashes);
}

int32_t maps_memory(void*) {
    const auto* map = current_map();
    return map != nullptr ? map->memory : 0;
}

const char* maps_description(void*) {
    const auto* map = current_map();
    return map != nullptr ? map->description.c_str() : "";
}

const char* maps_size(void*) {
    const auto* map = current_map();
    return map != nullptr ? map->size.c_str() : "";
}

int32_t maps_count(void*) {
    load_maps();
    return static_cast<int32_t>(ui().maps.size());
}

const char* maps_at(void*, int32_t index) {
    auto& maps = ui().maps;
    return index >= 0 && static_cast<std::size_t>(index) < maps.size()
               ? maps[static_cast<std::size_t>(index)].name.c_str()
               : nullptr;
}

bool maps_read(void*, const char* path, uint32_t offset, void* out, uint32_t size) {
    if (ui().assets == nullptr)
        return false;
    try {
        return ui().assets->read_chunk(path, offset, std::span(static_cast<uint8_t*>(out), size));
    } catch (const std::exception&) {
        return false;
    }
}

// The unit table header pass over every installed FBI: the FBI
// hash that keys a unit between machines, its weapon checksum and its unit
// name, beside the name, side, costs and restriction bits the battle room
// shows.
void load_units(const oa::AssetStore& assets) {
    auto& store = ui().units;
    auto& state = ui();
    if (store.loaded)
        return;
    store.loaded = true;
    store.assets = &assets;

    struct Loaded {
        std::string name, side, unit_name;
        float metal, energy;
        uint32_t flags, hash, weapon_checksum;
    };

    const auto files = oa::data::defs::asset_store_files(&assets);
    oa::data::defs::WeaponTdfSet weapon_files{};
    oa::data::defs::weapon_tdf_set_init(&weapon_files);
    (void)oa::data::defs::load_weapon_tdf_set(&files, nullptr, false, &weapon_files);
    // Each unit's name and description in other languages go to the
    // language's table, for the names the restrictions list shows.
    const oa::data::defs::UnitHeaderSources header_sources{
        "",
        &weapon_files,
        oa::data::defs::data_layout().build_version[0],
        oa::data::defs::data_layout().build_version[1],
        false,
        false,
        oa::data::languages::unit_text_sink()
    };
    std::vector<Loaded> loaded;
    for (const auto& path : assets.list_effective(
             oa::data::defs::directory_name(oa::data::defs::DataDirectory::units),
             oa::data::defs::unit_file_suffix()
         )) {
        try {
            auto header = std::make_unique<oa::UnitDef>();
            bool refused = false;
            if (!oa::data::defs::load_unit_header(
                    &files, path.c_str(), *header, header_sources, &refused
                ))
                continue;
            const auto text = [](const auto& field) {
                return std::string(field, ::strnlen(field, sizeof field));
            };
            loaded.push_back(
                {text(header->name),
                 text(header->side),
                 text(header->unit_name),
                 header->build_cost_metal,
                 header->build_cost_energy,
                 header->abilities,
                 header->fbi_hash,
                 header->weapon_checksum}
            );
        } catch (const std::exception&) {
        }
    }
    oa::data::defs::weapon_tdf_set_free(&weapon_files);
    store.names.reserve(loaded.size() + 1);
    store.sides.reserve(loaded.size() + 1);
    store.unit_names.reserve(loaded.size() + 1);
    store.names.emplace_back();
    store.sides.emplace_back();
    store.unit_names.emplace_back();
    for (const auto& unit : loaded) {
        store.names.push_back(unit.name);
        store.sides.push_back(unit.side);
        store.unit_names.push_back(unit.unit_name);
    }
    store.units.push_back(LobbyUnit{"", "", 0, 0, 0, 0, "", 0, 0});
    for (std::size_t index = 0; index < loaded.size(); ++index)
        store.units.push_back(
            LobbyUnit{
                store.names[index + 1].c_str(),
                store.sides[index + 1].c_str(),
                loaded[index].energy,
                loaded[index].metal,
                loaded[index].flags,
                loaded[index].hash,
                store.unit_names[index + 1].c_str(),
                0,
                loaded[index].weapon_checksum
            }
        );
    state.lobby.units = store.units.data();
    state.lobby.unit_count = static_cast<int32_t>(store.units.size());
}

// The installed files behind a unit's content checksum.
class UnitFileReader final : public oa::data::unit_definitions::CatalogAssetReader {
  public:

    explicit UnitFileReader(const oa::AssetStore& assets) : assets_(assets) {}

    oa::data::unit_definitions::Result<std::vector<std::string>>
    list_effective(std::string_view directory, std::string_view extension) const override {
        try {
            return {assets_.list_effective(directory, extension), {}};
        } catch (const std::exception& error) {
            return {{}, {oa::data::unit_definitions::ErrorCode::io, 0, error.what()}};
        }
    }

    oa::data::unit_definitions::Result<std::string> read(std::string_view path) const override {
        try {
            const auto bytes = assets_.read(path).bytes;
            return {std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), {}};
        } catch (const std::exception& error) {
            return {{}, {oa::data::unit_definitions::ErrorCode::io, 0, error.what()}};
        }
    }

  private:

    const oa::AssetStore& assets_;
};

void load_units() {
    if (ui().assets != nullptr)
        load_units(*ui().assets);
}

uint32_t service_unit_checksum(void*, const LobbyUnit& unit) {
    const auto* assets = ui().units.assets;
    if (assets == nullptr || unit.unit_name == nullptr)
        return unit.content_checksum;
    const UnitFileReader reader(*assets);
    const auto mixed = oa::netgame::sync::mix_unit_file_checksum(
        reader, unit.unit_name, unit.content_checksum, unit.weapon_checksum
    );
    return mixed ? mixed.value : unit.content_checksum;
}

bool settings_read(void*, char* out, std::size_t capacity) {
    const auto& state = ui();
    if (state.services == nullptr || state.services->read_string == nullptr)
        return false;
    return state.services->read_string(state.host, "multiplayer", "tcpaddr", out, capacity) != 0;
}

void settings_write(void*, const char* address) {
    const auto& state = ui();
    if (state.services != nullptr && state.services->write_string != nullptr)
        state.services->write_string(state.host, "multiplayer", "tcpaddr", address);
}

bool settings_user(void*, char* out, std::size_t capacity) {
    const auto user = oa::platform::environment_value("USER");
    if (!user || user->empty())
        return false;
    std::snprintf(out, capacity, "%s", user->c_str());
    return true;
}

/// Reads the file the host's .base names (LobbyServices::read_file): an
/// absolute host path when a file is there, or else a file of the game's
/// folders.
///
/// @param name the name as typed
/// @param limit the most bytes read
/// @param[out] contents the file's bytes
/// @return true when the file was read
bool service_read_file(void*, const char* name, std::size_t limit, std::string* contents) {
    if (name == nullptr || name[0] == '\0' || contents == nullptr)
        return false;
    std::error_code error;
    const auto path = std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(name), std::strlen(name))
    );
    if (path.is_absolute() && std::filesystem::is_regular_file(path, error)) {
        const auto size = std::filesystem::file_size(path, error);
        if (error || size > limit)
            return false;
        std::ifstream file(path, std::ios::binary);
        contents->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return !file.bad();
    }
    const auto& state = ui();
    if (state.assets == nullptr)
        return false;
    const auto bytes = state.assets->load_file_contents(name);
    if (!bytes || bytes->size() > limit)
        return false;
    contents->assign(bytes->begin(), bytes->end());
    return true;
}

/// Copies the bound presence record into a lobby's source (PresenceRecords::source).
///
/// @param[in,out] lobby the lobby
/// @param record the record; empty for none
void take_presence_record(Lobby& lobby, const std::vector<uint8_t>& record) noexcept {
    auto& records = lobby.presence_records;
    const auto size = std::min(record.size(), sizeof records.source);
    if (size != 0)
        std::memcpy(records.source, record.data(), size);
    records.source_size = static_cast<uint16_t>(size);
}

/// Binds the lobby to the screens' transport, services, map list and units.
///
/// A lobby with no game is reset first. The unit limits, the canvas size and
/// the side count (the bound SIDEDATA count, else 2, ARM and CORE) get their
/// defaults where the game has none.
void bind_boundaries() {
    auto& state = ui();
    auto& lobby = state.lobby;
    if (lobby.game == nullptr) {
        loopback_reset(state.loopback);
        lobby_reset(lobby, *state.game);
    }
    lobby.net = state.custom_net ? state.bound_net : loopback_lobby_net(state.loopback);
    if (state.assets != nullptr)
        state.picture_files = data::campaign::campaign_asset_files(*state.assets);
    lobby.services = {
        nullptr,
        service_play_sound,
        service_message,
        service_tick,
        service_display_modes,
        nullptr,
        service_unit_checksum,
        service_load_picture,
        service_free_picture,
        &state.picture_files,
        service_milliseconds,
        service_translate,
        service_random_below,
        service_read_file
    };
    lobby.maps = {
        nullptr,
        maps_selected,
        maps_name,
        maps_select,
        maps_hash,
        maps_memory,
        maps_description,
        maps_size,
        maps_count,
        maps_at,
        [](void*) -> const data::campaign::CampaignFile* { return bound_map_context(); },
        maps_read
    };
    // Set by name. LobbyMaps gains members at its end, and a positional
    // list would assign the next one to refusal.
    lobby.maps.refusal = maps_refusal;
    // The map packs' answer, bound by name; the battle room asks it again.
    lobby.maps.pack_context = state.pack_context;
    lobby.maps.pack = state.pack;
    lobby.presence_records.pack_rebound = true;
    take_presence_record(lobby, state.presence_record);
    lobby.wire_rules = state.wire_rules;
    lobby.unicode_chat = state.unicode_chat;
    lobby.chat_measure = state.chat_measure;
    lobby.local_version_major = state.wire_rules.version_major;
    lobby.local_version_minor = state.wire_rules.version_minor;
    std::snprintf(
        lobby.recorder.program, sizeof lobby.recorder.program, "%s", state.program_line.c_str()
    );
    lobby.launch_link = state.launch_link;
    lobby.side_names = state.side_names;
    lobby.rules = state.rules;
    lobby.lobby_buttons = state.lobby_buttons;
    // A tournament lock is the host's. A joiner takes a seat, not the host's rules.
    const auto* linked_block = state.launch_link.block;
    lobby.option_4 =
        linked_block != nullptr && linked_block->hosting != 0 && linked_block->tournament != 0;
    if (state.units.loaded) {
        lobby.units = state.units.units.data();
        lobby.unit_count = static_cast<int32_t>(state.units.units.size());
    }
    state.connect.settings = {nullptr, settings_read, settings_write, settings_user};
    if (lobby_max_units_default(*state.game) == 0)
        lobby_max_units_default(*state.game) = kDefaultMaxUnits;
    if (lobby_max_units(*state.game) == 0)
        lobby_max_units(*state.game) = kDefaultMaxUnits;
    if (static_cast<int32_t>(lobby_screen_width(*state.game)) == 0) {
        // The size the game plays at, when the game says, else the game's
        // own screen.
        DisplayMode playing{};
        if (g_display_modes.screen_size != nullptr)
            playing = g_display_modes.screen_size(g_display_modes.context);
        // The setup block carries each side in 16 bits.
        constexpr int32_t widest = std::numeric_limits<uint16_t>::max();
        const bool known = playing.width > 0 && playing.height > 0 && playing.width <= widest &&
                           playing.height <= widest;
        lobby_screen_width(*state.game) = known ? playing.width : kCanvasWidth;
        lobby_screen_height(*state.game) = known ? playing.height : kCanvasHeight;
    }
    if (state.game->side_count == 0)
        state.game->side_count = state.side_count > 0 ? state.side_count : 2; // 3.1c: ARM and CORE
    state.game->player_timeout_seconds = state.player_timeout_seconds;
}

// ---------------------------------------------------------------------------
// Resources and panels

bool load(
    Resources& out,
    ScreenContext* ctx,
    const char* layout_file,
    const char* background,
    const char* sprites
) {
    out = Resources{};
    if (ctx == nullptr || ctx->assets == nullptr)
        return false;
    const auto layout_path = oa::data::defs::gui_path(layout_file);
    const char* layout = layout_path.c_str();
    try {
        out.screen = renderer::load_screen(
            *ctx->assets, {layout, background, kGuiPalette, sprites, kCommonGaf}
        );
    } catch (const std::exception& error) {
        ui().message = std::string("Cannot load ") + layout + ": " + error.what();
        return false;
    }
    out.source = out.screen.layout;
    panel_load(out.panel, layout, out.source);
    out.panel.line_height = static_cast<int16_t>(formats::fnt::line_height(out.screen.font));
    // Lists and sliders are readied as the panel's first draw readies them
    // (lists trimmed, arrows added, range set from the art) before the
    // screen fills them and sets their values.
    panel_bind_lists(out.panel);
    // A panel's own SLIDERS art is in the GAF named after its GUI file, as in
    // 3.1c; the GAF the screen was loaded with counts only when it is that one.
    const bool own = renderer::gaf_named_after(layout, sprites != nullptr ? sprites : "");
    panel_bind_sliders(
        out.panel,
        renderer::scroll_art(own ? &out.screen.sprites : nullptr, out.screen.shared_sprites)
    );
    out.loaded = true;
    return true;
}

// A dialog that names no picture. paint_plain_panel lays the common panel
// tiles in its rectangle. paint_plain_list_frames lays the common list-box
// tiles around each list, under the rows the screen draws later.
bool paint_plain_panel(const Resources& modal, Image& image);
void paint_plain_list_frames(const Resources& modal, Image& image);

// Places a stacked panel. One with its own picture is centred over the screen
// below. One that names no picture lays the common panel tiles in its
// rectangle over that screen, and darkens the rectangle when those tiles
// cannot be drawn.
bool load_modal(
    ScreenContext* ctx,
    ModalKind kind,
    const char* layout,
    const char* background,
    const char* sprites
) {
    auto& state = ui();
    const char* base_background = background != nullptr ? background : "bitmaps/selconnect2.pcx";
    if (!load(state.modal, ctx, layout, base_background, sprites))
        return false;
    state.modal_kind = kind;
    auto& panel = state.modal.panel;
    const auto& root = panel.controls[0];
    const int32_t width = root.width;
    const int32_t height = root.height;
    const bool full_screen = width >= kCanvasWidth && height >= kCanvasHeight;
    state.modal.offset_x = full_screen ? 0 : root.x;
    state.modal.offset_y = full_screen ? 0 : root.y;
    auto& image = state.modal.screen.background;
    if (full_screen || !state.base.loaded)
        return true;
    if (background == nullptr) {
        // No picture of its own. The face is the common panel tiles. The
        // rectangle is darkened only when those tiles cannot be drawn.
        image = state.base.screen.background;
        state.modal.plain_panel = true;
        if (!paint_plain_panel(state.modal, image)) {
            for (int32_t y = std::max(0, static_cast<int32_t>(root.y));
                 y < root.y + height && y < static_cast<int32_t>(image.height);
                 ++y)
                for (int32_t x = std::max(0, static_cast<int32_t>(root.x));
                     x < root.x + width && x < static_cast<int32_t>(image.width);
                     ++x) {
                    auto* pixel = &image.rgb
                                       [(static_cast<std::size_t>(y) * image.width +
                                         static_cast<std::size_t>(x)) *
                                        3U];
                    for (int channel = 0; channel < 3; ++channel)
                        pixel[channel] = static_cast<uint8_t>(pixel[channel] / 4);
                }
        }
        paint_plain_list_frames(state.modal, image);
        return true;
    }
    // Own art: the panel occupies the top-left root-sized corner of the
    // image; centre that corner over the screen below.
    auto composed = state.base.screen.background;
    const auto copy_width = std::min<uint32_t>(image.width, static_cast<uint32_t>(width));
    const auto copy_height = std::min<uint32_t>(image.height, static_cast<uint32_t>(height));
    state.modal.offset_x =
        static_cast<int16_t>((kCanvasWidth - static_cast<int32_t>(copy_width)) / 2);
    state.modal.offset_y =
        static_cast<int16_t>((kCanvasHeight - static_cast<int32_t>(copy_height)) / 2);
    for (uint32_t y = 0; y < copy_height; ++y)
        for (uint32_t x = 0; x < copy_width; ++x) {
            const auto dx = static_cast<uint32_t>(std::max<int16_t>(0, state.modal.offset_x)) + x;
            const auto dy = static_cast<uint32_t>(std::max<int16_t>(0, state.modal.offset_y)) + y;
            if (dx >= composed.width || dy >= composed.height)
                continue;
            std::memcpy(
                &composed.rgb[(dy * composed.width + dx) * 3U],
                &image.rgb[(y * image.width + x) * 3U],
                3
            );
        }
    composed.palette = image.palette;
    image = std::move(composed);
    return true;
}

/// Closes the stacked dialog, and the one TIMEOUT.GUI covers with it.
void close_modal() {
    auto& state = ui();
    state.modal = Resources{};
    state.modal_kind = ModalKind::none;
    state.covered = Resources{};
    state.covered_kind = ModalKind::none;
}

/// Closes TIMEOUT.GUI or the exit confirmation, bringing back the dialog it covered, and has the
/// battle room refreshed.
void close_timeout() {
    auto& state = ui();
    state.modal = std::move(state.covered);
    state.modal_kind = state.covered_kind;
    state.covered = Resources{};
    state.covered_kind = ModalKind::none;
    if (state.lobby.game != nullptr)
        state.lobby.game->gui_flags |= kMenuTickFlag;
}

/// Opens TIMEOUT.GUI for a stalled player over whatever is in front.
///
/// A dialog already open stays loaded behind it and is back in front once
/// it closes.
///
/// @param ctx Screen context of the running frontend.
/// @param player_id The stalled player.
void open_timeout(ScreenContext* ctx, uint32_t player_id) {
    auto& state = ui();
    auto covered = std::move(state.modal);
    const auto covered_kind = state.modal_kind;
    state.modal = Resources{};
    state.modal_kind = ModalKind::none;
    if (!load_modal(ctx, ModalKind::timeout, "timeout.gui", nullptr, kCommonGaf) ||
        !timeout_open(state.lobby, state.modal.panel, player_id)) {
        state.modal = std::move(covered);
        state.modal_kind = covered_kind;
        return;
    }
    state.covered = std::move(covered);
    state.covered_kind = covered_kind;
}

/// Opens the exit confirmation (YESORNO.GUI) over whatever is in front.
///
/// A dialog already open stays loaded behind it and is back in front once
/// No closes it; TIMEOUT.GUI closes first, and the stall scan opens it again
/// over the confirmation while its player stays silent. The wait for the
/// host pauses under the confirmation, and its countdown, shown again as the
/// wait goes on, gives way to it.
///
/// @param ctx Screen context of the running frontend.
void open_exit_confirm(ScreenContext* ctx) {
    auto& state = ui();
    if (state.modal_kind == ModalKind::exit_confirm)
        return;
    if (state.connect.host_waiting)
        state.message.clear();
    if (state.modal_kind == ModalKind::timeout)
        close_timeout();
    auto covered = std::move(state.modal);
    const auto covered_kind = state.modal_kind;
    state.modal = Resources{};
    state.modal_kind = ModalKind::none;
    if (!load_modal(ctx, ModalKind::exit_confirm, "yesorno.gui", nullptr, kCommonGaf)) {
        state.modal = std::move(covered);
        state.modal_kind = covered_kind;
        return;
    }
    exit_confirm_open(state.lobby, state.modal.panel);
    state.covered = std::move(covered);
    state.covered_kind = covered_kind;
}

Resources& front() {
    auto& state = ui();
    return state.modal_kind != ModalKind::none && state.modal.loaded ? state.modal : state.base;
}

// ---------------------------------------------------------------------------
// Drawing

std::string_view template_name(const Resources& res, const Control& control) {
    if (control.source < 0 || static_cast<std::size_t>(control.source) >= res.source.gadgets.size())
        return {};
    return res.source.gadgets[static_cast<std::size_t>(control.source)].common.name;
}

// The common panel tiles, used when a dialog names no picture and no panel
// sequence of its own. The common list-box tiles frame a text field that is
// not drawn as a solid field, and a list, grown this far past each edge.
constexpr const char* kPanelTiles = "BackTile";
constexpr const char* kTextBoxTiles = "Listbox";
constexpr int32_t kTextBoxFrameMargin = 3;

const formats::gaf::Sequence*
find_sequence(const formats::gaf::Archive& archive, std::string_view name);

const PaletteBytes& screen_palette(const renderer::ScreenResources& screen) {
    return screen.background.palette.has_value() ? *screen.background.palette : screen.gui_palette;
}

/// Copies a frame's covered pixels into an RGB image through a palette.
///
/// A pixel the frame does not cover keeps what is already there. The frame's
/// origin is not applied: the common panel tiles are drawn with theirs
/// cleared, and these screens draw every other frame at the position given.
///
/// @param rgb the image, three bytes a pixel
/// @param image_width the image width
/// @param image_height the image height
/// @param frame the rendered frame
/// @param x the frame's left column
/// @param y the frame's top row
/// @param palette the palette the frame's indices select from
void blit_covered(
    std::span<uint8_t> rgb,
    uint32_t image_width,
    uint32_t image_height,
    const formats::gaf::RenderedFrame& frame,
    int32_t x,
    int32_t y,
    const PaletteBytes& palette
) {
    const auto pixel_count = static_cast<std::size_t>(frame.width) * frame.height;
    const auto image_bytes = static_cast<uint64_t>(image_width) * image_height * 3U;
    if (frame.pixels.size() != pixel_count || frame.coverage.size() != pixel_count ||
        image_width == 0 || image_height == 0 || image_bytes > rgb.size())
        return;
    const auto frame_w = static_cast<int64_t>(frame.width);
    const auto frame_h = static_cast<int64_t>(frame.height);
    const auto first_column = std::max<int64_t>(0, -static_cast<int64_t>(x));
    const auto first_row = std::max<int64_t>(0, -static_cast<int64_t>(y));
    const auto end_column = std::min<int64_t>(frame_w, static_cast<int64_t>(image_width) - x);
    const auto end_row = std::min<int64_t>(frame_h, static_cast<int64_t>(image_height) - y);
    if (first_column >= end_column || first_row >= end_row)
        return;
    constexpr std::size_t entry_bytes = 4;
    for (auto row = first_row; row < end_row; ++row) {
        const auto source = static_cast<std::size_t>(row * frame_w);
        auto* out = rgb.data() + (static_cast<std::size_t>(y + row) * image_width +
                                  static_cast<std::size_t>(x + first_column)) *
                                     3U;
        for (auto column = first_column; column < end_column; ++column, out += 3) {
            const auto offset = source + static_cast<std::size_t>(column);
            if (frame.coverage[offset] == 0)
                continue;
            const auto* color =
                palette.data() + static_cast<std::size_t>(frame.pixels[offset]) * entry_bytes;
            std::memcpy(out, color, 3);
        }
    }
}

/// Lays a skin's tiles over a rectangle, in the order the tiles are drawn.
///
/// Fewer than two frames draws the first at the rectangle's top-left. Two or
/// more are tiled from the skin's first frame, the far column and row ending
/// flush with the rectangle.
///
/// @param rgb the image, three bytes a pixel
/// @param image_width the image width
/// @param image_height the image height
/// @param skin the skin, with its frames
/// @param origin_x the rectangle's left column
/// @param origin_y the rectangle's top row
/// @param width the rectangle's width
/// @param height the rectangle's height
/// @param palette the palette the frames' indices select from
/// @return false when a frame the skin needs cannot be drawn
bool paint_skin(
    std::span<uint8_t> rgb,
    uint32_t image_width,
    uint32_t image_height,
    const formats::gaf::Sequence& skin,
    int32_t origin_x,
    int32_t origin_y,
    int32_t width,
    int32_t height,
    const PaletteBytes& palette
) {
    if (skin.frames.empty() || width < 1 || height < 1)
        return false;
    std::vector<std::optional<formats::gaf::RenderedFrame>> rendered(skin.frames.size());
    std::vector<char> tried(skin.frames.size(), 0);
    const auto frame_at = [&](std::size_t index) -> const formats::gaf::RenderedFrame* {
        if (index >= skin.frames.size())
            return nullptr;
        if (tried[index] == 0) {
            tried[index] = 1;
            auto result = formats::gaf::render_normal(skin.frames[index]);
            if (result.ok())
                rendered[index] = std::move(result.frame);
        }
        return rendered[index] ? &*rendered[index] : nullptr;
    };

    struct Spot {
        const formats::gaf::RenderedFrame* frame = nullptr;
        int32_t x = 0;
        int32_t y = 0;
    };

    std::vector<Spot> spots;
    if (skin.frames.size() < 2) {
        const auto* frame = frame_at(0);
        if (frame == nullptr)
            return false;
        spots.push_back({frame, origin_x, origin_y});
    } else {
        const auto tiles = oa::ui::gui_layout::skin_tiles(
            width, height, skin.frames[0].width, skin.frames[0].height
        );
        if (tiles.empty())
            return false;
        spots.reserve(tiles.size());
        for (const auto& tile : tiles) {
            const auto* frame = frame_at(tile.frame);
            if (frame == nullptr)
                return false;
            spots.push_back({frame, origin_x + tile.x, origin_y + tile.y});
        }
    }
    for (const auto& spot : spots)
        blit_covered(rgb, image_width, image_height, *spot.frame, spot.x, spot.y, palette);
    return true;
}

/// The sequence a dialog that names no picture is drawn with.
///
/// A sequence the layout names is taken from the shared GUI art. With none
/// named, or none there, the common panel tiles are used.
///
/// @param modal the dialog's resources
/// @return the sequence, or null when the shared art has neither
const formats::gaf::Sequence* plain_panel_skin(const Resources& modal) {
    const auto& shared = modal.screen.shared_sprites;
    if (!modal.source.gadgets.empty()) {
        const auto* fields =
            std::get_if<ui::gui_layout::PanelFields>(&modal.source.gadgets.front().fields);
        if (fields != nullptr && !fields->panel.empty()) {
            if (const auto* named = find_sequence(shared, fields->panel))
                return named;
        }
    }
    return find_sequence(shared, kPanelTiles);
}

bool paint_plain_panel(const Resources& modal, Image& image) {
    if (modal.panel.count < 1)
        return false;
    const auto* skin = plain_panel_skin(modal);
    if (skin == nullptr)
        return false;
    const auto& root = modal.panel.controls[0];
    return paint_skin(
        image.rgb,
        image.width,
        image.height,
        *skin,
        root.x,
        root.y,
        root.width,
        root.height,
        image.palette.has_value() ? *image.palette : modal.screen.gui_palette
    );
}

/// Draws the list-box frame around one control.
///
/// The frame is the common list-box tiles, grown past the control on every
/// side. A text field marked as a solid field is not framed.
///
/// @param rgb the image, three bytes a pixel
/// @param image_width the image width
/// @param image_height the image height
/// @param res the dialog's resources
/// @param control the text field or list
/// @param palette the palette the tiles' indices select from
/// @param solid_field_unframed true to skip a text field marked as a solid field
void paint_list_box_frame(
    std::span<uint8_t> rgb,
    uint32_t image_width,
    uint32_t image_height,
    const Resources& res,
    const Control& control,
    const PaletteBytes& palette,
    bool solid_field_unframed
) {
    if (solid_field_unframed && (control.attributes & ui::gui_layout::attribute::horizontal) != 0)
        return;
    const auto* skin = find_sequence(res.screen.shared_sprites, kTextBoxTiles);
    if (skin == nullptr)
        return;
    (void)paint_skin(
        rgb,
        image_width,
        image_height,
        *skin,
        control.x + res.offset_x - kTextBoxFrameMargin,
        control.y + res.offset_y - kTextBoxFrameMargin,
        control.width + kTextBoxFrameMargin * 2,
        control.height + kTextBoxFrameMargin * 2,
        palette
    );
}

void paint_plain_list_frames(const Resources& modal, Image& image) {
    if (!modal.plain_panel)
        return;
    const auto& palette = image.palette.has_value() ? *image.palette : modal.screen.gui_palette;
    for (int32_t index = 1; index < modal.panel.count; ++index) {
        const auto& control = modal.panel.controls[static_cast<std::size_t>(index)];
        if (control.type != ControlType::list_box || control.active == 0)
            continue;
        paint_list_box_frame(image.rgb, image.width, image.height, modal, control, palette, false);
    }
}

/// Draws the list-box frame around a plain dialog's text field.
///
/// The frame sits under the letters and the cursor. A field marked as a
/// solid field is left unframed.
///
/// @param[in,out] surface the drawn screen
/// @param res the dialog's resources
/// @param control the text field
void paint_plain_text_box(
    renderer::Surface& surface, const Resources& res, const Control& control
) {
    if (!res.plain_panel)
        return;
    paint_list_box_frame(
        surface.rgb, surface.width, surface.height, res, control, screen_palette(res.screen), true
    );
}

const formats::gaf::Sequence*
find_sequence(const formats::gaf::Archive& archive, std::string_view name) {
    for (const auto& sequence : archive.sequences)
        if (sequence.name.size() == name.size() &&
            std::equal(name.begin(), name.end(), sequence.name.begin(), [](char a, char b) {
                return std::toupper(static_cast<unsigned char>(a)) ==
                       std::toupper(static_cast<unsigned char>(b));
            }))
            return &sequence;
    return nullptr;
}

// Frames of a shared button sequence come in groups of four, one group per
// size; a button takes the group closest to its own size.
constexpr std::size_t kButtonStateFrames = 4;
// A size difference no group of frames reaches.
constexpr int32_t kNoSizeMatch = 1000;
// A staged button held down shows its sequence's last frame but one.
constexpr std::size_t kHeldStageFromEnd = 2;
// A grayed button shows the frame two past its normal one.
constexpr std::size_t kGrayedFrameOffset = 2;
// A staged button with this attribute, one stage or the caption "Off|On"
// takes the two-lamp art.
constexpr uint32_t kTwoLampAttribute = 0x4000;
constexpr std::string_view kTwoLampCaption = "Off|On";
constexpr std::string_view kPlainButtonArt = "BUTTONS0";
constexpr std::string_view kCheckboxArt = "CHECKBOX";
constexpr std::string_view kStageArtPrefix = "stagebuttn";
// A control copied from a template named LOGO<x> shows a player's colour:
// frame `stage` of LOGOS.GAF's 32xlogos.
constexpr std::string_view kColorTemplatePrefix = "LOGO";
constexpr std::string_view kColorArt = "32xlogos";
// The light table holds one row of 256 palette entries per light level.
constexpr std::size_t kLightLevels = 32;
constexpr std::size_t kPaletteEntries = 256;
// A button's lit art is kept in the screen's GAF under the button's name and this suffix.
constexpr std::string_view kLitArtSuffix = "+lit";

/// Tells whether a name starts with a prefix, ignoring ASCII case.
///
/// @param name name tested
/// @param prefix prefix looked for
/// @return true when `name` starts with `prefix`
bool starts_with_nocase(std::string_view name, std::string_view prefix) {
    return name.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), name.begin(), [](char a, char b) {
               return std::toupper(static_cast<unsigned char>(a)) ==
                      std::toupper(static_cast<unsigned char>(b));
           });
}

/// The GAF sequence a button's art comes from.
struct ButtonArt {
    const formats::gaf::Sequence* sequence{}; ///< null when the button is drawn without art
    std::size_t base{};                       ///< first frame of the button's size group
    renderer::SpriteOverride binding;         ///< the archive and name of `sequence`
    /// True when the renderer finds the sequence only when it is told
    /// `binding`. A renderer told the sequence counts frames from 0, and one
    /// that finds it itself from `base`.
    bool named{};
};

/// Returns the first frame of the group of four whose size is closest to a gadget's.
///
/// @param sequence sequence searched, four frames to a size
/// @param width gadget width in pixels
/// @param height gadget height in pixels
/// @return the group's first frame; 0 when no group comes within 1000 pixels
std::size_t
closest_size_group(const formats::gaf::Sequence& sequence, int32_t width, int32_t height) {
    std::size_t best = 0;
    int32_t best_difference = kNoSizeMatch;
    for (std::size_t index = 0; index < sequence.frames.size(); index += kButtonStateFrames) {
        const auto& frame = sequence.frames[index];
        const int32_t difference = std::abs(width - static_cast<int32_t>(frame.width)) +
                                   std::abs(height - static_cast<int32_t>(frame.height));
        if (difference < best_difference) {
            best = index;
            best_difference = difference;
        }
    }
    return best;
}

/// Finds the art a button is drawn with.
///
/// A row copied from a template keeps the template's own sequence, from the
/// screen's GAF and then the shared one. A checkbox-art button with no
/// sequence of its own shows the shared CHECKBOX art, as in 3.1c, unless
/// it steps a scroll bar or names its own GAF file. Every other button takes
/// the renderer's own lookup: its name in the screen's GAF, then in the shared
/// one, then the shared BUTTONS0, CHECKBOX or stagebuttn art closest to its
/// size, none for a button that flips or holds.
///
/// @param res screen resources
/// @param gadget the composed gadget, named after the control
/// @param source name of the layout gadget the control was copied from
/// @param with_sprites false to leave out the art the renderer must be told of
/// @return the art; no sequence when the button is drawn without one
ButtonArt button_art(
    const Resources& res,
    const ui::gui_layout::Gadget& gadget,
    std::string_view source,
    bool with_sprites
) {
    const auto& screen = res.screen.sprites;
    const auto& shared = res.screen.shared_sprites;
    const std::string_view name = gadget.common.name;
    if (with_sprites && source != name) {
        if (const auto* own = find_sequence(screen, source); own != nullptr && !own->frames.empty())
            return {own, 0, {renderer::SpriteArchive::screen, std::string(source)}, true};
        if (const auto* own = find_sequence(shared, source); own != nullptr && !own->frames.empty())
            return {own, 0, {renderer::SpriteArchive::shared, std::string(source)}, true};
    }
    if (const auto* own = find_sequence(screen, name))
        return {own, 0, {renderer::SpriteArchive::screen, std::string(name)}, false};
    if (const auto* own = find_sequence(shared, name))
        return {own, 0, {renderer::SpriteArchive::shared, std::string(name)}, false};
    const auto attributes = static_cast<uint32_t>(gadget.common.attributes);
    if (with_sprites && (attributes & kAttributeCheckbox) != 0 &&
        (attributes & ui::gui_layout::attribute::scroll_step_mask) == 0 &&
        !gadget.common.gaf_file) {
        const auto* checkbox = find_sequence(shared, kCheckboxArt);
        if (checkbox == nullptr || checkbox->frames.empty())
            return {};
        return {
            checkbox,
            closest_size_group(*checkbox, gadget.common.width, gadget.common.height),
            {renderer::SpriteArchive::shared, std::string(kCheckboxArt)},
            true
        };
    }
    constexpr uint32_t kRendererSkips =
        ui::gui_layout::attribute::checkbox | ui::gui_layout::attribute::text_list;
    if ((attributes & kRendererSkips) != 0)
        return {};
    std::string fallback(kPlainButtonArt);
    const auto* fields = std::get_if<ui::gui_layout::ButtonFields>(&gadget.fields);
    if ((attributes & kAttributeCheckbox) != 0) {
        fallback = kCheckboxArt;
    } else if (fields != nullptr && fields->stages != 0) {
        const bool two_lamps = fields->text == kTwoLampCaption || fields->stages == 1 ||
                               (attributes & kTwoLampAttribute) != 0;
        fallback = std::string(kStageArtPrefix) + std::to_string(two_lamps ? 1 : fields->stages);
    }
    const auto* art = find_sequence(shared, fallback);
    if (art == nullptr || art->frames.empty())
        return {};
    return {
        art,
        closest_size_group(*art, gadget.common.width, gadget.common.height),
        {renderer::SpriteArchive::shared, fallback},
        false
    };
}

/// Returns the frame of its art a button shows.
///
/// A staged button shows the frame of its stage, or its sequence's last frame
/// but one while it is held with the pointer over it; grayed, it shows its
/// stage, which the renderer darkens. A checkbox-art button shows its size
/// group's first frame, or the next while checked; grayed, the frame two
/// further on, not darkened. Any other button shows its group's first frame,
/// the next while held, or the one two further on when grayed. Each is kept
/// within the sequence.
///
/// @param control the button
/// @param art the button's art, with at least one frame
/// @param held true while the button is held down with the pointer over it
/// @return the frame's index in the sequence
std::size_t button_frame(const Control& control, const ButtonArt& art, bool held) {
    const auto count = art.sequence->frames.size();
    std::size_t frame = art.base;
    if (control.stages != 0) {
        frame = !control.grayed && held && control.stages < count ? count - kHeldStageFromEnd
                                                                  : control.stage;
    } else if ((control.attributes & kAttributeCheckbox) != 0) {
        const std::size_t checked = control.value != 0 ? 1 : 0;
        frame += control.grayed ? std::min(checked + kGrayedFrameOffset, count - 1) : checked;
    } else if (control.grayed) {
        frame += kGrayedFrameOffset;
    } else if (held) {
        frame += 1;
    }
    return std::min(frame, count - 1);
}

/// Remaps a frame's drawn pixels, and its layers', through one row of the light table.
///
/// @param[in,out] frame frame to light
/// @param row the light table's row for the level, one entry per palette index
void light_frame(formats::gaf::Frame& frame, std::span<const uint8_t> row) {
    for (std::size_t at = 0; at < frame.pixels.size() && at < frame.coverage.size(); ++at)
        if (frame.coverage[at] != 0)
            frame.pixels[at] = row[frame.pixels[at]];
    for (auto& layer : frame.layers)
        light_frame(layer, row);
}

/// Keeps a copy of a frame lit at a light level in the screen's GAF.
///
/// @param[in,out] res screen resources; the copy replaces the button's earlier one
/// @param name the button's name
/// @param frame the frame the button shows, copied before the GAF changes
/// @param level light level, 1..31
/// @return the copy's sequence name; empty without a whole light table
std::string
keep_lit_frame(Resources& res, std::string_view name, formats::gaf::Frame frame, uint8_t level) {
    const auto& table = res.screen.light_table;
    if (table.size() != kLightLevels * kPaletteEntries || level >= kLightLevels)
        return {};
    light_frame(frame, std::span(table).subspan(level * kPaletteEntries, kPaletteEntries));
    std::string lit_name = std::string(name) + std::string(kLitArtSuffix);
    auto& sequences = res.screen.sprites.sequences;
    auto kept = std::find_if(
        sequences.begin(), sequences.end(), [&](const formats::gaf::Sequence& sequence) {
            return sequence.name == lit_name;
        }
    );
    if (kept == sequences.end()) {
        sequences.emplace_back();
        kept = std::prev(sequences.end());
        kept->name = lit_name;
    }
    kept->frames.clear();
    kept->frames.push_back(std::move(frame));
    return lit_name;
}

// ---------------------------------------------------------------------------
// Grayed controls

using renderer::GrayedPaint;
using renderer::IndexedRect;

/// Returns the palettes and tables a screen's grayed controls are drawn with (renderer::grayed_paint).
///
/// @param[in,out] res screen resources; keeps the gray table
/// @return the palettes and tables
GrayedPaint grayed_paint(Resources& res) {
    return renderer::grayed_paint(res.screen, res.gray_table);
}

/// A grayed button with art, which render() grays and shades once the
/// renderer has drawn it.
struct GrayedButton {
    std::size_t order{}; ///< the button's place among the composed layout's gadgets
    int32_t left{};      ///< the button's left column on the screen
    int32_t top{};       ///< the button's top row on the screen
    int32_t width{};     ///< the width of the frame it shows
    int32_t height{};    ///< the height of the frame it shows
};

/// Tells whether a grayed button with art is grayed and shaded over its
/// rectangle, as 3.1c draws every one but a button that cycles its frames
/// and a CHECKBOX-art button that neither has stages nor steps a scroll bar.
///
/// @param control the grayed button
/// @return true when the button is grayed and shaded
bool grayed_art_shaded(const Control& control) {
    namespace attribute = ui::gui_layout::attribute;
    if ((control.attributes & attribute::cycle_frames) != 0)
        return false;
    return control.stages != 0 || (control.attributes & attribute::scroll_step_mask) != 0 ||
           (control.attributes & kAttributeCheckbox) == 0;
}

/// Returns the archive a sprite override names.
///
/// @param res screen resources
/// @param archive the archive named
/// @return the screen's own GAF, the shared one or the global one
const formats::gaf::Archive& sprite_archive(const Resources& res, renderer::SpriteArchive archive) {
    switch (archive) {
    case renderer::SpriteArchive::shared:
        return res.screen.shared_sprites;
    case renderer::SpriteArchive::global:
        return res.screen.global_sprites;
    case renderer::SpriteArchive::screen:
        break;
    }
    return res.screen.sprites;
}

/// Puts back the colours a picture drew over part of a grayed rectangle.
///
/// A hot surface given a picture at run time is drawn over its rectangle, as
/// the renderer draws it: a frame of the rectangle's size unscaled, and any
/// other stretched. Where it covers the grayed rectangle, the colours drawn
/// before the rectangle was grayed come back.
///
/// @param[in,out] surface the drawn screen
/// @param res screen resources
/// @param gadget a gadget the renderer drew after the grayed button
/// @param presentations the presentations the screen was drawn with
/// @param rect the grayed rectangle
/// @param drawn the rectangle's colours before it was grayed, three bytes a pixel
void keep_picture(
    renderer::Surface& surface,
    const Resources& res,
    const ui::gui_layout::Gadget& gadget,
    std::span<const renderer::ButtonPresentation> presentations,
    const IndexedRect& rect,
    std::span<const uint8_t> drawn
) {
    if (gadget.common.active == 0 ||
        gadget.common.type != ui::gui_layout::GadgetType::hot_surface || gadget.common.width <= 0 ||
        gadget.common.height <= 0)
        return;
    const auto state = std::find_if(
        presentations.begin(),
        presentations.end(),
        [&](const renderer::ButtonPresentation& presentation) {
            return presentation.name.size() == gadget.common.name.size() &&
                   starts_with_nocase(presentation.name, gadget.common.name);
        }
    );
    if (state == presentations.end() || !state->sprite.has_value() || !state->gaf_frame.has_value())
        return;
    const auto* sequence =
        find_sequence(sprite_archive(res, state->sprite->archive), state->sprite->sequence);
    if (sequence == nullptr || *state->gaf_frame >= sequence->frames.size())
        return;
    const auto rendered = formats::gaf::render_normal(sequence->frames[*state->gaf_frame]);
    if (!rendered.ok() || rendered.frame->width == 0 || rendered.frame->height == 0)
        return;
    const auto& picture = *rendered.frame;
    const int32_t width = gadget.common.width;
    const int32_t height = gadget.common.height;
    for (int32_t row = 0; row < rect.height; ++row)
        for (int32_t column = 0; column < rect.width; ++column) {
            const int32_t across = rect.left + column - gadget.common.x;
            const int32_t down = rect.top + row - gadget.common.y;
            if (across < 0 || down < 0 || across >= width || down >= height)
                continue;
            const auto from =
                static_cast<std::size_t>(down) * picture.height / static_cast<std::size_t>(height) *
                    picture.width +
                static_cast<std::size_t>(across) * picture.width / static_cast<std::size_t>(width);
            if (picture.coverage[from] == 0)
                continue;
            const auto at = static_cast<std::size_t>(row * rect.width + column) * 3U;
            std::memcpy(
                &surface.rgb
                     [(static_cast<std::size_t>(rect.top + row) * surface.width +
                       static_cast<std::size_t>(rect.left + column)) *
                      3U],
                &drawn[at],
                3
            );
        }
}

/// Grays and shades each grayed button with art over its frame's rectangle, as 3.1c draws it.
///
/// The renderer draws such a button as it would an enabled one, caption
/// included, and its rectangle is grayed and shaded here once the screen is
/// drawn. A picture drawn after the button keeps its colours where it covers
/// the rectangle, since it lies over the grayed button.
///
/// @param[in,out] surface the drawn screen
/// @param[in,out] res screen resources; the gray table is built on first use
/// @param grayed the grayed buttons, in the order they are drawn
/// @param presentations the presentations the screen was drawn with
void gray_buttons(
    renderer::Surface& surface,
    Resources& res,
    std::span<const GrayedButton> grayed,
    std::span<const renderer::ButtonPresentation> presentations
) {
    if (grayed.empty())
        return;
    const auto paint = grayed_paint(res);
    if (paint.shade_palette == nullptr)
        return;
    const auto& gadgets = res.screen.layout.gadgets;
    for (const auto& button : grayed) {
        const auto rect = renderer::read_indices(
            surface, *paint.shade_palette, button.left, button.top, button.width, button.height
        );
        std::vector<uint8_t> drawn(rect.pixels.size() * 3U);
        for (int32_t row = 0; row < rect.height; ++row)
            std::memcpy(
                &drawn[static_cast<std::size_t>(row * rect.width) * 3U],
                &surface.rgb
                     [(static_cast<std::size_t>(rect.top + row) * surface.width +
                       static_cast<std::size_t>(rect.left)) *
                      3U],
                static_cast<std::size_t>(rect.width) * 3U
            );
        renderer::gray_and_shade(surface, paint, rect);
        for (std::size_t later = button.order + 1; later < gadgets.size(); ++later)
            keep_picture(surface, res, gadgets[later], presentations, rect, drawn);
    }
}

/// Builds a button's presentation: its condition, caption stage and frame.
///
/// A grayed button with art that 3.1c grays and shades is presented as an
/// enabled one showing its grayed frame, and listed for render() to gray
/// and shade once it is drawn.
///
/// @param[in,out] res screen resources; a lit button's art is kept in its GAF
/// @param index the control's index
/// @param gadget the composed gadget
/// @param order the gadget's place among the composed layout's gadgets
/// @param with_sprites false to leave out the art the renderer must be told of
/// @param[in,out] grayed the grayed buttons render() grays and shades; the button is added
/// @return the presentation
renderer::ButtonPresentation button_presentation(
    Resources& res,
    int32_t index,
    const ui::gui_layout::Gadget& gadget,
    std::size_t order,
    bool with_sprites,
    std::vector<GrayedButton>& grayed
) {
    const auto& control = res.panel.controls[static_cast<std::size_t>(index)];
    renderer::ButtonPresentation presentation;
    presentation.name = gadget.common.name;
    // The screen's own focus marker, lit round a focused button. A label, a
    // list and the chat line show none, so a room with no focused row is unchanged.
    presentation.focused = index == res.panel.focus;
    // A button shows itself held only while the pointer that pressed it is over it.
    const bool held = index == res.pressed && res.pressed == res.hovered;
    if (control.grayed)
        presentation.condition = renderer::ButtonCondition::disabled;
    else if (held)
        presentation.condition = renderer::ButtonCondition::pressed;
    else if (index == res.hovered)
        presentation.condition = renderer::ButtonCondition::hovered;
    if (control.stages > 1)
        presentation.text_stage = control.stage;
    const auto art = button_art(res, gadget, template_name(res, control), with_sprites);
    if (art.sequence == nullptr || art.sequence->frames.empty())
        return presentation;
    const auto frame = button_frame(control, art, held);
    // A stage before the button's size group is reached by telling the
    // renderer the sequence; without sprites it shows the group's first frame.
    if (art.named || (with_sprites && frame < art.base)) {
        presentation.sprite = art.binding;
        presentation.gaf_frame = frame;
    } else {
        presentation.gaf_frame = frame >= art.base ? frame - art.base : 0;
    }
    if (control.grayed && grayed_art_shaded(control) &&
        grayed_paint(res).shade_palette != nullptr) {
        const auto& shown =
            art.sequence->frames
                [presentation.sprite.has_value() ? frame : art.base + *presentation.gaf_frame];
        presentation.condition = renderer::ButtonCondition::normal;
        grayed.push_back(
            {order,
             gadget.common.x,
             gadget.common.y,
             static_cast<int32_t>(shown.width),
             static_cast<int32_t>(shown.height)}
        );
    }
    if (with_sprites && control.light_level != 0) {
        const auto lit = keep_lit_frame(
            res, gadget.common.name, art.sequence->frames[frame], control.light_level
        );
        if (!lit.empty()) {
            presentation.sprite = renderer::SpriteOverride{renderer::SpriteArchive::screen, lit};
            presentation.gaf_frame = 0;
        }
    }
    return presentation;
}

/// Builds the presentation of a hot surface or image that shows a picture.
///
/// A hot surface copied from a LOGO<x> template shows frame `stage` of the
/// player colours, and any other the frame `stage` of the sequence named after
/// its template in the screen's GAF. A raw frame is stretched over the
/// rectangle; any other is drawn at its own size from the rectangle's top-left
/// corner. An image shows frame 0 of the sequence named after its template in
/// the screen's GAF, then the shared one, at its own size. A missing sequence
/// or a frame past its end shows nothing.
///
/// @param res screen resources
/// @param control the hot surface or image
/// @param[in,out] gadget the composed gadget; an image becomes a hot surface,
///        and one drawn at its frame's size takes that size
/// @return the presentation, or none when nothing is shown
std::optional<renderer::ButtonPresentation>
picture_presentation(const Resources& res, const Control& control, ui::gui_layout::Gadget& gadget) {
    const auto source = template_name(res, control);
    const formats::gaf::Sequence* sequence = nullptr;
    renderer::SpriteOverride sprite{renderer::SpriteArchive::screen, std::string(source)};
    std::size_t frame = control.stage;
    bool own_size = true;
    if (control.type == ControlType::hot_surface) {
        if (starts_with_nocase(source, kColorTemplatePrefix)) {
            sequence = find_sequence(res.screen.global_sprites, kColorArt);
            sprite = {renderer::SpriteArchive::global, std::string(kColorArt)};
        } else {
            sequence = find_sequence(res.screen.sprites, source);
        }
        if (sequence != nullptr && frame < sequence->frames.size()) {
            const auto& shown = sequence->frames[frame];
            own_size = shown.compressed || !shown.layers.empty();
        }
    } else {
        frame = 0;
        sequence = find_sequence(res.screen.sprites, source);
        if (sequence == nullptr) {
            sequence = find_sequence(res.screen.shared_sprites, source);
            sprite.archive = renderer::SpriteArchive::shared;
        }
    }
    if (sequence == nullptr || frame >= sequence->frames.size())
        return std::nullopt;
    gadget.common.type = ui::gui_layout::GadgetType::hot_surface;
    if (own_size) {
        gadget.common.width = static_cast<int16_t>(sequence->frames[frame].width);
        gadget.common.height = static_cast<int16_t>(sequence->frames[frame].height);
    }
    renderer::ButtonPresentation presentation;
    presentation.name = gadget.common.name;
    presentation.sprite = std::move(sprite);
    presentation.gaf_frame = frame;
    return presentation;
}

/// The slot a SIDE button names, or -1 when the name is not SIDE and one digit.
///
/// @param name control name
/// @return the slot, 0 to 9, or -1
int32_t side_button_slot(std::string_view name) {
    constexpr std::string_view prefix = "SIDE";
    if (name.size() != prefix.size() + 1 || !starts_with_nocase(name, prefix))
        return -1;
    const char digit = name.back();
    if (digit < '0' || digit > '9')
        return -1;
    return digit - '0';
}

/// A side past its art shows that side's name on the plain face.
///
/// A SIDE slot copied from SIDEx, not a watcher, whose stage names a loaded
/// side and is past the button's art, takes that name as its caption. The
/// presentation draws the bevel and the name, and no frame.
///
/// @param res screen resources
/// @param control the button
/// @param[in,out] gadget the composed gadget
/// @param with_sprites false to leave out the art the renderer must be told of
/// @param[in,out] presentation the button's presentation
void name_side_past_art(
    const Resources& res,
    const Control& control,
    ui::gui_layout::Gadget& gadget,
    bool with_sprites,
    renderer::ButtonPresentation& presentation
) {
    const int32_t slot = side_button_slot(gadget.common.name);
    if (slot < 0)
        return;
    const auto source = template_name(res, control);
    if (source.size() != 5 || !starts_with_nocase(source, "SIDEx"))
        return;
    auto& lobby = ui().lobby;
    if (lobby.game == nullptr || static_cast<std::size_t>(control.stage) >= lobby.side_names.size())
        return;
    const auto& player = slot_player(lobby, slot);
    const auto* info = player_info(lobby, player);
    if (info != nullptr && player.in_use != 0 && (info->options & option::watcher) != 0)
        return;
    const auto art = button_art(res, gadget, source, with_sprites);
    const std::size_t frames = art.sequence != nullptr ? art.sequence->frames.size() : 0;
    if (static_cast<std::size_t>(control.stage) < frames)
        return;
    auto* fields = std::get_if<ui::gui_layout::ButtonFields>(&gadget.fields);
    if (fields == nullptr)
        return;
    fields->text = lobby.side_names[static_cast<std::size_t>(control.stage)];
    presentation.caption_only = true;
    presentation.game_text = true;
    presentation.text_stage.reset();
}

/// Composes the front panel's layout and presentations for the renderer.
///
/// @param[in,out] res screen resources; the composed layout replaces the last
/// @param[out] buttons the buttons' and pictures' presentations
/// @param[out] lists the list boxes' presentations
/// @param[out] grayed the grayed buttons render() grays and shades once they are drawn
/// @param with_sprites false to leave out the art the renderer must be told of
void compose_layout(
    Resources& res,
    std::vector<renderer::ButtonPresentation>& buttons,
    std::vector<renderer::ListPresentation>& lists,
    std::vector<GrayedButton>& grayed,
    bool with_sprites
) {
    auto& panel = res.panel;
    auto& layout = res.screen.layout;
    layout.gadgets.clear();
    for (int32_t index = 0; index < panel.count; ++index) {
        const auto& control = panel.controls[static_cast<std::size_t>(index)];
        if (control.source < 0 ||
            static_cast<std::size_t>(control.source) >= res.source.gadgets.size())
            continue;
        auto gadget = res.source.gadgets[static_cast<std::size_t>(control.source)];
        gadget.common.name = std::string(control_name(control));
        const int16_t dx = index == 0 ? 0 : res.offset_x;
        const int16_t dy = index == 0 ? 0 : res.offset_y;
        gadget.common.x = static_cast<int16_t>(control.x + dx);
        gadget.common.y = static_cast<int16_t>(control.y + dy);
        gadget.common.width = control.width;
        gadget.common.height = control.height;
        gadget.common.active = static_cast<int8_t>(control.active);
        const std::string text(control_text(control));
        if (control.type == ControlType::label &&
            gadget.common.type == ui::gui_layout::GadgetType::button) {
            gadget.common.type = ui::gui_layout::GadgetType::label;
            gadget.fields = ui::gui_layout::LabelFields{text, text, ""};
        } else if (auto* button = std::get_if<ui::gui_layout::ButtonFields>(&gadget.fields)) {
            button->text = text;
        } else if (auto* label = std::get_if<ui::gui_layout::LabelFields>(&gadget.fields)) {
            label->text = text;
        } else if (
            auto* list = std::get_if<ui::gui_layout::ListBoxFields>(&gadget.fields);
            list != nullptr && control.list_item_height != 0
        ) {
            // A list is drawn at the pitch its rows are picked and scrolled by.
            list->item_height = control.list_item_height;
        }
        if (index > 0 && control.type == ControlType::button && control.active != 0) {
            buttons.push_back(
                button_presentation(res, index, gadget, layout.gadgets.size(), with_sprites, grayed)
            );
            name_side_past_art(res, control, gadget, with_sprites, buttons.back());
        }
        if (index > 0 && with_sprites && control.active != 0 &&
            (control.type == ControlType::hot_surface || control.type == ControlType::image))
            if (auto picture = picture_presentation(res, control, gadget))
                buttons.push_back(std::move(*picture));
        if (control.type == ControlType::list_box && !control.items.empty()) {
            renderer::ListPresentation presented{
                gadget.common.name,
                std::span<const std::string>(control.items),
                static_cast<std::size_t>(std::max<int16_t>(0, control.list_first)),
                static_cast<std::size_t>(std::max<int16_t>(0, control.list_selection))
            };
            if (control.item_grey.size() == control.items.size())
                presented.grey = control.item_grey;
            lists.push_back(presented);
        }
        layout.gadgets.push_back(std::move(gadget));
    }
}

/// Draws text in a font, clipped to a box, in the screen's palette.
///
/// The text keeps the game's own fonts whatever the Language settings say,
/// the battle room's chat entry included, as do the screens' lists and
/// buttons: a character the font lacks is drawn in the modern fonts, in the
/// font's own colour, as with them off.
///
/// @param[in,out] surface image drawn on
/// @param res screen resources
/// @param font font the text is drawn in
/// @param text text drawn
/// @param x left of the box in pixels
/// @param y top of the box in pixels
/// @param width box width in pixels
/// @param height box height in pixels
void draw_text_in(
    renderer::Surface& surface,
    const Resources& res,
    const formats::fnt::Font& font,
    std::string_view text,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height
) {
    if (surface.width == 0 || width <= 0 || height <= 0)
        return;
    const auto& palette = res.screen.background.palette.has_value() ? *res.screen.background.palette
                                                                    : res.screen.gui_palette;
    if (renderer::needs_text_runs(text, false)) {
        // The modern fonts' letters take the font's own colour.
        const std::size_t ink =
            static_cast<std::size_t>(renderer::fnt_font_ink(font, palette)) * 4U;
        std::ignore = renderer::draw_fnt_game_text(
            surface,
            font,
            text,
            x,
            y,
            {palette[ink], palette[ink + 1], palette[ink + 2]},
            palette,
            {x, y, x + width - 1, y + height - 1},
            false
        );
        return;
    }
    std::vector<uint8_t> pixels(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0
    );
    std::vector<uint8_t> coverage(pixels.size(), 0);
    formats::fnt::IndexedSurface target{
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        static_cast<std::size_t>(width),
        pixels,
        coverage
    };
    (void)formats::fnt::raster_text(target, font, text, 0, 0);
    for (int32_t row = 0; row < height; ++row)
        for (int32_t column = 0; column < width; ++column) {
            const auto at = static_cast<std::size_t>(row * width + column);
            if (coverage[at] == 0)
                continue;
            const auto px = x + column;
            const auto py = y + row;
            if (px < 0 || py < 0 || px >= static_cast<int32_t>(surface.width) ||
                py >= static_cast<int32_t>(surface.height))
                continue;
            auto* out = &surface.rgb
                             [(static_cast<std::size_t>(py) * surface.width +
                               static_cast<std::size_t>(px)) *
                              3U];
            const auto index = static_cast<std::size_t>(pixels[at]) * 4U;
            out[0] = palette[index];
            out[1] = palette[index + 1];
            out[2] = palette[index + 2];
        }
}

/// A colour of the drawn screen.
struct ScreenColor {
    uint8_t r{}; ///< red, 0 to 255
    uint8_t g{}; ///< green, 0 to 255
    uint8_t b{}; ///< blue, 0 to 255
};

/// A rectangle of the drawn screen, its edges included.
struct ScreenBox {
    int32_t left{};   ///< first column
    int32_t top{};    ///< first row
    int32_t right{};  ///< last column
    int32_t bottom{}; ///< last row
};

/// The most columns and rows of a text's box that draw_text_in_color draws.
constexpr int32_t kColorTextMostWidth = 64;
constexpr int32_t kColorTextMostHeight = 32;

/// Draws text's letters in one colour, laid out in a box as draw_text_in lays it out.
///
/// The text is drawn through the font's coverage, as draw_text_in draws it.
/// A glyph's letter pixels, those at least half as light as the font's ink
/// (renderer::fnt_font_ink), take the colour whatever palette index the font
/// gives them; its darker outline is left out, so that what lies under the
/// text shows around the letters. Only the pixels inside the box, the clip
/// rectangle and the surface are drawn, and of the box only its first
/// kColorTextMostWidth columns and kColorTextMostHeight rows. Nothing is
/// allocated.
///
/// @param[in,out] surface image drawn on
/// @param res screen resources, whose palette the font's indices select from
/// @param font font the text is drawn in
/// @param text text drawn, in the font's own characters
/// @param x left of the box in pixels
/// @param y top of the box in pixels
/// @param width box width in pixels
/// @param height box height in pixels
/// @param clip the only pixels that may be drawn
/// @param color the letters' colour
void draw_text_in_color(
    renderer::Surface& surface,
    const Resources& res,
    const formats::fnt::Font& font,
    std::string_view text,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    const ScreenBox& clip,
    ScreenColor color
) {
    width = std::min(width, kColorTextMostWidth);
    height = std::min(height, kColorTextMostHeight);
    if (surface.width == 0 || width <= 0 || height <= 0)
        return;
    const auto& palette = res.screen.background.palette.has_value() ? *res.screen.background.palette
                                                                    : res.screen.gui_palette;
    // Lightness as the sum of the three channels, as fnt_font_ink judges it.
    const auto lightness = [&palette](uint8_t index) {
        const auto at = static_cast<std::size_t>(index) * 4U;
        return palette[at] + palette[at + 1] + palette[at + 2];
    };
    const auto ink_lightness = lightness(renderer::fnt_font_ink(font, palette));
    uint8_t pixels[kColorTextMostWidth * kColorTextMostHeight]{};
    uint8_t coverage[kColorTextMostWidth * kColorTextMostHeight]{};
    const auto area = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    formats::fnt::IndexedSurface target{
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        static_cast<std::size_t>(width),
        std::span<uint8_t>(pixels, area),
        std::span<uint8_t>(coverage, area)
    };
    (void)formats::fnt::raster_text(target, font, text, 0, 0);
    for (int32_t row = 0; row < height; ++row)
        for (int32_t column = 0; column < width; ++column) {
            const auto at = static_cast<std::size_t>(row * width + column);
            if (coverage[at] == 0 || lightness(pixels[at]) * 2 < ink_lightness)
                continue;
            const auto px = x + column;
            const auto py = y + row;
            if (px < clip.left || px > clip.right || py < clip.top || py > clip.bottom || px < 0 ||
                py < 0 || px >= static_cast<int32_t>(surface.width) ||
                py >= static_cast<int32_t>(surface.height))
                continue;
            auto* out = &surface.rgb
                             [(static_cast<std::size_t>(py) * surface.width +
                               static_cast<std::size_t>(px)) *
                              3U];
            out[0] = color.r;
            out[1] = color.g;
            out[2] = color.b;
        }
}

/// Draws text in the screen's GUI font, clipped to a box, as draw_text_in does.
///
/// @param[in,out] surface image drawn on
/// @param res screen resources
/// @param text text drawn
/// @param x left of the box in pixels
/// @param y top of the box in pixels
/// @param width box width in pixels
/// @param height box height in pixels
void draw_text(
    renderer::Surface& surface,
    const Resources& res,
    std::string_view text,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height
) {
    draw_text_in(surface, res, res.screen.font, text, x, y, width, height);
}

/// Draws each seated player's game version over the player's colour square, as 3.1c does.
///
/// Every slot held by the local player, a computer player or a remote player
/// shows "major.minor" from its lobby block in the label font, centred on its
/// LOGO<slot> rectangle whether or not the square shows. The rectangle's
/// right and bottom edges are its last column and row, and halves are
/// truncated toward zero.
///
/// @param[in,out] surface image drawn on
/// @param res the battle room's resources
/// @param lobby lobby whose slots are shown
void draw_player_versions(renderer::Surface& surface, const Resources& res, Lobby& lobby) {
    if (lobby.game == nullptr)
        return;
    const auto& font = res.screen.label_font;
    const int32_t text_height = formats::fnt::line_height(font);
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        const auto& player = slot_player(lobby, slot);
        if (player.status != kSlotLocal && player.status != kSlotComputer &&
            player.status != kSlotRemote)
            continue;
        char name[16];
        std::snprintf(name, sizeof(name), "LOGO%d", static_cast<int>(slot));
        const auto* logo = panel_control(res.panel, name);
        const auto* info = player_info(lobby, player);
        if (logo == nullptr || info == nullptr)
            continue;
        char text[16];
        std::snprintf(text, sizeof(text), "%d.%d", info->version_major, info->version_minor);
        const auto text_width = static_cast<int32_t>(formats::fnt::measure_text(font, text));
        const int32_t left = logo->x + res.offset_x;
        const int32_t top = logo->y + res.offset_y;
        const int32_t right = left + logo->width - 1;
        const int32_t bottom = top + logo->height - 1;
        draw_text_in(
            surface,
            res,
            font,
            text,
            (left + right - text_width) / 2,
            (top + bottom - text_height) / 2,
            text_width,
            text_height
        );
    }
}

void fill(
    renderer::Surface& surface,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    uint8_t r,
    uint8_t g,
    uint8_t b
) {
    for (int32_t py = std::max(0, y);
         py < std::min<int32_t>(y + height, static_cast<int32_t>(surface.height));
         ++py)
        for (int32_t px = std::max(0, x);
             px < std::min<int32_t>(x + width, static_cast<int32_t>(surface.width));
             ++px) {
            auto* out = &surface.rgb
                             [(static_cast<std::size_t>(py) * surface.width +
                               static_cast<std::size_t>(px)) *
                              3U];
            out[0] = r;
            out[1] = g;
            out[2] = b;
        }
}

// The OA badge, drawn where a 3.1c player's row shows the CD icon, in the
// design's colours.
/// The badge's text.
constexpr std::string_view kBadgeText = "OA";
/// Badge border, #7d6a2c: the badge's 1-pixel edge.
constexpr ScreenColor kBadgeBorderColor{0x7d, 0x6a, 0x2c};
/// Badge top, #2c332c: the top row of the badge's vertical gradient.
constexpr ScreenColor kBadgeTopColor{0x2c, 0x33, 0x2c};
/// Badge bottom, #151915: the bottom row of the badge's vertical gradient.
constexpr ScreenColor kBadgeBottomColor{0x15, 0x19, 0x15};
/// Amber, #e2b23d: the badge's text and its dot.
constexpr ScreenColor kAmberColor{0xe2, 0xb2, 0x3d};
/// Background, #0d100e: the ring around the dot.
constexpr ScreenColor kBackgroundColor{0x0d, 0x10, 0x0e};
/// The dot's ring is presence_badge_dot_ring pixels across, its four corner
/// pixels left out, reaching presence_badge_dot_overhang pixels past the
/// badge's right edge and above its top; the dot is the square one pixel inside it.

/// Fills a square without its four corner pixels, skipping pixels outside the surface.
///
/// @param[in,out] surface image drawn on
/// @param left first column
/// @param top first row
/// @param size columns and rows, 2 or more
/// @param color the fill
void fill_square_without_corners(
    renderer::Surface& surface, int32_t left, int32_t top, int32_t size, ScreenColor color
) {
    for (int32_t row = 0; row < size; ++row) {
        const int32_t inset = (row == 0 || row == size - 1) ? 1 : 0;
        fill(surface, left + inset, top + row, size - 2 * inset, 1, color.r, color.g, color.b);
    }
}

/// Draws the OA badge over its CD<slot> rectangle, and the dot on it when its player's rules differ.
///
/// The rectangle is filled row by row with a vertical gradient from badge
/// top to badge bottom: row r of h takes, in each channel,
/// top + (bottom - top) * r / (h - 1), truncated toward zero, and a
/// rectangle one row high takes badge top. Its four edges are drawn in badge
/// border, and "OA" in the label font in amber, centred as
/// draw_player_versions centres a version and clipped to the rectangle
/// inside the border. The dot is drawn last, over the top-right corner.
///
/// @param[in,out] surface image drawn on
/// @param res the battle room's resources
/// @param spot where the badge goes, and whether it carries the dot
void draw_presence_badge(
    renderer::Surface& surface, const Resources& res, const PresenceBadgeSpot& spot
) {
    const int32_t x = spot.x;
    const int32_t y = spot.y;
    const int32_t width = spot.width;
    const int32_t height = spot.height;
    if (width <= 0 || height <= 0)
        return;
    const auto shade = [&](uint8_t top, uint8_t bottom, int32_t row) {
        return static_cast<uint8_t>(height == 1 ? top : top + (bottom - top) * row / (height - 1));
    };
    for (int32_t row = 0; row < height; ++row)
        fill(
            surface,
            x,
            y + row,
            width,
            1,
            shade(kBadgeTopColor.r, kBadgeBottomColor.r, row),
            shade(kBadgeTopColor.g, kBadgeBottomColor.g, row),
            shade(kBadgeTopColor.b, kBadgeBottomColor.b, row)
        );
    const int32_t right = x + width - 1;
    const int32_t bottom = y + height - 1;
    const auto& border = kBadgeBorderColor;
    fill(surface, x, y, width, 1, border.r, border.g, border.b);
    fill(surface, x, bottom, width, 1, border.r, border.g, border.b);
    fill(surface, x, y, 1, height, border.r, border.g, border.b);
    fill(surface, right, y, 1, height, border.r, border.g, border.b);
    const auto& font = res.screen.label_font;
    const auto text_width = static_cast<int32_t>(formats::fnt::measure_text(font, kBadgeText));
    const int32_t text_height = formats::fnt::line_height(font);
    draw_text_in_color(
        surface,
        res,
        font,
        kBadgeText,
        (x + right - text_width) / 2,
        (y + bottom - text_height) / 2,
        text_width,
        text_height,
        {x + 1, y + 1, right - 1, bottom - 1},
        kAmberColor
    );
    if (!spot.rules_differ)
        return;
    const int32_t ring_left = x + width + presence_badge_dot_overhang - presence_badge_dot_ring;
    const int32_t ring_top = y - presence_badge_dot_overhang;
    fill_square_without_corners(
        surface, ring_left, ring_top, presence_badge_dot_ring, kBackgroundColor
    );
    fill_square_without_corners(
        surface, ring_left + 1, ring_top + 1, presence_badge_dot_ring - 2, kAmberColor
    );
}

/// Lists the OA badges the battle room's rows show, in slot order, as multiplayer_presence_badges does.
///
/// @param res the battle room's resources
/// @param lobby lobby whose slots are shown
/// @param[out] out receives the badges; null receives none
/// @param capacity how many badges out holds
/// @return how many badges were written
int32_t presence_badge_spots(
    const Resources& res, Lobby& lobby, PresenceBadgeSpot* out, int32_t capacity
) noexcept {
    if (lobby.game == nullptr || out == nullptr)
        return 0;
    int32_t count = 0;
    for (int32_t slot = 0; slot < kSlotCount && count < capacity; ++slot) {
        const auto badge = lobby_row_badge(lobby, slot);
        if (badge.badge != RowBadge::open_annihilation)
            continue;
        char name[16];
        std::snprintf(name, sizeof(name), "CD%d", static_cast<int>(slot));
        const auto* disc = panel_control(res.panel, name);
        if (disc == nullptr || disc->width <= 0 || disc->height <= 0)
            continue;
        out[count++] = {
            slot,
            disc->x + res.offset_x,
            disc->y + res.offset_y,
            disc->width,
            disc->height,
            badge.rules_differ
        };
    }
    return count;
}

/// Draws the OA badge, and its dot, in each OA player's CD<slot> rectangle (draw_presence_badge).
///
/// Left out when the OA layer draws the badges at the window's own pixels
/// (multiplayer_bind_layer_presence_badges), so the frame's small badge is
/// not magnified under the sharp one. A renderer without the layer keeps it.
///
/// @param[in,out] surface image drawn on
/// @param res the battle room's resources
/// @param lobby lobby whose slots are shown
void draw_presence_badges(renderer::Surface& surface, const Resources& res, Lobby& lobby) {
    if (g_layer_draws_badges != nullptr && g_layer_draws_badges(g_layer_badge_context))
        return;
    PresenceBadgeSpot spots[kSlotCount]{};
    const auto count = presence_badge_spots(res, lobby, spots, kSlotCount);
    for (int32_t index = 0; index < count; ++index)
        draw_presence_badge(surface, res, spots[index]);
}

renderer::Surface render(Resources& res) {
    std::vector<renderer::ButtonPresentation> buttons;
    std::vector<renderer::ListPresentation> lists;
    std::vector<GrayedButton> grayed;
    for (const bool sprites : {true, false}) {
        buttons.clear();
        lists.clear();
        grayed.clear();
        compose_layout(res, buttons, lists, grayed, sprites);
        try {
            auto surface = renderer::render_screen(res.screen, buttons, lists);
            gray_buttons(surface, res, grayed, buttons);
            return surface;
        } catch (const std::exception&) {
        }
    }
    try {
        return renderer::render_screen(res.screen, {}, {});
    } catch (const std::exception&) {
        return {
            static_cast<uint32_t>(kCanvasWidth),
            static_cast<uint32_t>(kCanvasHeight),
            std::vector<uint8_t>(kCanvasWidth * kCanvasHeight * 3, 0)
        };
    }
}

// ---------------------------------------------------------------------------
// Pictures

// A hot surface without art or a picture is filled with this GUI palette colour.
constexpr size_t kBlankPictureColor = 7;
// Bytes from a texture's first pixel that the quad sampler may read.
constexpr size_t kPictureTextureWindowBytes = 0x10000;

/// Tells whether a hot surface shows art, which the renderer draws.
///
/// @param res screen resources
/// @param control the hot surface
/// @return true for a player colour square, or a sequence named after its
///         template in the screen's GAF
bool hot_surface_has_art(const Resources& res, const Control& control) {
    const auto source = template_name(res, control);
    return starts_with_nocase(source, kColorTemplatePrefix) ||
           find_sequence(res.screen.sprites, source) != nullptr;
}

/// Draws a picture over its hot surface's rectangle, one texel in from its edges, as 3.1c does.
///
/// The picture is stretched from texel (1, 1) to its far edges over every
/// pixel of the rectangle but its last column and row, which keep what was
/// drawn under them.
///
/// @param[in,out] surface the drawn screen
/// @param palette the palette the picture's indices select from
/// @param control the hot surface and its picture
/// @param x the rectangle's left column on the screen
/// @param y the rectangle's top row on the screen
void draw_picture(
    renderer::Surface& surface,
    const PaletteBytes& palette,
    const Control& control,
    int32_t x,
    int32_t y
) {
    const int32_t width = control.width;
    const int32_t height = control.height;
    const int32_t picture_width = control.picture_width;
    const int32_t picture_height = control.picture_height;
    if (width < 2 || height < 2 || picture_width < 2 || picture_height < 2 ||
        picture_width > std::numeric_limits<uint16_t>::max() ||
        picture_height > std::numeric_limits<uint16_t>::max() ||
        control.picture.size() / static_cast<size_t>(picture_width) <
            static_cast<size_t>(picture_height))
        return;
    std::vector<uint8_t> texels(std::max(control.picture.size(), kPictureTextureWindowBytes), 0);
    std::copy(control.picture.begin(), control.picture.end(), texels.begin());
    oa::Sprite texture{};
    texture.width = static_cast<uint16_t>(picture_width);
    texture.height = static_cast<uint16_t>(picture_height);
    texture.encoding = OA_SPRITE_RAW;
    texture.data = texels.data();
    auto face = present::create_surface(width, height);
    const present::PolygonVertex quad[4] = {
        {0, 0}, {width - 1, 0}, {width - 1, height - 1}, {0, height - 1}
    };
    const present::model::TexturePoint uv[4] = {
        {1, 1},
        {picture_width - 1, 1},
        {picture_width - 1, picture_height - 1},
        {1, picture_height - 1}
    };
    present::model::texture_quad(&face.surface, &texture, quad, uv);
    for (int32_t row = 0; row < height - 1; ++row)
        for (int32_t column = 0; column < width - 1; ++column) {
            const int32_t px = x + column;
            const int32_t py = y + row;
            if (px < 0 || py < 0 || px >= static_cast<int32_t>(surface.width) ||
                py >= static_cast<int32_t>(surface.height))
                continue;
            const auto index = face.pixels
                                   [static_cast<size_t>(row) * static_cast<size_t>(width) +
                                    static_cast<size_t>(column)];
            std::memcpy(
                &surface
                     .rgb[(static_cast<size_t>(py) * surface.width + static_cast<size_t>(px)) * 3U],
                &palette[static_cast<size_t>(index) * 4U],
                3
            );
        }
}

/// Draws the hot surfaces that have no art, as 3.1c draws them.
///
/// One with a picture shows it (draw_picture); one without is filled with
/// the GUI palette's colour 7, matched to the screen's palette. MAPPIC is the
/// only such surface in the multiplayer screens.
///
/// @param[in,out] surface the drawn screen
/// @param res screen resources
void draw_pictures(renderer::Surface& surface, const Resources& res) {
    const auto& screen = res.screen;
    const auto& palette =
        screen.background.palette.has_value() ? *screen.background.palette : screen.gui_palette;
    const auto& panel = res.panel;
    for (int32_t index = 1; index < panel.count; ++index) {
        const auto& control = panel.controls[static_cast<size_t>(index)];
        if (control.type != ControlType::hot_surface || control.active == 0 || control.width <= 0 ||
            control.height <= 0 || hot_surface_has_art(res, control))
            continue;
        const int32_t x = control.x + res.offset_x;
        const int32_t y = control.y + res.offset_y;
        if (!control.picture.empty()) {
            draw_picture(surface, palette, control, x, y);
            continue;
        }
        const auto blank = remap_palette(screen.gui_palette, palette)[kBlankPictureColor];
        const auto* rgb = &palette[static_cast<size_t>(blank) * 4U];
        fill(surface, x, y, control.width, control.height, rgb[0], rgb[1], rgb[2]);
    }
}

void draw_text_boxes(renderer::Surface& surface, Resources& res) {
    auto& panel = res.panel;
    const std::string& composition = ui().composition;
    for (int32_t index = 1; index < panel.count; ++index) {
        const auto& control = panel.controls[static_cast<std::size_t>(index)];
        if (control.type != ControlType::text_box || control.active == 0)
            continue;
        paint_plain_text_box(surface, res, control);
        std::string text(control_text(control));
        const bool focused = index == panel.focus;
        const bool cursor = focused && ((elapsed_ms() / 500U) & 1U) == 0;
        const int32_t x = control.x + res.offset_x + 3;
        const int32_t y = control.y + res.offset_y + 3;
        const int32_t width = control.width - 4;
        const int32_t height = control.height - 2;
        // The input method's composition at the end of the focused box is
        // drawn in the modern fonts, as the characters it composes are,
        // and underlined.
        if (focused && !composition.empty() && text.ends_with(composition)) {
            text.resize(text.size() - composition.size());
            draw_text(surface, res, text, x, y, width, height);
            const auto& palette = res.screen.background.palette.has_value()
                                      ? *res.screen.background.palette
                                      : res.screen.gui_palette;
            const std::size_t ink =
                static_cast<std::size_t>(renderer::fnt_font_ink(res.screen.font, palette)) * 4U;
            const int32_t pen = renderer::draw_fnt_composition(
                surface,
                res.screen.font,
                composition,
                x + renderer::measure_fnt_game_text(res.screen.font, text, false),
                y,
                {palette[ink], palette[ink + 1], palette[ink + 2]},
                palette,
                {x, y, x + width - 1, y + height - 1}
            );
            if (cursor && pen < x + width)
                draw_text(surface, res, "_", pen, y, x + width - pen, height);
            continue;
        }
        if (cursor)
            text += '_';
        draw_text(surface, res, text, x, y, width, height);
    }
}

// ---------------------------------------------------------------------------
// Sliders

/// Draws a screen's bound sliders and their arrows as 3.1c draws them (renderer::draw_scroll_bar).
///
/// @param[in,out] surface image drawn on
/// @param[in,out] res screen resources; the gray table is built on first use
void draw_sliders(renderer::Surface& surface, Resources& res) {
    const auto& screen = res.screen;
    const auto paint = grayed_paint(res);
    auto& panel = res.panel;
    for (int32_t index = 1; index < panel.count; ++index) {
        auto& control = panel.controls[static_cast<std::size_t>(index)];
        if (control.type != ControlType::slider)
            continue;
        const auto& bar = slider_bar(control);
        renderer::draw_scroll_bar(
            surface,
            paint,
            renderer::scroll_art_sequence(&screen.sprites, screen.shared_sprites, bar.art),
            bar,
            res.offset_x,
            res.offset_y,
            res.hold.bar == index ? res.hold.part : oa::ui::gui_input::ScrollPart::none
        );
    }
}

void draw_message(renderer::Surface& surface, const Resources& res) {
    const auto& text = ui().message;
    if (text.empty())
        return;
    constexpr int32_t width = 400, height = 60;
    const int32_t x = (kCanvasWidth - width) / 2, y = (kCanvasHeight - height) / 2;
    fill(surface, x - 1, y - 1, width + 2, height + 2, 160, 160, 160);
    fill(surface, x, y, width, height, 16, 16, 24);
    draw_text(surface, res, text, x + 10, y + 10, width - 20, 20);
    draw_text(surface, res, "OK", x + width / 2 - 8, y + height - 22, 40, 20);
}

// ---------------------------------------------------------------------------
// Input

void dispatch(ScreenContext* ctx);

/// Returns the current tick of the lobby's 30 Hz clock, which held sliders and arrows run on.
///
/// @return the tick
uint32_t ui_tick() {
    const auto& services = ui().lobby.services;
    return services.tick != nullptr ? services.tick(services.context) : service_tick(nullptr);
}

/// Brings a slider's group in line with it and runs its handler, once its
/// knob moved or its arrow stepped it, as 3.1c does.
///
/// The lists the slider scrolls follow its knob first. A slider with a
/// handler then runs it, and one that notifies its panel is clicked for the
/// panel's handler, as RESTRICT2's are; a list's scroll bar has neither.
///
/// @param ctx screen context of the running frontend
/// @param[in,out] res the front screen's resources
/// @param index the slider, or kNoControl for none
void notify_slider(ScreenContext* ctx, Resources& res, int32_t index) {
    auto& panel = res.panel;
    if (index <= 0 || index >= panel.count)
        return;
    panel.dirty = true;
    panel_sync_group(panel, index);
    auto& slider = panel.controls[static_cast<std::size_t>(index)];
    if (slider.on_change != nullptr) {
        slider.on_change(panel, &ui().lobby);
        return;
    }
    if (slider.notifies_panel && panel_press(panel, index))
        dispatch(ctx);
}

/// Runs the handler a screen gives a list for a new selection, as 3.1c runs it.
///
/// SELMAP's MAPNAMES previews the map now selected, without telling the
/// other players, and SELGAME's columns refresh JOINGAME and WATCH for the
/// game now selected. No other list has one.
///
/// @param[in,out] res the front screen's resources
/// @param index the list
void list_changed(Resources& res, int32_t index) {
    auto& state = ui();
    if (&res == &state.modal && state.modal_kind == ModalKind::selmap) {
        if (index == panel_find(res.panel, "MAPNAMES"))
            mapselect_preview(state.lobby, state.mapselect, res.panel);
    } else if (&res == &state.base && state.screen == kScreenGameList) {
        game_list_refresh_join(state.lobby, state.connect, res.panel);
    }
}

/// Scrolls a list a row for a notch of the mouse wheel over it, its scroll bar or an arrow.
///
/// 3.1c reads no wheel. A list whose group follows it scrolls within its
/// pages and its knob follows. Otherwise a list's scroll bar steps its knob
/// once, as its arrow would, and its handler runs, which keeps RESTRICT2's
/// count sliders on the rows it shows. The wheel moves no other slider.
///
/// @param ctx screen context of the running frontend
/// @param[in,out] res the front screen's resources
/// @param hit the control under the pointer, or kNoControl
/// @param back true for a notch toward the list's start
void wheel_list(ScreenContext* ctx, Resources& res, int32_t hit, bool back) {
    auto& panel = res.panel;
    if (hit == kNoControl)
        return;
    const auto index = hit;
    const auto type = panel.controls[static_cast<std::size_t>(index)].type;
    const auto list = type == ControlType::list_box
                          ? index
                          : panel_group_member(panel, index, ControlType::list_box);
    if (list == kNoControl)
        return;
    if (panel_scroll_list(panel, list, back ? -1 : 1) ||
        panel.controls[static_cast<std::size_t>(list)].list_item_height != 0)
        return;
    const auto bar =
        type == ControlType::slider ? index : panel_group_member(panel, list, ControlType::slider);
    if (bar == kNoControl)
        return;
    auto& slider = slider_bar(panel.controls[static_cast<std::size_t>(bar)]);
    if (!oa::ui::gui_input::scroll_takes_input(slider))
        return;
    oa::ui::gui_input::scroll_step(slider, !back);
    notify_slider(ctx, res, bar);
}

bool click(ScreenContext* ctx, int32_t index, int32_t x, int32_t y, uint8_t button) {
    auto& res = front();
    auto& panel = res.panel;
    // A double click is two presses in a row: every press, on any control
    // or on none, becomes the press the next one pairs with.
    const auto previous_index = ui().last_click_index;
    const auto previous_ms = ui().last_click_ms;
    ui().last_click_index = index;
    ui().last_click_ms = elapsed_ms();
    if (index <= 0 || index >= panel.count)
        return false;
    auto& control = panel.controls[static_cast<std::size_t>(index)];
    if (control.grayed)
        return false;
    bool activate = true;
    if (control.type == ControlType::text_box) {
        panel.focus = index;
        activate = false;
    } else if (control.type == ControlType::list_box) {
        const auto press = panel_press_list(panel, index, y);
        if (press == ListPress::missed)
            return false;
        const bool again = previous_index == index && press == ListPress::same &&
                           ui().last_click_ms - previous_ms < kDoubleClickMs;
        if (press == ListPress::changed)
            list_changed(res, index);
        // Only a second press on the selected row starts the list's action:
        // one press on a SELGAME column selects that game without joining it.
        activate = again;
    } else if (control.type == ControlType::slider) {
        // A click on a slider is a press and a release at one point: an
        // arrow's press steps the knob, a press on the bar steps it on release.
        notify_slider(ctx, res, slider_press(panel, res.hold, index, x, y, ui_tick()));
        notify_slider(ctx, res, slider_release(panel, res.hold, x, y));
        return true;
    }
    if (!activate)
        return true;
    if (!panel_press(panel, index, button))
        return false;
    (void)ctx;
    dispatch(ctx);
    return true;
}

// ---------------------------------------------------------------------------
// Screen flows

/// Returns to the main menu, as a launched game leaves the game list or the battle room.
///
/// Application mode 1 is asked for, the frontend state becomes the main
/// menu with the initialize signal, and a frontend pass runs it: the main
/// menu asks for the return after a launch (step return_after_launch) when
/// the frontend's queries call for it, or opens itself.
///
/// @param ctx Screen context of the running frontend.
void return_to_main_menu(ScreenContext* ctx) {
    release_pack_source();
    const auto& link = ui().launch_link;
    if (link.request_app_mode != nullptr)
        link.request_app_mode(link.context, kAppModeFrontend);
    const auto* services = ctx->services;
    if (services == nullptr)
        return;
    if (services->set_frontend_state != nullptr)
        services->set_frontend_state(ctx->host, kStateMainMenu);
    if (services->set_frontend_signal != nullptr)
        services->set_frontend_signal(ctx->host, kSignalInitialize);
    if (services->run_frontend != nullptr)
        services->run_frontend(ctx->host);
    else
        app::screen_request(ctx, kScreenMainMenu);
}

/// Leaves the game and ends the program through the launch's link.
///
/// @param with_reason whether the local player's disconnect reason shows first
void leave_game(bool with_reason) {
    const auto& link = ui().launch_link;
    if (link.leave_game != nullptr)
        link.leave_game(link.context, with_reason);
}

/// Turns the platform's text input on or off through the host.
///
/// The text boxes (the TCP address, names and passwords, the battle room's
/// chat) take typed characters only as text events, which the platform
/// sends only while text input is on.
///
/// @param ctx Screen context of the running frontend.
/// @param enabled True to turn it on.
void set_text_input(ScreenContext* ctx, bool enabled) {
    if (ctx != nullptr && ctx->services != nullptr && ctx->services->set_text_input != nullptr)
        ctx->services->set_text_input(ctx->host, enabled ? 1 : 0);
}

/// Returns the most bytes a text box holds.
///
/// @param control a text box
/// @return its length limit, below the text field's size
std::size_t text_box_limit(const Control& control) noexcept {
    return control.value > 0 ? static_cast<std::size_t>(control.value) : kControlTextBytes - 1;
}

/// Adds the printable ASCII of typed text to a box's text while it stays within the box's limit, as the address,
/// name and password boxes take typing.
///
/// @param[in,out] value the box's text
/// @param typed the typed text
/// @param limit the most bytes the box holds
void take_printable(std::string& value, std::string_view typed, std::size_t limit) {
    for (const char c : typed)
        if (static_cast<unsigned char>(c) >= 0x20 && static_cast<unsigned char>(c) < 0x7f &&
            value.size() < limit)
            value.push_back(c);
}

/// Clears a named text box and types text into it, as a player does.
///
/// @param[in,out] panel the screen's panel
/// @param name the box's name
/// @param text the text typed; empty leaves the box as it is
void retype(Panel& panel, const char* name, std::string_view text) {
    Control* control = panel_control(panel, name);
    if (text.empty() || control == nullptr || control->type != ControlType::text_box)
        return;
    std::string value;
    take_printable(value, text, text_box_limit(*control));
    set_control_text(*control, value);
    panel.dirty = true;
}

/// Ends the direct game on the screen in front, which refused it with the notice it shows.
///
/// @param fallback the notice reported when the screen shows none
void fail_direct_game(const char* fallback) {
    g_direct.stage = DirectStage::none;
    g_direct.progress.reached = DirectGameStep::failed;
    g_direct.progress.notice = ui().message.empty() ? fallback : ui().message;
}

void apply_lobby_action(ScreenContext* ctx, LobbyAction action) {
    auto& state = ui();
    auto& lobby = state.lobby;
    switch (action) {
    case LobbyAction::leave:
        lobby_leave_battleroom(lobby);
        if (lobby.net.leave != nullptr)
            lobby.net.leave(lobby.net.context);
        state.in_lobby = false;
        close_modal();
        // A launched battle goes back to the main menu, and from there on to
        // the return after a launch.
        if (lobby_launch_active(lobby))
            return_to_main_menu(ctx);
        else
            app::screen_request(ctx, kScreenGameList);
        break;
    case LobbyAction::start:
        if (state.start_handler == nullptr) {
            app::screen_status(
                ctx, "Multiplayer match start is not connected to the match loader yet."
            );
            break;
        }
        state.in_lobby = false;
        close_modal();
        // The match loader takes over from here; the chat line turns text
        // input back on when it opens. The engine shows the loading screen
        // and the match without leaving this screen, so it no longer counts
        // as showing, and a close request left for its tick goes with it.
        set_text_input(ctx, false);
        state.showing = false;
        state.exit_confirm_requested = false;
        state.start_handler(state.start_context, lobby);
        break;
    case LobbyAction::select_map:
        if (load_modal(
                ctx,
                ModalKind::selmap,
                "selmap.gui",
                "bitmaps/dselectmap2.pcx",
                "anims/skirmish.gaf"
            ) &&
            !mapselect_open(lobby, state.mapselect, state.modal.panel))
            close_modal();
        break;
    case LobbyAction::view_map:
        if (load_modal(ctx, ModalKind::viewmap, "viewmap.gui", "bitmaps/dviewmap.pcx", kCommonGaf))
            viewmap_open(lobby, state.modal.panel);
        break;
    case LobbyAction::restrictions:
        load_units();
        if (load_modal(
                ctx, ModalKind::restrict, "restrict2.gui", "bitmaps/unitrestrict5x.pcx", kCommonGaf
            ))
            restrict_open(lobby, state.restrict, state.modal.panel);
        break;
    case LobbyAction::confirm_reject:
        if (load_modal(ctx, ModalKind::confirm, "yesorno.gui", nullptr, kCommonGaf))
            confirm_open(lobby, state.modal.panel, lobby.confirm_slot);
        break;
    case LobbyAction::none:
        break;
    }
}

/// Goes on with the chosen provider: TCP.GUI for TCP/IP, the service at once for IPX and the others.
///
/// @param ctx Screen context of the running frontend.
void open_chosen_provider(ScreenContext* ctx) {
    auto& state = ui();
    auto& connect = state.connect;
    const auto kind = provider_kind(connect.providers[connect.provider].guid);
    if (kind == ProviderKind::tcpip) {
        app::screen_request(ctx, kScreenTcp);
    } else if (kind == ProviderKind::ipx || kind == ProviderKind::other) {
        app::screen_request(
            ctx, connect_open_service(state.lobby, connect, "") ? kScreenGameList : kScreenProviders
        );
    } else {
        state.message = "Modem and serial connections are not available in this build.";
    }
}

/// Carries out what the game list asked for.
///
/// @param ctx Screen context of the running frontend.
/// @param action the game list's action
void apply_game_list_action(ScreenContext* ctx, ConnectAction action) {
    auto& state = ui();
    if (action == ConnectAction::providers)
        app::screen_request(ctx, kScreenProviders);
    else if (action == ConnectAction::main_menu)
        return_to_main_menu(ctx);
    else if (action == ConnectAction::leave_game)
        leave_game(true);
    else if (action == ConnectAction::new_game)
        app::screen_request(ctx, kScreenNewGame);
    else if (
        (action == ConnectAction::join || action == ConnectAction::watch) &&
        connect_join(state.lobby, state.connect, action == ConnectAction::watch)
    ) {
        state.in_lobby = false;
        app::screen_request(ctx, kScreenBattleroom);
        if (g_direct.stage == DirectStage::joining)
            g_direct.stage = DirectStage::entering;
        return;
    }
    // A direct join ends where the game list stops waiting without joining.
    if (g_direct.stage == DirectStage::joining && !state.connect.join_pending &&
        !state.connect.host_waiting)
        fail_direct_game("The game could not be joined.");
}

/// Creates the game NEWMULTI accepted and opens the battle room.
///
/// A direct game that reached NEWMULTI is marked entering, including a launch
/// that hosts at once and never types its names through OK. The battle room
/// reports that arrival. A direct game that cannot be created is refused.
///
/// @param ctx Screen context of the running frontend.
void host_new_game(ScreenContext* ctx) {
    auto& state = ui();
    if (connect_host(state.lobby, state.connect)) {
        state.in_lobby = false;
        if (g_direct.stage == DirectStage::new_game)
            g_direct.stage = DirectStage::entering;
        app::screen_request(ctx, kScreenBattleroom);
    } else {
        state.message = "The game could not be created.";
        if (g_direct.stage == DirectStage::new_game)
            fail_direct_game("The game could not be created.");
    }
}

/// Types the direct game's name and password into NEWMULTI and takes OK, as a player does.
///
/// @param ctx Screen context of the running frontend.
void host_direct_game(ScreenContext* ctx) {
    auto& state = ui();
    auto& panel = state.base.panel;
    const auto& game = g_direct.game;
    // A notice still up is dismissed first, as a player's click does.
    state.message.clear();
    retype(panel, "GAMENAME", game.game_name);
    retype(panel, "NICKNAME", game.player_name);
    retype(panel, "PASSWORD", game.password);
    panel.selected = panel_find(panel, "OK");
    if (panel.selected == kNoControl) {
        fail_direct_game("The game could not be created.");
        return;
    }
    if (new_game_handle_event(state.lobby, state.connect, panel) == ConnectAction::host)
        host_new_game(ctx);
    if (state.message.empty())
        g_direct.stage = DirectStage::entering;
    else
        fail_direct_game("The game could not be created.");
}

/// Goes on from the game list as the direct game asks: its names typed, then NEWMULTI to host, or the wait to join.
///
/// @param[in,out] state The screens' state, on the game list.
void continue_direct_game(Ui& state) {
    auto& panel = state.base.panel;
    auto& connect = state.connect;
    const auto& game = g_direct.game;
    retype(panel, "NICKNAME", game.player_name);
    if (game.kind == DirectGame::Kind::host) {
        // The game list's next tick goes on to NEWMULTI, as STARTNEW does.
        connect.update_requested = true;
        g_direct.stage = DirectStage::new_game;
        return;
    }
    retype(panel, "PASSWORD", game.password);
    connect.join_pending = true;
    connect.join_direct = true;
    std::snprintf(
        connect.join_game_name, sizeof connect.join_game_name, "%s", game.game_name.c_str()
    );
    g_direct.stage = DirectStage::joining;
}

void dispatch(ScreenContext* ctx) {
    auto& state = ui();
    auto& lobby = state.lobby;
    auto& connect = state.connect;
    if (state.modal_kind != ModalKind::none) {
        auto& panel = state.modal.panel;
        bool closed = false;
        switch (state.modal_kind) {
        case ModalKind::tcp: {
            const auto action = tcp_handle_event(lobby, connect, panel);
            if (action == ConnectAction::providers) {
                close_modal();
                app::screen_request(ctx, kScreenProviders);
            } else if (action == ConnectAction::game_list) {
                close_modal();
                app::screen_request(ctx, kScreenGameList);
            }
            return;
        }
        case ModalKind::confirm:
            closed = confirm_handle_event(lobby, panel);
            break;
        case ModalKind::viewmap:
            closed = viewmap_handle_event(lobby, panel);
            break;
        case ModalKind::selmap:
            closed = mapselect_handle_event(lobby, state.mapselect, panel);
            break;
        case ModalKind::restrict: {
            const auto action = restrict_handle_event(lobby, state.restrict, panel);
            if (action == RestrictAction::load_list || action == RestrictAction::save_list)
                state.message = "Restriction lists are loaded and saved by the options package.";
            if (action == RestrictAction::close) {
                restrict_close(lobby, state.restrict);
                closed = true;
            }
            break;
        }
        case ModalKind::timeout:
            closed = timeout_handle_event(lobby, panel);
            break;
        case ModalKind::exit_confirm: {
            const auto answer = exit_confirm_handle_event(lobby, panel);
            if (answer == ExitConfirmAnswer::leave)
                leave_game(false);
            closed = answer == ExitConfirmAnswer::closed;
            break;
        }
        case ModalKind::none:
            break;
        }
        if (closed && (state.modal_kind == ModalKind::timeout ||
                       state.modal_kind == ModalKind::exit_confirm)) {
            close_timeout();
        } else if (closed) {
            close_modal();
            lobby.game->gui_flags |= kMenuTickFlag;
        }
        return;
    }
    auto& panel = state.base.panel;
    switch (state.screen) {
    case kScreenProviders: {
        const auto action = providers_handle_event(lobby, connect, panel);
        if (action == ConnectAction::main_menu) {
            release_pack_source();
            app::screen_request(ctx, kScreenMainMenu);
        } else if (action == ConnectAction::options) {
            app::screen_request(ctx, kScreenOptions);
        } else if (action == ConnectAction::provider) {
            open_chosen_provider(ctx);
        }
        break;
    }
    case kScreenGameList:
        apply_game_list_action(ctx, game_list_handle_event(lobby, connect, panel));
        break;
    case kScreenNewGame: {
        const auto action = new_game_handle_event(lobby, connect, panel);
        if (action == ConnectAction::game_list)
            app::screen_request(ctx, kScreenGameList);
        else if (action == ConnectAction::host)
            host_new_game(ctx);
        break;
    }
    case kScreenBattleroom:
        apply_lobby_action(ctx, lobby_handle_event(lobby, panel));
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Screen hooks

/// Says the engine's line in the battle room's chat, as the local player,
/// when it differs from the line said last since the battle room was
/// entered.
///
/// @param[in,out] state The screens' state, in the battle room.
void say_engine_banner(Ui& state) {
    if (state.engine_banner.line == nullptr || state.lobby.game == nullptr)
        return;
    std::string line = state.engine_banner.line(state.engine_banner.context);
    if (line.empty() || line == state.banner_said)
        return;
    const Player& me = local_player(state.lobby);
    if (me.player_id == 0)
        return;
    lobby_say(state.lobby, me, line.c_str());
    state.banner_said = std::move(line);
}

/// Binds the frontend to a screen being entered and turns text input on for its text boxes.
///
/// @param ctx Screen context of the running frontend.
void begin(ScreenContext* ctx) {
    remember_screen(ui(), ctx);
    ui().showing = true;
    bind_boundaries();
    set_text_input(ctx, true);
}

void enter_providers(ScreenContext* ctx, void*) {
    begin(ctx);
    auto& state = ui();
    state.screen = kScreenProviders;
    close_modal();
    if (load(state.base, ctx, "selprov.gui", "bitmaps/selconnect2.pcx", "anims/selprov.gaf")) {
        providers_open(state.lobby, state.connect, state.base.panel);
        oa::ui::gui_input::mark_label_shadows(state.base.source.gadgets);
        // A launch names its transport: the selection goes on with it.
        if (providers_take_launch(state.lobby, state.connect))
            open_chosen_provider(ctx);
    }
}

void enter_tcp(ScreenContext* ctx, void*) {
    begin(ctx);
    auto& state = ui();
    state.screen = kScreenTcp;
    if (!state.base.loaded || state.base.panel.name[0] == '\0' ||
        std::string_view(state.base.panel.name.data()) != oa::data::defs::gui_path("selprov.gui")) {
        if (load(state.base, ctx, "selprov.gui", "bitmaps/selconnect2.pcx", "anims/selprov.gaf")) {
            providers_open(state.lobby, state.connect, state.base.panel);
            oa::ui::gui_input::mark_label_shadows(state.base.source.gadgets);
        }
    }
    if (!load_modal(ctx, ModalKind::tcp, "tcp.gui", nullptr, "anims/selprov.gaf"))
        return;
    tcp_open(state.lobby, state.connect, state.modal.panel);
    if (g_direct.stage == DirectStage::tcp) {
        // The address is typed and accepted at once; a host keeps the
        // address the box shows, as a player who only presses OK does.
        retype(state.modal.panel, "ADDRESS", g_direct.game.address);
        const auto action = tcp_accept_direct(state.lobby, state.connect, state.modal.panel);
        close_modal();
        if (action == ConnectAction::game_list) {
            g_direct.stage = DirectStage::game_list;
        } else {
            state.message = state.connect.error_text;
            state.connect.error_text[0] = '\0';
            fail_direct_game(kServiceErrorText);
        }
        app::screen_request(
            ctx, action == ConnectAction::game_list ? kScreenGameList : kScreenProviders
        );
        return;
    }
    if (!state.connect.launch_address_used)
        return;
    // The launch's address is accepted at once; while a launch is
    // active ENDMULTI.GUI stands over the panel until the game list opens.
    const auto action = tcp_accept_launch_address(state.lobby, state.connect, state.modal.panel);
    if (lobby_launch_active(state.lobby)) {
        close_modal();
        (void)load_modal(ctx, ModalKind::tcp, "endmulti.gui", nullptr, "anims/selprov.gaf");
    }
    app::screen_request(
        ctx, action == ConnectAction::game_list ? kScreenGameList : kScreenProviders
    );
}

void enter_game_list(ScreenContext* ctx, void*) {
    begin(ctx);
    auto& state = ui();
    state.screen = kScreenGameList;
    close_modal();
    if (!load(state.base, ctx, "selgame.gui", "bitmaps/selectgame2x.pcx", "anims/selgame.gaf"))
        return;
    // A direct join the host refused in its battle room comes back here,
    // where the game list tells why. So does a join that left a 3.1c host
    // of a mod only Open Annihilation can play. The game list clears the
    // line, so the direct join keeps its own copy.
    std::string left_because;
    if (state.lobby.leave_line != nullptr)
        left_because.assign(oa::data::languages::interface_text(state.lobby.leave_line));
    const auto reason =
        state.lobby.game != nullptr ? local_player(state.lobby).reject_reason : uint8_t{0};
    const bool refused = g_direct.progress.reached == DirectGameStep::battle_room &&
                         g_direct.game.kind == DirectGame::Kind::join && refuses_arrival(reason);
    const bool left_wide = !left_because.empty() && g_direct.game.kind == DirectGame::Kind::join &&
                           g_direct.progress.reached == DirectGameStep::battle_room;
    if (!game_list_open(state.lobby, state.connect, state.base.panel)) {
        if (g_direct.stage == DirectStage::game_list)
            fail_direct_game(left_wide ? left_because.c_str() : "Invalid TCP/IP Address");
        app::screen_request(ctx, kScreenProviders);
        return;
    }
    oa::ui::gui_input::mark_label_shadows(state.base.source.gadgets);
    connect_show_error_text(state.lobby, state.connect);
    if (g_direct.stage == DirectStage::game_list)
        continue_direct_game(state);
    else if (left_wide)
        fail_direct_game(left_because.c_str());
    else if (refused)
        fail_direct_game(reject_reason_text(reason));
}

void enter_new_game(ScreenContext* ctx, void*) {
    begin(ctx);
    auto& state = ui();
    state.screen = kScreenNewGame;
    close_modal();
    if (load(state.base, ctx, "newmulti.gui", "bitmaps/createnew.pcx", "anims/selgame.gaf")) {
        new_game_open(state.lobby, state.connect, state.base.panel);
        oa::ui::gui_input::mark_label_shadows(state.base.source.gadgets);
        if (state.connect.host_at_once)
            host_new_game(ctx);
        else if (g_direct.stage == DirectStage::new_game)
            host_direct_game(ctx);
    }
}

void enter_battleroom(ScreenContext* ctx, void*) {
    begin(ctx);
    auto& state = ui();
    state.screen = kScreenBattleroom;
    close_modal();
    if (!load(state.base, ctx, "lounge2.gui", "bitmaps/battleroom.pcx", "anims/lounge2.gaf"))
        return;
    load_maps();
    // A screen left on the way here unmounted the pack map. Mount it again
    // before the battle room hashes the map it plays.
    if (state.map >= 0 && static_cast<std::size_t>(state.map) < state.maps.size()) {
        const std::string selected = state.maps[static_cast<std::size_t>(state.map)].name;
        (void)maps_select(nullptr, selected.c_str());
    }
    load_units();
    local_info(state.lobby).memory_mb = kMachineMemoryMb;
    lobby_enter_battleroom(state.lobby, state.base.panel);
    oa::ui::gui_input::mark_label_shadows(state.base.source.gadgets);
    state.in_lobby = true;
    // The engine's line is said again in each battle room entered.
    state.banner_said.clear();
    if (g_direct.stage == DirectStage::entering) {
        auto& game = *state.game;
        const bool joined = g_direct.game.kind == DirectGame::Kind::join;
        const char* name = joined ? state.connect.chosen.name : lobby_game_name(game);
        std::string shown(name, ::strnlen(name, kSessionGameNameBytes));
        while (!shown.empty() && shown.back() == ' ')
            shown.pop_back();
        const char* nickname = lobby_nickname(game);
        g_direct.stage = DirectStage::none;
        g_direct.progress.reached = DirectGameStep::battle_room;
        g_direct.progress.game_name = std::move(shown);
        g_direct.progress.player_name.assign(nickname, ::strnlen(nickname, sizeof(Game::nickname)));
    }
}

/// Closes any stacked dialog and turns text input off as a multiplayer screen is left.
///
/// The next multiplayer screen turns it on again as it is entered.
///
/// @param ctx Screen context of the running frontend.
/// @param state Screen state (unused).
void leave_screen(ScreenContext* ctx, void* /*state*/) {
    close_modal();
    set_text_input(ctx, false);
    release_pack_source();
    ui().showing = false;
    ui().exit_confirm_requested = false;
}

/// Names the active button whose quick key a key is, in either case.
///
/// A grayed-out button takes no key; a key with Ctrl, Alt or the system key
/// down types no character and names none.
///
/// @param panel the panel in front
/// @param input the key press
/// @return the button's name, or empty for none
std::string quick_key_button(const Panel& panel, const ScreenInput& input) {
    if ((input.modifiers & (oa::app::kInputModifierCtrl | oa::app::kInputModifierAlt |
                            oa::app::kInputModifierSystem)) != 0 ||
        input.key == 0 || input.key >= 0x80)
        return {};
    const auto typed = std::tolower(static_cast<int>(input.key));
    const auto count = std::min<int32_t>(panel.count, static_cast<int32_t>(kPanelControls));
    for (int32_t index = 1; index < count; ++index) {
        const auto& control = panel.controls[static_cast<std::size_t>(index)];
        if (control.type != ControlType::button || control.active == 0 || control.grayed ||
            control.quick_key == 0 ||
            std::tolower(static_cast<unsigned char>(control.quick_key)) != typed)
            continue;
        return std::string(control_name(control));
    }
    return {};
}

/// Tells whether typed text is the character of the quick key that answered a question.
///
/// @param text the typed text, UTF-8
/// @param answered the quick key, lowercase, or zero for none
/// @return true for that one character, in either case
bool typed_answered_key(const char* text, int answered) {
    return answered != 0 && text != nullptr && text[0] != '\0' && text[1] == '\0' &&
           std::tolower(static_cast<unsigned char>(text[0])) == answered;
}

namespace {

/// Returns the focused text box of the front panel.
///
/// @return the box; null when no text box has the focus
Control* focused_text_box() noexcept {
    auto& panel = front().panel;
    if (panel.focus <= 0 || panel.focus >= panel.count)
        return nullptr;
    auto& control = panel.controls[static_cast<std::size_t>(panel.focus)];
    return control.type == ControlType::text_box ? &control : nullptr;
}

/// Tells whether a text box is the battle room's chat line, which takes
/// every character as game text. The other boxes (the address, names and
/// passwords) take printable ASCII alone.
///
/// @param control a text box
/// @return true for the chat line
bool takes_any_character(const Control& control) noexcept {
    return control_name(control) == "MESSAGE";
}

/// Adds typed text to the chat line as game text, whole characters at a
/// time, while the line stays within its limit.
///
/// @param[in,out] value the line's game text
/// @param typed UTF-8, as the platform sends it
/// @param limit the most bytes the line holds
void take_game_text(std::string& value, std::string_view typed, std::size_t limit) {
    const bool utf8 = oa::present::game_text_settings().utf8;
    const std::string taken =
        oa::present::typed_characters(typed, oa::present::TypedCharacters::text);
    for (std::size_t at = 0; at < taken.size();) {
        const std::size_t bytes =
            std::max<std::size_t>(oa::present::utf8_sequence(taken.substr(at)).bytes, 1);
        const std::string game = oa::present::encode_game_text(taken.substr(at, bytes), utf8);
        if (value.size() + game.size() > limit)
            break;
        value += game;
        at += bytes;
    }
}

/// Takes the input method's composition away from the end of a text box.
///
/// @param[in,out] value the box's text
void drop_composition(std::string& value) {
    auto& composition = ui().composition;
    if (!composition.empty() && value.ends_with(composition))
        value.resize(value.size() - composition.size());
    composition.clear();
}

/// Returns the slot a PLAYER<slot> control names.
///
/// @param name the control's name
/// @return the slot, 0..9; -1 when the name is not a player's row
int32_t player_row_slot(std::string_view name) noexcept {
    constexpr std::string_view row_name = "PLAYER";
    if (name.size() != row_name.size() + 1 || !name.starts_with(row_name))
        return -1;
    const char digit = name.back();
    if (digit < '0' || digit > '9')
        return -1;
    const auto slot = static_cast<int32_t>(digit - '0');
    return slot < kSlotCount ? slot : -1;
}

/// Tells whether a list takes a place in the battle room's focus order.
///
/// @param control the control
/// @return true for a shown list a key can select
bool focusable_list(const Control& control) noexcept {
    return control.active != 0 && control.type == ControlType::list_box &&
           (control.attributes & ui::gui_layout::attribute::no_focus) == 0 &&
           (control.attributes & ui::gui_layout::attribute::cycle_frames) == 0;
}

/// Tells whether a remote player's row takes a place in the focus order.
///
/// The local player's row is a label and takes none. An empty slot takes
/// none: it is not another player's row.
///
/// @param lobby the battle room
/// @param control the control
/// @return true for a shown row of a remote or computer player
bool focusable_player_row(Lobby& lobby, const Control& control) noexcept {
    if (control.active == 0 || control.grayed || control.type != ControlType::button)
        return false;
    if ((control.attributes & ui::gui_layout::attribute::no_focus) != 0)
        return false;
    const int32_t slot = player_row_slot(control_name(control));
    if (slot < 0)
        return false;
    const Player& player = slot_player(lobby, slot);
    return slot_active(player) && player.status != kSlotLocal;
}

/// The battle room's focus order: its lists, then each remote player's row
/// in slot order, then the chat line.
struct FocusOrder {
    std::array<int32_t, kPanelControls> index{}; ///< control indexes
    int32_t count = 0;                           ///< how many indexes are set
};

/// Builds the battle room's focus order.
///
/// @param panel the battle room's panel
/// @param lobby the battle room
/// @return the order, lists then remote rows then the chat line
FocusOrder battle_room_focus_order(const Panel& panel, Lobby& lobby) noexcept {
    FocusOrder order;
    const auto add = [&](int32_t index) {
        if (order.count < static_cast<int32_t>(order.index.size()))
            order.index[static_cast<std::size_t>(order.count++)] = index;
    };
    for (int32_t index = 1; index < panel.count; ++index)
        if (focusable_list(panel.controls[static_cast<std::size_t>(index)]))
            add(index);
    std::array<int32_t, kSlotCount> player_at{};
    player_at.fill(kNoControl);
    for (int32_t index = 1; index < panel.count; ++index) {
        const auto& control = panel.controls[static_cast<std::size_t>(index)];
        if (!focusable_player_row(lobby, control))
            continue;
        const int32_t slot = player_row_slot(control_name(control));
        if (player_at[static_cast<std::size_t>(slot)] == kNoControl)
            player_at[static_cast<std::size_t>(slot)] = index;
    }
    for (int32_t slot = 0; slot < kSlotCount; ++slot)
        if (player_at[static_cast<std::size_t>(slot)] != kNoControl)
            add(player_at[static_cast<std::size_t>(slot)]);
    for (int32_t index = 1; index < panel.count; ++index) {
        const auto& control = panel.controls[static_cast<std::size_t>(index)];
        if (control.active != 0 && takes_any_character(control) &&
            (control.attributes & ui::gui_layout::attribute::no_focus) == 0)
            add(index);
    }
    return order;
}

/// Moves the battle room's focus one place along its order, wrapping.
///
/// @param[in,out] panel the battle room's panel; its focus moves
/// @param forward true toward the chat line, false back toward the lists
void move_battle_room_focus(Panel& panel, bool forward) noexcept {
    const FocusOrder order = battle_room_focus_order(panel, ui().lobby);
    if (order.count == 0)
        return;
    int32_t place = -1;
    for (int32_t index = 0; index < order.count; ++index) {
        if (order.index[static_cast<std::size_t>(index)] == panel.focus) {
            place = index;
            break;
        }
    }
    int32_t next = forward ? 0 : order.count - 1;
    if (place >= 0)
        next = forward ? (place + 1) % order.count : (place + order.count - 1) % order.count;
    const int32_t chosen = order.index[static_cast<std::size_t>(next)];
    if (panel.focus == chosen)
        return;
    panel.focus = chosen;
    panel.dirty = true;
}

/// Tells whether the battle room itself is the screen taking keys.
///
/// @return true while it shows and no dialog covers it
bool battle_room_keys() noexcept {
    const auto& state = ui();
    return state.screen == kScreenBattleroom && state.base.loaded && &front() == &state.base;
}

/// The card trigger Return and the pad's A use while a remote row is focused.
/// The presence card screen binds its own; null opens no card.
PresenceCardTrigger* row_key_trigger = nullptr;

} // namespace

int screen_event(ScreenContext* ctx, void*) {
    auto& state = ui();
    remember_screen(state, ctx);
    const auto* input = ctx->input;
    if (input == nullptr)
        return 0;
    // The wait for the host, and the "Host not found" text that ends it,
    // take every input while no exit confirmation is up.
    if (state.screen == kScreenGameList &&
        (state.connect.host_waiting || state.connect.host_not_found_exiting) &&
        state.modal_kind != ModalKind::exit_confirm)
        return 1;
    auto& res = front();
    auto& panel = res.panel;
    const auto px = static_cast<int32_t>(input->x) - res.offset_x;
    const auto py = static_cast<int32_t>(input->y) - res.offset_y;
    // Declared here: a variable inside one case cannot be jumped over by another.
    const bool focus_valid = panel.focus > 0 && panel.focus < panel.count;
    switch (input->kind) {
    case ScreenInputKind::pointer_move:
        res.hovered = panel_hit(panel, px, py);
        oa::ui::gui_input::scroll_move(res.hold, px, py);
        return 1;
    case ScreenInputKind::pointer_down:
        if (!state.message.empty())
            return 1;
        res.pressed = panel_hit(panel, px, py);
        res.hovered = res.pressed;
        // A press anywhere but on a list, including on no control, ends a
        // pair of list presses.
        if (res.pressed == kNoControl ||
            panel.controls[static_cast<std::size_t>(res.pressed)].type != ControlType::list_box)
            ui().last_click_index = kNoControl;
        notify_slider(ctx, res, slider_press(panel, res.hold, res.pressed, px, py, ui_tick()));
        return 1;
    case ScreenInputKind::pointer_up: {
        const auto pressed = res.pressed;
        res.pressed = kNoControl;
        // A held slider or arrow takes the release; an arrow is never clicked.
        if (res.hold.bar != oa::ui::gui_input::kNoScrollBar) {
            notify_slider(ctx, res, slider_release(panel, res.hold, px, py));
            return 1;
        }
        if (!state.message.empty()) {
            state.message.clear();
            return 1;
        }
        const auto hit = panel_hit(panel, px, py);
        if (hit != kNoControl && hit == pressed)
            (void)click(ctx, hit, px, py, input->button == kSdlRightButton ? 2 : 1);
        return 1;
    }
    case ScreenInputKind::wheel:
        if (state.message.empty() && input->wheel_y != 0)
            wheel_list(ctx, res, panel_hit(panel, px, py), input->wheel_y > 0);
        return 1;
    case ScreenInputKind::text:
        // The character a quick key types after its press answered a
        // question goes nowhere.
        if (typed_answered_key(input->text, std::exchange(state.answered_key, 0)))
            return 1;
        if (input->text != nullptr)
            multiplayer_type(ctx, input->text);
        return 1;
    case ScreenInputKind::key_down:
        state.answered_key = 0;
        if (!state.message.empty()) {
            if (input->key == kSdlKeyReturn || input->key == kSdlKeyEscape)
                state.message.clear();
            return 1;
        }
        // The yes-or-no questions take their buttons' quick keys, in either
        // case: Y and N in English. A key with Ctrl, Alt or the system key
        // down types no character.
        if (state.modal_kind == ModalKind::confirm || state.modal_kind == ModalKind::exit_confirm) {
            if (const auto name = quick_key_button(panel, *input); !name.empty()) {
                state.answered_key = std::tolower(static_cast<int>(input->key));
                (void)multiplayer_click(ctx, name.c_str());
                return 1;
            }
        }
        if (input->key == kSdlKeyBackspace && panel.focus > 0 && panel.focus < panel.count) {
            // Backspace takes the last character away from a text box: a
            // whole UTF-8 sequence of the chat line where game text holds
            // UTF-8, else one byte. A focused player's row is not a text box.
            auto& control = panel.controls[static_cast<std::size_t>(panel.focus)];
            if (control.type == ControlType::text_box) {
                std::string text(control_text(control));
                if (takes_any_character(control) && oa::present::game_text_settings().utf8)
                    std::ignore = oa::present::erase_last_character(text);
                else if (!text.empty())
                    text.pop_back();
                set_control_text(control, text);
            }
            return 1;
        }
        if (input->key == kSdlKeyReturn && focus_valid) {
            // A focused remote row takes Return and the pad's A as the
            // information key. The card opens when that row shows an OA
            // badge, and nothing happens otherwise. The row is not pressed,
            // so a player is not rejected and an AI is not cycled. A click
            // on the row still presses it.
            const auto& focused = panel.controls[static_cast<std::size_t>(panel.focus)];
            if (battle_room_keys() && focusable_player_row(state.lobby, focused)) {
                if (row_key_trigger != nullptr) {
                    const int32_t slot = player_row_slot(control_name(focused));
                    PresenceBadgeSpot spots[kSlotCount]{};
                    const auto count = multiplayer_presence_badges(spots, kSlotCount);
                    for (int32_t index = 0; index < count; ++index) {
                        if (spots[static_cast<std::size_t>(index)].slot == slot) {
                            row_key_trigger->key_info(slot);
                            break;
                        }
                    }
                }
                return 1;
            }
            if (panel_press(panel, panel.focus))
                dispatch(ctx);
            return 1;
        }
        // Up and Down move a focused list's selection a row and run its
        // handler for a new selection, as 3.1c does; they never click it.
        if ((input->key == kSdlKeyUp || input->key == kSdlKeyDown) && focus_valid) {
            const auto& focused = panel.controls[static_cast<std::size_t>(panel.focus)];
            if (focused.type == ControlType::list_box && focused.list_item_height != 0) {
                panel_step_list(panel, panel.focus, input->key == kSdlKeyDown);
                list_changed(res, panel.focus);
                return 1;
            }
        }
        // In the battle room, Tab and the pad's D-pad (the arrow keys) walk
        // the focus order: the lists, then each remote player's row, then
        // the chat line. Left and Right stay in the chat line. Up and Down
        // stay in a list, where they move its selection.
        if (battle_room_keys() &&
            (input->key == kSdlKeyTab || input->key == kSdlKeyUp || input->key == kSdlKeyDown ||
             input->key == kSdlKeyLeft || input->key == kSdlKeyRight)) {
            const bool along_chat =
                (input->key == kSdlKeyLeft || input->key == kSdlKeyRight) && focus_valid &&
                takes_any_character(panel.controls[static_cast<std::size_t>(panel.focus)]);
            if (!along_chat) {
                const bool backward =
                    input->key == kSdlKeyUp || input->key == kSdlKeyLeft ||
                    (input->key == kSdlKeyTab && (input->modifiers & kSdlModifierShift) != 0);
                move_battle_room_focus(panel, !backward);
            }
            return 1;
        }
        if ((input->key == kSdlKeyUp || input->key == kSdlKeyDown) && focus_valid)
            return 1;
        if (input->key == kSdlKeyEscape) {
            for (const char* name : {"PREVMENU", "PREV", "CANCEL", "Cancel", "CHOICE2", "OK"})
                if (panel_find(panel, name) != kNoControl) {
                    (void)multiplayer_click(ctx, name);
                    break;
                }
        }
        return 1;
    default:
        return 1;
    }
}

void screen_tick(ScreenContext* ctx, void*) {
    auto& state = ui();
    remember_screen(state, ctx);
    if (state.exit_confirm_requested) {
        state.exit_confirm_requested = false;
        open_exit_confirm(ctx);
    }
    if (auto& res = front(); res.loaded)
        notify_slider(ctx, res, slider_hold_tick(res.panel, res.hold, ui_tick()));
    if (state.screen == kScreenGameList && state.base.loaded &&
        state.modal_kind == ModalKind::none) {
        apply_game_list_action(ctx, game_list_tick(state.lobby, state.connect, state.base.panel));
        return;
    }
    if (state.screen != kScreenBattleroom || !state.in_lobby)
        return;
    auto& lobby = state.lobby;
    say_engine_banner(state);
    if (state.modal_kind == ModalKind::restrict)
        restrict_tick(lobby, state.restrict, state.modal.panel);
    if (state.modal_kind == ModalKind::timeout && timeout_tick(lobby, state.modal.panel))
        close_timeout();
    const auto lobby_front = state.modal_kind == ModalKind::none      ? LobbyFront::battleroom
                             : state.modal_kind == ModalKind::viewmap ? LobbyFront::view_map
                                                                      : LobbyFront::dialog;
    apply_lobby_action(
        ctx,
        lobby_tick(
            lobby,
            state.base.panel,
            lobby_front,
            lobby_front == LobbyFront::view_map ? &state.modal.panel : nullptr
        )
    );
    // TIMEOUT.GUI follows the pump's stall scan: it opens for the player the
    // scan names unless it is already open, and closes once the scan names
    // no one.
    if (state.in_lobby) {
        const auto stalled = lobby.stalled_player;
        const bool open = state.modal_kind == ModalKind::timeout;
        if (open && stalled == 0)
            close_timeout();
        else if (!open && stalled != 0 && slot_for_player_id(lobby, stalled) >= 0)
            open_timeout(ctx, stalled);
    }
    // A client leaves the battle room when the host's 0x08 arrives.
    if (state.in_lobby && state.start_handler != nullptr && lobby.game != nullptr &&
        (local_info(lobby).role & kRoleHost) == 0 &&
        (local_info(lobby).options & option::started) != 0)
        apply_lobby_action(ctx, LobbyAction::start);
}

void screen_draw(ScreenContext* ctx, void*) {
    auto& state = ui();
    remember_screen(state, ctx);
    if (ctx->surface == nullptr || !state.base.loaded)
        return;
    auto& res = front();
    auto surface = render(res);
    draw_pictures(surface, res);
    draw_sliders(surface, res);
    if (&res == &state.base && state.screen == kScreenBattleroom) {
        draw_player_versions(surface, res, state.lobby);
        draw_presence_badges(surface, res, state.lobby);
    }
    if (&res != &state.base)
        draw_text_boxes(surface, state.base);
    draw_text_boxes(surface, res);
    draw_message(surface, res);
    *ctx->surface = std::move(surface);
}

void add(app::ScreenRegistry* registry, app::ScreenId id, const char* name, app::ScreenFn enter) {
    app::ScreenDesc desc{};
    desc.id = id;
    desc.name = name;
    desc.enter = enter;
    desc.leave = leave_screen;
    desc.event = screen_event;
    desc.tick = screen_tick;
    desc.draw = screen_draw;
    (void)app::screen_register(registry, &desc);
}

/// The place of one control in the battle room's focus order.
///
/// @param panel the battle room's panel
/// @param lobby the battle room
/// @param index the control
/// @return its place, or -1 when the order leaves it out
int32_t battle_room_focus_place(const Panel& panel, Lobby& lobby, int32_t index) noexcept {
    const FocusOrder order = battle_room_focus_order(panel, lobby);
    for (int32_t at = 0; at < order.count; ++at)
        if (order.index[static_cast<std::size_t>(at)] == index)
            return at;
    return -1;
}

} // namespace

void multiplayer_bind_launch_link(const LaunchLink& link) noexcept {
    ui().launch_link = link;
    ui().lobby.launch_link = link;
}

void multiplayer_bind_rules(const data::match_rules::MatchRules* rules) noexcept {
    ui().rules = rules;
    ui().lobby.rules = rules;
}

void multiplayer_bind_engine_banner(const EngineBanner& banner) noexcept {
    ui().engine_banner = banner;
}

std::string
engine_banner_line(std::string_view version, bool developer_mode, bool remote_controlled) {
    std::string line = "[Engine: OpenAnnihilation ";
    line += version;
    if (developer_mode)
        line += " DEV MODE";
    if (remote_controlled)
        line += " REMOTED";
    line += ']';
    return line;
}

void multiplayer_bind_translation(const TextTranslation& translation) noexcept {
    ui().translation = translation;
}

void multiplayer_bind_side_count(int32_t side_count) noexcept {
    ui().side_count = side_count > 0 ? side_count : 0;
}

void multiplayer_bind_side_names(std::span<const std::string> names) noexcept {
    auto& state = ui();
    state.side_names.assign(names.begin(), names.end());
    state.lobby.side_names = state.side_names;
}

bool multiplayer_showing() noexcept {
    return ui().showing;
}

bool multiplayer_request_exit_confirm() noexcept {
    auto& state = ui();
    if (!state.showing)
        return false;
    state.exit_confirm_requested = true;
    return true;
}

void multiplayer_bind_start(StartHandler handler, void* context) noexcept {
    ui().start_handler = handler;
    ui().start_context = context;
}

void multiplayer_bind_net(const LobbyNet& net) noexcept {
    ui().bound_net = net;
    ui().custom_net = true;
    ui().lobby.net = net;
}

void multiplayer_bind_unicode_chat(bool on) noexcept {
    ui().unicode_chat = on;
    ui().lobby.unicode_chat = on;
}

void multiplayer_bind_chat_measure(const ChatMeasure& measure) noexcept {
    ui().chat_measure = measure;
    ui().lobby.chat_measure = measure;
}

void multiplayer_bind_presence_record(const uint8_t* record, std::size_t size) noexcept {
    auto& state = ui();
    if (record == nullptr || size > netgame::presence_record_max_bytes)
        size = 0;
    state.presence_record.assign(record, record + size);
    take_presence_record(state.lobby, state.presence_record);
}

void multiplayer_bind_mutator_loadout(const netgame::PresenceMutators* loadout) noexcept {
    auto& records = ui().lobby.presence_records;
    if (loadout == nullptr)
        records.part_source.reset();
    else
        records.part_source = *loadout;
}

void multiplayer_units_changed() {
    auto& state = ui();
    state.units = UnitStore{};
    if (state.assets != nullptr) {
        load_units();
    } else {
        state.lobby.units = nullptr;
        state.lobby.unit_count = 0;
    }
    unit_sync_rebuild(state.lobby);
}

void multiplayer_regreet(uint32_t player_id) {
    unit_sync_regreet(ui().lobby, player_id);
}

bool multiplayer_room_mutators(netgame::PresenceLoadoutAssembly* out) {
    auto& lobby = ui().lobby;
    if (lobby.game != nullptr && (local_info(lobby).role & kRoleHost) != 0)
        return false;
    const auto& assembly = lobby.presence_records.host_loadout;
    if (assembly.total == 0)
        return false;
    if (out != nullptr)
        *out = assembly;
    return true;
}

uint32_t multiplayer_room_mutators_generation() noexcept {
    return ui().lobby.presence_records.host_loadout_generation;
}

void multiplayer_bind_map_pack(
    void* context, bool (*pack)(void* context, netgame::PresenceMapPack* out)
) noexcept {
    auto& state = ui();
    state.pack_context = context;
    state.pack = pack;
    state.lobby.maps.pack_context = context;
    state.lobby.maps.pack = pack;
    state.lobby.presence_records.pack_rebound = true;
}

void multiplayer_bind_wire_rules(const netgame::WireRules& rules, const char* program) noexcept {
    auto& state = ui();
    state.wire_rules = rules;
    state.program_line = program != nullptr ? program : "";
    state.lobby.wire_rules = rules;
    state.lobby.local_version_major = rules.version_major;
    state.lobby.local_version_minor = rules.version_minor;
    std::snprintf(
        state.lobby.recorder.program,
        sizeof state.lobby.recorder.program,
        "%s",
        state.program_line.c_str()
    );
}

void multiplayer_bind_player_timeout(int32_t seconds) noexcept {
    auto& state = ui();
    state.player_timeout_seconds = seconds > 0 ? seconds : kDefaultPlayerTimeoutSeconds;
    state.game->player_timeout_seconds = state.player_timeout_seconds;
}

void multiplayer_bind_lobby_buttons(uint8_t buttons) noexcept {
    auto& state = ui();
    state.lobby_buttons = buttons;
    state.lobby.lobby_buttons = buttons;
}

Lobby& multiplayer_lobby() noexcept {
    return ui().lobby;
}

LoopbackNet& multiplayer_loopback() noexcept {
    return ui().loopback;
}

ConnectState& multiplayer_connect() noexcept {
    return ui().connect;
}

Panel& multiplayer_panel() noexcept {
    return front().panel;
}

PanelOffset multiplayer_panel_offset() noexcept {
    const auto& res = front();
    return {res.offset_x, res.offset_y};
}

int32_t multiplayer_presence_badges(PresenceBadgeSpot* out, int32_t capacity) noexcept {
    auto& state = ui();
    // Drawn only as screen_draw draws them: the battle room shows, with no
    // dialog over it.
    if (!state.showing || state.screen != kScreenBattleroom || !state.base.loaded ||
        &front() != &state.base)
        return 0;
    return presence_badge_spots(state.base, state.lobby, out, capacity);
}

bool multiplayer_map_list_rect(int32_t* x, int32_t* y, int32_t* width, int32_t* height) noexcept {
    auto& state = ui();
    if (!state.showing || state.screen != kScreenBattleroom ||
        state.modal_kind != ModalKind::selmap || !state.modal.loaded ||
        (local_info(state.lobby).role & kRoleHost) == 0)
        return false;
    const auto* list = panel_control(state.modal.panel, "MAPNAMES");
    if (list == nullptr || list->width <= 0 || list->height <= 0)
        return false;
    if (x != nullptr)
        *x = list->x + state.modal.offset_x;
    if (y != nullptr)
        *y = list->y + state.modal.offset_y;
    if (width != nullptr)
        *width = list->width;
    if (height != nullptr)
        *height = list->height;
    return true;
}

namespace {

/// A point on the canvas, in a rectangle whose right and bottom edges are open.
///
/// @param x the point's column
/// @param y the point's row
/// @param left the rectangle's left column
/// @param top the rectangle's top row
/// @param width the rectangle's width
/// @param height the rectangle's height
/// @return true when the point lies inside
bool point_inside(
    int32_t x, int32_t y, int32_t left, int32_t top, int32_t width, int32_t height
) noexcept {
    return width > 0 && height > 0 && x >= left && y >= top && x < left + width && y < top + height;
}

/// The list a canvas point's scroll arrow pages, and which way.
struct PageArrow {
    int32_t list = kNoControl; ///< the list; kNoControl when the point pages none
    bool toward_end = false;   ///< the arrow toward the list's end, rather than its start
};

/// Finds the shown list whose scroll arrow contains a canvas point.
///
/// A bar that is hidden, a list with no row pitch and a point off both arrows
/// page nothing. The frontmost bar wins, as a click does.
///
/// @param x the point's column on the canvas
/// @param y the point's row on the canvas
/// @return the list and the way; the list is kNoControl when there is none
PageArrow page_arrow_at(int32_t x, int32_t y) noexcept {
    PageArrow found;
    if (!ui().showing)
        return found;
    auto& shown = front();
    auto& panel = shown.panel;
    for (int32_t index = panel.count - 1; index >= 1; --index) {
        const auto& slider = panel.controls[static_cast<std::size_t>(index)];
        if (slider.type != ControlType::slider || slider.active == 0 ||
            slider.scroll.art == oa::ui::gui_input::ScrollArt::unbound)
            continue;
        const auto list = panel_group_member(panel, index, ControlType::list_box);
        if (list == kNoControl)
            continue;
        const auto& rows = panel.controls[static_cast<std::size_t>(list)];
        if (rows.list_item_height <= 0)
            continue;
        const int32_t left = shown.offset_x;
        const int32_t top = shown.offset_y;
        const auto& back = slider.scroll.back_arrow;
        const auto& forward = slider.scroll.forward_arrow;
        const bool on_back =
            point_inside(x, y, back.x + left, back.y + top, back.width, back.height);
        const bool on_forward =
            point_inside(x, y, forward.x + left, forward.y + top, forward.width, forward.height);
        if (!on_back && !on_forward)
            continue;
        found.list = list;
        found.toward_end = on_forward && !on_back;
        return found;
    }
    return found;
}

} // namespace

bool multiplayer_list_page_arrow(int32_t x, int32_t y) noexcept {
    return page_arrow_at(x, y).list != kNoControl;
}

bool multiplayer_scroll_list_page(int32_t x, int32_t y) noexcept {
    const PageArrow arrow = page_arrow_at(x, y);
    if (arrow.list == kNoControl)
        return false;
    auto& panel = front().panel;
    const auto& rows = panel.controls[static_cast<std::size_t>(arrow.list)];
    const int32_t page = list_shown_rows(rows, panel.line_height);
    return panel_scroll_list(panel, arrow.list, arrow.toward_end ? page : -page);
}

bool multiplayer_offer_map(const char* name) noexcept {
    auto& state = ui();
    if (name == nullptr || !multiplayer_map_list_rect(nullptr, nullptr, nullptr, nullptr))
        return false;
    const std::string_view wanted(name);
    const auto listed = [&state, wanted] {
        return std::any_of(state.maps.begin(), state.maps.end(), [wanted](const MapInfo& map) {
            return map.name == wanted;
        });
    };
    if (!listed()) {
        // The list is built once a visit: build it again, so that a pack
        // installed since joins it, keeping the map selected now.
        const std::string current =
            state.map >= 0 && static_cast<std::size_t>(state.map) < state.maps.size()
                ? state.maps[static_cast<std::size_t>(state.map)].name
                : std::string();
        state.maps_loaded = false;
        load_maps();
        for (std::size_t index = 0; index < state.maps.size(); ++index)
            if (state.maps[index].name == current)
                state.map = static_cast<int32_t>(index);
        if (!listed())
            return false;
    }
    return mapselect_offer(state.lobby, state.mapselect, state.modal.panel, wanted);
}

void multiplayer_reload_maps() {
    auto& state = ui();
    if (!state.maps_loaded)
        return;
    const std::string selected =
        state.map >= 0 && static_cast<std::size_t>(state.map) < state.maps.size()
            ? state.maps[static_cast<std::size_t>(state.map)].name
            : std::string();
    std::string highlighted;
    if (state.modal_kind == ModalKind::selmap && state.modal.loaded) {
        if (const Control* list = panel_control(state.modal.panel, "MAPNAMES")) {
            const int32_t row = list->list_selection;
            if (row >= 0 && static_cast<std::size_t>(row) < state.mapselect.names.size())
                highlighted = state.mapselect.names[static_cast<std::size_t>(row)];
        }
    }
    state.maps.clear();
    state.maps_loaded = false;
    state.map = -1;
    load_maps();
    for (std::size_t index = 0; index < state.maps.size() && !selected.empty(); ++index)
        if (state.maps[index].name == selected)
            state.map = static_cast<int32_t>(index);
    if (state.modal_kind != ModalKind::selmap || !state.modal.loaded)
        return;
    state.mapselect.names.clear();
    state.mapselect.names.reserve(state.maps.size());
    for (const MapInfo& info : state.maps)
        state.mapselect.names.push_back(info.name);
    multiplayer_fill_map_names(state.modal.panel, state.mapselect.names);
    if (highlighted.empty())
        return;
    for (std::size_t row = 0; row < state.mapselect.names.size(); ++row) {
        if (state.mapselect.names[row] != highlighted)
            continue;
        panel_select_list_row(state.modal.panel, "MAPNAMES", static_cast<int32_t>(row));
        return;
    }
}

int32_t multiplayer_map_count() {
    load_maps();
    const auto count = ui().maps.size();
    if (count > static_cast<std::size_t>(std::numeric_limits<int32_t>::max()))
        return std::numeric_limits<int32_t>::max();
    return static_cast<int32_t>(count);
}

void multiplayer_order_map_names(std::vector<std::string>& names) {
    load_maps();
    const auto& maps = ui().maps;
    std::vector<MapListFact> facts;
    facts.reserve(names.size());
    for (const std::string& key : names) {
        const auto found = std::find_if(maps.begin(), maps.end(), [&](const MapInfo& info) {
            return info.name == key;
        });
        MapListFact fact;
        fact.key = key;
        if (found == maps.end() && oa::data::map_pack::split_pack_map_name(key))
            fact.pack = true;
        if (found != maps.end()) {
            fact.title = found->title;
            fact.file_title = found->file_title;
            fact.pack_name = found->pack_name;
            fact.pack = found->pack;
            fact.blocked_reason = found->blocked_reason;
        }
        facts.push_back(fact);
    }
    std::vector<std::size_t> order;
    try {
        order = map_list_order(facts);
    } catch (const std::exception&) {
        return;
    }
    if (order.size() != names.size())
        return;
    for (const std::size_t index : order) {
        if (index >= names.size())
            return;
    }
    std::vector<std::string> sorted;
    sorted.reserve(names.size());
    for (const std::size_t index : order)
        sorted.push_back(std::move(names[index]));
    names = std::move(sorted);
}

std::vector<std::string> multiplayer_map_texts(std::span<const std::string> names) {
    load_maps();
    const auto& maps = ui().maps;
    std::vector<MapListFact> facts;
    facts.reserve(names.size());
    for (const std::string& key : names) {
        const auto found = std::find_if(maps.begin(), maps.end(), [&](const MapInfo& info) {
            return info.name == key;
        });
        MapListFact fact;
        fact.key = key;
        if (found == maps.end() && oa::data::map_pack::split_pack_map_name(key))
            fact.pack = true;
        if (found != maps.end()) {
            fact.title = found->title;
            fact.file_title = found->file_title;
            fact.pack_name = found->pack_name;
            fact.pack = found->pack;
            fact.blocked_reason = found->blocked_reason;
        }
        facts.push_back(fact);
    }
    std::vector<std::string> texts = map_list_texts(facts);
    if (texts.size() != names.size())
        texts.assign(names.begin(), names.end());
    return texts;
}

std::string multiplayer_shown_map_name(const char* key) {
    if (key == nullptr)
        return {};
    load_maps();
    const std::string wanted(key);
    std::vector<std::string> names;
    names.reserve(ui().maps.size() + 1);
    bool listed = false;
    for (const MapInfo& info : ui().maps) {
        names.push_back(info.name);
        if (info.name == wanted)
            listed = true;
    }
    if (!listed)
        names.push_back(wanted);
    const std::vector<std::string> texts = multiplayer_map_texts(names);
    for (std::size_t index = 0; index < names.size() && index < texts.size(); ++index)
        if (names[index] == wanted)
            return texts[index];
    return wanted;
}

void multiplayer_fill_map_names(Panel& panel, const std::vector<std::string>& names) {
    std::vector<std::string> texts = multiplayer_map_texts(names);
    if (texts.size() != names.size())
        texts.assign(names.begin(), names.end());
    panel_fill_list(panel, "MAPNAMES", std::move(texts));
    panel_set_item_keys(panel, "MAPNAMES", names);
    std::vector<uint8_t> grey(names.size(), 0);
    const auto& maps = ui().maps;
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto found = std::find_if(maps.begin(), maps.end(), [&](const MapInfo& info) {
            return info.name == names[index];
        });
        if (found != maps.end() && !found->blocked_reason.empty())
            grey[index] = 1;
    }
    panel_set_item_grey(panel, "MAPNAMES", std::move(grey));
}

const char* multiplayer_map_at(int32_t index) {
    load_maps();
    const auto& maps = ui().maps;
    if (index < 0 || static_cast<std::size_t>(index) >= maps.size())
        return nullptr;
    return maps[static_cast<std::size_t>(index)].name.c_str();
}

const char* multiplayer_pack_map_title(const char* name) {
    if (name == nullptr || g_map_source.count == nullptr || g_map_source.at == nullptr)
        return nullptr;
    const int32_t count = g_map_source.count(g_map_source.context);
    for (int32_t row = 0; row < count; ++row) {
        LobbyPackMap pack{};
        if (!g_map_source.at(g_map_source.context, row, &pack) || pack.name == nullptr)
            continue;
        if (std::strcmp(pack.name, name) == 0)
            return pack.title;
    }
    return nullptr;
}

bool multiplayer_battle_room_map(BattleRoomMap* out) noexcept {
    if (out == nullptr)
        return false;
    auto& state = ui();
    // As the badges: the battle room shows, with no dialog over it.
    if (!state.showing || state.screen != kScreenBattleroom || !state.base.loaded ||
        &front() != &state.base) {
        *out = BattleRoomMap{};
        return false;
    }
    return battle_room_map(
        state.lobby, state.base.panel, state.base.offset_x, state.base.offset_y, out
    );
}

namespace {

/// True while the battle room's panel is loaded, whether or not a dialog covers it.
///
/// @return true while that screen is showing
bool battle_room_loaded() noexcept {
    const auto& state = ui();
    return state.showing && state.screen == kScreenBattleroom && state.base.loaded;
}

/// True while the battle room's own panel is the one in front.
///
/// @return true while no dialog covers it
bool battle_room_in_front() noexcept {
    const auto& state = ui();
    return battle_room_loaded() && &front() == &state.base;
}

} // namespace

std::vector<BattleRoomControl> multiplayer_battle_room_controls() {
    std::vector<BattleRoomControl> controls;
    if (!battle_room_in_front())
        return controls;
    auto& state = ui();
    const Panel& panel = state.base.panel;
    const int32_t limit = std::min(panel.count, static_cast<int32_t>(kPanelControls));
    for (int32_t index = 1; index < limit; ++index) {
        const Control& control = panel.controls[static_cast<std::size_t>(index)];
        if (control.width <= 0 || control.height <= 0)
            continue;
        BattleRoomControl one;
        one.index = index;
        one.name = std::string(control_name(control));
        one.x = static_cast<int32_t>(control.x) + state.base.offset_x;
        one.y = static_cast<int32_t>(control.y) + state.base.offset_y;
        one.width = control.width;
        one.height = control.height;
        one.focus_place = battle_room_focus_place(panel, state.lobby, index);
        controls.push_back(std::move(one));
    }
    return controls;
}

int32_t multiplayer_battle_room_focus() noexcept {
    if (!battle_room_in_front())
        return kNoControl;
    return ui().base.panel.focus;
}

void multiplayer_set_battle_room_focus(int32_t index) noexcept {
    if (!battle_room_in_front())
        return;
    Panel& panel = ui().base.panel;
    if (panel.focus == index)
        return;
    panel.focus = index;
    panel.dirty = true;
}

bool multiplayer_battle_room_open() noexcept {
    return battle_room_loaded();
}

int32_t multiplayer_focused_player_slot() noexcept {
    auto& state = ui();
    // As the badges: the battle room shows, with no dialog over it.
    if (!state.showing || state.screen != kScreenBattleroom || !state.base.loaded ||
        &front() != &state.base)
        return -1;
    const auto& panel = state.base.panel;
    if (panel.focus <= 0 || panel.focus >= panel.count)
        return -1;
    const auto& name = panel.controls[static_cast<std::size_t>(panel.focus)].name;
    const std::string_view focused(name.data(), ::strnlen(name.data(), name.size()));
    return player_row_slot(focused);
}

const ui::gui_layout::Layout& multiplayer_screen_layout() noexcept {
    return ui().base.source;
}

Panel* multiplayer_modal() noexcept {
    return ui().modal_kind == ModalKind::none ? nullptr : &ui().modal.panel;
}

ModalKind multiplayer_modal_kind() noexcept {
    return ui().modal_kind;
}

Game& multiplayer_game() noexcept {
    return *ui().game;
}

void multiplayer_bind_direct_game(const DirectGame& game) {
    g_direct = DirectGameState{};
    if (game.kind == DirectGame::Kind::none)
        return;
    g_direct.game = game;
    g_direct.stage = DirectStage::tcp;
    g_direct.progress.reached = DirectGameStep::connecting;
}

const DirectGameProgress& multiplayer_direct_game_progress() noexcept {
    return g_direct.progress;
}

void multiplayer_reload_unit_headers(ScreenContext* ctx) {
    auto& state = ui();
    auto* previous = state.ctx;
    const oa::AssetStore* previous_assets = state.assets;
    void* previous_host = state.host;
    const app::ScreenServices* previous_services = state.services;
    remember_screen(state, ctx);
    const oa::data::defs::UnitHeaderLoader loader{nullptr, [](void*, Game* game) {
                                                      auto& units = ui().units;
                                                      units = UnitStore{};
                                                      load_units();
                                                      game->unit_def_count =
                                                          static_cast<int32_t>(units.units.size());
                                                  }};
    oa::data::defs::reload_unit_headers(state.game.get(), loader);
    state.ctx = previous;
    state.assets = previous_assets;
    state.host = previous_host;
    state.services = previous_services;
}

RestrictPanel& multiplayer_restrict() noexcept {
    return ui().restrict;
}

LobbyUnit* multiplayer_units(const oa::AssetStore& assets, int32_t* count) noexcept {
    try {
        load_units(assets);
    } catch (const std::exception&) {
    }
    auto& units = ui().units.units;
    *count = static_cast<int32_t>(units.size());
    return units.data();
}

uint32_t multiplayer_unit_checksum(void* context, const LobbyUnit& unit) noexcept {
    try {
        return service_unit_checksum(context, unit);
    } catch (const std::exception&) {
        return unit.content_checksum;
    }
}

const std::string& multiplayer_message() noexcept {
    return ui().message;
}

bool multiplayer_click(ScreenContext* ctx, const char* name, uint8_t button) noexcept {
    remember_screen(ui(), ctx);
    if (!ui().message.empty()) {
        ui().message.clear();
        return true;
    }
    auto& res = front();
    const auto index = panel_find(res.panel, name);
    if (index == kNoControl)
        return false;
    const auto& control = res.panel.controls[static_cast<std::size_t>(index)];
    return click(ctx, index, control.x + control.width / 2, control.y + 3, button);
}

void multiplayer_type(ScreenContext* ctx, const char* text) noexcept {
    remember_screen(ui(), ctx);
    Control* control = focused_text_box();
    if (control == nullptr || text == nullptr)
        return;
    std::string value(control_text(*control));
    const std::size_t limit = text_box_limit(*control);
    if (takes_any_character(*control)) {
        drop_composition(value);
        take_game_text(value, text, limit);
    } else {
        take_printable(value, text, limit);
    }
    set_control_text(*control, value);
    front().panel.dirty = true;
}

bool multiplayer_compose(const char* composition) noexcept {
    if (!ui().showing)
        return false;
    Control* control = focused_text_box();
    if (control == nullptr || !takes_any_character(*control))
        return false;
    std::string value(control_text(*control));
    drop_composition(value);
    const std::size_t committed = value.size();
    take_game_text(value, composition != nullptr ? composition : "", text_box_limit(*control));
    ui().composition = value.substr(committed);
    set_control_text(*control, value);
    front().panel.dirty = true;
    return true;
}

void multiplayer_bind_clock(const LobbyClock& clock) noexcept {
    g_clock = clock;
}

void multiplayer_bind_display_modes(const LobbyDisplayModes& display_modes) noexcept {
    g_display_modes = display_modes;
}

void multiplayer_choose_screen_size(int32_t width, int32_t height) noexcept {
    if (g_display_modes.choose != nullptr)
        g_display_modes.choose(g_display_modes.context, width, height);
}

void multiplayer_bind_map_source(const LobbyMapSource& source) noexcept {
    release_pack_source();
    g_map_source = source;
    auto& state = ui();
    state.maps.clear();
    state.maps_loaded = false;
    state.map = -1;
    clear_pack_refusal();
    state.pack_prepared = false;
}

void multiplayer_bind_row_key(PresenceCardTrigger* trigger) noexcept {
    row_key_trigger = trigger;
}

void multiplayer_bind_layer_presence_badges(void* context, bool (*draws)(void*)) noexcept {
    g_layer_badge_context = context;
    g_layer_draws_badges = draws;
}

void multiplayer_reset() noexcept {
    release_pack_source();
    auto& state = ui();
    const bool custom = state.custom_net;
    const auto net = state.bound_net;
    state.game = std::make_unique<Game>();
    state.lobby = Lobby{};
    state.lobby.launch_link = state.launch_link;
    state.lobby.side_names = state.side_names;
    state.lobby.rules = state.rules;
    state.lobby.chat_measure = state.chat_measure;
    state.connect = ConnectState{};
    state.restrict = RestrictPanel{};
    state.mapselect = MapSelect{};
    state.base = Resources{};
    close_modal();
    state.message.clear();
    state.answered_key = 0;
    state.composition.clear();
    state.in_lobby = false;
    state.banner_said.clear();
    state.custom_net = custom;
    state.bound_net = net;
    loopback_reset(state.loopback);
}

} // namespace oa::ui::frontend_multiplayer

namespace oa::app {

void register_multiplayer_screens(ScreenRegistry* registry) {
    using namespace oa::ui::frontend_multiplayer;
    add(registry, kScreenProviders, "mp_providers", enter_providers);
    add(registry, kScreenTcp, "mp_tcp", enter_tcp);
    add(registry, kScreenGameList, "mp_game_list", enter_game_list);
    add(registry, kScreenNewGame, "mp_new_game", enter_new_game);
    add(registry, kScreenBattleroom, "mp_battleroom", enter_battleroom);
}

} // namespace oa::app

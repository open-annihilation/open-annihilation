// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Network play's map lookup by mission name and the network load's player
// bars in the app.
#include "oa/app/runtime.hpp"
#include "network_play.hpp"
#include "oa/app/check_host.hpp"
#include "oa/app/game_directory.hpp"
#include "net_state.hpp"

#include "oa/ui/frontend_renderer/gadget_draw.hpp"
#include "oa/netgame/match/net_match.hpp"
#include "oa/formats/ota.hpp"
#include "oa/present/raster.hpp"
#include "oa/data/map_pack/map_name.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace oa::app {

// A pack map name that does not fit is cut short in the setup block, and
// every machine then looks for a map that does not exist.
// clang-format off
static_assert(oa::data::map_pack::most_map_name_bytes < sizeof(oa::ui::frontend_multiplayer::PlayerSetupInfo{}.map_name), "a name cut short in the setup block names no map");
// clang-format on

namespace {

// Offsets into the guipal-derived colour table the loading screen draws in.
constexpr std::size_t kLoadChipDoneColor = 10;
constexpr std::size_t kLoadPlayerBarColor = 4;

} // namespace

// A name with a package suffix is one map. It is selected when that map is
// installed, and never by matching another map's title: the title search
// would start a different map from the one the lobby named.
//
// A mission map has no Network schema, so select_map refuses its file. When
// the name is that map's own name, or the file's stem, the campaign loader
// takes it. A map select_map already accepts is unchanged. A mission's own
// name holds a colon, which is not a file name, so it is never handed to
// select_map or the campaign loader as a stem.
bool NetworkPlay::select_map_named(std::string_view name) {
    mission_map_taken_ = false;
    if (name.empty())
        return false;
    const auto file_name = [](std::string_view candidate) {
        return candidate.find(':') == std::string_view::npos &&
               candidate.find('/') == std::string_view::npos &&
               candidate.find('\\') == std::string_view::npos &&
               candidate.find('\0') == std::string_view::npos;
    };
    if (file_name(name) && runtime_.select_map(name) != 0)
        return true;
    if (oa::data::map_pack::split_pack_map_name(name))
        return false;
    const auto same = [](std::string_view left, std::string_view right) {
        return left.size() == right.size() &&
               std::equal(
                   left.begin(), left.end(), right.begin(), [](unsigned char a, unsigned char b) {
                       return std::tolower(a) == std::tolower(b);
                   }
               );
    };
    // A stem select_map refuses is a mission map. The load only selects that
    // map; the flag records that, and playback keeps the mission's unit table.
    const auto campaign_stem = [this](std::string_view stem) {
        if (stem.empty() || !runtime_.load_campaign_map(stem))
            return false;
        mission_map_taken_ = true;
        return true;
    };
    const CheckHost host = check_host(runtime_);
    for (const auto& path : host.assets(host.context)->list_effective("maps", ".ota")) {
        const auto bytes = runtime_.read(path);
        if (!bytes)
            continue;
        const auto parsed = oa::formats::ota::parse(
            std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size())
        );
        if (!parsed.ok() || !same(parsed.metadata->mission_name, name))
            continue;
        const std::string stem = path_to_utf8(path_from_utf8(path).stem());
        if (runtime_.select_map(stem) != 0)
            return true;
        if (campaign_stem(stem))
            return true;
    }
    // The name may be the file's own stem, which the title search does not match.
    // A mission title is not a file name, so it is not read as a stem.
    if (!file_name(name))
        return false;
    return campaign_stem(name);
}

void NetworkPlay::draw_loading_players(oa::Surface& target, const oa::present::GafSprites* font) {
    namespace nm = oa::netgame::match;
    const auto* session = loading_net_match();
    if (session == nullptr)
        return;
    nm::LoadingScreenStatus status{};
    nm::net_match_loading_screen_status(session->net.get(), &status);
    for (int32_t i = 0; i < status.bar_count; ++i) {
        const auto& bar = status.bars[i];
        oa::Rect32 area{bar.left, nm::loading_bar_top, bar.right, nm::loading_bar_bottom};
        oa::present::fill_clipped_rect(&target, area, runtime_.ui_colors_[kLoadPlayerBarColor]);
        area.x2 = bar.filled;
        oa::present::fill_clipped_rect(&target, area, runtime_.ui_colors_[kLoadChipDoneColor]);
        // The loading screen keeps the game's own fonts, the players' names
        // included: a character the font lacks is drawn in the modern fonts.
        renderer::draw_gadget_text(
            &target,
            font,
            bar.player->name,
            bar.left,
            nm::loading_bar_top,
            bar.right - bar.left,
            0,
            false
        );
    }
    renderer::draw_gadget_text(
        &target,
        font,
        status.text,
        nm::loading_status_x,
        nm::loading_status_y,
        renderer::gadget_text_unbounded,
        0,
        false
    );
}

} // namespace oa::app

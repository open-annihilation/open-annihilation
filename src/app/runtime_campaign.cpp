// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Runtime glue for the campaign package: binds the campaign object, the
// NEWGAME lists and the MSNBRIEF panel to the frontend runtime.
#include "oa/app/runtime.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/defs/unit_catalog.hpp"
#include "oa/base/text/line_break.hpp"
#include "oa/data/languages/translation.hpp"
#include "oa/ui/decoded.hpp"
#include "oa/app/asset_files.hpp"

#include "oa/sim/ai.hpp"
#include "oa/data/campaign/campaign_assets.hpp"
#include "oa/data/campaign/campaign_file.hpp"
#include "oa/core/unit_def.h"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/match_runtime/mission_unit_binding.hpp"
#include "oa/present/world_renderer/world_camera.hpp"
#include "oa/ui/campaign/campaign.hpp"
#include "oa/ui/campaign/single_player.hpp"
#include "oa/ui/frontend_renderer/game_text.hpp"
#include "oa/ui/gui_input/gadget_panel.hpp"
#include "oa/ui/gui_layout/gui_gadget.hpp"
#include "oa/ui/hud/player_records.hpp"
#include "match_fault.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace oa::app {
namespace {

namespace campaign = oa::ui::campaign;
namespace missions = oa::data::campaign;

// Palette indices of a briefing's text by side, ARM then CORE (the sides
// after CORE take CORE's): the rows, then by HighlightColor slot the green
// highlights, which the MOREBAR caption shares, the yellow and the red.
constexpr std::array<std::array<uint8_t, 4>, 2> kBriefingTextColours{
    {{53, 51, 64, 208}, {117, 86, 82, 212}}
};
// The mission briefing's Enter and Escape keys click these, whatever the
// panel's crdefault and escdefault name.
constexpr std::string_view kBriefingEnterDefault = "Start";
// A streamed sound's delay is counted on the engine clock.
constexpr uint32_t kSoundClockTicksPerSecond = 30;
constexpr uint32_t kMillisecondsPerSecond = 1000;
constexpr std::string_view kBriefingEscapeDefault = "PrevMenu";
constexpr std::size_t kBriefingRowColour = 0;
constexpr std::size_t kBriefingMoreColour = 1;
// The briefing art's window frames: one 640 by 480 frame for each side, ARM
// first, drawn from the screen's corner over the panorama and the planet.
constexpr std::string_view kBriefingFrames = "PanMask";
// The wind and gravity lines in the SOLARSYSTEM gadget: their pens' column
// from the gadget's left, the wind line's pen row from its top, and the
// rows from one line to the next.
constexpr int32_t kSolarLabelColumn = 80;
constexpr int32_t kSolarLabelFirstRow = 20;
constexpr int32_t kSolarLabelRowStep = 20;

/// Returns the width of a briefing panorama's strip: its frames side by side.
///
/// @param panorama the panorama sequence; null for none
/// @return the strip's width in pixels; 0 for none
int32_t briefing_strip_width(const oa::formats::gaf::Sequence* panorama) {
    if (panorama == nullptr)
        return 0;
    int32_t width = 0;
    for (const auto& frame : panorama->frames)
        width += frame.width;
    return width;
}

// A highlighted word shows in its colour for kHighlightShownMs, then in
// kHighlightFlashColour for kHighlightFlashMs, over and over from when its
// page is laid out.
constexpr uint8_t kHighlightFlashColour = 94;
constexpr uint32_t kHighlightShownMs = 1000;
constexpr uint32_t kHighlightFlashMs = 250;
// A briefing's TextRegion is drawn in its side's font: the panel's font
// records hold the small font first, then the side fonts, ARM first.
constexpr int32_t kFirstSideFontRecord = 1;
// BRIEFING.GUI opens at its authored position, over the panel below without
// darkening it.
constexpr uint32_t kInGameBriefingFlags = 0;
constexpr const char* kDefaultAiProfile = "default.txt";
// Width of the message box the mission loader's messages open in.
constexpr int32_t kMissionMessageWidth = 480;

// Messages and actions raised by package callbacks, applied by the runtime
// after the package call returns.
struct PackageEvents {
    std::vector<std::string> sounds;
    std::string message;
    std::string narration;
    uint32_t narration_delay = 0; // engine clock ticks
    uint8_t signal = campaign::signal::none;
    bool stop_narration = false;
};

struct CampaignRuntime {
    missions::CampaignFile file{};
    // The map context of a skirmish or multiplayer match.
    missions::CampaignFile map{};
    campaign::BriefingPanel panel{};
    missions::CampaignFiles files{};
    // `files` with the mission loader's messages shown in message boxes
    // (Runtime::campaign_dialog_env), and the runtime that shows them.
    missions::CampaignFiles dialog_files{};
    Runtime* dialog_owner = nullptr;
    PackageEvents events;
    const oa::formats::fnt::Font* font = nullptr;
};

CampaignRuntime& campaign_runtime() {
    static auto* state = [] {
        auto* created = new CampaignRuntime;
        // The single-player map list's campaign object and the match's map
        // context; nothing is loaded yet, so no file services are needed.
        const missions::CampaignEnv unloaded{};
        missions::campaign_file_construct(
            &created->file, missions::SessionKind::campaign, &unloaded
        );
        missions::campaign_file_construct(
            &created->map, missions::SessionKind::skirmish, &unloaded
        );
        return created;
    }();
    return *state;
}

int32_t asset_size(void* context, const char* path) {
    try {
        return static_cast<int32_t>(static_cast<oa::AssetStore*>(context)->read(path).bytes.size());
    } catch (const std::exception&) {
        return -1;
    }
}

int32_t asset_read(void* context, const char* path, char* buffer, uint32_t capacity) {
    try {
        const auto data = static_cast<oa::AssetStore*>(context)->read(path);
        const auto count = std::min<std::size_t>(capacity, data.bytes.size());
        std::memcpy(buffer, data.bytes.data(), count);
        return static_cast<int32_t>(count);
    } catch (const std::exception&) {
        return -1;
    }
}

void asset_list(
    void* context,
    const char* directory,
    const char* extension,
    void (*visit)(void*, const char*),
    void* visit_context
) {
    std::vector<std::string> paths;
    try {
        paths = static_cast<oa::AssetStore*>(context)->list_effective(
            directory, std::string(".") + extension
        );
    } catch (const std::exception&) {
        return;
    }
    for (const auto& path : paths) {
        const auto slash = path.find_last_of("/\\");
        const auto name = slash == std::string::npos ? path : path.substr(slash + 1);
        visit(visit_context, name.c_str());
    }
}

int32_t asset_count(void* context, const char* pattern) {
    try {
        return static_cast<oa::AssetStore*>(context)->count_entries(pattern, false);
    } catch (const std::exception&) {
        return 0;
    }
}

void asset_find(
    void* context,
    const char* pattern,
    void (*visit)(void*, const missions::FindRecord&),
    void* user
) {
    std::vector<oa::FoundEntry> found;
    try {
        found = static_cast<oa::AssetStore*>(context)->find(pattern);
    } catch (const std::exception&) {
        return;
    }
    for (const auto& entry : found)
        visit(
            user,
            {entry.directory ? missions::kFindDirectory : 0U, 0, entry.size, entry.name.c_str()}
        );
}

void package_message(void*, const char* text) {
    campaign_runtime().events.message = text;
}

// Measures a briefing's text as draw_briefing_overlays draws it: in its FNT
// font, and in the modern fonts what the Language settings give them.
int32_t measure_font(void* context, const char* text) {
    return oa::ui::frontend_renderer::measure_fnt_game_text(
        *static_cast<const oa::formats::fnt::Font*>(context), text, true
    );
}

// Loads the FNT font of a panel's font record: the font-resource record
// `ordinal` places among those after the root, whose file names a font in
// fonts/. Empty when the panel holds no such record or the font does not load.
std::optional<oa::formats::fnt::Font> load_panel_font(
    const oa::ui::gui_layout::Layout& layout,
    int32_t ordinal,
    oa::AssetStore& assets,
    const char* language
) {
    int32_t seen = 0;
    for (std::size_t index = 1; index < layout.gadgets.size(); ++index) {
        const auto& gadget = layout.gadgets[index];
        if (gadget.common.type != oa::ui::gui_layout::GadgetType::font_resource ||
            seen++ != ordinal)
            continue;
        const auto* file = std::get_if<oa::ui::gui_layout::FileResourceFields>(&gadget.fields);
        if (file == nullptr || file->filename.empty())
            return std::nullopt;
        try {
            return oa::ui::decoded::require(
                oa::formats::fnt::load_named_fnt(
                    assets, file->filename, language != nullptr ? language : ""
                ),
                file->filename
            );
        } catch (const std::exception& error) {
            std::cerr << "briefing font " << file->filename << ": " << error.what() << '\n';
            return std::nullopt;
        }
    }
    return std::nullopt;
}

int32_t lcg_random(void*) {
    return std::rand() & 0x7fff;
}

campaign::FrontendHost event_host() {
    campaign::FrontendHost host{};
    // The briefing's words in the game's language, as gamedata\translate.tdf
    // gives them.
    host.translate = [](void*, const char* text) -> const char* {
        const auto& hooks = oa::data::languages::translation_hooks();
        return hooks.translate != nullptr ? hooks.translate(hooks.context, text) : nullptr;
    };
    host.play_sound = [](void*, const char* name) {
        campaign_runtime().events.sounds.emplace_back(name);
    };
    host.message_box = [](void*, const char* text, int32_t) {
        campaign_runtime().events.message = text;
    };
    host.signal = [](void*, uint8_t value) { campaign_runtime().events.signal = value; };
    host.stop_narration = [](void*) { campaign_runtime().events.stop_narration = true; };
    host.play_narration = [](void*, const char* path, uint32_t delay) {
        campaign_runtime().events.narration = path;
        campaign_runtime().events.narration_delay = delay;
    };
    return host;
}

bool names_equal(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(
               left.begin(), left.end(), right.begin(), [](unsigned char a, unsigned char b) {
                   return std::tolower(a) == std::tolower(b);
               }
           );
}

// A skirmish reloads the session object for its map at Start as kind 2; the
// schema match picks the schema on the highest occupied roster slot. A
// multiplayer session's object loads the same way.
bool load_skirmish_session(
    missions::CampaignFile* session,
    oa::AssetStore& assets,
    uint32_t difficulty,
    const entry::SkirmishSettings& settings,
    const std::string& map
) {
    int32_t roster = 0;
    for (std::size_t slot = 0; slot < settings.slots.size(); ++slot)
        if (settings.slots[slot].controller != entry::controller::disabled)
            roster = static_cast<int32_t>(slot) + 1;
    const missions::CampaignFiles files{
        &assets,
        asset_size,
        asset_read,
        asset_list,
        oa::data::languages::installed_translation,
        nullptr,
        oa::data::languages::installed_word(),
        nullptr,
        nullptr
    };
    const missions::CampaignEnv env{&files, nullptr, static_cast<int32_t>(difficulty), roster};
    session->kind = missions::SessionKind::skirmish;
    return missions::campaign_load_mission_info(session, &env, map.c_str());
}

} // namespace

bool Runtime::tdf_names_equal(std::string_view left, std::string_view right) {
    return names_equal(left, right);
}

// Binds the package's file services to this runtime's asset store.
static missions::CampaignEnv campaign_env(
    oa::AssetStore& assets,
    uint32_t difficulty,
    const oa::data::match_rules::AiDifficultyNames& names
) {
    auto& state = campaign_runtime();
    // The campaign's texts in the game's language, and its language folders.
    state.files = {
        &assets,
        asset_size,
        asset_read,
        asset_list,
        oa::data::languages::installed_translation,
        package_message,
        oa::data::languages::installed_word(),
        asset_count,
        asset_find
    };
    return {&state.files, nullptr, static_cast<int32_t>(difficulty), 0, names};
}

namespace {

// SINGLE.GUI keeps the side and the all-missions unlock the panel reads.
campaign::CampaignSetup& single_player_setup() {
    static auto* setup = [] {
        auto* created = new campaign::CampaignSetup;
        campaign::campaign_setup_init(created);
        return created;
    }();
    return *setup;
}

// NEWGAME.GUI's setup state: the side, difficulty and list selections.
campaign::CampaignSetup& new_game_setup() {
    static auto* setup = [] {
        auto* created = new campaign::CampaignSetup;
        campaign::campaign_setup_init(created);
        return created;
    }();
    return *setup;
}

} // namespace

campaign::FrontendHost Runtime::single_player_host() {
    campaign::FrontendHost host{};
    host.context = this;
    host.translate = translation_hook;
    // A button's keyboard shortcut, which Spanish moves to the letter its
    // caption has (single_player_enter).
    host.set_quick_key = [](void* context, const char* name, char key) {
        if (auto* gadget = static_cast<Runtime*>(context)->widget(name))
            if (auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget->fields))
                button->quick_key = static_cast<int8_t>(key);
    };
    host.set_control_value = [](void* context, const char* name, int32_t value) {
        if (auto* gadget = static_cast<Runtime*>(context)->widget(name))
            gadget->common.active = static_cast<int8_t>(value != 0 ? 1 : 0);
    };
    host.select_cursor_animation = [](void* context, int32_t index) {
        static_cast<Runtime*>(context)->select_cursor_animation(static_cast<uint32_t>(index));
    };
    host.write_all_missions = [](void* context, bool unlocked) {
        static_cast<Runtime*>(context)->store_all_missions(unlocked);
    };
    return host;
}

void Runtime::set_widget_y(void* context, const char* name, uint8_t type, int16_t y) {
    if (auto* gadget = static_cast<Runtime*>(context)->widget(name);
        gadget != nullptr && static_cast<uint8_t>(gadget->common.type) == type)
        gadget->common.y = y;
}

void Runtime::store_all_missions(bool unlocked) {
    if (unlocked)
        preferences_.campaign_unlock_flags |= init::preference_flags::all_missions;
    else
        preferences_.campaign_unlock_flags &=
            static_cast<uint16_t>(~init::preference_flags::all_missions);
    init::write_all_missions(preferences_, *this);
}

void Runtime::enter_single_player_panel() {
    auto& setup = single_player_setup();
    setup.side = static_cast<int32_t>(preferences_.side);
    setup.unlock_flags = preferences_.campaign_unlock_flags;
    const auto host = single_player_host();
    campaign::single_player_enter(&setup, &host, game_language());
    typed_key_hook_ = TypedKeyHook::single_player_code;
    if (!offers_saved_games())
        if (auto* gadget = widget("LoadGame"))
            if (auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget->fields))
                button->grayed_out = true;
    if (!offers_any_mission())
        if (auto* gadget = widget("AnyMsn"))
            gadget->common.active = 0;
}

bool Runtime::offers_saved_games() const {
    return assets_.file_size(oa::data::defs::gui_path("loadgame.gui")) != 0;
}

bool Runtime::offers_any_mission() const {
    return assets_.file_size(oa::data::defs::gui_path("newgame.gui")) != 0 &&
           assets_.file_size("bitmaps/playanygame4.pcx") != 0;
}

bool Runtime::side_has_campaign(uint32_t side) {
    const auto env = campaign_env(assets_, preferences_.difficulty, difficulty_names());
    std::vector<char> names(64 * 1024);
    return missions::campaign_load_names(
               env.files, side == 0 ? "ARM" : "CORE", names.data(), names.size()
           ) > 0;
}

void Runtime::enter_new_game_panel(bool any_mission) {
    // A side the game data has no campaign for, such as Core in the Total
    // Annihilation demo (1997), is grayed out, and the panel opens on the
    // other side when the preferred one has none.
    const bool side_campaigns[2]{side_has_campaign(0), side_has_campaign(1)};
    const uint32_t other_side = preferences_.side == 0 ? 1U : 0U;
    if (!side_campaigns[preferences_.side != 0 ? 1 : 0] && side_campaigns[other_side])
        preferences_.side = other_side;
    auto& setup = new_game_setup();
    setup.side = static_cast<int32_t>(preferences_.side);
    setup.difficulty = static_cast<int32_t>(preferences_.difficulty);
    setup.unlock_flags = preferences_.campaign_unlock_flags;
    std::snprintf(setup.side_names[0], sizeof setup.side_names[0], "%s", "ARM");
    std::snprintf(setup.side_names[1], sizeof setup.side_names[1], "%s", "CORE");
    setup.side_count = 2;
    setup.env = campaign_env(assets_, preferences_.difficulty, difficulty_names());
    setup.campaign = &campaign_runtime().file;
    setup.campaign_selected = 0;
    setup.mission_selected = 0;
    campaign_files_.clear();
    campaign_labels_.clear();
    campaign_mission_labels_.clear();
    campaign_mission_files_.clear();
    selected_campaign_index_ = 0;
    selected_mission_index_ = 0;
    campaign_first_visible_ = 0;
    campaign_mission_first_visible_ = 0;
    auto host = single_player_host();
    host.load_background = [](void* context, const char* name) {
        // A bitmap that cannot be read throws; whether the backdrop changed
        // is not needed.
        std::ignore =
            static_cast<Runtime*>(context)->load_named_background(name, false, false, false);
    };
    host.set_control_y = set_widget_y;
    host.set_control_height = [](void* context, const char* name, uint8_t type, int16_t height) {
        if (auto* gadget = static_cast<Runtime*>(context)->widget(name);
            gadget != nullptr && static_cast<uint8_t>(gadget->common.type) == type)
            gadget->common.height = height;
    };
    host.select_group = [](void* context, const char* name) {
        static_cast<Runtime*>(context)->set_button_status(name, 1);
    };
    host.focus_control = [](void* context, const char* name) {
        static_cast<Runtime*>(context)->campaign_setup_focus_ = name != nullptr ? name : "";
    };
    host.zero_sequence_origins = [](void* context, const char* name) {
        for (auto& sequence : static_cast<Runtime*>(context)->resources_.sprites.sequences)
            if (names_equal(sequence.name, name))
                for (auto& frame : sequence.frames) {
                    frame.origin_x = 0;
                    frame.origin_y = 0;
                }
    };
    host.set_list = [](void* context, const char* name, const char* entries, int32_t count) {
        auto& runtime = *static_cast<Runtime*>(context);
        std::vector<std::string> rows;
        for (int32_t row = 0; row < count; ++row, entries += std::strlen(entries) + 1)
            rows.emplace_back(entries);
        if (names_equal(name, "Campaign")) {
            runtime.campaign_labels_ = rows;
            runtime.campaign_files_.clear();
            for (const auto& label : rows)
                runtime.campaign_files_.push_back("camps/" + label + ".tdf");
            return;
        }
        auto& file = campaign_runtime().file;
        runtime.campaign_mission_labels_ = rows;
        runtime.campaign_mission_files_.clear();
        for (int32_t row = 0; row < count; ++row) {
            char mission[missions::kCampaignNameBytes];
            if (missions::campaign_mission_file(&file, row, mission, sizeof mission))
                runtime.campaign_mission_files_.emplace_back(mission);
        }
    };
    campaign::new_game_enter(&setup, &host, any_mission);
    if (campaign_labels_.empty())
        discover_campaigns();
    else if (campaign_mission_files_.empty())
        load_campaign_missions(selected_campaign_index_);
    const char* const side_buttons[2][2]{
        {"Side0", oa::data::defs::side_name(0)}, {"Side1", oa::data::defs::side_name(1)}
    };
    for (std::size_t side = 0; side < 2; ++side)
        if (!side_campaigns[side])
            for (const char* name : side_buttons[side])
                if (auto* gadget = widget(name))
                    if (auto* button =
                            std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget->fields))
                        button->grayed_out = true;
}

void Runtime::check_single_player_code() {
    if (!offers_any_mission())
        return;
    constexpr std::size_t code_offset = 8; // the last seven of the fifteen keys the history holds
    char recent[campaign::kCheatCodeLength + 1]{};
    std::copy_n(typed_keys_.begin() + code_offset, campaign::kCheatCodeLength, recent);
    auto& setup = single_player_setup();
    setup.unlock_flags = preferences_.campaign_unlock_flags;
    const auto host = single_player_host();
    campaign::check_cheat_code(&setup, &host, recent);
}

missions::CampaignFile& Runtime::campaign_object() {
    return campaign_runtime().file;
}

missions::CampaignEnv Runtime::campaign_object_env() {
    return campaign_env(assets_, preferences_.difficulty, difficulty_names());
}

missions::CampaignEnv Runtime::campaign_dialog_env() {
    missions::CampaignEnv env = campaign_env(assets_, preferences_.difficulty, difficulty_names());
    auto& state = campaign_runtime();
    state.dialog_files = state.files;
    state.dialog_owner = this;
    state.dialog_files.message = [](void*, const char* text) {
        auto& campaign = campaign_runtime();
        campaign.events.message = text;
        std::cerr << "open-annihilation: " << text << '\n';
        if (campaign.dialog_owner != nullptr)
            campaign.dialog_owner->show_frontend_message(
                campaign.dialog_owner->translate_ui(text),
                kMissionMessageWidth,
                entry::message_show_ok,
                entry::message_fit_width
            );
    };
    env.files = &state.dialog_files;
    return env;
}

const missions::CampaignFile* Runtime::match_map_context() {
    auto& state = campaign_runtime();
    if (campaign_mission_)
        return &state.file;
    if (selected_map_name_runtime_.empty())
        return nullptr;
    const char* name = selected_map_name_runtime_.c_str();
    if (oa::formats::tdf::compare_nocase(state.map.mission_name, name) != 0) {
        const auto files = missions::campaign_asset_files(assets_);
        const missions::CampaignEnv env{
            &files, nullptr, static_cast<int32_t>(preferences_.difficulty), 0
        };
        if (!missions::campaign_select_mission(&state.map, &env, name)) {
            state.map.mission_name[0] = '\0';
            return nullptr;
        }
    }
    return &state.map;
}

bool Runtime::load_campaign_map(std::string_view mission_file) {
    auto stem = std::string(mission_file);
    const auto dot = stem.find_last_of('.');
    if (dot != std::string::npos)
        stem.resize(dot);
    if (stem.empty())
        return false;
    const auto tnt_data = read("maps/" + stem + ".tnt");
    const auto ota_data = read("maps/" + stem + ".ota");
    if (!tnt_data || !ota_data)
        return false;
    const std::string_view ota_text(
        reinterpret_cast<const char*>(ota_data->data()), ota_data->size()
    );
    auto parsed = oa::formats::ota::parse(ota_text);
    if (!parsed.ok())
        throw std::runtime_error(
            "cannot parse campaign mission metadata: " + parsed.error->message
        );
    auto terrain = oa::formats::tnt::parse(*tnt_data);
    if (!terrain.ok())
        throw std::runtime_error(
            "cannot parse campaign mission terrain: " + terrain.error->message
        );
    oa::formats::tdf::OwnedDocument scenario_document;
    oa::formats::tdf::ParseError scenario_error{};
    if (!scenario_document.parse(ota_text, &scenario_error))
        throw std::runtime_error(
            "cannot parse campaign scenario definitions: " +
            oa::formats::tdf::describe(scenario_error)
        );
    selected_map_metadata_ = std::move(*parsed.metadata);
    selected_ota_document_ = std::move(scenario_document);
    selected_tnt_ = std::move(*terrain.map);
    selected_map_name_runtime_ = stem;
    selected_start_markers_.clear();
    for (const auto& schema : selected_map_metadata_->schemas)
        for (const auto& position : schema.start_positions)
            selected_start_markers_.push_back({1, position.index, position.x, position.z});
    if (selected_start_markers_.empty())
        selected_start_markers_.push_back({1, 0, 0, 0});
    return true;
}

void Runtime::configure_campaign_players() {
    const auto& file = campaign_runtime().file;
    skirmish_settings_.slot_count = 2;
    for (auto& slot : skirmish_settings_.slots)
        slot.controller = entry::controller::disabled;
    auto& human = skirmish_settings_.slots[0];
    auto& cpu = skirmish_settings_.slots[1];
    human.controller = entry::controller::human;
    human.side = preferences_.side;
    human.alliance = 0;
    human.color = 0;
    cpu.controller = entry::controller::computer;
    cpu.side = preferences_.side == 0 ? 1 : 0;
    cpu.alliance = 1;
    cpu.color = 1;
    human.metal = static_cast<int32_t>(file.metal[0]);
    human.energy = static_cast<int32_t>(file.energy[0]);
    cpu.metal = static_cast<int32_t>(file.metal[1]);
    cpu.energy = static_cast<int32_t>(file.energy[1]);
    match_local_player_ = 0;
    state_.player_count = 2;
}

void Runtime::spawn_campaign_units() {
    const auto& file = campaign_runtime().file;
    if (!match_)
        return;
    if (file.units == nullptr && file.unit_count != 0)
        throw std::runtime_error("campaign schema has no unit list");
    // Mission start: the schema's units and their scripts, then the
    // view on the schema's first start position.
    if (!oa::sim::match_runtime::create_mission_units(*match_, file.units, file.unit_count)) {
        raise_match_fault(*match_);
        throw std::runtime_error("campaign mission units were not created");
    }
    place_campaign_camera();
    std::size_t placed = 0;
    for (const auto& slot : match_->world().slots)
        if (slot.unit_index != 0 && slot.record.type_index != 0)
            ++placed;
    // The eleventh slot record has no player to take its side.
    for (std::size_t player = 0; player < match_->world().players.size(); ++player) {
        const auto& setup = skirmish_settings_.slots[player];
        if (setup.controller != entry::controller::disabled)
            match_->world().players[player].setup_side = static_cast<uint8_t>(setup.side);
    }
    // A mission grants its resources as start storage and stores.
    oa::ui::hud::MissionResources resources{};
    static_assert(sizeof file.metal == sizeof resources.metal);
    static_assert(sizeof file.energy == sizeof resources.energy);
    std::memcpy(resources.metal, file.metal, sizeof resources.metal);
    std::memcpy(resources.energy, file.energy, sizeof resources.energy);
    oa::ui::hud::set_starting_resources(
        match_->state(), oa::data::campaign::SessionKind::campaign, &resources, nullptr
    );
    status_ = "Campaign mission " + selected_map_name_runtime_ + " with " + std::to_string(placed) +
              " placed units.";
    std::cerr << status_ << '\n';
}

std::string Runtime::session_ai_profile_path() {
    if (campaign_mission_) {
        const char* path =
            missions::campaign_path(&campaign_runtime().file, missions::CampaignPath::ai_profile);
        return path != nullptr ? path : std::string();
    }
    if (selected_map_name_runtime_.empty())
        return {};
    auto session = std::make_unique<missions::CampaignFile>();
    missions::campaign_file_init(session.get());
    std::string path;
    if (load_skirmish_session(
            session.get(),
            assets_,
            preferences_.difficulty,
            skirmish_settings_,
            selected_map_name_runtime_
        ))
        if (const char* resolved =
                missions::campaign_path(session.get(), missions::CampaignPath::ai_profile))
            path = resolved;
    missions::campaign_file_free(session.get());
    return path;
}

std::string Runtime::session_schema() {
    if (campaign_mission_)
        return campaign_runtime().file.schema;
    if (selected_map_name_runtime_.empty())
        return {};
    auto session = std::make_unique<missions::CampaignFile>();
    missions::campaign_file_init(session.get());
    std::string schema;
    if (load_skirmish_session(
            session.get(),
            assets_,
            preferences_.difficulty,
            skirmish_settings_,
            selected_map_name_runtime_
        ))
        schema = session->schema;
    missions::campaign_file_free(session.get());
    return schema;
}

void Runtime::campaign_session_rules(int32_t (&record)[4]) {
    missions::campaign_session_record(&campaign_runtime().file, record);
}

void Runtime::mark_campaign_units(oa::UnitDef* headers, uint32_t count) {
    auto& state = campaign_runtime();
    if (campaign::load_unit_availability(headers, count, &state.file, &state.files))
        return;
    // Without a use-only file every unit stays buildable. One that is there
    // but cannot be read leaves them buildable too, and is reported.
    const char* path = missions::campaign_path(&state.file, missions::CampaignPath::use_only);
    if (path != nullptr && state.files.size != nullptr &&
        state.files.size(state.files.context, path) >= 0)
        std::cerr << "use-only file " << path << " unreadable; every unit stays buildable\n";
}

void Runtime::load_mission_features(
    std::span<const oa::formats::tdf::OwnedDocument> documents,
    const oa::sim::map_runtime::FeatureDefHost& host
) {
    mission_features_.clear();
    // A resumed save's Features section places every feature.
    if (resuming_saved_game())
        return;
    const auto copy = [this](const missions::CampaignFile& file) {
        mission_features_.assign(
            static_cast<std::size_t>(file.features != nullptr ? file.feature_count : 0), {}
        );
        for (std::size_t index = 0; index < mission_features_.size(); ++index) {
            const auto& source = file.features[index];
            auto& placement = mission_features_[index];
            static_assert(sizeof placement.name == sizeof source.name);
            std::memcpy(placement.name, source.name, sizeof placement.name);
            placement.x = source.x;
            placement.z = source.z;
        }
    };
    if (campaign_mission_) {
        copy(campaign_runtime().file);
    } else if (!selected_map_name_runtime_.empty()) {
        auto session = std::make_unique<missions::CampaignFile>();
        missions::campaign_file_init(session.get());
        if (load_skirmish_session(
                session.get(),
                assets_,
                preferences_.difficulty,
                skirmish_settings_,
                selected_map_name_runtime_
            ))
            copy(*session);
        missions::campaign_file_free(session.get());
    }
    for (const auto& placement : mission_features_) {
        const std::string_view name(
            placement.name, ::strnlen(placement.name, sizeof placement.name)
        );
        if (name.empty())
            continue;
        const auto loaded =
            oa::sim::map_runtime::find_or_load_feature(feature_table_, documents, name, &host);
        if (!loaded.ok())
            throw std::runtime_error(
                "cannot load schema feature '" + std::string(name) + "': " + loaded.error->message
            );
    }
}

void Runtime::place_mission_feature_draws() {
    if (!match_ || mission_features_.empty())
        return;
    sync_dead_feature_draws();
    auto& world = match_->state();
    const auto drawn = [&](int32_t cell_x, int32_t cell_z, uint16_t index) {
        const auto same = [&](const auto& draw) {
            return draw.cell_x == cell_x && draw.cell_z == cell_z && draw.feature_index == index;
        };
        return std::any_of(match_features_.begin(), match_features_.end(), same) ||
               std::any_of(match_gaf_features_.begin(), match_gaf_features_.end(), same);
    };
    for (const auto& placement : mission_features_) {
        const std::string_view name(
            placement.name, ::strnlen(placement.name, sizeof placement.name)
        );
        if (name.empty())
            continue;
        const auto index = oa::sim::map_runtime::find_feature_index(feature_table_, name);
        if (index >= world.feature_def_count)
            continue;
        int32_t cell_x = 0;
        int32_t cell_z = 0;
        oa::sim::feature_runtime::feature_placement_cell(
            world.feature_defs[index], placement, &cell_x, &cell_z
        );
        const auto* plot = oa::world_plot(&world, cell_x, cell_z);
        if (plot != nullptr && plot->feature == index && !drawn(cell_x, cell_z, index))
            place_catalog_feature_draw(cell_x, cell_z, index);
    }
}

std::string Runtime::read_computer_profile() {
    std::optional<std::vector<uint8_t>> bytes;
    if (const auto path = session_ai_profile_path(); !path.empty())
        bytes = assets_.load_file_contents(path);
    if (!bytes)
        bytes = assets_.load_file_contents(
            oa::data::defs::data_path(oa::data::defs::DataDirectory::ai, kDefaultAiProfile)
        );
    return bytes ? std::string(bytes->begin(), bytes->end()) : std::string();
}

void Runtime::configure_computer_players() {
    if (!match_)
        return;
    // Every type's build list as the game holds it, its CANBUILD entries then
    // the download menus': per type, a count then its ids.
    const auto& tables = unit_table_.tables;
    std::vector<uint16_t> lists;
    for (uint32_t type = 0; type < tables.count; ++type) {
        const auto& unit = tables.records[type];
        const uint16_t* ids = oa::data::defs::unit_def_build_ids(&tables, unit);
        const uint32_t count = ids != nullptr ? unit.build_id_count : 0;
        lists.push_back(static_cast<uint16_t>(count));
        lists.insert(lists.end(), ids, ids + count);
    }
    const auto profile = read_computer_profile();
    if (!oa::sim::ai::configure_match_computer_players(*match_, profile, lists, campaign_mission_))
        throw std::runtime_error("cannot store the computer players' profile and build lists");
}

void Runtime::reload_computer_profiles() {
    if (!match_ || oa::sim::ai::reload_match_computer_profiles(*match_, read_computer_profile()))
        return;
    status_ = "ReloadAIProfiles: cannot store the computer players' profile";
    std::cerr << "open-annihilation: " << status_ << '\n';
}

void Runtime::place_campaign_camera() {
    const auto& file = campaign_runtime().file;
    if (!match_ || file.rules == nullptr || file.rule_count <= 0)
        return;
    std::vector<oa::present::world_renderer::StartEntry> entries(
        static_cast<std::size_t>(file.rule_count)
    );
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const auto& rule = file.rules[index];
        entries[index] = {static_cast<int32_t>(rule.type), rule.index, rule.x, rule.z};
    }
    auto view = std::make_unique<oa::Game>();
    const auto& game = match_->state().game;
    view->map_pixel_width = game.map_pixel_width;
    view->map_pixel_height = game.map_pixel_height;
    view->viewport_width = kBattlefieldWidth;
    view->viewport_height = kBattlefieldHeight;
    view->camera_x = static_cast<uint32_t>(match_camera_x_);
    view->camera_y = static_cast<uint32_t>(match_camera_z_);
    oa::present::world_renderer::camera_to_start_entry(*view, entries.data(), file.rule_count);
    set_camera_position(
        static_cast<int32_t>(view->camera_x), static_cast<int32_t>(view->camera_y), 0
    );
}

void Runtime::load_campaign_missions(std::size_t campaign_index) {
    campaign_mission_labels_.clear();
    campaign_mission_files_.clear();
    selected_mission_index_ = 0;
    campaign_mission_first_visible_ = 0;
    if (campaign_index >= campaign_labels_.size())
        return;
    auto& state = campaign_runtime();
    const auto env = campaign_env(assets_, preferences_.difficulty, difficulty_names());
    if (!missions::campaign_load_file(
            &state.file, &env, campaign_labels_[campaign_index].c_str()
        )) {
        std::cerr << "campaign file unavailable: " << state.events.message << '\n';
        return;
    }
    // The missions as players see them in the game's language.
    auto names =
        std::make_unique<char[][missions::kCampaignNameBytes]>(missions::kMaxCampaignMissions);
    const auto count = missions::campaign_load_mission_titles(
        &state.file, game_language(), names.get(), missions::kMaxCampaignMissions
    );
    for (int32_t index = 0; index < count && index < missions::kMaxCampaignMissions; ++index) {
        char file[missions::kCampaignNameBytes];
        if (!missions::campaign_mission_file(&state.file, index, file, sizeof(file)))
            continue;
        campaign_mission_labels_.emplace_back(names[index]);
        campaign_mission_files_.emplace_back(file);
    }
    if (first_draw_after_setup(screen_))
        fill_frontend_list("Missions", campaign_mission_labels_.size());
}

void Runtime::discover_campaigns() {
    campaign_files_.clear();
    campaign_labels_.clear();
    selected_campaign_index_ = 0;
    campaign_first_visible_ = 0;
    const auto env = campaign_env(assets_, preferences_.difficulty, difficulty_names());
    const char* side = preferences_.side == 0 ? "ARM" : "CORE";
    std::vector<char> names(64 * 1024);
    const auto count = missions::campaign_load_names(env.files, side, names.data(), names.size());
    const char* entry = names.data();
    for (int32_t i = 0; i < count; ++i) {
        campaign_labels_.emplace_back(entry);
        campaign_files_.push_back("camps/" + campaign_labels_.back() + ".tdf");
        entry += std::strlen(entry) + 1;
    }
    const char* preferred = preferences_.side != 0 ? "Core Campaign" : "Arm Campaign";
    for (std::size_t i = 0; i < campaign_labels_.size(); ++i)
        if (names_equal(campaign_labels_[i], preferred)) {
            selected_campaign_index_ = i;
            break;
        }
    if (first_draw_after_setup(screen_)) {
        fill_frontend_list("Campaign", campaign_labels_.size());
        select_frontend_list_row("Campaign", selected_campaign_index_);
    }
    load_campaign_missions(selected_campaign_index_);
}

bool Runtime::select_campaign_list_row(std::string_view gadget_name, float canvas_y) {
    const auto* list = widget(std::string(gadget_name));
    if (list == nullptr)
        return false;
    auto item_height =
        static_cast<std::size_t>(oa::formats::fnt::line_height(resources_.font)) + 1U;
    if (const auto* fields = std::get_if<oa::ui::gui_layout::ListBoxFields>(&list->fields);
        fields != nullptr && fields->item_height > 0)
        item_height = static_cast<std::size_t>(fields->item_height);
    const auto local_y = static_cast<int32_t>(canvas_y) - list->common.y - 2;
    if (local_y < 0 || item_height == 0)
        return false;
    // A list its scroll bar scrolls picks as 3.1c does, from the row it shows first.
    const auto bound = frontend_list_first(gadget_name).has_value();
    const auto picked = bound ? frontend_list_row_at(gadget_name, canvas_y) : std::nullopt;
    if (bound && !picked)
        return false;
    const auto row = static_cast<std::size_t>(local_y) / item_height;
    if (gadget_name == "Campaign") {
        const auto index = picked ? *picked : campaign_first_visible_ + row;
        if (index >= campaign_files_.size())
            return false;
        // Only a press that changes the campaign refills the Missions list;
        // one on the selected row keeps the chosen mission.
        const bool changed = index != selected_campaign_index_;
        selected_campaign_index_ = index;
        select_frontend_list_row("Campaign", index);
        if (changed)
            load_campaign_missions(index);
        rebuild_surface();
        return true;
    }
    const auto index = picked ? *picked : campaign_mission_first_visible_ + row;
    if (index >= campaign_mission_files_.size())
        return false;
    selected_mission_index_ = index;
    select_frontend_list_row("Missions", index);
    rebuild_surface();
    return true;
}

void Runtime::step_campaign_list(bool forward) {
    const auto campaigns = names_equal(campaign_setup_focus_, "Campaign");
    if (!campaigns && !names_equal(campaign_setup_focus_, "Missions"))
        return;
    const auto row = step_frontend_list_row(campaign_setup_focus_, forward);
    if (!row)
        return;
    if (campaigns && *row < campaign_files_.size()) {
        // Any Mission refills its Missions list on every step, even one that
        // stays at either end; a new campaign lists no missions.
        const bool moved = *row != selected_campaign_index_;
        selected_campaign_index_ = *row;
        if (moved || screen_ == Screen::any_mission)
            load_campaign_missions(*row);
    } else if (!campaigns && *row < campaign_mission_files_.size()) {
        selected_mission_index_ = *row;
    }
    rebuild_surface();
}

std::string Runtime::resolve_campaign_mission_file() {
    if (campaign_mission_files_.empty())
        discover_campaigns();
    if (selected_mission_index_ < campaign_mission_files_.size())
        return campaign_mission_files_[selected_mission_index_];
    return {};
}

void Runtime::play_briefing_narration(std::string_view narration, uint32_t delay) {
    if (options_.mute || narration.empty())
        return;
    std::string error;
    if (!audio_player_.play_stream(
            narration, delay * kMillisecondsPerSecond / kSoundClockTicksPerSecond, error
        ))
        std::cerr << "briefing narration: " << error << '\n';
}

void Runtime::stop_briefing_audio() {
    audio_player_.stop_stream();
}

campaign::BriefingRegion Runtime::briefing_region() {
    const auto* text_region = widget("TextRegion");
    const auto& font = briefing_text_font();
    auto line_height = static_cast<int32_t>(oa::formats::fnt::line_height(font));
    // A briefing in Chinese, Japanese or Korean, drawn in the modern fonts,
    // spaces its rows to hold the ideographs, which may stand taller than
    // the font's letters.
    const char* text = missions::campaign_briefing_text(&campaign_runtime().file);
    if (const auto settings = oa::present::game_text_settings();
        settings.style.modern_fonts && text != nullptr && oa::base::text::has_wide_script(text))
        if (const auto layers = oa::present::modern_text(
                "\xE4\xB8\xAD",
                oa::ui::frontend_renderer::fnt_font_face(font),
                1,
                std::min(
                    oa::present::held_text_size(settings.style.size),
                    oa::present::game_font_text_size
                ),
                false
            ))
            line_height = std::max(line_height, layers->height);
    return {
        text_region != nullptr ? text_region->common.x : 0,
        text_region != nullptr ? text_region->common.y : 0,
        text_region != nullptr ? text_region->common.height : 0,
        line_height
    };
}

void Runtime::show_mission_briefing() {
    briefing_parent_ = screen_ == Screen::any_mission || screen_ == Screen::campaign_end
                           ? screen_
                           : Screen::new_campaign;
    if (campaign_mission_files_.empty())
        discover_campaigns();
    auto& state = campaign_runtime();
    const auto env = campaign_env(assets_, preferences_.difficulty, difficulty_names());
    if (state.file.campaign_name[0] == '\0' ||
        !missions::campaign_bind_mission(
            &state.file, &env, static_cast<int32_t>(selected_mission_index_)
        )) {
        status_ = state.events.message.empty() ? "No campaign mission is selected."
                                               : state.events.message;
        return;
    }
    const auto bg = preferences_.side == 0 ? "bitmaps/mbriefarm.pcx" : "bitmaps/mbriefcor.pcx";
    renderer::ScreenAssetNames names{
        oa::data::defs::gui_path("msnbrief.gui"),
        bg,
        "palettes/guipal.pal",
        "anims/commongui.gaf",
        "anims/commongui.gaf"
    };
    resources_ = renderer::load_screen(assets_, names);
    const auto& art = campaign::planet_art(
        campaign::briefing_planet_index(
            missions::campaign_planet(&state.file), static_cast<int32_t>(preferences_.side)
        )
    );
    widget_sprites_.clear();
    widget_gaf_frames_.clear();
    widget_text_stages_.clear();
    briefing_frames_.clear();
    const oa::data::defs::Files files = asset_files(assets_);
    uint8_t* planet_gaf = nullptr;
    uint32_t planet_gaf_size = 0;
    if (oa::data::defs::read_named_gaf(
            &files, oa::data::defs::gui_gaf_directory, art.gaf, &planet_gaf, &planet_gaf_size
        )) {
        try {
            const auto parsed = oa::formats::gaf::parse({planet_gaf, planet_gaf_size});
            if (parsed.ok())
                for (auto& sequence : parsed.archive->sequences) {
                    if (names_equal(sequence.name, art.panorama))
                        widget_sprites_["PANORAMA"] = {
                            renderer::SpriteArchive::screen, sequence.name
                        };
                    if (names_equal(sequence.name, art.rotation))
                        widget_sprites_["PLANET"] = {
                            renderer::SpriteArchive::screen, sequence.name
                        };
                    if (names_equal(sequence.name, kBriefingFrames))
                        briefing_frames_ = sequence.name;
                    resources_.sprites.sequences.push_back(std::move(sequence));
                }
        } catch (const std::exception& error) {
            // The briefing then shows no planet.
            std::cerr << "briefing planet art " << art.gaf << ": " << error.what() << '\n';
        }
        files.release(files.context, planet_gaf);
    }
    set_button_stage("SHUTUP", 1);
    for (auto& gadget : resources_.layout.gadgets) {
        if (gadget.common.name == "TextRegion" || gadget.common.name == "MOREBAR" ||
            gadget.common.name == "PANORAMA" || gadget.common.name == "PLANET" ||
            gadget.common.name == "Start" || gadget.common.name == "PrevMenu" ||
            gadget.common.name == "SHUTUP")
            gadget.common.active = 1;
    }
    load_briefing_fonts();
    const auto* text_region = widget("TextRegion");
    const auto region = briefing_region();
    // The text wraps at the TextRegion's width.
    const int32_t wrap_width = text_region != nullptr ? text_region->common.width : 0;
    state.font = &briefing_text_font();
    state.events = {};
    const auto host = event_host();
    campaign::briefing_panel_enter(
        &state.panel,
        &state.file,
        static_cast<int32_t>(preferences_.side),
        &region,
        wrap_width,
        measure_font,
        &briefing_text_font(),
        lcg_random,
        nullptr,
        &host,
        !options_.mute
    );
    // The wind and gravity lines show from the first draw.
    campaign::briefing_solar_system_tick(
        &state.panel,
        &state.file,
        lcg_random,
        nullptr,
        frontend_tick(),
        briefing_strip_width(briefing_sequence("PANORAMA")),
        &host
    );
    briefing_text_ = missions::campaign_briefing_text(&state.file) != nullptr
                         ? missions::campaign_briefing_text(&state.file)
                         : "";
    briefing_lines_.clear();
    briefing_first_visible_ = 0;
    screen_ = Screen::briefing;
    briefing_from_pause_ = false;
    hovered_.reset();
    selected_ = -1;
    // The focus was a record of the screen before, which Enter and the
    // gamepad's A would press here.
    frontend_focus_ = -1;
    apply_output_mode();
    play_briefing_narration(state.events.narration, state.events.narration_delay);
    rebuild_surface();
}

void Runtime::show_in_game_briefing() {
    renderer::ScreenAssetNames names{
        oa::data::defs::gui_path("briefing.gui"),
        "bitmaps/igmbrief.pcx",
        "palettes/guipal.pal",
        "anims/brief.gaf",
        "anims/commongui.gaf"
    };
    renderer::ScreenResources loaded;
    try {
        loaded = renderer::load_screen(assets_, names);
    } catch (const std::exception& error) {
        status_ = std::string("in-game briefing: ") + error.what();
        return;
    }
    if (loaded.layout.gadgets.empty()) {
        status_ = "in-game briefing: guis/briefing.gui has no panel";
        return;
    }
    renderer::Surface below;
    ensure_screen_world();
    compose_match_layers(below);
    const bool over_match = below.width != 0 && below.height != 0;
    resources_ = std::move(loaded);
    briefing_parent_ = Screen::match;
    briefing_from_pause_ = true;
    // The briefing leaves the match palette on the display.
    show_background_in(match_palette_);
    auto& root = resources_.layout.gadgets.front().common;
    oa::ui::gui_input::place_root(
        root.x,
        root.y,
        root.width,
        root.height,
        kInGameBriefingFlags | oa::ui::gui_input::panel_flag::first_draw,
        over_match ? static_cast<int32_t>(below.width) : kCanvasWidth,
        over_match ? static_cast<int32_t>(below.height) : kCanvasHeight,
        oa::ui::gui_input::hud_strip_width
    );
    in_game_briefing_parent_ = std::move(below);
    auto& state = campaign_runtime();
    load_briefing_fonts();
    const auto region = briefing_region();
    const auto* text_region = widget("TextRegion");
    const int32_t wrap_width = text_region != nullptr ? text_region->common.width : 0;
    state.events = {};
    auto host = event_host();
    host.context = this;
    host.clear_attributes = [](void* context, const char* name, uint32_t bits) {
        if (auto* gadget = static_cast<Runtime*>(context)->widget(name))
            gadget->common.attributes =
                static_cast<int32_t>(static_cast<uint32_t>(gadget->common.attributes) & ~bits);
    };
    campaign::ingame_briefing_enter(
        &state.panel, &state.file, &region, wrap_width, measure_font, &briefing_text_font(), &host
    );
    screen_ = Screen::briefing;
    match_paused_ = true;
    hovered_.reset();
    frontend_focus_ = -1;
    selected_ = -1;
    apply_output_mode();
    rebuild_surface();
}

void Runtime::activate_briefing_gadget() {
    if (!hovered_ || *hovered_ >= resources_.layout.gadgets.size())
        return;
    click_briefing_gadget(resources_.layout.gadgets[*hovered_].common.name);
}

bool Runtime::press_briefing_default(bool escape) {
    if (briefing_from_pause_)
        return false;
    const auto* gadget = widget(escape ? kBriefingEscapeDefault : kBriefingEnterDefault);
    if (gadget == nullptr || gadget->common.active == 0)
        return false;
    click_briefing_gadget(gadget->common.name);
    return true;
}

void Runtime::click_briefing_gadget(std::string name) {
    auto& state = campaign_runtime();
    const auto region = briefing_region();
    // The TextRegion and MOREBAR lay out the next page, whose highlighted
    // words start flashing afresh.
    if (name == "TextRegion" || name == "MOREBAR")
        briefing_page_ms_ = clock_milliseconds();
    if (briefing_from_pause_) {
        state.events = {};
        const auto host = event_host();
        campaign::ingame_briefing_click(
            &state.panel, &host, name.c_str(), &region, measure_font, &briefing_text_font()
        );
        for (const auto& sound : state.events.sounds)
            play_ui_sound(sound, 0);
        if (name == "OK") {
            campaign::ingame_briefing_click(
                &state.panel, &host, nullptr, &region, measure_font, &briefing_text_font()
            );
            screen_ = Screen::match;
            match_paused_ = true;
            in_game_briefing_parent_ = {};
            apply_output_mode();
            render_match_surface();
            return;
        }
        rebuild_surface();
        return;
    }
    state.events = {};
    const auto host = event_host();
    campaign::briefing_click(
        &state.panel,
        &state.file,
        &host,
        name.c_str(),
        &region,
        measure_font,
        &briefing_text_font(),
        !options_.mute
    );
    set_button_stage("SHUTUP", state.panel.narration_on ? 1 : 0);
    for (const auto& sound : state.events.sounds)
        play_ui_sound(sound, 0);
    if (state.events.stop_narration)
        stop_briefing_audio();
    if (!state.events.narration.empty())
        play_briefing_narration(state.events.narration, state.events.narration_delay);
    if (state.events.signal == campaign::signal::start_mission) {
        start_campaign_mission();
        return;
    }
    if (state.events.signal == campaign::signal::back) {
        load(briefing_parent_);
        return;
    }
    rebuild_surface();
}

void Runtime::tick_mission_briefing() {
    auto& state = campaign_runtime();
    auto& panel = state.panel;
    const bool narration_on = panel.narration_on;
    const int32_t scroll = panel.panorama_scroll;
    const int32_t rotation = panel.rotation_frame;
    const std::string wind = panel.wind_label;
    const auto* panorama = briefing_sequence("PANORAMA");
    const auto* planet = briefing_sequence("PLANET");
    const auto host = event_host();
    campaign::briefing_solar_system_tick(
        &panel,
        &state.file,
        lcg_random,
        nullptr,
        frontend_tick(),
        briefing_strip_width(panorama),
        &host
    );
    // The ticker's redraw request is left aside: the screen is redrawn below
    // only when what it shows has changed.
    std::ignore = campaign::briefing_ticker(
        &panel,
        static_cast<uint32_t>(SDL_GetTicks()),
        frontend_tick() / campaign::kRotationTickDivisor,
        audio_player_.stream_busy(),
        planet != nullptr ? static_cast<int32_t>(planet->frames.size()) : 0,
        nullptr
    );
    if (narration_on && !panel.narration_on)
        set_button_stage("SHUTUP", 0);
    if (narration_on != panel.narration_on || scroll != panel.panorama_scroll ||
        rotation != panel.rotation_frame || wind != panel.wind_label)
        rebuild_surface();
}

const oa::formats::gaf::Sequence* Runtime::briefing_sequence(std::string_view gadget) const {
    const auto sprite = widget_sprites_.find(std::string(gadget));
    return sprite != widget_sprites_.end()
               ? gaf_sequence(resources_.sprites, sprite->second.sequence)
               : nullptr;
}

void Runtime::draw_briefing_overlays() {
    // The screen is drawn in its background's palette, as the gadgets are.
    const auto& pal =
        resources_.background.palette ? *resources_.background.palette : resources_.gui_palette;

    // A frame's covered pixels, one to one, with its first column and row at
    // (x, y), inside the clip rectangle and the screen.
    struct Clip {
        int left, top, right, bottom;
    };

    const Clip screen_clip{
        0, 0, static_cast<int>(surface_.width), static_cast<int>(surface_.height)
    };
    const auto blit_frame =
        [&](const oa::formats::gaf::Frame& source, int x, int y, const Clip& clip) {
            const auto rendered = oa::formats::gaf::render_normal(source);
            if (!rendered.ok())
                return;
            const auto& frame = *rendered.frame;
            const int left = std::max({clip.left, screen_clip.left, x});
            const int top = std::max({clip.top, screen_clip.top, y});
            const int right =
                std::min({clip.right, screen_clip.right, x + static_cast<int>(frame.width)});
            const int bottom =
                std::min({clip.bottom, screen_clip.bottom, y + static_cast<int>(frame.height)});
            for (int row = top; row < bottom; ++row)
                for (int column = left; column < right; ++column) {
                    const auto offset = static_cast<std::size_t>(row - y) * frame.width +
                                        static_cast<std::size_t>(column - x);
                    if (offset >= frame.coverage.size() || frame.coverage[offset] == 0)
                        continue;
                    const auto pal_i = static_cast<std::size_t>(frame.pixels[offset]) * 4U;
                    if (pal_i + 2 >= pal.size())
                        continue;
                    const auto di = (static_cast<std::size_t>(row) * surface_.width +
                                     static_cast<std::size_t>(column)) *
                                    3U;
                    surface_.rgb[di] = pal[pal_i];
                    surface_.rgb[di + 1] = pal[pal_i + 1];
                    surface_.rgb[di + 2] = pal[pal_i + 2];
                }
        };
    const auto gadget_clip = [](const oa::ui::gui_layout::Gadget& gadget) {
        return Clip{
            gadget.common.x,
            gadget.common.y,
            gadget.common.x + gadget.common.width,
            gadget.common.y + gadget.common.height
        };
    };
    auto& panel = campaign_runtime().panel;
    // The briefing opened from the pause menu has no planet art.
    const bool planet_art = !briefing_from_pause_;
    // The panorama's strip of frames, laid side by side, shows from its
    // scroll on; the strip's start follows its end.
    if (const auto* gadget = widget("PANORAMA"); planet_art && gadget != nullptr)
        if (const auto* panorama = briefing_sequence("PANORAMA")) {
            const int width = briefing_strip_width(panorama);
            const int start = width > 0 ? panel.panorama_scroll % width : 0;
            for (const int repeat : {0, width}) {
                int left = gadget->common.x - start + repeat;
                for (const auto& frame : panorama->frames) {
                    blit_frame(frame, left, gadget->common.y, gadget_clip(*gadget));
                    left += frame.width;
                }
            }
        }
    if (const auto* gadget = widget("PLANET"); planet_art && gadget != nullptr)
        if (const auto* planet = briefing_sequence("PLANET");
            planet != nullptr && !planet->frames.empty()) {
            const auto frame =
                static_cast<std::size_t>(panel.rotation_frame) % planet->frames.size();
            blit_frame(
                planet->frames[frame], gadget->common.x, gadget->common.y, gadget_clip(*gadget)
            );
        }
    if (const auto* frames =
            briefing_frames_.empty() ? nullptr : gaf_sequence(resources_.sprites, briefing_frames_);
        planet_art && frames != nullptr && !frames->frames.empty()) {
        const auto side = std::min<std::size_t>(preferences_.side, frames->frames.size() - 1U);
        blit_frame(frames->frames[side], 0, 0, screen_clip);
    }
    const auto& page = panel.page;
    const auto& colours = kBriefingTextColours[preferences_.side == 0 ? 0 : 1];
    const auto pixel_count =
        static_cast<std::size_t>(surface_.width) * static_cast<std::size_t>(surface_.height);
    std::vector<uint8_t> indices(pixel_count, 0);
    std::vector<uint8_t> coverage(pixel_count, 0);
    const oa::formats::fnt::IndexedSurface target{
        surface_.width, surface_.height, surface_.width, indices, coverage
    };
    // Copies the glyphs drawn since the last copy onto the screen: an FNT
    // font's in `colour`, the GUI font's in their own colours.
    const auto copy_glyphs = [&](uint8_t colour, bool fnt_font) {
        for (std::size_t i = 0; i < pixel_count; ++i) {
            if (coverage[i] == 0)
                continue;
            coverage[i] = 0;
            const auto pal_i = static_cast<std::size_t>(fnt_font ? colour : indices[i]) * 4U;
            if (pal_i + 2 >= pal.size())
                continue;
            auto* pixel = surface_.rgb.data() + i * 3U;
            pixel[0] = pal[pal_i];
            pixel[1] = pal[pal_i + 1];
            pixel[2] = pal[pal_i + 2];
        }
    };
    // Each text's pen row is at its y; the font's lift raises its glyphs.
    // Every text has its own place, so where a pen stops is not needed.
    const auto& font = briefing_text_font();
    const bool fnt_font = briefing_text_font_.has_value();
    namespace renderer = oa::ui::frontend_renderer;
    auto canvas = oa::present::rgb_canvas(
        surface_.rgb,
        static_cast<int32_t>(surface_.width),
        static_cast<int32_t>(surface_.height),
        pal
    );
    // A text in a font, unless the Language settings give some of it to
    // the modern fonts, such as a Chinese briefing: those runs are laid on
    // the screen at once in the colour, on the font's baseline, and the
    // font's runs wait for copy_glyphs.
    const auto draw_text = [&](const oa::formats::fnt::Font& text_font,
                               std::string_view text,
                               int32_t x,
                               int32_t y,
                               uint8_t colour) {
        if (!renderer::needs_text_runs(text, true)) {
            std::ignore = oa::formats::fnt::raster_text(target, text_font, text, x, y);
            return;
        }
        const auto entry = static_cast<std::size_t>(colour) * 4U;
        const std::array<uint8_t, 3> rgb =
            entry + 2 < pal.size()
                ? std::array<uint8_t, 3>{pal[entry], pal[entry + 1], pal[entry + 2]}
                : std::array<uint8_t, 3>{255, 255, 255};
        const auto face = renderer::fnt_font_face(text_font);
        int32_t pen = x;
        for (const auto& run :
             renderer::split_game_text(text, renderer::fnt_font_characters(text_font), true)) {
            if (run.modern)
                if (const auto layers = oa::present::modern_text(
                        run.text, face, 1, renderer::screen_text_size(run)
                    )) {
                    oa::present::lay_text(
                        canvas, *layers, pen, y + renderer::fnt_font_baseline(text_font), rgb
                    );
                    pen += layers->advance;
                    continue;
                }
            const std::string bytes =
                run.modern ? oa::present::encode_game_text(run.text, false) : run.text;
            std::ignore = oa::formats::fnt::raster_text(target, text_font, bytes, pen, y);
            pen += static_cast<int32_t>(oa::formats::fnt::measure_text(text_font, bytes));
        }
    };
    const auto draw_line = [&](std::string_view text, int32_t x, int32_t y, uint8_t colour) {
        draw_text(font, text, x, y, colour);
    };
    for (uint32_t i = 0; i < page.row_count; ++i)
        draw_line(page.rows[i].text, page.rows[i].x, page.rows[i].y, colours[kBriefingRowColour]);
    // The wind and gravity lines, in the rows' font and colour.
    if (const auto* solar = widget("SOLARSYSTEM"); planet_art && solar != nullptr) {
        const int32_t x = solar->common.x + kSolarLabelColumn;
        const int32_t y = solar->common.y + kSolarLabelFirstRow;
        draw_line(panel.wind_label, x, y, colours[kBriefingRowColour]);
        draw_line(panel.gravity_label, x, y + kSolarLabelRowStep, colours[kBriefingRowColour]);
    }
    copy_glyphs(colours[kBriefingRowColour], fnt_font);
    // The highlighted words are drawn over their rows, all in the flash
    // colour while it shows.
    const auto since_layout = clock_milliseconds() - briefing_page_ms_;
    const bool flash = since_layout % (kHighlightShownMs + kHighlightFlashMs) >= kHighlightShownMs;
    for (const auto slot :
         {oa::ui::campaign::HighlightColor::green,
          oa::ui::campaign::HighlightColor::yellow,
          oa::ui::campaign::HighlightColor::red}) {
        bool drawn = false;
        for (uint32_t i = 0; i < page.highlight_count; ++i) {
            const auto& highlight = page.highlights[i];
            if (!flash && highlight.color != slot)
                continue;
            draw_line(
                highlight.text,
                highlight.x,
                highlight.y,
                flash ? kHighlightFlashColour : colours[static_cast<std::size_t>(slot)]
            );
            drawn = true;
        }
        if (drawn)
            copy_glyphs(
                flash ? kHighlightFlashColour : colours[static_cast<std::size_t>(slot)], fnt_font
            );
        if (flash)
            break;
    }
    // MORE... or BACK TO START on the MOREBAR, in the language shown as 3.1c
    // translates it, placed by its alignment as measured in the GUI label
    // font.
    const auto* more = widget("MOREBAR");
    const char* english = oa::ui::campaign::more_label_text(page.more);
    const char* translated = english[0] != '\0' ? game_translation(english) : nullptr;
    const std::string_view caption = translated != nullptr ? translated : english;
    if (more == nullptr || caption.empty())
        return;
    const auto& label_font =
        resources_.label_font.glyphs['I'] ? resources_.label_font : resources_.font;
    const auto width = renderer::measure_fnt_game_text(label_font, caption, true);
    const auto attributes = static_cast<uint32_t>(more->common.attributes);
    int32_t x = more->common.x;
    if ((attributes & oa::ui::gui_layout::attribute::right_aligned) != 0)
        x += more->common.width - width;
    else if ((attributes & oa::ui::gui_layout::attribute::centered) != 0)
        x += more->common.width / 2 - width / 2;
    // The caption is placed from its measured width; where the pen stops is
    // not needed. Letters the modern fonts draw taller than the bar's font,
    // such as ideographs, are lowered to rise no higher than the bar, clear
    // of the page's last row.
    const auto& more_font = briefing_more_font_ ? *briefing_more_font_ : resources_.font;
    draw_text(
        more_font,
        caption,
        x,
        more->common.y + renderer::fnt_game_text_rise(more_font, caption, true),
        colours[kBriefingMoreColour]
    );
    copy_glyphs(colours[kBriefingMoreColour], briefing_more_font_.has_value());
}

void Runtime::load_briefing_fonts() {
    const char* language = game_language();
    briefing_text_font_ = load_panel_font(
        resources_.layout,
        kFirstSideFontRecord + static_cast<int32_t>(preferences_.side),
        assets_,
        language
    );
    const auto* more = widget("MOREBAR");
    briefing_more_font_ =
        more != nullptr
            ? load_panel_font(resources_.layout, more->common.font_number, assets_, language)
            : std::nullopt;
    briefing_page_ms_ = clock_milliseconds();
}

oa::formats::fnt::Font& Runtime::briefing_text_font() {
    return briefing_text_font_ ? *briefing_text_font_ : resources_.font;
}

std::string Runtime::briefing_row(std::size_t row) {
    const auto& page = campaign_runtime().panel.page;
    if (row >= page.row_count)
        return {};
    const auto& text = page.rows[row].text;
    std::string shown(std::begin(text), std::find(std::begin(text), std::end(text), '\0'));
    if (!shown.empty() && shown.back() == '\r')
        shown.pop_back();
    return shown;
}

void Runtime::start_campaign_mission() {
    // A mod that cannot start a game says so, and the briefing stays.
    if (refuse_incomplete_mod_start())
        return;
    const auto mission_file = resolve_campaign_mission_file();
    if (mission_file.empty()) {
        status_ = "The requested campaign has no mission file.";
        return;
    }
    auto& state = campaign_runtime();
    const auto env = campaign_env(assets_, preferences_.difficulty, difficulty_names());
    if (!missions::campaign_bind_mission(
            &state.file, &env, static_cast<int32_t>(selected_mission_index_)
        )) {
        status_ = "campaign start: " + state.events.message;
        return;
    }
    campaign_mission_ = true;
    try {
        if (!load_campaign_map(mission_file)) {
            campaign_mission_ = false;
            status_ = "Campaign mission map '" + mission_file + "' is not available";
            return;
        }
        configure_campaign_players();
        // The mission info sets the unit limit from the GlobalHeader's maxunits
        // and mission start leaves it for a campaign.
        bootstrap_match(
            {.units_per_player = static_cast<uint16_t>(state.file.units_per_player),
             .place_commanders = false}
        );
        if (match_ && !altitude_sight_blocked_)
            enter_match_view();
    } catch (const std::exception& error) {
        campaign_mission_ = false;
        status_ = std::string("campaign start: ") + error.what();
        std::cerr << "unsupported operation: " << status_ << '\n';
    }
}

void Runtime::leave_load_dialog() {
    if (save_dialog_open())
        close_save_dialog();
    else if (
        (options_parent_ == Screen::match && match_) || options_parent_ == Screen::campaign_end
    )
        leave_options_screen();
    else
        load(Screen::single_player);
}

std::string Runtime::bound_mission_name() {
    const auto& file = campaign_runtime().file;
    return {file.mission_name, ::strnlen(file.mission_name, sizeof file.mission_name)};
}

std::string Runtime::bound_mission_title() {
    auto& file = campaign_runtime().file;
    char title[missions::kCampaignNameBytes];
    if (!missions::campaign_mission_title(
            &file, file.mission_index, game_language(), title, sizeof title
        ))
        return bound_mission_name();
    return title;
}

int32_t Runtime::bound_mission_index() {
    return missions::campaign_mission_index(&campaign_runtime().file);
}

void Runtime::reload_campaign_file() {
    auto& state = campaign_runtime();
    char name[missions::kCampaignNameBytes]{};
    if (const char* loaded = missions::campaign_name_if_loaded(&state.file))
        std::snprintf(name, sizeof name, "%s", loaded);
    const auto env = campaign_env(assets_, preferences_.difficulty, difficulty_names());
    if (!missions::campaign_load_file(&state.file, &env, name))
        status_ = "campaign restart: " + state.events.message;
}

bool Runtime::bind_campaign_mission(int32_t index) {
    auto& state = campaign_runtime();
    const auto env = campaign_env(assets_, preferences_.difficulty, difficulty_names());
    if (!missions::campaign_bind_mission(&state.file, &env, index)) {
        status_ = "campaign restart: " + state.events.message;
        return false;
    }
    selected_mission_index_ = static_cast<std::size_t>(index);
    return true;
}

void Runtime::restart_campaign_mission() {
    const auto parent = std::exchange(briefing_parent_, Screen::any_mission);
    start_campaign_mission();
    briefing_parent_ = parent;
}

} // namespace oa::app

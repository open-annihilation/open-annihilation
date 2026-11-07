// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The team panels of a multiplayer game over the running match: TABMENU.GUI
// (Tab), SHARE.GUI ('h' and the tab menu), ALLIES.GUI, CONTROL.GUI and the
// removal question CONTROL.GUI asks. Each loads as the match HUD over the
// running game; what they tell the other players' machines goes through the
// extension's TeamPanelHost.
#include "oa/app/runtime.hpp"
#include "panel_first_draw.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/defs/categories.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/messages.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/gui_input.hpp"
#include "oa/ui/gui_input/gadget_panel.hpp"
#include "oa/ui/gui_layout/gui_gadget.hpp"
#include "oa/present/colour_vision/colour_vision.hpp"
#include "oa/ui/hud/chat_panel.hpp"
#include "oa/ui/hud/ingame_menu.hpp"
#include "oa/ui/hud/order_panel.hpp"
#include "oa/ui/hud/shape_cues.hpp"
#include "oa/ui/hud/share_panel.hpp"
#include "oa/app/view_rules.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/sim/speed.hpp"
#include "oa/ui/hud/team_panels.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace oa::app {

namespace {

namespace hud = oa::ui::hud;

// The 640x480 screen a panel placed at a negative position counts from.
constexpr int16_t kSourceScreenWidth = 640;
constexpr int16_t kSourceScreenHeight = 480;

// Which team menu or panel is loaded as the match HUD.
enum class TeamPanel : uint8_t { none, tab_menu, share, allies, control, removal_question };

// The open team panel and what it keeps between clicks. The runtime shows one
// match at a time, so a single session suffices.
struct TeamPanelSession {
    TeamPanel panel = TeamPanel::none;
    hud::SharePanel share{};                  // SHARE.GUI's recipients and slider ranges
    std::vector<std::string> share_rows;      // PLYRLIST's rows, the recipients' names
    int32_t share_row = -1;                   // chosen PLYRLIST row, -1 for none
    int32_t metal = 0;                        // METAL slider's amount
    int32_t energy = 0;                       // ENERGY slider's amount
    uint8_t removal_player = OA_PLAYER_COUNT; // player the removal question names
    std::string translated;                   // the last text the panels translated
};

/// Returns the team panel session, created on first use.
///
/// @return the session
TeamPanelSession& team_session() {
    static TeamPanelSession session;
    return session;
}

/// Compares two names without regard to case.
///
/// @param left one name
/// @param right the other
/// @return whether they match
bool same_name(std::string_view left, std::string_view right) {
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
               return std::toupper(static_cast<unsigned char>(a)) ==
                      std::toupper(static_cast<unsigned char>(b));
           });
}

/// Tells whether a name is a prefix and a number ("LOGO3", "LIVEALLY12"),
/// without regard to case.
///
/// @param name the name
/// @param prefix the prefix
/// @return whether it is
bool numbered(std::string_view name, std::string_view prefix) {
    if (name.size() <= prefix.size() || !same_name(name.substr(0, prefix.size()), prefix))
        return false;
    return std::all_of(
        name.begin() + static_cast<std::ptrdiff_t>(prefix.size()), name.end(), [](char c) {
            return std::isdigit(static_cast<unsigned char>(c)) != 0;
        }
    );
}

/// Places a panel whose root lies at a negative position from the bottom or
/// right edge of the 640x480 screen, as the game places it.
///
/// @param[in,out] layout the loaded panel, its records already at their root's origin
void place_from_edges(oa::ui::gui_layout::Layout& layout) {
    if (layout.gadgets.empty())
        return;
    const auto& root = layout.gadgets.front().common;
    const int16_t dx = root.x < 0 ? kSourceScreenWidth : 0;
    const int16_t dy = root.y < 0 ? kSourceScreenHeight : 0;
    if (dx == 0 && dy == 0)
        return;
    for (auto& gadget : layout.gadgets) {
        gadget.common.x = static_cast<int16_t>(gadget.common.x + dx);
        gadget.common.y = static_cast<int16_t>(gadget.common.y + dy);
    }
}

/// The share panel's added switches and the command each gives.
struct ShareSwitch {
    const char* gadget{};
    const char* command{};
};

constexpr ShareSwitch kShareSwitches[] = {
    {"EN_SHAREMETAL", "+sharemetal"},
    {"EN_SHAREENERGY", "+shareenergy"},
    {"EN_SHOOTALL", "+shootall"},
    {"EN_NOSHAKE", "+noshake"},
};

/// The share panel's added threshold sliders, their readouts and commands.
struct ShareThreshold {
    const char* slider{};
    const char* readout{};
    const char* command{};
    bool metal{};
};

/// The share panel's added game speed slider, and its stops past 0.
constexpr std::string_view kShareSpeedSlider = "SRL_GAMESPEED";
constexpr int16_t kShareSpeedStops = 20;
constexpr ShareThreshold kShareThresholds[] = {
    {"SRL_SETSHRMETAL", "SM#", "+setsharemetal %d", true},
    {"SRL_SETSHAREGRY", "SE#", "+setshareenergy %d", false},
};

/// The per-player state SHARE.GUI works over, gathered from a World.
struct ShareView {
    std::array<oa::UnitEconomy*, OA_PLAYER_COUNT> economies{};
    std::array<uint8_t, OA_PLAYER_COUNT> setup_flags{};
    hud::ShareWorld world{};

    /// Gathers the players' economy blocks and setup options.
    ///
    /// @param[in,out] match_world the running match's World
    explicit ShareView(oa::World& match_world) {
        for (std::size_t i = 0; i < OA_PLAYER_COUNT; ++i) {
            oa::Player& player = match_world.game.players[i];
            economies[i] = oa::world_player_economy(&match_world, &player);
            if (const auto* info = oa::world_player_info(&match_world, &player); info != nullptr)
                setup_flags[i] = static_cast<uint8_t>(info->options);
        }
        world = {
            match_world.game.players,
            economies.data(),
            setup_flags.data(),
            match_world.game.local_player_index,
            match_world.game.difficulty
        };
    }

    ShareView(const ShareView&) = delete;
    ShareView& operator=(const ShareView&) = delete;
};

/// Returns a button's fields when a match HUD record is a button.
///
/// @param gadget the record
/// @return its button fields, or null
oa::ui::gui_layout::ButtonFields* button_of(oa::ui::gui_layout::Gadget& gadget) {
    return std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
}

} // namespace

void assign_button_quick_key(std::vector<oa::ui::gui_layout::Gadget>& gadgets, std::size_t index) {
    auto* button = button_of(gadgets[index]);
    if (button == nullptr)
        return;
    const auto rule = oa::ui::gui_input::caption_quick_key(
        static_cast<uint32_t>(gadgets[index].common.attributes), button->stages, button->text
    );
    if (rule == oa::ui::gui_input::CaptionQuickKey::keep)
        return;
    button->quick_key = 0;
    if (rule == oa::ui::gui_input::CaptionQuickKey::none)
        return;
    std::vector<int8_t> taken;
    for (auto& other : gadgets)
        if (const auto* fields = button_of(other))
            taken.push_back(fields->quick_key);
    button->quick_key = static_cast<int8_t>(oa::ui::gui_input::free_quick_key(button->text, taken));
}

bool Runtime::multiplayer_session() const {
    return (current_extension_state() & extension_state::multiplayer) != 0;
}

bool Runtime::team_panel_open() const {
    return team_session().panel != TeamPanel::none;
}

void Runtime::forget_team_panel() {
    auto& session = team_session();
    if (session.panel == TeamPanel::none)
        return;
    session = {};
    if (!match_)
        return;
    auto& game = match_->state().game;
    game.gui_flags = static_cast<uint8_t>(game.gui_flags & ~hud::kGuiFlagsMenuOpen);
    game.frame_flags = static_cast<uint16_t>(
        game.frame_flags & ~(hud::kFrameSharePanelOpen | hud::kFrameAlliesPanelOpen)
    );
}

bool Runtime::load_team_panel(const char* file, bool back_tile_face) {
    if (!match_ || !load_match_hud_layout(oa::data::defs::gui_path(file)))
        return false;
    place_from_edges(match_hud_->layout);
    if (back_tile_face)
        draw_match_panel_back_tile();
    // The loader gives the focus from where the panel is placed.
    match_hud_focus_ = loaded_panel_focus();
    // The panel's first draw, where it is placed, binds its scroll bars.
    bind_hud_scrolls(1);
    match_paused_ = true;
    match_command_ = MatchCommand::none;
    pending_build_type_ = 0;
    return true;
}

oa::ui::hud::PanelControls Runtime::team_panel_controls() {
    hud::PanelControls controls{};
    controls.user = this;
    controls.find = [](void* user, const char* name) -> int32_t {
        const auto& self = *static_cast<Runtime*>(user);
        if (!self.match_hud_ || name == nullptr)
            return -1;
        const auto& gadgets = self.match_hud_->layout.gadgets;
        for (std::size_t index = 1; index < gadgets.size(); ++index)
            if (same_name(gadgets[index].common.name, name))
                return static_cast<int32_t>(index);
        return -1;
    };
    controls.set_value = [](void* user, int32_t index, int32_t value) {
        auto& self = *static_cast<Runtime*>(user);
        const auto& name =
            self.match_hud_->layout.gadgets[static_cast<std::size_t>(index)].common.name;
        const auto stage = static_cast<std::size_t>(std::max(value, 0));
        self.widget_text_stages_[name] = stage;
        self.widget_gaf_frames_[name] = stage;
    };
    controls.value = [](void* user, int32_t index) -> int32_t {
        const auto& self = *static_cast<Runtime*>(user);
        const auto& name =
            self.match_hud_->layout.gadgets[static_cast<std::size_t>(index)].common.name;
        const auto found = self.widget_text_stages_.find(name);
        return found != self.widget_text_stages_.end() ? static_cast<int32_t>(found->second) : 0;
    };
    // A button takes the quick key its new caption gives it, as the gadget
    // engine gives one when a caption is set.
    controls.set_text = [](void* user, int32_t index, const char* text) {
        auto& gadgets = static_cast<Runtime*>(user)->match_hud_->layout.gadgets;
        auto& gadget = gadgets[static_cast<std::size_t>(index)];
        if (auto* button = button_of(gadget)) {
            button->text = text;
            assign_button_quick_key(gadgets, static_cast<std::size_t>(index));
        } else if (auto* label = std::get_if<oa::ui::gui_layout::LabelFields>(&gadget.fields))
            label->text = text;
    };
    controls.set_active = [](void* user, int32_t index, bool shown) {
        auto& gadget = static_cast<Runtime*>(user)
                           ->match_hud_->layout.gadgets[static_cast<std::size_t>(index)];
        gadget.common.active = shown ? 1 : 0;
    };
    controls.set_grayed = [](void* user, int32_t index, bool grayed) {
        auto& self = *static_cast<Runtime*>(user);
        auto& gadget = self.match_hud_->layout.gadgets[static_cast<std::size_t>(index)];
        if (auto* button = button_of(gadget))
            button->grayed_out = grayed;
        if (static_cast<std::size_t>(index) < self.match_hud_states_.size())
            self.match_hud_states_[static_cast<std::size_t>(index)].grayed = grayed;
    };
    controls.disable = [](void* user, int32_t index) {
        auto& self = *static_cast<Runtime*>(user);
        if (auto* button =
                button_of(self.match_hud_->layout.gadgets[static_cast<std::size_t>(index)]))
            button->grayed_out = true;
        if (static_cast<std::size_t>(index) < self.match_hud_states_.size())
            self.match_hud_states_[static_cast<std::size_t>(index)].grayed = true;
    };
    // A renamed control keeps its stage and frame under its new name.
    controls.set_name = [](void* user, int32_t index, const char* name) {
        auto& self = *static_cast<Runtime*>(user);
        auto& gadget = self.match_hud_->layout.gadgets[static_cast<std::size_t>(index)];
        const std::string old = gadget.common.name;
        for (auto* map : {&self.widget_text_stages_, &self.widget_gaf_frames_}) {
            map->erase(name);
            if (const auto found = map->find(old); found != map->end()) {
                const auto kept = found->second;
                map->erase(found);
                (*map)[name] = kept;
            }
        }
        gadget.common.name = name;
    };
    return controls;
}

void Runtime::present_team_panel(
    std::vector<renderer::ButtonPresentation>& presentation,
    std::vector<renderer::ListPresentation>& lists
) const {
    const auto& session = team_session();
    if (session.panel == TeamPanel::none || !match_hud_)
        return;
    const auto stage_of = [this](const std::string& name) -> std::optional<std::size_t> {
        const auto found = widget_text_stages_.find(name);
        return found != widget_text_stages_.end() ? std::optional<std::size_t>(found->second)
                                                  : std::nullopt;
    };
    for (auto& entry : presentation) {
        // A player's logo shows its colour's frame of the game's logos.
        if (numbered(entry.name, "LOGO")) {
            if (const auto frame = widget_gaf_frames_.find(entry.name);
                frame != widget_gaf_frames_.end()) {
                entry.gaf_frame = frame->second;
                entry.sprite =
                    renderer::SpriteOverride{renderer::SpriteArchive::global, "32xlogos"};
            }
            continue;
        }
        // The rows' toggles and team icons keep their templates' art under
        // their row names.
        const char* sequence = numbered(entry.name, "LIVEALLY") || numbered(entry.name, "ALLY")
                                   ? "ALLYx"
                               : numbered(entry.name, "TEAMICONS") ? "TEAMICONSx"
                                                                   : nullptr;
        if (sequence == nullptr)
            continue;
        entry.sprite = renderer::SpriteOverride{renderer::SpriteArchive::screen, sequence};
        entry.gaf_frame = stage_of(entry.name).value_or(0);
    }
    if (session.panel == TeamPanel::share)
        lists.push_back(
            {"PLYRLIST",
             session.share_rows,
             share_list_first(),
             session.share_row >= 0 ? std::optional<std::size_t>(session.share_row) : std::nullopt}
        );
}

std::size_t Runtime::share_list_first() const {
    if (!match_hud_ || hud_scrolls_layout_ != match_hud_->layout.gadgets.data())
        return 0;
    for (const auto& entry : hud_scrolls_.lists)
        if (entry.gadget < match_hud_->layout.gadgets.size() &&
            match_hud_->layout.gadgets[entry.gadget].common.name == "PLYRLIST")
            return static_cast<std::size_t>(std::max<int16_t>(0, entry.list.first));
    return 0;
}

void Runtime::share_bar_moved(std::size_t gadget) {
    auto& session = team_session();
    auto* scrolls = hud_scrolls();
    if (session.panel != TeamPanel::share || scrolls == nullptr ||
        gadget >= match_hud_->layout.gadgets.size())
        return;
    const auto* entry = renderer::find_layout_bar(*scrolls, gadget);
    const auto& name = match_hud_->layout.gadgets[gadget].common.name;
    // The profile's game speed slider asks for its speed, as the speed keys
    // do, when it moved off the speed asked for.
    if (entry != nullptr && name == kShareSpeedSlider && match_) {
        auto& world = match_->state();
        const auto speed = static_cast<uint16_t>(entry->bar.knob);
        if (speed == world.game.requested_speed)
            return;
        std::ignore = oa::sim::speed::set_speed(world, speed, message_hooks());
        match_timing_.requested_rate = world.game.requested_speed;
        match_timing_.actual_rate = world.game.current_speed;
        preferences_.current_game_speed = world.game.current_speed;
        call_hook_or_report<&Extension::speed_changed>(
            extension_, hook_error_report(), *this, world.game.requested_speed
        );
        return;
    }
    // The profile's threshold sliders show the threshold they stand for,
    // when it differs from the player's.
    for (const auto& threshold : kShareThresholds) {
        if (entry == nullptr || name != threshold.slider || !match_)
            continue;
        const auto& world = match_->state();
        const auto& me = world.game.players[world.game.local_player_index % OA_PLAYER_COUNT];
        const float storage = threshold.metal ? me.metal_storage : me.energy_storage;
        const float current =
            threshold.metal ? me.metal_share_threshold : me.energy_share_threshold;
        const auto value =
            view_rules::share_threshold_value(entry->bar.knob, entry->bar.range, storage);
        const auto controls = team_panel_controls();
        if (const auto label = hud::find_control(controls, threshold.readout);
            label != -1 && value != static_cast<int32_t>(current))
            controls.set_text(controls.user, label, std::to_string(value).c_str());
        return;
    }
    // PLYRLIST's bar has scrolled the list itself.
    if (entry == nullptr || (name != "METAL" && name != "ENERGY"))
        return;
    // The counter shows the amount the knob stands for.
    const bool metal = name == "METAL";
    const auto amount = hud::slider_amount(
        entry->bar.knob,
        entry->bar.range,
        metal ? session.share.metal_max : session.share.energy_max
    );
    (metal ? session.metal : session.energy) = amount;
    char text[16];
    hud::format_share_amount(text, sizeof text, amount);
    const auto controls = team_panel_controls();
    if (const auto label = hud::find_control(controls, metal ? "METAL#" : "ENERGY#"); label != -1)
        controls.set_text(controls.user, label, text);
}

bool Runtime::team_menu_every_game() const {
    return match_ && match_->rules().teams.alliance_menu_all_game_types.enabled;
}

void Runtime::toggle_team_menu() {
    if (!match_ || match_finished_ || (!multiplayer_session() && !team_menu_every_game()))
        return;
    const hud::HudEvents events{
        this,
        [](void* user, const char* name) { static_cast<Runtime*>(user)->play_ui_sound(name, 0); },
        nullptr
    };
    // Tab over any team panel closes it, as over the tab menu itself.
    if (team_panel_open()) {
        hud::play_sound(events, "SmallButton");
        resume_match_pause();
        return;
    }
    auto& world = match_->state();
    hud::PanelLoader loader{};
    loader.user = this;
    loader.is_loaded = [](void*, const char*) {
        return team_session().panel == TeamPanel::tab_menu;
    };
    loader.close_to_root = [](void* user) {
        static_cast<Runtime*>(user)->resume_match_pause();
        return true;
    };
    loader.load = [](void* user, const char* name, const oa::Unit*, int32_t) {
        auto& self = *static_cast<Runtime*>(user);
        self.keep_panel_below_darkened();
        self.forget_team_panel();
        if (!self.load_team_panel(name, true))
            return false;
        team_session().panel = TeamPanel::tab_menu;
        return true;
    };
    // CONTROL is the host's, and withheld in a tournament game.
    const bool opened = hud::toggle_tab_menu(
        world,
        multiplayer_session() ? oa::data::campaign::SessionKind::multiplayer : match_session_kind(),
        hud::control_offered(world, team_panel_host_),
        loader,
        team_panel_controls(),
        events,
        team_menu_every_game()
    );
    if (opened) {
        // The tab menu gives the match's panels the keyboard until the match
        // resumes.
        match_panels_keyboard_ = true;
        status_ = "Team menu";
        render_match_surface();
    } else if (!team_panel_open()) {
        // The menu could not load: nothing holds the Game.gui_flags bit.
        world.game.gui_flags = static_cast<uint8_t>(world.game.gui_flags & ~hud::kGuiFlagsMenuOpen);
    }
}

void Runtime::open_team_share_panel() {
    if (!match_ || match_finished_ || !multiplayer_session())
        return;
    auto& world = match_->state();
    hud::SharePanel panel{};
    {
        ShareView view(world);
        uint16_t frame_flags = world.game.frame_flags;
        if (!hud::open_share_panel(view.world, panel, frame_flags))
            return;
    }
    // With nobody to share with the panel closes as it opens.
    if (panel.recipient_count == 0)
        return;
    keep_panel_below_darkened();
    forget_team_panel();
    if (!load_team_panel("SHARE.GUI", true))
        return;
    world.game.frame_flags =
        static_cast<uint16_t>(world.game.frame_flags | hud::kFrameSharePanelOpen);
    auto& session = team_session();
    session.panel = TeamPanel::share;
    session.share = panel;
    session.share_rows.clear();
    for (int32_t row = 0; row < panel.recipient_count; ++row) {
        const oa::Player& player = world.game.players[panel.recipients[row]];
        session.share_rows.emplace_back(player.name, strnlen(player.name, sizeof player.name));
    }
    session.share_row = 0;
    session.metal = 0;
    session.energy = 0;
    // METAL and ENERGY take a knob as long as the bar is high and the bar's
    // width less that as their positions, and run from 0 to the whole store;
    // PLYRLIST is filled with the recipients.
    if (auto* scrolls = hud_scrolls()) {
        for (auto& entry : scrolls->bars) {
            const auto& name = match_hud_->layout.gadgets[entry.gadget].common.name;
            if (name != "METAL" && name != "ENERGY")
                continue;
            auto& bar = entry.bar;
            bar.knob_size = bar.rect.height;
            bar.range = static_cast<int16_t>(bar.rect.width - bar.rect.height);
            bar.maximum = name == "METAL" ? panel.metal_max : panel.energy_max;
            oa::ui::gui_input::scroll_set_value(bar, 0);
            auto& fields = std::get<oa::ui::gui_layout::ScrollBarFields>(
                match_hud_->layout.gadgets[entry.gadget].fields
            );
            fields.knob_position = bar.knob;
            fields.knob_size = bar.knob_size;
            fields.range = bar.range;
        }
        for (std::size_t index = 1; index < match_hud_->layout.gadgets.size(); ++index)
            if (match_hud_->layout.gadgets[index].common.name == "PLYRLIST")
                renderer::fill_layout_list(
                    *scrolls,
                    match_hud_->layout,
                    index,
                    static_cast<int32_t>(session.share_rows.size())
                );
    }
    const auto controls = team_panel_controls();
    char amount[16];
    hud::format_share_amount(amount, sizeof amount, 0);
    for (const char* label : {"METAL#", "ENERGY#"})
        if (const auto index = hud::find_control(controls, label); index != -1)
            controls.set_text(controls.user, index, amount);
    for (const char* box : {"SHARUNIT", "MAPINFO"}) {
        widget_text_stages_.erase(box);
        widget_gaf_frames_.erase(box);
    }
    if (ui_rules().share_dialog_and_lobby_buttons.enabled)
        prepare_share_dialog_extras();
    status_ = "Share";
    render_match_surface();
}

void Runtime::prepare_share_dialog_extras() {
    if (!match_ || !match_hud_)
        return;
    auto& world = match_->state();
    const auto& me = world.game.players[world.game.local_player_index % OA_PLAYER_COUNT];
    const auto* info = oa::world_player_info(&world, &me);
    const auto share_bits =
        info != nullptr
            ? static_cast<uint16_t>(info->role | (uint16_t{info->reserved_after_role} << 8))
            : uint16_t{0};
    const auto flags = oa::ui::console::console_flags(world.game);
    const auto controls = team_panel_controls();
    const auto set_stage = [&](const char* gadget, bool on) {
        if (const auto index = hud::find_control(controls, gadget); index != -1)
            controls.set_value(controls.user, index, on ? 1 : 0);
    };
    namespace console = oa::ui::console;
    set_stage("EN_SHAREMETAL", (share_bits & console::share_flag::metal) != 0);
    set_stage("EN_SHAREENERGY", (share_bits & console::share_flag::energy) != 0);
    set_stage("EN_SHOOTALL", (flags & console::console_flag::shoot_all) != 0);
    set_stage("EN_NOSHAKE", (flags & console::console_flag::no_shake) != 0);
    auto* scrolls = hud_scrolls();
    for (const auto& threshold : kShareThresholds) {
        const float storage = threshold.metal ? me.metal_storage : me.energy_storage;
        const float current =
            threshold.metal ? me.metal_share_threshold : me.energy_share_threshold;
        if (scrolls != nullptr)
            for (auto& entry : scrolls->bars) {
                if (match_hud_->layout.gadgets[entry.gadget].common.name != threshold.slider)
                    continue;
                auto& bar = entry.bar;
                bar.knob_size = bar.rect.height;
                bar.range = static_cast<int16_t>(bar.rect.width - bar.rect.height);
                bar.maximum = static_cast<int32_t>(storage);
                bar.knob = view_rules::share_threshold_knob(current, storage, bar.range);
                auto& fields = std::get<oa::ui::gui_layout::ScrollBarFields>(
                    match_hud_->layout.gadgets[entry.gadget].fields
                );
                fields.knob_position = bar.knob;
                fields.knob_size = bar.knob_size;
                fields.range = bar.range;
            }
        if (const auto label = hud::find_control(controls, threshold.readout); label != -1)
            controls.set_text(
                controls.user, label, std::to_string(static_cast<int32_t>(current)).c_str()
            );
    }
    // A game speed slider, when the dialog's GUI has one, runs over the
    // speeds with its knob at the speed asked for.
    if (scrolls != nullptr)
        for (auto& entry : scrolls->bars) {
            if (match_hud_->layout.gadgets[entry.gadget].common.name != kShareSpeedSlider)
                continue;
            auto& bar = entry.bar;
            bar.knob_size = 1;
            bar.range = kShareSpeedStops;
            bar.maximum = kShareSpeedStops;
            bar.knob = static_cast<int16_t>(
                std::clamp<int32_t>(world.game.requested_speed, 0, kShareSpeedStops)
            );
            auto& fields = std::get<oa::ui::gui_layout::ScrollBarFields>(
                match_hud_->layout.gadgets[entry.gadget].fields
            );
            fields.knob_position = bar.knob;
            fields.knob_size = bar.knob_size;
            fields.range = bar.range;
        }
}

bool Runtime::share_dialog_extras_click(const std::string& name) {
    if (!match_ || !ui_rules().share_dialog_and_lobby_buttons.enabled)
        return false;
    const auto controls = team_panel_controls();
    for (const auto& toggle : kShareSwitches) {
        if (name != toggle.gadget)
            continue;
        if (const auto index = hud::find_control(controls, toggle.gadget); index != -1)
            controls.set_value(
                controls.user, index, controls.value(controls.user, index) == 0 ? 1 : 0
            );
        give_view_command(toggle.command, false);
        render_match_surface();
        return true;
    }
    if (name == "EN_READY") {
        give_view_command(".ready", true);
        render_match_surface();
        return true;
    }
    if (name != "OK")
        return false;
    // OK gives the thresholds moved off the player's, then closes as before.
    const auto& world = match_->state();
    const auto& me = world.game.players[world.game.local_player_index % OA_PLAYER_COUNT];
    if (auto* scrolls = hud_scrolls())
        for (const auto& threshold : kShareThresholds)
            for (const auto& entry : scrolls->bars) {
                if (match_hud_->layout.gadgets[entry.gadget].common.name != threshold.slider)
                    continue;
                const float storage = threshold.metal ? me.metal_storage : me.energy_storage;
                const float current =
                    threshold.metal ? me.metal_share_threshold : me.energy_share_threshold;
                const auto value =
                    view_rules::share_threshold_value(entry.bar.knob, entry.bar.range, storage);
                if (value == static_cast<int32_t>(current))
                    continue;
                char line[64];
                std::snprintf(line, sizeof line, threshold.command, static_cast<int>(value));
                give_view_command(line, false);
            }
    return false;
}

void Runtime::open_allies_team_panel() {
    if (!match_ || match_finished_ || (!multiplayer_session() && !team_menu_every_game()))
        return;
    keep_panel_below_darkened();
    forget_team_panel();
    if (!load_team_panel("ALLIES.GUI", true))
        return;
    team_session().panel = TeamPanel::allies;
    hud::open_allies_panel(match_->state(), team_panel_controls());
    status_ = "Allies";
    render_match_surface();
}

void Runtime::open_control_team_panel(bool darken_panel_below) {
    if (!match_ || match_finished_ || !multiplayer_session() || local_player_watches())
        return;
    if (darken_panel_below)
        keep_panel_below_darkened();
    forget_team_panel();
    if (!load_team_panel("CONTROL.GUI", true))
        return;
    team_session().panel = TeamPanel::control;
    // It refuses only a watching player, who never gets here.
    std::ignore = hud::open_control_panel(match_->state(), team_panel_controls());
    status_ = "Control";
    render_match_surface();
}

void Runtime::open_removal_question(uint8_t player) {
    if (!match_ || match_finished_)
        return;
    // 3.1c asks over CONTROL.GUI, left as it is, centring the question on the
    // whole screen over the BackTile face as YESORNO.GUI names no picture.
    auto under = panel_under_dialog();
    forget_team_panel();
    if (!load_team_panel("YESORNO.GUI", false))
        return;
    place_match_panel(oa::ui::gui_input::panel_flag::centre, true);
    match_panel_under_ = std::move(under);
    auto& session = team_session();
    session.panel = TeamPanel::removal_question;
    session.removal_player = player;
    hud::open_removal_question(
        match_->state(),
        player,
        team_panel_controls(),
        [](void* context, const char* text) -> const char* {
            auto& runtime = *static_cast<Runtime*>(context);
            auto& translated = team_session().translated;
            translated = runtime.translate_ui(text);
            return translated.c_str();
        },
        this
    );
    render_match_surface();
}

void Runtime::give_selected_units_to(uint8_t recipient) {
    if (!match_)
        return;
    auto& world = match_->state();
    // Commanders stay with their player: the COMMANDER category's types.
    const auto* commanders =
        oa::data::defs::category_registry_find(&unit_table_.tables.categories, "Commander");
    const hud::UnitTransfer transfer{
        this, [](void* user, oa::Unit& unit, oa::Player& to) {
            auto& self = *static_cast<Runtime*>(user);
            auto& match_world = self.match_->state();
            self.match_->share_gift_unit(
                static_cast<uint16_t>(oa::world_unit_slot(&match_world, &unit)), to.index
            );
        }
    };
    // A mod's rules may hold structures back when many went across lately.
    match_->begin_share_gift();
    hud::give_selected_units(
        world, recipient, commanders != nullptr ? commanders->words : nullptr, transfer
    );
    const auto outcome = match_->end_share_gift();
    if (outcome.waiting != 0) {
        char line[96];
        std::snprintf(
            line,
            sizeof line,
            "Sharing %d structures - transfer will complete in %d seconds.",
            static_cast<int>(outcome.waiting),
            static_cast<int>(outcome.wait_seconds)
        );
        oa::sim::messages::post_message(
            world,
            line,
            oa::sim::messages::kind_unit_report,
            0,
            oa::sim::messages::sender_none,
            message_hooks()
        );
    }
}

void Runtime::click_team_panel(std::string_view clicked) {
    if (!match_ || !match_hud_)
        return;
    auto& session = team_session();
    auto& world = match_->state();
    const std::string name(clicked);
    const hud::HudEvents events{
        this,
        [](void* user, const char* sound) { static_cast<Runtime*>(user)->play_ui_sound(sound, 0); },
        nullptr
    };
    const auto controls = team_panel_controls();
    const auto index = hud::find_control(controls, name.c_str());
    // A two-stage toggle the panel reads (allied victory, the share panel's
    // boxes) turns over as it is clicked.
    if (index != -1 && (name == "VICTORY" || name == "SHARUNIT" || name == "MAPINFO")) {
        const auto stage = controls.value(controls.user, index);
        controls.set_value(controls.user, index, stage == 0 ? 1 : 0);
    }
    switch (session.panel) {
    case TeamPanel::none:
        return;
    case TeamPanel::tab_menu: {
        auto& game = world.game;
        const auto action =
            hud::ingame_menu_click(name.c_str(), game.gui_flags, game.frame_flags, events);
        switch (action) {
        case hud::IngameMenuAction::options:
            // The runtime's options menu keeps no Game.frame_flags bit of its own.
            game.frame_flags = static_cast<uint16_t>(game.frame_flags & ~hud::kFrameIngameOptions);
            show_match_pause_menu();
            return;
        case hud::IngameMenuAction::share:
            open_team_share_panel();
            return;
        case hud::IngameMenuAction::allies:
            open_allies_team_panel();
            return;
        case hud::IngameMenuAction::control:
            open_control_team_panel();
            return;
        case hud::IngameMenuAction::closed:
            resume_match_pause();
            return;
        case hud::IngameMenuAction::cancel:
        case hud::IngameMenuAction::none:
            render_match_surface();
            return;
        }
        return;
    }
    case TeamPanel::share: {
        // The profile's share dialog takes its own gadgets first.
        if (share_dialog_extras_click(name))
            return;
        const auto source = oa::ui::display_layout::canvas_to_source(
            match_layout_, static_cast<int>(match_pointer_x_), static_cast<int>(match_pointer_y_)
        );
        // METAL and ENERGY are never clicked: their bars and arrows take the
        // pointer (share_bar_moved).
        if (index != -1 && (name == "METAL" || name == "ENERGY"))
            return;
        if (index != -1 && name == "PLYRLIST") {
            const auto& gadget = match_hud_->layout.gadgets[static_cast<std::size_t>(index)];
            auto item_height =
                static_cast<int32_t>(oa::formats::fnt::line_height(match_hud_->font)) + 1;
            if (const auto* list = std::get_if<oa::ui::gui_layout::ListBoxFields>(&gadget.fields);
                list != nullptr && list->item_height > 0)
                item_height = list->item_height;
            const auto row = (source.y - gadget.common.y - 2) / std::max(item_height, 1) +
                             static_cast<int32_t>(share_list_first());
            if (source.y >= gadget.common.y + 2 && row < session.share.recipient_count)
                session.share_row = row;
            render_match_surface();
            return;
        }
        ShareView view(world);
        hud::ShareHost host{};
        host.user = this;
        host.send_metal = [](void* user, uint8_t from, uint8_t to, float amount) {
            const auto& panels = static_cast<Runtime*>(user)->team_panel_host_;
            if (panels.resources_given != nullptr)
                panels.resources_given(panels.context, from, to, true, amount);
        };
        host.send_energy = [](void* user, uint8_t from, uint8_t to, float amount) {
            const auto& panels = static_cast<Runtime*>(user)->team_panel_host_;
            if (panels.resources_given != nullptr)
                panels.resources_given(panels.context, from, to, false, amount);
        };
        host.give_units = [](void* user, uint8_t, uint8_t to) {
            static_cast<Runtime*>(user)->give_selected_units_to(to);
        };
        host.share_map = [](void* user, uint8_t from, uint8_t to) {
            auto& runtime = *static_cast<Runtime*>(user);
            // The receiver takes the giver's map here first.
            if (runtime.match_)
                runtime.match_->share_mapped_area(from, to);
            const auto& panels = runtime.team_panel_host_;
            if (panels.sight_shared != nullptr)
                panels.sight_shared(panels.context, from, to);
        };
        const auto stage = [&](const char* box) {
            const auto found = hud::find_control(controls, box);
            return found != -1 && controls.value(controls.user, found) != 0;
        };
        const auto click = hud::share_panel_click(
            view.world,
            session.share,
            name.c_str(),
            session.share_row,
            session.metal,
            session.energy,
            stage("SHARUNIT"),
            stage("MAPINFO"),
            world.game.frame_flags,
            events,
            host
        );
        // OK and CANCEL close the panel once they have done their part.
        if (click == hud::ShareClick::closed || name == "OK" || name == "CANCEL")
            resume_match_pause();
        else
            render_match_surface();
        return;
    }
    case TeamPanel::allies: {
        const auto result =
            hud::allies_panel_click(world, name.c_str(), controls, events, team_panel_host_);
        // The simulation's alliances follow the players' alliance rows.
        for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player)
            match_->follow_player_alliances(player);
        if (result.announcement[0] != '\0') {
            const auto& speaker =
                world.game.players[world.game.local_player_index % OA_PLAYER_COUNT];
            oa::sim::messages::post_chat(
                world, speaker, result.announcement, hud::kMessageKindChat, nullptr, message_hooks()
            );
        }
        if (result.click == hud::TeamPanelClick::closed)
            resume_match_pause();
        else
            render_match_surface();
        return;
    }
    case TeamPanel::control: {
        const auto result =
            hud::control_panel_click(world, name.c_str(), controls, events, team_panel_host_);
        if (result.click == hud::TeamPanelClick::confirm_removal)
            open_removal_question(result.player);
        else if (result.click == hud::TeamPanelClick::closed)
            resume_match_pause();
        else
            render_match_surface();
        return;
    }
    case TeamPanel::removal_question: {
        const auto result =
            hud::removal_question_click(name.c_str(), session.removal_player, team_panel_host_);
        if (result.click != hud::TeamPanelClick::closed) {
            render_match_surface();
            return;
        }
        // Answered, the question gives way to the CONTROL.GUI it was asked
        // over, its rows as the players now stand, over the same panels below.
        open_control_team_panel(false);
        if (team_session().panel != TeamPanel::control)
            resume_match_pause();
        return;
    }
    }
}

void Runtime::stamp_colour_cell_markers(
    renderer::Surface& surface,
    const oa::ui::gui_layout::Layout& layout,
    std::span<const uint8_t> palette,
    bool shape_only
) {
    if (!oa::present::colour_vision::shape_cues_active())
        return;
    if (surface.width == 0 || surface.height == 0 || surface.rgb.empty())
        return;
    const int picture_w = static_cast<int>(surface.width);
    const int picture_h = static_cast<int>(surface.height);
    for (const auto& gadget : layout.gadgets) {
        const auto& common = gadget.common;
        if (common.active == 0 || common.width <= 0 || common.height <= 0)
            continue;
        // LOGO and one digit, or Color and one digit. A team panel numbers
        // LOGO by the row; the row's LIVEPLYR line carries the seat.
        const auto same_letter = [](char value, char upper) {
            const auto lower = static_cast<char>(upper - 'A' + 'a');
            return value == upper || value == lower;
        };
        const auto named = [&](std::string_view name, std::string_view prefix) {
            if (name.size() != prefix.size() + 1)
                return false;
            for (std::size_t letter = 0; letter < prefix.size(); ++letter)
                if (!same_letter(name[letter], prefix[letter]))
                    return false;
            const char digit = name.back();
            return digit >= '0' && digit <= '9';
        };
        const bool logo = named(common.name, "LOGO");
        if (!logo && !named(common.name, "Color"))
            continue;
        int player = common.name.back() - '0';
        bool seated = false;
        if (logo) {
            const int top = common.y;
            const int bottom = static_cast<int>(common.y) + common.height;
            for (const auto& other : layout.gadgets) {
                if (!named(other.common.name, "LIVEPLYR"))
                    continue;
                const int row_top = other.common.y;
                const int row_bottom = static_cast<int>(other.common.y) + other.common.height;
                if (row_bottom <= top || bottom <= row_top)
                    continue;
                player = other.common.name.back() - '0';
                seated = true;
                break;
            }
        }
        // The colour number, not the pixels. A team panel's row shows the
        // seat's colour. A battle-room LOGO and a skirmish Color show that
        // frame: the control's stage, or the frame this screen stored.
        int colour_number = -1;
        if (seated && match_ && player >= 0 && player < OA_PLAYER_COUNT) {
            const auto& world = match_->state();
            const PlayerSetupInfo* info = world_player_info(&world, &world.game.players[player]);
            if (info != nullptr && info->color < OA_PLAYER_COUNT)
                colour_number = info->color;
        }
        if (colour_number < 0 && !logo) {
            const auto frame = widget_gaf_frames_.find(common.name);
            if (frame != widget_gaf_frames_.end() && frame->second < OA_PLAYER_COUNT)
                colour_number = static_cast<int>(frame->second);
        }
        if (colour_number < 0 && logo) {
            if (const std::optional<uint8_t> stage =
                    oa::ui::frontend_multiplayer::multiplayer_control_stage(common.name);
                stage.has_value() && *stage < OA_PLAYER_COUNT)
                colour_number = *stage;
            else if (
                const auto frame = widget_gaf_frames_.find(common.name);
                frame != widget_gaf_frames_.end() && frame->second < OA_PLAYER_COUNT
            )
                colour_number = static_cast<int>(frame->second);
        }
        if (colour_number < 0)
            continue;
        const uint8_t index = oa::ui::hud::colour_dot_index(static_cast<uint8_t>(colour_number));
        const auto entry = static_cast<std::size_t>(index) * 4U;
        if (entry + 2 >= palette.size())
            continue;
        const uint8_t red = palette[entry];
        const uint8_t green = palette[entry + 1];
        const uint8_t blue = palette[entry + 2];
        // A name bar is wide enough for the marker at its left, before the
        // letters. A colour square holds the marker in its middle. The plate
        // keeps a filled shape visible on the cell's own colour. The battle
        // room draws the shape alone, so the version on the cell stays readable.
        const int centre_y = static_cast<int>(common.y) + static_cast<int>(common.height) / 2;
        if (shape_only) {
            const int centre_x = static_cast<int>(common.x) + static_cast<int>(common.width) / 2;
            oa::ui::hud::stamp_compact_marker(
                surface.rgb.data(),
                picture_w,
                picture_h,
                centre_x,
                centre_y,
                player,
                red,
                green,
                blue
            );
            continue;
        }
        const bool wide = static_cast<int>(common.width) >= oa::ui::hud::kPlayerMarkerWidth + 8;
        const int centre_x =
            wide ? static_cast<int>(common.x) + oa::ui::hud::kPlayerMarkerCentreColumn + 1
                 : static_cast<int>(common.x) + static_cast<int>(common.width) / 2;
        oa::ui::hud::stamp_player_marker(
            surface.rgb.data(),
            picture_w,
            picture_h,
            centre_x,
            centre_y,
            player,
            red,
            green,
            blue,
            1,
            true
        );
    }
}

} // namespace oa::app

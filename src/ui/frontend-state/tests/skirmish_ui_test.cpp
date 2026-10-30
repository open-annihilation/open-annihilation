// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/frontend_state/skirmish_ui.hpp"
#include <algorithm>
#include <map>
#include <optional>
#include <iostream>
#include <stdexcept>
#include <vector>
using namespace oa::ui::frontend_state;
namespace ui = oa::ui::frontend_state::skirmish_ui;

namespace {
void check(bool v, const char* message) {
    if (!v)
        throw std::runtime_error(message);
}

struct TestHost final : ui::Host {
    std::vector<std::string> calls;
    std::vector<ui::SlotWidget> widgets;
    std::array<uint32_t, 8> buttons{};
    std::string selection = "None", fallback = "Fallback";
    int32_t mouse = 1, map_result = 1, capacity = 10;
    uint16_t colors = 10;
    std::optional<uint16_t> teams = uint16_t{3};
    uint32_t disc = 1;

    ui::MenuHandle frontend_menu() override { return {1}; }

    void clear_backbuffer() override { calls.push_back("clear-backbuffer"); }

    ui::MenuHandle load_menu(std::string_view name) override {
        calls.push_back("load:" + std::string(name));
        return {2};
    }

    void install_event_callback(ui::MenuHandle m) override {
        check(m.value == 2, "installed callback menu");
        calls.push_back("callback");
    }

    void load_background(std::string_view name) override {
        calls.push_back("background:" + std::string(name));
    }

    void install_input_callback() override { calls.push_back("input-callback"); }

    void set_input_enabled(int32_t value) override {
        check(value == 1, "enabled input");
        calls.push_back("input");
    }

    void add_menu_flags(uint32_t value) override {
        check(value == 0x40, "menu flag");
        calls.push_back("flags");
    }

    void select_map_index(int32_t value) override {
        check(value == 0, "fallback map index");
        calls.push_back("map-index");
    }

    std::string selected_map_name() override {
        calls.push_back("map-name");
        return fallback;
    }

    int16_t count = 13;

    int16_t widget_count() override { return count; }

    void set_widget_count(int16_t value) override {
        calls.push_back("widget-count:" + std::to_string(value));
        count = value;
    }

    void create_slot_widget(const ui::SlotWidget& widget) override { widgets.push_back(widget); }

    std::string translate_ui(std::string_view text) override {
        calls.push_back("translate:" + std::string(text));
        return std::string(text);
    }

    void set_text(
        ui::MenuHandle menu, std::string_view widget, std::string_view text, int32_t arg
    ) override {
        calls.push_back(
            "text:" + std::to_string(menu.value) + ":" + std::string(widget) + ":" +
            std::string(text) + ":" + std::to_string(arg)
        );
    }

    void set_enabled(std::string_view widget, int32_t enabled) override {
        calls.push_back("enable:" + std::string(widget) + ":" + std::to_string(enabled));
    }

    void set_button_stage(std::string_view widget, uint8_t stage) override {
        calls.push_back("stage:" + std::string(widget) + ":" + std::to_string(stage));
    }

    oa::data::match_rules::AiDifficultyNames names{};

    oa::data::match_rules::AiDifficultyNames difficulty_names() const override { return names; }

    void select_difficulty_label(std::string_view text, int32_t value) override {
        check(value == 1, "difficulty selection");
        calls.push_back("difficulty:" + std::string(text));
    }

    void set_side_stage(std::string_view widget, uint8_t stage) override {
        calls.push_back("side:" + std::string(widget) + ":" + std::to_string(stage));
    }

    void set_tooltip(std::string_view widget, std::string_view text) override {
        calls.push_back("tooltip:" + std::string(widget) + ":" + std::string(text));
    }

    void set_image(std::string_view widget, ui::Sprite sprite, uint16_t frame) override {
        calls.push_back(
            "image:" + std::string(widget) + ":" + std::to_string(static_cast<int>(sprite)) + ":" +
            std::to_string(frame)
        );
    }

    void set_image_frame(std::string_view widget, uint16_t frame) override {
        calls.push_back("frame:" + std::string(widget) + ":" + std::to_string(frame));
    }

    std::optional<uint16_t> team_icon_frame_count() override { return teams; }

    void zero_team_icon_frame_origin(uint32_t index) override {
        calls.push_back("origin:" + std::to_string(index));
    }

    uint16_t color_frame_count() override { return colors; }

    void invalidate_menu() override { calls.push_back("invalidate"); }

    void refresh_help_text() override { calls.push_back("help"); }

    std::string selected_widget_name(const game_entry::Event&) override {
        calls.push_back("selected");
        return selection;
    }

    uint32_t button_result(ui::MenuHandle, ui::Button b) override {
        calls.push_back("button:" + std::string(ui::resource_name(b)));
        return buttons[static_cast<std::size_t>(b)];
    }

    int32_t event_button(ui::MenuHandle) override { return mouse; }

    void capture_input() override { calls.push_back("capture"); }

    void play_ui_sound(std::string_view name, uint32_t arg) override {
        check(arg == 0, "sound zero");
        calls.push_back("sound:" + std::string(name));
    }

    void open_map_selection() override { calls.push_back("map-dialog"); }

    void play_sound(game_entry::Sound sound, uint32_t arg) override {
        play_ui_sound(game_entry::resource_name(sound), arg);
    }

    void select_cursor_animation(uint32_t index) override {
        calls.push_back("cursor:" + std::to_string(index));
    }

    uint32_t find_disc(game_entry::Disc) override { return disc; }

    void refresh_disc_archives() override { calls.push_back("archives"); }

    std::string translate(game_entry::Message message) override {
        return std::string(game_entry::message_text(message));
    }

    void show_frontend_message(std::string_view, int32_t width, int32_t, int32_t) override {
        calls.push_back("message:" + std::to_string(width));
    }

    void clear_event_selection(ui::MenuHandle menu) override {
        calls.push_back("clear:" + std::to_string(menu.value));
    }

    void clear_frontend_selection() override { calls.push_back("clear-frontend"); }

    int32_t select_map(std::string_view map) override {
        calls.push_back("map:" + std::string(map));
        return map_result;
    }

    int32_t map_player_capacity() override { return capacity; }

    void apply_skirmish_players() override { calls.push_back("apply"); }

    void save_preferences() override { calls.push_back("save"); }

    bool saw(std::string_view wanted) const {
        return std::find(calls.begin(), calls.end(), wanted) != calls.end();
    }
};

ui::Settings settings() {
    ui::Settings s;
    s.slot_count = 4;
    s.map_name = "Map";
    for (int32_t i = 0; i < 4; ++i) {
        auto& slot = s.slots[static_cast<std::size_t>(i)];
        slot.color = i;
        slot.alliance = 5;
        slot.metal = 1000;
        slot.energy = 1000;
    }
    return s;
}

void helper_tests() {
    auto s = settings();
    TestHost h;
    check(ui::all_slots_disabled(s), "all disabled");
    check(ui::first_unused_color(s) == 4, "first unused includes disabled slots");
    check(!ui::color_in_use(s, 1, 0), "disabled color available");
    s.slots[1].controller = 2;
    check(ui::color_in_use(s, 1, 0) && !ui::color_in_use(s, 1, 1), "enabled color excluding self");
    const ui::UiState plain;
    ui::cycle_player(s, 0, plain, h);
    check(s.slots[0].controller == 2, "open to computer");
    ui::cycle_player(s, 0, plain, h);
    check(s.slots[0].controller == 1, "computer to human if none");
    ui::cycle_player(s, 1, plain, h);
    check(s.slots[1].controller == 0, "computer to open if human");
    {
        // The fifth row is Player. Cycling the first row's Computer does not
        // make a second Player.
        auto rows = settings();
        rows.slot_count = 10;
        rows.slots[0].controller = 2;
        rows.slots[4].controller = 1;
        ui::cycle_player(rows, 0, plain, h);
        check(
            rows.slots[0].controller == 0 && rows.slots[4].controller == 1,
            "computer stays short of a second player"
        );
    }
    ui::cycle_player(s, 0, plain, h);
    check(s.slots[0].controller == 0, "human to open");
    s.slots[1].controller = 2;
    s.slots[0].color = 1;
    ui::cycle_player(s, 0, plain, h);
    check(s.slots[0].color == 0, "duplicate color repaired");
    ui::cycle_color(s, 0, false, h);
    check(s.slots[0].color == 2, "color skips enabled color only");
    s.slots[0].color = 0;
    ui::cycle_color(s, 0, true, h);
    check(s.slots[0].color == 9, "reverse wrap");
    s.slots[0].alliance = 2;
    s.slots[1].alliance = 2;
    ui::update_alliance_images(s, h);
    check(h.saw("frame:Allies0:4"), "shared alliance frame");
    s.slots[1].controller = 0;
    ui::update_alliance_images(s, h);
    check(h.saw("frame:Allies0:5"), "single alliance frame");
    s.slots[0].controller = 0;
    ui::update_alliance_images(s, h);
    check(h.saw("frame:Allies0:10"), "unused alliance frame");
    s.slot_count = 2;
    s.slots[0].controller = 1;
    s.slots[1].controller = 2;
    s.slots[0].color = 0;
    s.slots[1].color = 0;
    h.colors = 1;
    bool rejected = false;
    try {
        ui::cycle_color(s, 0, false, h);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "bounded occupied colors");
}

void setup_tests() {
    State state;
    auto s = settings();
    ui::Preferences p;
    p.skirmish_difficulty = 2;
    ui::UiState u;
    TestHost h;
    h.map_result = 0;
    ui::setup(state, s, p, u, h);
    check(
        p.difficulty == 2 && u.base_widget_count == 13 && s.map_name == "Fallback", "setup state"
    );
    check(
        s.slots[0].controller == 1 && s.slots[1].controller == 2 && s.slots[2].controller == 0,
        "default initial opponents"
    );
    check(h.widgets.size() == 24, "six widgets per slot");
    check(
        h.widgets[0].name == "Player0" && h.widgets[0].x == 45 && h.widgets[0].y == 94 &&
            h.widgets[0].width == 112,
        "row geometry"
    );
    check(
        h.widgets[6].y == 144 && h.widgets[4].flags == 0x10002 && h.widgets[1].stages == 2,
        "spacing/control flags"
    );
    check(
        h.saw("enable:Side2:0") && !h.saw("enable:Side0:1"), "disabled-only setup enable updates"
    );
    check(
        std::count(h.calls.begin(), h.calls.end(), "origin:0") == 3 && !h.saw("origin:1"),
        "repeated frame zero"
    );
    check(h.calls.back() == "cursor:19" && h.saw("difficulty:Hard"), "setup completion");
    {
        // ai.difficulty-names: the label is the name the difficulty carries.
        using Names = oa::data::match_rules::AiDifficultyNamesNames;
        State named_state;
        auto named_settings = settings();
        ui::Preferences named_preferences;
        named_preferences.skirmish_difficulty = 2;
        ui::UiState named_ui;
        TestHost named;
        named.map_result = 0;
        named.names.names = {Names::hard, Names::medium, Names::easy};
        ui::setup(named_state, named_settings, named_preferences, named_ui, named);
        check(named.saw("difficulty:Easy") && !named.saw("difficulty:Hard"), "swapped names");
    }
    check(!h.saw("translate:Open") && !h.saw("translate:"), "literal empty/open setup fields");
    TestHost saved;
    auto enabled = settings();
    enabled.slots[3].controller = 1;
    ui::populate(enabled, p, u, saved);
    check(
        enabled.slots[0].controller == 0 && enabled.slots[1].controller == 0,
        "existing participants preserved"
    );
}

void event_tests() {
    State state;
    auto s = settings();
    s.slots[0].controller = 1;
    s.slots[1].controller = 2;
    ui::Preferences p;
    ui::UiState u;
    u.side_count = 2;
    TestHost h;
    const game_entry::Event event{{9}, 0};
    auto run = [&](std::string name) {
        h.selection = std::move(name);
        return ui::handle_event(state, s, p, u, event, h);
    };
    ui::handle_event(state, s, p, u, {{9}, -1}, h);
    check(h.calls.empty(), "destroy returns");
    h.selection = "PrevMenu";
    h.buttons[1] = 0x100;
    ui::handle_event(state, s, p, u, event, h);
    check(state.pending_signal == 3 && h.calls.back() == "cursor:20", "whole-word previous branch");
    h.buttons.fill(0);
    state.pending_signal = 0;
    s.slots[0].metal = 200;
    run("Metal0");
    check(s.slots[0].metal == 500 && h.saw("text:9:Metal0:500:10"), "metal special floor step");
    h.mouse = 2;
    run("Metal0");
    check(s.slots[0].metal == 200, "metal lower clamp");
    h.mouse = 1;
    s.slots[0].energy = 9800;
    run("Energy0");
    check(s.slots[0].energy == 10000 && h.saw("capture"), "energy upper clamp/input capture");
    run("Side0");
    check(s.slots[0].side == 1 && h.saw("side:Side0:1"), "side cycle");
    run("Side0");
    check(s.slots[0].side == 0 && h.saw("side:Side0:0"), "side wrap");
    // A SIDEDATA with four sides: the button shows each side's frame, not
    // its own two stages.
    u.side_count = 4;
    for (const int32_t side : {1, 2, 3, 0}) {
        h.calls.clear();
        run("Side0");
        check(
            s.slots[0].side == side && h.saw("side:Side0:" + std::to_string(side)),
            ("the side button shows side " + std::to_string(side) + " of four").c_str()
        );
    }
    // The Lost Legacy: WATCH (2) is hidden, so Core steps to TLL (3).
    u.hidden_sides = 1U << 2;
    s.slots[0].side = 1;
    h.calls.clear();
    run("Side0");
    check(s.slots[0].side == 3 && h.saw("side:Side0:3"), "the side button skips a hidden side");
    // Four sides is still within 3.1c's five: a right click steps forward.
    u.hidden_sides = 0;
    u.side_count = 4;
    s.slots[0].side = 0;
    h.mouse = 2;
    h.calls.clear();
    run("Side0");
    check(s.slots[0].side == 1 && h.saw("side:Side0:1"), "a right click steps forward within five");
    // Seven sides: a right click steps back, wrapping and skipping a hidden side.
    u.side_count = 7;
    s.slots[0].side = 0;
    h.calls.clear();
    run("Side0");
    check(s.slots[0].side == 6 && h.saw("side:Side0:6"), "a right click wraps to the last side");
    s.slots[0].side = 3;
    h.calls.clear();
    run("Side0");
    check(s.slots[0].side == 2 && h.saw("side:Side0:2"), "a right click steps back one side");
    u.hidden_sides = 1U << 5;
    s.slots[0].side = 6;
    h.calls.clear();
    run("Side0");
    check(s.slots[0].side == 4 && h.saw("side:Side0:4"), "a right click skips a hidden side");
    h.mouse = 1;
    s.slots[0].side = 4;
    h.calls.clear();
    run("Side0");
    check(s.slots[0].side == 6 && h.saw("side:Side0:6"), "a left click skips a hidden side");
    u.hidden_sides = 0;
    u.side_count = 2;
    h.mouse = 1;
    s.slots[0].side = 0;
    run("Allies0");
    check(s.slots[0].alliance == 0, "alliance modulo6");
    for (auto b : {ui::Button::commander_death, ui::Button::start_location, ui::Button::mapping}) {
        h.buttons[static_cast<std::size_t>(b)] = 1;
        run("unmatched");
        h.buttons.fill(0);
    }
    check(
        p.skirmish.commander_death == 1 && p.skirmish_location == 1 && p.skirmish.mapping == 1 &&
            h.saw("help"),
        "rule toggles"
    );
    h.buttons[static_cast<std::size_t>(ui::Button::line_of_sight)] = 1;
    run("LineOfSight");
    check(p.skirmish.line_of_sight == 1 && p.skirmish.los_type == 1, "LOS off to true");
    run("LineOfSight");
    check(p.skirmish.line_of_sight == 1 && p.skirmish.los_type == 0, "LOS true to circular");
    run("LineOfSight");
    check(p.skirmish.line_of_sight == 0 && p.skirmish.los_type == 1, "LOS circular to off");
    h.buttons.fill(0);
    h.buttons[static_cast<std::size_t>(ui::Button::difficulty)] = 1;
    p.difficulty = 1;
    p.skirmish_difficulty = 0;
    run("Difficulty");
    check(
        p.difficulty == 2 && p.skirmish_difficulty == 2 && h.saw("sound:SKirmish"),
        "global difficulty source copied back"
    );
    h.buttons.fill(0);
    h.buttons[static_cast<std::size_t>(ui::Button::select_map)] = 1;
    run("SelectMap");
    check(h.saw("map-dialog") && h.calls.back() == "clear:9", "map dialog cleanup");
    h.buttons.fill(0);
    h.buttons[0] = 1;
    check(run("Start") && state.pending_signal == 2 && h.saw("apply"), "Start integrated branch");
    check(u.selected_slot == 0, "nonnumeric name final character atoi");
}

// Stored settings for the typed player-count code.
struct Prefs final : initialization::PreferencesHost {
    std::map<std::string, uint32_t> numbers;
    std::vector<std::string> writes;

    static std::string key(std::string_view section, std::string_view name) {
        return std::string(section) + ":" + std::string(name);
    }

    std::optional<uint32_t> read_number(std::string_view s, std::string_view k) override {
        const auto it = numbers.find(key(s, k));
        if (it == numbers.end())
            return {};
        return it->second;
    }

    void write_number(std::string_view s, std::string_view k, uint32_t v) override {
        writes.push_back(std::string(k));
        numbers[key(s, k)] = v;
    }

    std::optional<std::string>
    read_string(std::string_view, std::string_view, std::size_t) override {
        return {};
    }

    void write_string(std::string_view, std::string_view, std::string_view) override {}

    void audio_mode(initialization::AudioMode) override {}

    void mixing_buffers(uint32_t) override {}

    void wave_volume(uint32_t) override {}

    void cd_volume(uint32_t) override {}

    uint32_t nickname_override_enabled() override { return 0; }

    std::string nickname_override() override { return {}; }

    std::string game_name_override() override { return {}; }

    std::optional<std::string> user_name() override { return std::string("Player"); }

    std::string application_directory() override { return "root"; }

    std::string own_image_output_directory() override { return {}; }

    void select_map_list(int32_t) override {}

    void select_map_index(int32_t) override {}

    std::string selected_map_name() override { return "Map"; }

    uint32_t mixing_buffer_count() override { return 0; }

    uint32_t wave_out_volume() override { return 0; }

    uint32_t cd_audio_volume() override { return 0; }

    uint8_t keep_stored_password() override { return 0; }
};

ui::TypedKeys typed(std::string_view tail) {
    ui::TypedKeys keys{};
    std::copy(tail.begin(), tail.end(), keys.end() - static_cast<std::ptrdiff_t>(tail.size()));
    return keys;
}

void player_count_code_tests() {
    const auto run = [](std::string_view tail, int32_t expected, bool cleared) {
        auto s = settings();
        TestHost h;
        Prefs prefs;
        State state;
        ui::Preferences p;
        ui::UiState u{};
        u.side_count = 2;
        u.base_widget_count = 9;
        auto keys = typed(tail);
        ui::handle_player_count_code(state, s, p, u, keys, h, prefs);
        check(s.slot_count == expected, "typed code sets the slot count");
        check(
            prefs.numbers[Prefs::key(initialization::general_section, "NumSkirmishPlayers")] ==
                static_cast<uint32_t>(expected),
            "typed code stores NumSkirmishPlayers"
        );
        check(
            h.saw("save") && h.saw("widget-count:9") && h.saw("sound:SkirmishCheat"),
            "typed code saves, rebuilds and plays its sound"
        );
        check(h.widgets.size() == static_cast<std::size_t>(expected) * 6, "rows rebuilt");
        const bool empty = std::all_of(keys.begin(), keys.end(), [](uint8_t k) { return k == 0; });
        check(empty == cleared, "history cleared by the terminal codes only");
    };
    run("*III", 3, true);
    run("*IV", 4, true);
    run("*V", 5, false);
    run("*VI", 6, false);
    run("*VII", 7, false);
    run("*VIII", 8, true);
    run("*IX", 9, true);
    run("*X", 10, true);

    auto s = settings();
    TestHost h;
    Prefs prefs;
    State state;
    ui::Preferences p;
    ui::UiState u{};
    auto keys = typed("*XI");
    ui::handle_player_count_code(state, s, p, u, keys, h, prefs);
    check(s.slot_count == 4 && h.calls.empty() && prefs.writes.empty(), "other text is ignored");
}

void side_and_alliance_tests() {
    auto s = settings();
    TestHost h;
    s.slots[1].side = 1;
    ui::advance_side(s, 1, 2);
    check(s.slots[1].side == 0, "side wraps at the loaded side count");
    ui::advance_side(s, 1, 2);
    check(s.slots[1].side == 1, "side steps forward");
    // Rhyoss: four sides, the spare WATCH (3) hidden; The Lost Legacy: its
    // spare WATCH (2) hidden before its own side, 3.
    s.slots[1].side = 2;
    ui::advance_side(s, 1, 4, 1U << 3);
    check(s.slots[1].side == 0, "a hidden last side is stepped past to the first");
    s.slots[1].side = 1;
    ui::advance_side(s, 1, 4, 1U << 2);
    check(s.slots[1].side == 3, "a hidden middle side is stepped past");
    ui::advance_side(s, 1, 2, 0b11U);
    check(s.slots[1].side == 0, "with every side hidden the button still steps");
    check(ui::offered_side(3, 4, 1U << 3) == 0, "a saved hidden last side opens on the first");
    check(ui::offered_side(2, 4, 1U << 2) == 3, "a saved hidden side opens on the next");
    check(ui::offered_side(1, 4, 1U << 2) == 1, "an offered side is kept");
    check(ui::offered_side(1, 2, 0b11U) == 1, "with every side hidden the side is kept");
    // A side saved under another mod: The Lost Legacy's TLL (3) or Rhyoss
    // (2) opens on Arm in the base game's two sides, never on the art past
    // Core, which is WATCH; and past a hidden first side on the next.
    check(ui::offered_side(3, 2, 0) == 0, "a side past the loaded ones opens on the first");
    check(ui::offered_side(2, 2, 0) == 0, "the side just past the last opens on the first");
    check(ui::offered_side(-1, 4, 0) == 0, "a negative side opens on the first");
    check(ui::offered_side(4, 4, 1U << 0) == 1, "out of range, past a hidden first side");
    check(ui::offered_side(5, 2, 0b11U) == 0, "with every side hidden it is still a loaded side");
    s.slots[2].alliance = 5;
    ui::cycle_alliance(s, 2, h);
    check(s.slots[2].alliance == 0, "alliance wraps after six groups");
    check(
        std::any_of(
            h.calls.begin(),
            h.calls.end(),
            [](const std::string& call) { return call.rfind("frame:Allies", 0) == 0; }
        ),
        "alliance images refresh"
    );
}

/// A computer slot's Side button offers only the sides computer players may take.
void computer_side_tests() {
    State state;
    auto s = settings();
    ui::Preferences p;
    ui::UiState u;
    u.side_count = 5;
    u.computer_hidden_sides = ~1U;
    TestHost h;
    const game_entry::Event event{{9}, 0};
    auto run = [&](std::string name) {
        h.selection = std::move(name);
        return ui::handle_event(state, s, p, u, event, h);
    };
    s.slots[0].controller = game_entry::controller::human;
    s.slots[1].controller = game_entry::controller::computer;
    h.mouse = 1;
    for (int32_t step = 0; step < 5; ++step) {
        run("Side1");
        check(s.slots[1].side == 0, "a computer stays on the one side it may take");
    }
    const int32_t human_sides[] = {1, 2, 3, 4, 0};
    for (const int32_t side : human_sides) {
        run("Side0");
        check(s.slots[0].side == side, "a human still cycles every loaded side");
    }
    s.slots[2].side = 3;
    h.calls.clear();
    run("Player2");
    check(
        s.slots[2].controller == game_entry::controller::computer && s.slots[2].side == 0 &&
            h.saw("side:Side2:0"),
        "becoming a computer moves to an allowed side"
    );

    auto stored = settings();
    stored.slots[0].controller = game_entry::controller::human;
    stored.slots[0].side = 2;
    stored.slots[1].controller = game_entry::controller::computer;
    stored.slots[1].side = 2;
    ui::UiState populated = u;
    TestHost host;
    ui::populate(stored, p, populated, host);
    check(stored.slots[0].side == 2, "a stored human side stays");
    check(stored.slots[1].side == 0, "a stored computer side moves to an allowed side");

    u.side_count = 7;
    u.hidden_sides = 0;
    u.computer_hidden_sides = ~((1U << 0) | (1U << 5));
    s.slots[1].side = 5;
    h.mouse = 1;
    const int32_t forward[] = {0, 5, 0};
    for (const int32_t side : forward) {
        run("Side1");
        check(s.slots[1].side == side, "a computer steps only through the sides it may take");
    }
    s.slots[1].side = 0;
    h.mouse = 2;
    const int32_t backward[] = {5, 0, 5};
    for (const int32_t side : backward) {
        run("Side1");
        check(s.slots[1].side == side, "a computer steps back only through the sides it may take");
    }
    u.hidden_sides = 1U << 5;
    s.slots[1].side = 0;
    h.mouse = 1;
    run("Side1");
    check(s.slots[1].side == 0, "a computer stays when its other side is hidden");
    h.mouse = 2;
    run("Side1");
    check(s.slots[1].side == 0, "a right click stays when only one side is offered");

    u.computer_hidden_sides = 0;
    u.hidden_sides = 0;
    u.side_count = 5;
    s.slots[1].side = 0;
    h.mouse = 1;
    run("Side1");
    check(s.slots[1].side == 1, "with no limit a computer steps forward");
    u.side_count = 7;
    s.slots[1].side = 0;
    h.mouse = 2;
    run("Side1");
    check(s.slots[1].side == 6, "with no limit a right click steps back");
}
} // namespace

int main() {
    try {
        helper_tests();
        setup_tests();
        event_tests();
        player_count_code_tests();
        side_and_alliance_tests();
        computer_side_tests();
        std::cout << "skirmish UI passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

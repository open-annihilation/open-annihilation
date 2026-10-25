// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/data/match_rules.hpp"
#include "oa/ui/frontend_state/initialization.hpp"

namespace oa::ui::frontend_state::skirmish_ui {
using MenuHandle = game_entry::MenuHandle;
using Settings = game_entry::SkirmishSettings;
using Preferences = initialization::Preferences;
inline constexpr uint32_t active_cursor_index = 19, transition_cursor_index = 20;
inline constexpr int32_t resource_step = 500, resource_minimum = 200, resource_maximum = 10000;
inline constexpr int32_t alliance_count = 6, color_search_limit = 10;

// Panel state; the first two fields are kept in the skirmish settings block.
struct UiState {
    int32_t selected_slot{};          // the digit that ends the selected gadget's name
    int32_t base_widget_count{};      // the GUI's signed record count before the slot rows
    int32_t side_count{};             // Game.side_count, loaded side definitions
    uint32_t hidden_sides{};          // bit n: the Side button never offers side n
    uint32_t computer_hidden_sides{}; // bit n: a computer slot's Side button never offers side n
};
enum class Button {
    start,
    previous_menu,
    commander_death,
    start_location,
    mapping,
    line_of_sight,
    select_map,
    difficulty
};

/// Returns the gadget name of a SKIRMISH.GUI button.
///
/// @param button Button.
/// @return Its record name.
constexpr std::string_view resource_name(Button button) noexcept {
    switch (button) {
    case Button::start:
        return "Start";
    case Button::previous_menu:
        return "PrevMenu";
    case Button::commander_death:
        return "CommanderDeath";
    case Button::start_location:
        return "StartLocation";
    case Button::mapping:
        return "Mapping";
    case Button::line_of_sight:
        return "LineOfSight";
    case Button::select_map:
        return "SelectMap";
    case Button::difficulty:
        return "Difficulty";
    }
    return {};
}
enum class WidgetKind { button, image };
enum class Sprite { team_icons, player_colors };

struct SlotWidget {
    WidgetKind kind{};
    std::string name;
    int16_t x{}, y{}, width{}, height{};
    uint32_t flags{};
    uint8_t stages{};
    std::string_view sprite_resource;
    std::string tooltip;
    // For buttons only, the sprite resource's frame 0 width/height replace the
    // dimensions when the named frame exists. The host must keep the fallback
    // dimensions otherwise.
};

// The routines the skirmish panel calls.
class Host : public game_entry::SkirmishHost {
  public:

    /// Returns the frontend panel.
    ///
    /// @return The panel.
    virtual MenuHandle frontend_menu() = 0;

    /// Clears the back buffer.
    virtual void clear_backbuffer() = 0;

    /// Loads a GUI as a panel without load flags.
    ///
    /// @param resource GUI file name.
    /// @return The panel.
    virtual MenuHandle load_menu(std::string_view resource) = 0;

    /// Installs the skirmish event callback on a panel, with the application as its context.
    ///
    /// @param menu The panel.
    virtual void install_event_callback(MenuHandle menu) = 0;

    /// Loads the panel's background bitmap.
    ///
    /// @param name Bitmap name.
    virtual void load_background(std::string_view name) = 0;

    /// Installs the typed-key callback on the active panel.
    virtual void install_input_callback() = 0;

    /// Enables or disables panel input.
    ///
    /// @param enabled Nonzero enables.
    virtual void set_input_enabled(int32_t enabled) = 0;

    /// Adds flags to the menu and draws it with them.
    ///
    /// @param flags Panel flags.
    virtual void add_menu_flags(uint32_t flags) = 0;

    /// Selects a map of the current list.
    ///
    /// @param index Map index, from 0.
    virtual void select_map_index(int32_t index) = 0;

    /// Returns the name of the selected map.
    ///
    /// @return The map name.
    virtual std::string selected_map_name() = 0;

    /// Returns the active GUI's last record index (its root record's record count).
    ///
    /// @return The signed count.
    virtual int16_t widget_count() = 0;

    /// Sets the active GUI's last record index, dropping later records.
    ///
    /// @param count New count.
    virtual void set_widget_count(int16_t count) = 0;

    /// Appends one per-slot widget to the active GUI.
    ///
    /// @param widget Widget description.
    virtual void create_slot_widget(const SlotWidget& widget) = 0;

    /// Translates interface text.
    ///
    /// @param text Text to translate.
    /// @return The translation, or the text itself.
    virtual std::string translate_ui(std::string_view text) = 0;

    /// Sets a gadget's text, in the language shown, as 3.1c's gadget text
    /// setter translates every text it sets.
    ///
    /// @param menu Panel holding the gadget.
    /// @param widget Gadget name.
    /// @param text New text, before translation.
    /// @param length Text-box length; 0 keeps the current one.
    virtual void
    set_text(MenuHandle menu, std::string_view widget, std::string_view text, int32_t length) = 0;

    /// Sets a gadget's active byte.
    ///
    /// @param widget Gadget name.
    /// @param enabled Nonzero activates.
    virtual void set_enabled(std::string_view widget, int32_t enabled) = 0;

    /// Sets a button's stage.
    ///
    /// @param widget Gadget name.
    /// @param stage New stage.
    virtual void set_button_stage(std::string_view widget, uint8_t stage) = 0;

    /// Selects the difficulty label.
    ///
    /// @param label "Easy", "Medium" or "Hard".
    /// @param value Second argument, always 1.
    virtual void select_difficulty_label(std::string_view label, int32_t value) = 0;

    /// Returns which of 3.1c's names each difficulty carries (ai.difficulty-names).
    ///
    /// @return the names; 3.1c's unless overridden
    [[nodiscard]] virtual data::match_rules::AiDifficultyNames difficulty_names() const {
        return {};
    }

    /// Sets a side button's stage.
    ///
    /// @param widget Gadget name.
    /// @param stage Side index.
    virtual void set_side_stage(std::string_view widget, uint8_t stage) = 0;

    /// Sets a gadget's help text.
    ///
    /// @param widget Gadget name.
    /// @param text Translated help.
    virtual void set_tooltip(std::string_view widget, std::string_view text) = 0;

    /// Sets a gadget's image sequence and frame; a missing gadget is ignored.
    ///
    /// @param widget Gadget name.
    /// @param sprite Sequence to show.
    /// @param frame Frame index.
    virtual void set_image(std::string_view widget, Sprite sprite, uint16_t frame) = 0;

    /// Sets a gadget's image frame; a missing gadget is ignored.
    ///
    /// @param widget Gadget name.
    /// @param frame Frame index.
    virtual void set_image_frame(std::string_view widget, uint16_t frame) = 0;

    /// Returns the frame count of the TEAMICONS sequence.
    ///
    /// @return The count, or nothing when the sequence is missing.
    virtual std::optional<uint16_t> team_icon_frame_count() = 0;

    /// Zeroes the x and y origin of a TEAMICONS frame.
    ///
    /// @param frame Frame index.
    virtual void zero_team_icon_frame_origin(uint32_t frame) = 0;

    /// Returns the frame count of the player colour animation.
    ///
    /// @return The count.
    virtual uint16_t color_frame_count() = 0;

    /// Marks the menu for a redraw.
    virtual void invalidate_menu() = 0;

    /// Refreshes the help text gadget.
    virtual void refresh_help_text() = 0;

    /// Returns the name of the gadget an event selected.
    ///
    /// @param event Menu event.
    /// @return The name; at most 16 bytes are copied.
    virtual std::string selected_widget_name(const game_entry::Event& event) = 0;

    /// Reports whether a button was activated by the event.
    ///
    /// @param menu Menu the event came from.
    /// @param button Button to test.
    /// @return Nonzero when it was; the whole word is tested.
    virtual uint32_t button_result(MenuHandle menu, Button button) = 0;

    /// Returns the mouse button of an event, as the event's top panel records it.
    ///
    /// @param menu Menu the event came from.
    /// @return 1 for left, 2 for right.
    virtual int32_t event_button(MenuHandle menu) = 0;

    /// Reads the current pointer state; the returned 24 bytes are unused here.
    virtual void capture_input() = 0;

    /// Plays an interface sound.
    ///
    /// @param name ALLSOUND name.
    /// @param argument Second argument, always 0.
    virtual void play_ui_sound(std::string_view name, uint32_t argument) = 0;

    /// Opens the map-selection modal; the modal policy stays with the host.
    virtual void open_map_selection() = 0;
};

/// Reports whether every skirmish slot is disabled.
///
/// @param settings Skirmish settings.
/// @return True when no slot among the first slot_count has a controller.
/// @throws std::invalid_argument for a slot_count outside 0..11.
bool all_slots_disabled(const Settings& settings);

/// Reports whether an enabled slot other than one uses a colour.
///
/// @param settings Skirmish settings.
/// @param color Colour index.
/// @param excluded_slot Slot left out of the test.
/// @return True when another enabled slot uses it.
/// @throws std::invalid_argument for a slot_count outside 0..11.
bool color_in_use(const Settings& settings, int32_t color, int32_t excluded_slot);

/// Returns the first colour no slot uses; unlike color_in_use() this counts disabled slots.
///
/// @param settings Skirmish settings.
/// @return Colour 0..9, or -1 when all ten are used.
/// @throws std::invalid_argument for a slot_count outside 0..11.
int32_t first_unused_color(const Settings& settings);

/// Counts the enabled slots in an alliance group.
///
/// @param settings Skirmish settings.
/// @param alliance Alliance group, 0..5.
/// @return The number of enabled slots in it.
/// @throws std::invalid_argument for a slot_count outside 0..11.
int32_t alliance_members(const Settings& settings, int32_t alliance);

/// Refreshes every slot's alliance button image and redraws the menu.
///
/// @param settings Skirmish settings.
/// @param[in,out] h Routines the panel calls.
/// @throws std::invalid_argument for a slot_count outside 0..11.
void update_alliance_images(const Settings& settings, Host& h);

/// Cycles a slot's controller: disabled, computer, then human when none is human.
///
/// A slot that becomes enabled with a colour in use takes the first unused colour.
/// A slot that becomes a computer moves to the first side its Side button offers.
///
/// @param[in,out] settings Skirmish settings.
/// @param slot Slot index.
/// @param ui Panel state; a computer slot's side mask comes from it.
/// @param[in,out] h Routines the panel calls.
/// @throws std::out_of_range for a slot outside slot_count.
void cycle_player(Settings& settings, int32_t slot, const UiState& ui, Host& h);

/// Cycles a slot's colour, skipping colours in use.
///
/// @param[in,out] settings Skirmish settings.
/// @param slot Slot index.
/// @param reverse True steps backwards (right click).
/// @param[in,out] h Routines the panel calls.
/// @throws std::invalid_argument when the colour animation is empty or every
///         colour is in use, and std::out_of_range for a bad slot.
void cycle_color(Settings& settings, int32_t slot, bool reverse, Host& h);

/// Steps a slot to the next loaded side, wrapping to the first, past the
/// hidden ones; when every side is hidden, to the next side.
///
/// @param[in,out] settings Skirmish settings.
/// @param slot Slot index.
/// @param side_count Number of loaded sides.
/// @param hidden_sides Bit n set for a side the Side button never offers.
/// @throws std::invalid_argument for an empty side table, and std::out_of_range for a bad slot.
void advance_side(Settings& settings, int32_t slot, int32_t side_count, uint32_t hidden_sides = 0);

/// Steps a slot to the previous loaded side, wrapping to the last, past the
/// hidden ones. When only the current side is offered, it stays. When every
/// side is hidden, it steps to the previous side.
///
/// @param[in,out] settings Skirmish settings.
/// @param slot Slot index.
/// @param side_count Number of loaded sides.
/// @param hidden_sides Bit n set for a side the Side button never offers.
/// @throws std::invalid_argument for an empty side table, and std::out_of_range for a bad slot.
void retreat_side(Settings& settings, int32_t slot, int32_t side_count, uint32_t hidden_sides = 0);

/// Returns the side a slot shows: its own, or the next one offered when its
/// own is hidden, as a skirmish saved on a hidden side opens. A side past
/// the loaded ones, saved while a mod with more sides was played, wraps to
/// the first, as the Side button does after the last.
///
/// @param side The slot's side.
/// @param side_count Number of loaded sides.
/// @param hidden_sides Bit n set for a side the Side button never offers.
/// @return the side, unchanged when it is loaded and offered; a loaded side
///         when every side is hidden
[[nodiscard]] int32_t
offered_side(int32_t side, int32_t side_count, uint32_t hidden_sides) noexcept;

/// Steps a slot to the next of the six alliance groups and refreshes the alliance images.
///
/// @param[in,out] settings Skirmish settings.
/// @param slot Slot index.
/// @param[in,out] h Routines the panel calls.
/// @throws std::out_of_range for a slot outside slot_count.
void cycle_alliance(Settings& settings, int32_t slot, Host& h);

/// Creates the per-slot player, side, colour, alliance, metal and energy widgets.
///
/// Rows are spread over 200 pixels and centred on a 180-pixel span from y 79.
///
/// @param settings Skirmish settings.
/// @param[in,out] h Routines the panel calls.
/// @throws std::invalid_argument for no slots or a slot_count outside 0..11.
void create_slot_widgets(const Settings& settings, Host& h);

/// Fills the skirmish panel from the persisted settings.
///
/// @param[in,out] settings Skirmish settings; a panel with every slot
///        disabled gets a human in slot 0 and a computer in slot 1.
/// @param preferences Preferences supplying the rules and location.
/// @param[out] ui Panel state; the base widget count is recorded.
/// @param[in,out] h Routines the panel calls.
/// @quirk The first two stored slots are written even when fewer rows show,
///        and the TEAMICONS origin reset fetches frame zero every time.
void populate(Settings& settings, Preferences& preferences, UiState& ui, Host& h);

/// Loads SKIRMISH.GUI and fills it from the settings.
///
/// Falls back to the first map of the list when the saved map does not load.
///
/// @param state Dispatcher state (unused).
/// @param[in,out] settings Skirmish settings.
/// @param[in,out] preferences Preferences; difficulty takes the skirmish difficulty.
/// @param[out] ui Panel state.
/// @param[in,out] h Routines the panel calls.
/// @throws std::invalid_argument when the fallback map name exceeds the game's string field.
void setup(State& state, Settings& settings, Preferences& preferences, UiState& ui, Host& h);

/// Handles a SKIRMISH.GUI event, including the selected-name bookkeeping.
///
/// The selected gadget's last character picks the slot; Start runs
/// game_entry::start_selected_skirmish().
///
/// @param[in,out] state Dispatcher state.
/// @param[in,out] settings Skirmish settings.
/// @param[in,out] preferences Preferences; rule buttons toggle them.
/// @param[in,out] ui Panel state; selected_slot is updated.
/// @param event Menu event.
/// @param[in,out] h Routines the panel calls.
/// @return True only for a successful Start handoff.
/// @throws std::invalid_argument for an empty selected name.
bool handle_event(
    State& state,
    Settings& settings,
    Preferences& preferences,
    UiState& ui,
    const game_entry::Event& event,
    Host& h
);

// The panel's 15-byte typed-key history: upper-cased keys, newest last.
using TypedKeys = std::array<uint8_t, 15>;
inline constexpr int32_t default_player_count = 3; // "*III"
inline constexpr std::string_view player_count_sound = "SkirmishCheat";

/// Handles the SKIRMISH.GUI typed-key codes that set the number of player slots.
///
/// Typing "*III" to "*X" sets that many player slots, stores the count,
/// reloads the settings and rebuilds the rows.
///
/// @param[in,out] state Dispatcher state.
/// @param[in,out] settings Skirmish settings.
/// @param[in,out] preferences Preferences, reloaded.
/// @param[in,out] ui Panel state.
/// @param[in,out] typed Typed-key history; cleared after a code unless it may continue.
/// @param[in,out] h Routines the panel calls.
/// @param[in,out] preferences_host Settings services for the reload.
/// @quirk "*V", "*VI" and "*VII" keep the history so longer codes can follow.
void handle_player_count_code(
    State& state,
    Settings& settings,
    Preferences& preferences,
    UiState& ui,
    TypedKeys& typed,
    Host& h,
    initialization::PreferencesHost& preferences_host
);

} // namespace oa::ui::frontend_state::skirmish_ui

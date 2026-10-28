// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Settings screen on the OA layer (settings_menu.hpp), settings, which
// the main menu's SETTINGS opens (Runtime::push_settings_menu_screen). It
// shows the open settings dialog's sections
// (oa/ui/engine_settings/dialog.hpp), and beside them the game's own
// options, which the game's Options screen sets: Sound, Music, the visuals
// and the speeds.
//
// The dialog's rows are read with section_rows and moved with
// dialog_control_key, as the dialog's own keys move them, so that every
// lock, every setting put in effect as it changes and every action the
// dialog asks of its host stay the dialog's (Runtime::take_engine_settings_action).
// The game's options are the preferences the Options screen's handlers set
// (oa/ui/frontend/options.hpp), each put in effect as it changes as there:
// the volumes and the gamma at once, the music's switch and play mode at the
// next frame. Their stages' captions are the game's, read from SOUNDS.GUI,
// MUSIC.GUI and SPEEDS.GUI. OK keeps both and saves them, Cancel and Undo
// put back what the screen opened with, and Restore defaults sets both to
// their defaults, the game's options as their panels' RESTORE does.
//
// The screen is drawn with the game's art (options_screen_art): the game's menu art the Library shares
// (read_game_menu_art), its title SETTINGS, and the Options screen's
// Restore Default, Undo Changes, Cancel and OK as the game draws them, each
// cut out with its plate and clear around it.

#include "gui_record.hpp"
#include "library_host.hpp"
#include "menu_paint.hpp"
#include "oa_layer.hpp"
#include "runtime_game_menu_art.hpp"
#include "settings_menu.hpp"

#include "oa/app/runtime.hpp"
#include "oa/audio/game_audio.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/languages.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_state/initialization.hpp"
#include "oa/ui/gui_layout.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/text.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace oa::app {

namespace {

namespace settings = oa::ui::engine_settings;
namespace renderer = oa::ui::frontend_renderer;
namespace kit = oa::ui::kit;
namespace library = oa::ui::library;
namespace init = oa::ui::frontend_state::initialization;
namespace flag = init::preference_flags;
using menu_paint::worded;

/// The most buttons a language's row shows: the Library's first that can be
/// pressed, of those the list offers.
constexpr std::size_t language_buttons_most = 2;

/// The sound a step of one of the game's options plays, as its button does.
constexpr std::string_view options_sound = "Options";
/// The sound OK plays, as the dialog's does.
constexpr std::string_view accept_sound = "Options";
/// The sound Cancel plays, as the dialog's does.
constexpr std::string_view cancel_sound = "Previous";
/// The wave the Sound's TEST plays.
constexpr std::string_view test_wave = "sounds\\explode.wav";

/// The effects and music volumes' highest value.
constexpr int32_t volume_most = 0x40;
/// The gamma's highest value.
constexpr int32_t gamma_most = 0x14;
/// The game speed's highest value; the slider never goes under 1.
constexpr int32_t game_speed_most = 0x15;
/// The game speed the game plays at as it is, shown as 0.
constexpr int32_t game_speed_normal = 10;
/// The scroll speed's highest value; the slider never goes under 1.
constexpr int32_t scroll_speed_most = 0x41;
/// The message lines' highest count.
constexpr int32_t lines_most = 0x1e;
/// The text scroll time's highest number of seconds.
constexpr int32_t text_scroll_most = 0x28;
/// The unit speech and chat text volumes are their stage times this.
constexpr int32_t chat_volume_step = 5;
/// The sound mode that plays no sound.
constexpr int32_t sound_mode_off = 0;
/// The sound mode that plays sound in 3D.
constexpr int32_t sound_mode_spatial = 2;
/// The music volume the music panel's RESTORE sets.
constexpr uint32_t music_volume_default = 32;
/// The play mode the music panel's RESTORE sets: by the kind of each track.
constexpr uint8_t cd_mode_default = 4;

/// The Options screen's buttons the Settings screen's RESTORE DEFAULTS, UNDO,
/// CANCEL and OK are drawn as, in that order.
constexpr std::array<std::string_view, 4> options_buttons{"RESTORE", "UNDO", "CANCEL", "PREV"};

/// Returns the art the Settings screen is drawn with over the game's own
/// main menu: the game's menu art (read_game_menu_art), its title SETTINGS,
/// and the Options screen's Restore Default, Undo Changes, Cancel and OK as
/// the game draws them up and held over the picture STARTOPT.GUI's setup
/// asks for, with their captions and quick keys, each cut out with its
/// plate, clear where the bare picture's ground lies around it.
///
/// @param assets the game's files
/// @return the art; none when a file is missing, or a button or its plate
///         does not lie on the picture, the reason on stderr
std::optional<SettingsMenuGameArt> options_screen_art(oa::AssetStore& assets) {
    std::optional<GameMenuArt> common = read_game_menu_art(assets, "SETTINGS", "settings");
    if (!common)
        return std::nullopt;
    try {
        const renderer::ScreenResources screen = read_options_screen(assets);
        const oa::Image bare = read_bare_picture(assets);
        SettingsMenuGameArt art;
        std::array<std::size_t, options_buttons.size()> found{};
        std::vector<renderer::ButtonPresentation> up;
        for (std::size_t at = 0; at < options_buttons.size(); ++at) {
            const auto& gadgets = screen.layout.gadgets;
            const auto named = gui_record_named(gadgets, options_buttons[at]);
            const auto* fields =
                named ? std::get_if<oa::ui::gui_layout::ButtonFields>(&gadgets[*named].fields)
                      : nullptr;
            if (fields == nullptr) {
                std::cerr << "settings: STARTOPT.GUI has no button " << options_buttons[at] << '\n';
                return std::nullopt;
            }
            found[at] = *named;
            renderer::ButtonPresentation shown;
            shown.name = gadgets[*named].common.name;
            shown.quick_key = static_cast<char>(fields->quick_key);
            up.push_back(std::move(shown));
        }
        std::vector<renderer::ButtonPresentation> held = up;
        for (auto& shown : held)
            shown.condition = renderer::ButtonCondition::pressed;
        const renderer::Surface drawn_up = renderer::render_screen(screen, up);
        const renderer::Surface drawn_held = renderer::render_screen(screen, held);
        for (std::size_t at = 0; at < options_buttons.size(); ++at) {
            const auto& placed = screen.layout.gadgets[found[at]].common;
            const int32_t x = placed.x - options_plate_left;
            const int32_t y = placed.y - options_plate_top;
            const int32_t width = placed.width + options_plate_left + options_plate_right;
            const int32_t height = placed.height + options_plate_top + options_plate_bottom;
            art.buttons[at] = {
                cut_out(drawn_up, x, y, width, height), cut_out(drawn_held, x, y, width, height)
            };
            for (WindowIcon& cut : art.buttons[at])
                clear_bare_ground(cut, bare, x, y);
            if (art.buttons[at][0].width == 0 || art.buttons[at][1].width == 0) {
                std::cerr << "settings: " << options_buttons[at] << "'s plate does not lie on the Options screen\n";
                return std::nullopt;
            }
        }
        art.common = std::move(*common);
        return art;
    } catch (const std::exception& error) {
        std::cerr << "settings: the Options screen's art is unavailable: " << error.what() << '\n';
        return std::nullopt;
    }
}

/// A group of the game's own options: one of its Options screen's panels.
enum class ClassicGroup : uint8_t { sound, music, visuals, speeds };

/// One category of the screen: what it lists and where its rows come from.
struct Category {
    SettingsMenuCategory shown{};             ///< its entry
    std::optional<ClassicGroup> classic{};    ///< the game's options it shows
    std::optional<settings::Page> page{};     ///< the dialog's section it shows
    std::optional<std::size_t> registered{};  ///< the registered page it shows, by its place
};

/// Returns a section's picture.
///
/// @param page the section
/// @return its picture
SettingsMenuIcon icon_of(settings::Page page) noexcept {
    switch (page) {
    case settings::Page::controls:
        return SettingsMenuIcon::controls;
    case settings::Page::common_tweaks:
        return SettingsMenuIcon::limits;
    case settings::Page::language:
        return SettingsMenuIcon::language;
    case settings::Page::graphics:
        return SettingsMenuIcon::graphics;
    case settings::Page::user_interface:
        return SettingsMenuIcon::interface;
    case settings::Page::touch:
        return SettingsMenuIcon::touch;
    case settings::Page::controller:
        return SettingsMenuIcon::controller;
    case settings::Page::developer:
        return SettingsMenuIcon::developer;
    case settings::Page::game_files:
        return SettingsMenuIcon::game_files;
    case settings::Page::downloads:
        return SettingsMenuIcon::downloads;
    case settings::Page::system:
        return SettingsMenuIcon::system;
    case settings::Page::game_content:
        return SettingsMenuIcon::content;
    default:
        return SettingsMenuIcon::page;
    }
}

/// Returns when a change of one of the dialog's rows takes effect, by the
/// row's word.
///
/// @param id the row's word
/// @param kind the row's kind
/// @return the note, in the language shown; empty for a row that sets nothing
std::string effect_of(std::string_view id, kit::RowKind kind) {
    if (kind == kit::RowKind::text || kind == kit::RowKind::link ||
        kind == kit::RowKind::buttons || kind == kit::RowKind::value_and_button)
        return {};
    if (id == "screen-size" || id == "interface-size" || id == "content-updates" ||
        id.starts_with("file-type-") || id.starts_with("install-id"))
        return worded("Takes effect when you press OK");
    if (id == "unit-limit" || id == "path-search")
        return worded("Takes effect from the next game");
    if (id == "native-density")
        return worded("Takes effect from the next start");
    return worded("Takes effect immediately");
}

/// Joins a row's hint lines into one paragraph.
///
/// @param lines the lines
/// @return the paragraph
std::string paragraph_of(const std::vector<std::string>& lines) {
    std::string joined;
    for (const std::string& line : lines) {
        if (line.empty())
            continue;
        if (!joined.empty())
            joined += ' ';
        joined += line;
    }
    return joined;
}

} // namespace

/// What the Settings screen reads and sets: the open settings
/// dialog's sections and the game's own options.
struct Runtime::SettingsMenuHost {
    /// One of the game's options as a row of the screen.
    struct ClassicRow {
        std::string_view id{};            ///< its word
        std::string_view label{};         ///< what it sets, in English
        std::string_view detail{};        ///< what it does, in English
        SettingsMenuControl control{};    ///< its control
        int32_t count{};                  ///< how many stops it has
        /// Its stop in a set of preferences.
        std::function<int32_t(const init::Preferences&)> index{};
        /// Gives the runtime's preferences a stop and puts it in effect.
        std::function<void(Runtime&, int32_t)> set{};
        /// A stop's caption, in the language shown.
        std::function<std::string(int32_t)> caption{};
        /// Whether it takes a press in a set of preferences; unset for always.
        std::function<bool(const init::Preferences&)> usable{};
        std::vector<std::string> buttons{};             ///< a row of buttons' captions
        std::function<void(Runtime&, std::size_t)> press{}; ///< what a button does
        bool sounds{};                                  ///< a step plays the options' sound
    };

    /// Makes the host of a screen opening now: the game's options as they
    /// are now are the ones Cancel and Undo put back.
    ///
    /// @param runtime the runtime
    explicit SettingsMenuHost(Runtime& runtime) : runtime_(runtime) {
        init::capture_options_entry(runtime_.preferences_, entry_);
        // The dialog opens on its first section; the screen opens on its
        // first category, whose rows are the game's.
        chosen_ = 0;
        // The Library's language tile opens it on Language & Text.
        if (!runtime_.settings_menu_first_category_.empty()) {
            const std::vector<Category> listed = categories();
            for (std::size_t at = 0; at < listed.size(); ++at)
                if (listed[at].shown.id == runtime_.settings_menu_first_category_)
                    chosen_ = at;
            runtime_.settings_menu_first_category_.clear();
        }
        show_page_of(chosen_);
    }

    /// Tells whether the screen shows: while the dialog is open on the
    /// game's screen it opened over, the main menu, and has not been handed
    /// to its own look. Off that
    /// screen, an open dialog closes as Cancel closes it.
    ///
    /// @return true while it shows
    [[nodiscard]] bool shows() {
        if (handed_over_ || runtime_.engine_settings_dialog() == nullptr)
            return false;
        if (runtime_.screen_ != opened_on_) {
            cancel(false);
            return false;
        }
        return true;
    }

    /// Returns what the screen shows now.
    ///
    /// @return the facts
    [[nodiscard]] SettingsMenuFacts facts() {
        follow();
        SettingsMenuFacts now;
        const std::vector<Category> listed = categories();
        if (chosen_ >= listed.size())
            chosen_ = 0;
        for (const Category& category : listed)
            now.categories.push_back(category.shown);
        now.chosen = chosen_;
        if (listed.empty())
            return now;
        const Category& category = listed[chosen_];
        now.heading = category.shown.name;
        for (char& character : now.heading)
            if (character >= 'a' && character <= 'z')
                character = static_cast<char>(character - 'a' + 'A');
        if (category.classic) {
            SettingsMenuSection section;
            section.title = worded("CLASSIC OPTIONS");
            section.badge = "3.1c";
            init::Preferences defaults = default_preferences();
            for (const ClassicRow& row : classic_rows(*category.classic))
                section.rows.push_back(shown_row(row, defaults));
            now.sections.push_back(std::move(section));
        }
        settings::Dialog* dialog = runtime_.engine_settings_dialog();
        if (dialog != nullptr && (category.page || category.registered)) {
            show_page_of(chosen_);
            SettingsMenuSection section;
            section.title = worded("OPEN ANNIHILATION");
            engine_rows_ = settings::section_rows(*dialog);
            for (const settings::SectionRow& row : engine_rows_)
                section.rows.push_back(shown_row(row));
            if (category.page == settings::Page::developer) {
                SettingsMenuRow hacks;
                hacks.id = "standard-hacks";
                hacks.label = worded("Standard hacks");
                hacks.control = SettingsMenuControl::buttons;
                hacks.buttons = {worded("OPEN LIST...")};
                hacks.detail = worded(
                    "Developer Mode's list of the standard hacks opens in the settings dialog. "
                    "The game's own options changed here are kept."
                );
                section.rows.push_back(std::move(hacks));
            }
            if (!section.rows.empty())
                now.sections.push_back(std::move(section));
            if (category.page == settings::Page::system)
                now.notice = dialog->folder_notice;
        }
        language_section_.reset();
        language_rows_.clear();
        if (category.page == settings::Page::language) {
            SettingsMenuSection languages = languages_section();
            if (!languages.rows.empty()) {
                language_section_ = now.sections.size();
                now.sections.push_back(std::move(languages));
            }
        }
        return now;
    }

    /// Shows a category.
    ///
    /// @param entry the category, by its place
    void choose(std::size_t entry) {
        if (entry >= categories().size() || entry == chosen_)
            return;
        chosen_ = entry;
        show_page_of(chosen_);
    }

    /// Steps a row one stop.
    ///
    /// @param section the row's section
    /// @param row its place there
    /// @param up the next stop; otherwise the one before
    void step(std::size_t section, std::size_t row, bool up) {
        if (const ClassicRow* classic = classic_row_at(section, row)) {
            if (classic->usable && !classic->usable(runtime_.preferences_))
                return;
            const int32_t now = classic->index(runtime_.preferences_);
            int32_t next = now + (up ? 1 : -1);
            // A selector goes round, as the game's buttons do; a slider
            // stops at its ends.
            if (classic->control == SettingsMenuControl::selector)
                next = (next + classic->count) % std::max(classic->count, 1);
            next = std::clamp(next, 0, std::max(classic->count - 1, 0));
            if (next == now)
                return;
            classic->set(runtime_, next);
            if (classic->sounds)
                runtime_.play_ui_sound(options_sound, 0);
            return;
        }
        const settings::SectionRow* engine = engine_row_at(section, row);
        settings::Dialog* dialog = runtime_.engine_settings_dialog();
        if (engine == nullptr || dialog == nullptr)
            return;
        take(settings::dialog_control_key(
            *dialog, engine->control, up ? settings::DialogKey::right : settings::DialogKey::left
        ));
    }

    /// Moves a slider's row to a stop.
    ///
    /// @param section the row's section
    /// @param row its place there
    /// @param stop the stop
    void set_stop(std::size_t section, std::size_t row, int32_t stop) {
        if (const ClassicRow* classic = classic_row_at(section, row)) {
            if (classic->usable && !classic->usable(runtime_.preferences_))
                return;
            const int32_t held = std::clamp(stop, 0, std::max(classic->count - 1, 0));
            if (held != classic->index(runtime_.preferences_))
                classic->set(runtime_, held);
            return;
        }
        const settings::SectionRow* engine = engine_row_at(section, row);
        settings::Dialog* dialog = runtime_.engine_settings_dialog();
        if (engine == nullptr || dialog == nullptr)
            return;
        // The dialog's slider moves a stop a key; the knob goes as many
        // stops as it was dragged.
        const int32_t control = engine->control;
        int32_t now = engine->view.index;
        const int32_t target = std::clamp(stop, 0, std::max(engine->view.count - 1, 0));
        bool moved = false;
        while (now != target) {
            const bool up = target > now;
            const auto action = settings::dialog_control_key(
                *dialog, control, up ? settings::DialogKey::right : settings::DialogKey::left
            );
            if (action != settings::DialogAction::changed)
                break;
            moved = true;
            now += up ? 1 : -1;
        }
        if (moved)
            take(settings::DialogAction::changed);
    }

    /// Presses a row's button.
    ///
    /// @param section the row's section
    /// @param row its place there
    /// @param button the button, from 0 at the left
    void press(std::size_t section, std::size_t row, std::size_t button) {
        if (const ClassicRow* classic = classic_row_at(section, row)) {
            if (classic->press && (!classic->usable || classic->usable(runtime_.preferences_)))
                classic->press(runtime_, button);
            return;
        }
        if (language_section_ && section == *language_section_) {
            if (row < language_rows_.size() && button < language_rows_[row].actions.size())
                press_language(language_rows_[row].entry, language_rows_[row].actions[button]);
            return;
        }
        settings::Dialog* dialog = runtime_.engine_settings_dialog();
        if (dialog == nullptr)
            return;
        if (const settings::SectionRow* engine = engine_row_at(section, row)) {
            take(settings::dialog_control_key(
                *dialog, engine->control, settings::DialogKey::space, button
            ));
            return;
        }
        // The row after the dialog's on Developer: its list, in the
        // dialog's own look.
        if (current_page() == settings::Page::developer && section == engine_section() &&
            row == engine_rows_.size())
            hand_over();
    }

    /// Sets the dialog's settings and the game's options to their defaults.
    void restore_defaults() {
        settings::Dialog* dialog = runtime_.engine_settings_dialog();
        if (dialog != nullptr)
            take(settings::dialog_control_key(
                *dialog, settings::restore_control, settings::DialogKey::space
            ));
        auto& preferences = runtime_.preferences_;
        const init::Preferences defaults = default_preferences();
        preferences.fx_volume = defaults.fx_volume;
        preferences.sound_flags = defaults.sound_flags;
        preferences.unit_chat = defaults.unit_chat;
        runtime_.sound_spatial_ = sound_mode(preferences) == sound_mode_spatial ? 1 : 0;
        preferences.music_volume = defaults.music_volume;
        preferences.music_flags = defaults.music_flags;
        preferences.cd_mode = defaults.cd_mode;
        preferences.graphics_flags = defaults.graphics_flags;
        preferences.gamma = defaults.gamma;
        init::reset_speed_options(preferences);
        runtime_.apply_saved_volumes();
        runtime_.play_ui_sound(options_sound, 0);
    }

    /// Puts back what the screen opened with, and stays open.
    void undo() {
        settings::Dialog* dialog = runtime_.engine_settings_dialog();
        if (dialog != nullptr)
            take(settings::undo_dialog_changes(*dialog));
        put_back_game_options();
        runtime_.play_ui_sound(options_sound, 0);
    }

    /// Puts back what the screen opened with and closes the dialog.
    ///
    /// @param sound Cancel's sound plays
    void cancel(bool sound) {
        put_back_game_options();
        if (runtime_.engine_settings_dialog() != nullptr)
            std::ignore = runtime_.take_engine_settings_action(settings::DialogAction::cancelled);
        if (sound)
            runtime_.play_ui_sound(cancel_sound, 0);
    }

    /// Keeps the settings and the game's options, saves them and closes the
    /// dialog.
    void accept() {
        if (runtime_.engine_settings_dialog() != nullptr)
            std::ignore = runtime_.take_engine_settings_action(settings::DialogAction::accepted);
        runtime_.save_preferences();
        runtime_.play_ui_sound(accept_sound, 0);
    }

  private:

    /// One row of Language & Text's list of languages: the Library's entry
    /// and what its buttons do, left to right.
    struct LanguageRow {
        library::EntryId entry{};               ///< the language
        std::vector<library::Action> actions{}; ///< its buttons' actions
    };

    /// Returns one of the Library's words in the language shown.
    ///
    /// @param english the word, as text.hpp holds it
    /// @return the word shown
    [[nodiscard]] std::string library_word(std::string_view english) {
        const library::TextHooks& text = runtime_.library_host().model.text;
        if (text.shown == nullptr)
            return std::string(english);
        return std::string(text.shown(text.context, english));
    }

    /// Returns Language & Text's list of languages, from the Library's
    /// languages: the languages built into the game first, then the packs
    /// in the Library's order with no search: updates, the other installed
    /// ones, those that can be got, then those that cannot, each by name.
    /// A row shows the language's name in itself and the Library's buttons
    /// for it that can be pressed: USE NOW, GET, UPDATE, CANCEL, RETRY and
    /// UNINSTALL, at most language_buttons_most; DETAILS tells its facts.
    /// The rows' entries and actions are kept for press.
    ///
    /// @return the section; no rows while the Library lists no language
    [[nodiscard]] SettingsMenuSection languages_section() {
        LibraryHost& host = runtime_.library_host();
        // The list follows the language shown at once, as Language and
        // USE NOW move it.
        if (host.own_feed && host.refreshed &&
            host.model.inputs.language.tag != runtime_.shown_language().tag)
            host.rebuild(runtime_);
        const library::Library& model = host.model;
        std::vector<const library::Entry*> built_in;
        std::vector<const library::Entry*> packs;
        for (const library::Entry& entry : model.entries) {
            if (entry.id.kind != library::Kind::language)
                continue;
            (library::is_built_in_language(entry.id) ? built_in : packs).push_back(&entry);
        }
        const auto order = [](const library::Entry& entry) {
            switch (entry.state) {
            case library::State::update:
                return 0;
            case library::State::installed:
            case library::State::playing:
                return 1;
            case library::State::get:
                return entry.can_act ? 2 : 3;
            }
            return 3;
        };
        const auto folded = [](std::string_view name) {
            std::string lower(name);
            for (char& character : lower)
                if (character >= 'A' && character <= 'Z')
                    character = static_cast<char>(character - 'A' + 'a');
            return lower;
        };
        std::stable_sort(
            packs.begin(), packs.end(),
            [&](const library::Entry* left, const library::Entry* right) {
                const int left_order = order(*left);
                const int right_order = order(*right);
                if (left_order != right_order)
                    return left_order < right_order;
                return folded(library::entry_name(*left)) < folded(library::entry_name(*right));
            }
        );
        SettingsMenuSection section;
        section.title = worded("LANGUAGES");
        for (const std::vector<const library::Entry*>* group : {&built_in, &packs})
            for (const library::Entry* entry : *group)
                section.rows.push_back(language_row(model, *entry));
        return section;
    }

    /// Returns one language of the list as a row, and keeps its entry and
    /// its buttons' actions.
    ///
    /// @param model the Library's model
    /// @param entry the language
    /// @return the row
    [[nodiscard]] SettingsMenuRow
    language_row(const library::Library& model, const library::Entry& entry) {
        const bool built_in = library::is_built_in_language(entry.id);
        const bool in_use = built_in ? entry.state == library::State::playing
                                     : entry.installed != nullptr && entry.installed->in_use;
        LanguageRow kept{entry.id, {}};
        SettingsMenuRow shown;
        shown.id = built_in ? "language-" + std::string(library::built_in_language_tag(entry.id))
                            : "language-pack-" + entry.id.key;
        shown.label = std::string(library::entry_name(entry));
        shown.control = SettingsMenuControl::buttons;
        for (const library::ActionButton& button : library::entry_actions(model, entry)) {
            if (!button.enabled || kept.actions.size() >= language_buttons_most)
                continue;
            switch (button.action) {
            case library::Action::uninstall:
                // The pack the game shows its words from is never uninstalled.
                if (in_use)
                    continue;
                break;
            case library::Action::use_now:
            case library::Action::get:
            case library::Action::update:
            case library::Action::cancel:
            case library::Action::retry:
                break;
            default:
                continue;
            }
            kept.actions.push_back(button.action);
            shown.buttons.push_back(button.text);
        }
        const std::string status = library::status_line(model, entry);
        // Two buttons take the whole control; one leaves room for the state.
        if (shown.buttons.size() < 2) {
            if (in_use)
                shown.value = library_word(library::words::chip_in_use);
            else if (built_in)
                shown.value = library_word(library::words::registry_built_in);
            else if (entry.state == library::State::get && entry.queued == nullptr &&
                     entry.listing != nullptr)
                shown.value = library::size_text(entry.listing->size, model.text);
            else
                shown.value = status;
        }
        std::vector<std::string> lines;
        const auto add = [&lines](std::string line) {
            if (!line.empty())
                lines.push_back(std::move(line));
        };
        add(std::string(library::entry_summary(entry)));
        add(library::byline(entry, model.text));
        add(status);
        add(entry.blocked);
        for (const library::Fact& fact : library::facts(model, entry))
            add(fact.long_label + ": " + fact.long_value);
        for (const std::string& line : lines) {
            if (!shown.detail.empty())
                shown.detail += '\n';
            shown.detail += line;
        }
        if (std::find(kept.actions.begin(), kept.actions.end(), library::Action::use_now) !=
            kept.actions.end())
            shown.takes_effect = effect_of("language", kit::RowKind::choice);
        language_rows_.push_back(std::move(kept));
        return shown;
    }

    /// Presses one of a language's buttons. USE NOW chooses the language on
    /// the dialog, as its Language does, so OK keeps it and Cancel and Undo
    /// put back what the screen opened with; the others are the Library's
    /// own (Runtime::library_entry_press).
    ///
    /// @param id the language
    /// @param action the button's
    void press_language(const library::EntryId& id, library::Action action) {
        if (action != library::Action::use_now) {
            runtime_.library_entry_press(action, id);
            return;
        }
        LibraryHost& host = runtime_.library_host();
        const library::Entry* entry = library::find_entry(host.model, id);
        settings::Dialog* dialog = runtime_.engine_settings_dialog();
        if (entry == nullptr || dialog == nullptr || !library::package_can_run(host.model, *entry))
            return;
        std::string tag;
        if (library::is_built_in_language(id))
            tag = std::string(library::built_in_language_tag(id));
        else if (entry->installed != nullptr)
            tag = id.key;
        const oa::data::languages::Language* language = oa::data::languages::find_by_tag(tag);
        if (language == nullptr || !oa::data::languages::playable(*language))
            return;
        dialog->chosen.language = std::string(language->tag);
        if (language->needs == oa::data::languages::TextNeeds::modern_fonts)
            dialog->chosen.modern_fonts = true;
        take(settings::DialogAction::changed);
        runtime_.play_ui_sound(options_sound, 0);
        host.rebuild(runtime_);
    }

    /// Brings the dialog up to date, as its own screen does each frame:
    /// Hardware acceleration's status follows the renderer, Touch shows once
    /// a finger turns the touch controls on and Controller once a gamepad
    /// sends input, and Language's rows and Game Content's follow the
    /// catalogues, the download queue and the copies.
    void follow() {
        settings::Dialog* dialog = runtime_.engine_settings_dialog();
        if (dialog == nullptr)
            return;
        std::ignore =
            settings::set_acceleration_status(*dialog, runtime_.acceleration_report().status);
        std::ignore = settings::set_touch_controls(*dialog, runtime_.touch_controls_active());
        std::ignore = settings::set_controller_section(
            *dialog, runtime_.pad_used(), runtime_.pad_steam_input()
        );
        std::ignore = runtime_.refresh_settings_languages(*dialog);
        std::ignore = runtime_.refresh_game_content(*dialog);
    }

    /// Returns the categories the screen lists now: the game's options'
    /// four, the dialog's sections before its divider, then after it the
    /// files, the downloads, the system and Developer, and the registered
    /// pages.
    ///
    /// @return the categories, top to bottom
    [[nodiscard]] std::vector<Category> categories() const {
        std::vector<Category> listed;
        const auto classic = [&](std::string_view id, std::string_view name, SettingsMenuIcon icon,
                                 ClassicGroup group, std::optional<settings::Page> page) {
            Category category;
            category.shown = {std::string(id), worded(name), icon, false};
            category.classic = group;
            category.page = page;
            listed.push_back(std::move(category));
        };
        const settings::Dialog* dialog = runtime_.engine_settings_dialog();
        const std::span<const settings::Page> pages =
            dialog != nullptr ? settings::dialog_pages(
                                    dialog->kind, dialog->touch, dialog->game_files,
                                    dialog->controller
                                )
                              : std::span<const settings::Page>{};
        const auto has = [&pages](settings::Page page) {
            return std::find(pages.begin(), pages.end(), page) != pages.end();
        };
        const auto engine = [&](settings::Page page, std::string_view name, bool after) {
            if (!has(page))
                return;
            Category category;
            category.shown = {
                std::string(settings::page_word(page)), worded(name), icon_of(page), after
            };
            category.page = page;
            listed.push_back(std::move(category));
        };
        classic("sound", "Sound", SettingsMenuIcon::sound, ClassicGroup::sound, std::nullopt);
        classic("music", "Music", SettingsMenuIcon::music, ClassicGroup::music, std::nullopt);
        classic(
            "graphics", "Graphics", SettingsMenuIcon::graphics, ClassicGroup::visuals,
            has(settings::Page::graphics) ? std::optional(settings::Page::graphics) : std::nullopt
        );
        classic("game-speeds", "Game Speeds", SettingsMenuIcon::speeds, ClassicGroup::speeds, std::nullopt);
        engine(settings::Page::controls, settings::section_name(settings::Page::controls), false);
        engine(settings::Page::user_interface, "Interface", false);
        engine(settings::Page::language, settings::section_name(settings::Page::language), false);
        engine(settings::Page::common_tweaks, settings::section_name(settings::Page::common_tweaks), false);
        engine(settings::Page::touch, settings::section_name(settings::Page::touch), false);
        engine(settings::Page::controller, settings::section_name(settings::Page::controller), false);
        engine(settings::Page::game_files, "Game Files", true);
        engine(settings::Page::downloads, settings::section_name(settings::Page::downloads), true);
        engine(settings::Page::game_content, settings::section_name(settings::Page::game_content), true);
        engine(settings::Page::system, settings::section_name(settings::Page::system), true);
        engine(settings::Page::developer, settings::section_name(settings::Page::developer), true);
        if (dialog != nullptr && dialog->kind == settings::DialogKind::engine)
            for (std::size_t index = 0; index < dialog->registered_pages.size(); ++index) {
                const settings::RegisteredPage& page = dialog->registered_pages[index];
                Category category;
                category.shown = {"page-" + page.id, page.title, SettingsMenuIcon::page, true};
                category.registered = index;
                listed.push_back(std::move(category));
            }
        return listed;
    }

    /// Shows the dialog's section a category holds, as a press on its entry
    /// does.
    ///
    /// @param entry the category, by its place
    void show_page_of(std::size_t entry) {
        settings::Dialog* dialog = runtime_.engine_settings_dialog();
        const std::vector<Category> listed = categories();
        if (dialog == nullptr || entry >= listed.size())
            return;
        const Category& category = listed[entry];
        int32_t control = settings::no_control;
        if (category.page && (dialog->page != *category.page))
            control = settings::page_control(*category.page);
        else if (category.registered && (dialog->page != settings::Page::registered ||
                                         dialog->registered_page != *category.registered))
            control = settings::registered_nav_control(*category.registered);
        if (control != settings::no_control)
            take(settings::dialog_control_key(*dialog, control, settings::DialogKey::space));
    }

    /// Returns the dialog's section the chosen category shows.
    ///
    /// @return the section; none for a category of the game's options alone
    [[nodiscard]] std::optional<settings::Page> current_page() const {
        const std::vector<Category> listed = categories();
        if (chosen_ >= listed.size())
            return std::nullopt;
        if (listed[chosen_].registered)
            return settings::Page::registered;
        return listed[chosen_].page;
    }

    /// Returns the place of the chosen category's section of the dialog's
    /// rows: after the game's options' when it has them.
    ///
    /// @return the section's place
    [[nodiscard]] std::size_t engine_section() const {
        const std::vector<Category> listed = categories();
        return chosen_ < listed.size() && listed[chosen_].classic ? 1 : 0;
    }

    /// Returns a row of the game's options, by its place on the screen.
    ///
    /// @param section the row's section
    /// @param row its place there
    /// @return the row; null when it is not one of the game's options
    [[nodiscard]] const ClassicRow* classic_row_at(std::size_t section, std::size_t row) {
        const std::vector<Category> listed = categories();
        if (section != 0 || chosen_ >= listed.size() || !listed[chosen_].classic)
            return nullptr;
        classic_now_ = classic_rows(*listed[chosen_].classic);
        return row < classic_now_.size() ? &classic_now_[row] : nullptr;
    }

    /// Returns a row of the dialog's, by its place on the screen.
    ///
    /// @param section the row's section
    /// @param row its place there
    /// @return the row; null when it is not one of the dialog's
    [[nodiscard]] const settings::SectionRow* engine_row_at(std::size_t section, std::size_t row) {
        if (!current_page() || section != engine_section())
            return nullptr;
        if (const settings::Dialog* dialog = runtime_.engine_settings_dialog())
            engine_rows_ = settings::section_rows(*dialog);
        return row < engine_rows_.size() ? &engine_rows_[row] : nullptr;
    }

    /// Puts in effect what one of the dialog's events asked for.
    ///
    /// @param action what it asked
    void take(settings::DialogAction action) {
        if (action == settings::DialogAction::none)
            return;
        std::ignore = runtime_.take_engine_settings_action(action);
    }

    /// Shows the open dialog in its own look in the screen's place, on the
    /// chosen section, the game's options changed until now saved.
    void hand_over() {
        runtime_.save_preferences();
        init::capture_options_entry(runtime_.preferences_, entry_);
        // The screen leaves at its next tick, as it no longer shows; the
        // dialog's own screen stands over it until then.
        handed_over_ = true;
        runtime_.push_settings_screen(false);
    }

    /// Puts the game's options back as the screen opened with them, as the
    /// Options screen's CANCEL does.
    void put_back_game_options() {
        init::restore_all_options(runtime_.preferences_, runtime_.state_, entry_, runtime_);
        runtime_.apply_saved_volumes();
    }

    /// Returns the game's options as their panels' RESTORE sets them: the
    /// sound's, the music's, the visuals' and the speeds'. The screen size
    /// stays: the dialog's Screen size sets it.
    ///
    /// @return the preferences, every other one as it is now
    [[nodiscard]] init::Preferences default_preferences() const {
        init::Preferences defaults = runtime_.preferences_;
        oa::audio::game_audio::SoundOptions sound{
            defaults.fx_volume,
            defaults.sound_flags,
            defaults.unit_chat,
            static_cast<uint32_t>(runtime_.sound_spatial_)
        };
        oa::audio::game_audio::restore_sound_options(sound);
        defaults.fx_volume = sound.fx_volume;
        defaults.sound_flags = sound.sound_flags;
        defaults.unit_chat = sound.unit_sound_volume;
        defaults.music_volume = music_volume_default;
        defaults.music_flags = static_cast<uint16_t>(defaults.music_flags | flag::music_mode);
        defaults.cd_mode = cd_mode_default;
        init::reset_visual_options(defaults, runtime_.state_);
        defaults.display_width = runtime_.preferences_.display_width;
        defaults.display_height = runtime_.preferences_.display_height;
        init::reset_speed_options(defaults);
        return defaults;
    }

    /// Returns the sound mode a set of preferences holds.
    ///
    /// @param preferences the preferences
    /// @return 0 for no sound, then the game's modes
    [[nodiscard]] static int32_t sound_mode(const init::Preferences& preferences) noexcept {
        return static_cast<int32_t>(preferences.sound_flags & flag::sound_mode);
    }

    /// Returns a button's stages' captions from one of the game's GUI files,
    /// in the language shown, read once.
    ///
    /// @param file the GUI file, such as SOUNDS.GUI
    /// @param record the button's name
    /// @return the captions; none when the file or the button has none
    [[nodiscard]] const std::vector<std::string>& stage_captions(
        std::string_view file, std::string_view record
    ) {
        const std::string key = std::string(file) + "|" + std::string(record);
        if (const auto found = captions_.find(key); found != captions_.end())
            return found->second;
        std::vector<std::string>& captions = captions_[key];
        const auto bytes = runtime_.read(oa::data::defs::gui_path(file));
        if (!bytes)
            return captions;
        const auto parsed = oa::ui::gui_layout::parse(*bytes);
        if (!parsed.ok())
            return captions;
        const auto translated = oa::ui::gui_layout::game_translation_lookup();
        for (const auto& gadget : parsed.layout->gadgets) {
            if (gadget.common.name.size() != record.size() ||
                !std::equal(record.begin(), record.end(), gadget.common.name.begin(),
                            [](char a, char b) {
                                const auto lower = [](char c) {
                                    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
                                };
                                return lower(a) == lower(b);
                            }))
                continue;
            const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget.fields);
            if (button == nullptr || button->stages <= 0)
                break;
            std::string_view text = button->text;
            for (int32_t stage = 0; stage < button->stages; ++stage) {
                const std::size_t bar = text.find('|');
                const std::string piece(text.substr(0, bar));
                const auto shown = translated ? translated(piece) : std::nullopt;
                captions.push_back(shown ? *shown : piece);
                if (bar == std::string_view::npos)
                    break;
                text.remove_prefix(bar + 1);
            }
            break;
        }
        return captions;
    }

    /// Returns a caption of a button's stages, or a fallback.
    ///
    /// @param file the GUI file
    /// @param record the button's name
    /// @param stage the stage
    /// @param fallback the caption, in English, when the file has none for it
    /// @return the caption, in the language shown
    [[nodiscard]] std::string stage_caption(
        std::string_view file, std::string_view record, int32_t stage, std::string_view fallback
    ) {
        const auto& captions = stage_captions(file, record);
        if (stage >= 0 && static_cast<std::size_t>(stage) < captions.size() &&
            !captions[static_cast<std::size_t>(stage)].empty())
            return captions[static_cast<std::size_t>(stage)];
        return worded(fallback);
    }

    /// Returns how many stages a button of one of the game's GUI files has.
    ///
    /// @param file the GUI file
    /// @param record the button's name
    /// @param fallback the count when the file gives none
    /// @return the count, at least 1
    [[nodiscard]] int32_t stage_count(std::string_view file, std::string_view record, int32_t fallback) {
        const auto& captions = stage_captions(file, record);
        return std::max(captions.empty() ? fallback : static_cast<int32_t>(captions.size()), 1);
    }

    /// Returns the rows of a group of the game's options.
    ///
    /// @param group the group
    /// @return its rows, top to bottom
    [[nodiscard]] std::vector<ClassicRow> classic_rows(ClassicGroup group) {
        std::vector<ClassicRow> rows;
        const auto on_off = [](int32_t stage) { return worded(stage != 0 ? "On" : "Off"); };
        const auto percent = [](int32_t value, int32_t most) {
            return std::to_string((value * 100 + most / 2) / most) + "%";
        };
        const auto sound_on = [](const init::Preferences& preferences) {
            return sound_mode(preferences) != sound_mode_off;
        };
        const auto music_on = [](const init::Preferences& preferences) {
            return (preferences.music_flags & flag::music_mode) != 0;
        };
        const auto graphics_flag = [&](std::string_view id, std::string_view label,
                                       std::string_view detail, uint16_t mask) {
            ClassicRow row;
            row.id = id;
            row.label = label;
            row.detail = detail;
            row.control = SettingsMenuControl::selector;
            row.count = 2;
            row.index = [mask](const init::Preferences& preferences) {
                return (preferences.graphics_flags & mask) != 0 ? 1 : 0;
            };
            row.set = [mask](Runtime& runtime, int32_t stage) {
                auto& flags = runtime.preferences_.graphics_flags;
                flags = static_cast<uint16_t>(stage != 0 ? (flags | mask) : (flags & ~mask));
                // Unit and vehicle shadows follow the feature shadows, as
                // the visuals panel sets them.
                if (mask == flag::feature_shadows)
                    for (const uint16_t follower : {flag::shadows, flag::vehicle_shadows})
                        flags = static_cast<uint16_t>(
                            stage != 0 ? (flags | follower) : (flags & ~follower)
                        );
            };
            row.caption = on_off;
            row.sounds = true;
            rows.push_back(std::move(row));
        };
        switch (group) {
        case ClassicGroup::sound: {
            ClassicRow mode;
            mode.id = "classic-sound-mode";
            mode.label = "Sound Mode";
            mode.detail = "How the game plays its sounds, or whether it plays none.";
            mode.control = SettingsMenuControl::selector;
            mode.count = stage_count("SOUNDS.GUI", "MODE", 3);
            mode.index = [](const init::Preferences& preferences) { return sound_mode(preferences); };
            mode.set = [](Runtime& runtime, int32_t stage) {
                auto& flags = runtime.preferences_.sound_flags;
                flags = static_cast<uint16_t>(
                    (flags & ~flag::sound_mode) | (static_cast<uint16_t>(stage) & flag::sound_mode)
                );
                runtime.sound_spatial_ = stage == sound_mode_spatial ? 1 : 0;
            };
            mode.caption = [this](int32_t stage) {
                return stage_caption("SOUNDS.GUI", "MODE", stage, stage == 0 ? "Off" : "On");
            };
            mode.sounds = true;
            rows.push_back(std::move(mode));

            ClassicRow volume;
            volume.id = "classic-sound-volume";
            volume.label = "Sound Volume";
            volume.detail = "How loud the game's sound effects play.";
            volume.control = SettingsMenuControl::slider;
            volume.count = volume_most + 1;
            volume.index = [](const init::Preferences& preferences) {
                return std::clamp(static_cast<int32_t>(preferences.fx_volume), 0, volume_most);
            };
            volume.set = [](Runtime& runtime, int32_t value) {
                runtime.preferences_.fx_volume = static_cast<uint32_t>(value);
                runtime.apply_saved_volumes();
            };
            volume.caption = [percent](int32_t value) { return percent(value, volume_most); };
            volume.usable = sound_on;
            rows.push_back(std::move(volume));

            ClassicRow speech;
            speech.id = "classic-unit-speech";
            speech.label = "Unit Speech";
            speech.detail = "How much the units say when they are given orders.";
            speech.control = SettingsMenuControl::selector;
            speech.count = stage_count("SOUNDS.GUI", "SPEECH", 2);
            speech.index = [count = speech.count](const init::Preferences& preferences) {
                if ((preferences.sound_flags & flag::speech_fx) == 0)
                    return 0;
                return std::clamp(preferences.unit_chat / chat_volume_step, 0, count - 1);
            };
            speech.set = [](Runtime& runtime, int32_t stage) {
                auto& preferences = runtime.preferences_;
                preferences.sound_flags = static_cast<uint16_t>(
                    stage != 0 ? (preferences.sound_flags | flag::speech_fx)
                               : (preferences.sound_flags & ~flag::speech_fx)
                );
                preferences.unit_chat = static_cast<uint8_t>(stage * chat_volume_step);
            };
            speech.caption = [this](int32_t stage) {
                return stage_caption("SOUNDS.GUI", "SPEECH", stage, stage == 0 ? "Off" : "On");
            };
            speech.usable = sound_on;
            speech.sounds = true;
            rows.push_back(std::move(speech));

            ClassicRow test;
            test.id = "classic-test-sound";
            test.label = "Test Sound";
            test.detail = "Plays a sound at the volume set.";
            test.control = SettingsMenuControl::buttons;
            test.buttons = {worded("TEST")};
            test.press = [](Runtime& runtime, std::size_t) {
                runtime.play_wave_file(test_wave);
            };
            test.usable = sound_on;
            rows.push_back(std::move(test));
            break;
        }
        case ClassicGroup::music: {
            ClassicRow music;
            music.id = "classic-music";
            music.label = "Music";
            music.detail = "Whether the game plays its music.";
            music.control = SettingsMenuControl::selector;
            music.count = 2;
            music.index = [music_on](const init::Preferences& preferences) {
                return music_on(preferences) ? 1 : 0;
            };
            music.set = [](Runtime& runtime, int32_t stage) {
                auto& flags = runtime.preferences_.music_flags;
                flags = static_cast<uint16_t>(
                    stage != 0 ? (flags | flag::music_mode) : (flags & ~flag::music_mode)
                );
            };
            music.caption = on_off;
            music.sounds = true;
            rows.push_back(std::move(music));

            ClassicRow volume;
            volume.id = "classic-music-volume";
            volume.label = "Music Volume";
            volume.detail = "How loud the music plays.";
            volume.control = SettingsMenuControl::slider;
            volume.count = volume_most + 1;
            volume.index = [](const init::Preferences& preferences) {
                return std::clamp(static_cast<int32_t>(preferences.music_volume), 0, volume_most);
            };
            volume.set = [](Runtime& runtime, int32_t value) {
                runtime.preferences_.music_volume = static_cast<uint32_t>(value);
                runtime.apply_saved_volumes();
            };
            volume.caption = [percent](int32_t value) { return percent(value, volume_most); };
            volume.usable = music_on;
            rows.push_back(std::move(volume));

            ClassicRow mode;
            mode.id = "classic-play-mode";
            mode.label = "Play Mode";
            mode.detail = "Which tracks the music plays, and in what order.";
            mode.control = SettingsMenuControl::selector;
            mode.count = stage_count("MUSIC.GUI", "TRACKMODE", 4);
            mode.index = [count = mode.count](const init::Preferences& preferences) {
                return std::clamp(static_cast<int32_t>(preferences.cd_mode) - 1, 0, count - 1);
            };
            mode.set = [](Runtime& runtime, int32_t stage) {
                runtime.preferences_.cd_mode = static_cast<uint8_t>(stage + 1);
            };
            mode.caption = [this](int32_t stage) {
                return stage_caption("MUSIC.GUI", "TRACKMODE", stage, std::to_string(stage + 1));
            };
            mode.usable = music_on;
            mode.sounds = true;
            rows.push_back(std::move(mode));
            break;
        }
        case ClassicGroup::visuals: {
            ClassicRow gamma;
            gamma.id = "classic-gamma";
            gamma.label = "Gamma";
            gamma.detail = "How bright the game's pictures are drawn.";
            gamma.control = SettingsMenuControl::slider;
            gamma.count = gamma_most + 1;
            gamma.index = [](const init::Preferences& preferences) {
                return std::clamp(static_cast<int32_t>(preferences.gamma), 0, gamma_most);
            };
            gamma.set = [](Runtime& runtime, int32_t value) {
                runtime.preferences_.gamma = static_cast<uint32_t>(value);
                runtime.apply_saved_volumes();
            };
            gamma.caption = [percent](int32_t value) { return percent(value, gamma_most); };
            rows.push_back(std::move(gamma));
            graphics_flag(
                "classic-shadows", "Shadows",
                "Whether units, vehicles and features cast shadows.", flag::feature_shadows
            );
            graphics_flag(
                "classic-shading", "Shading", "Whether the units are shaded by the light.",
                flag::shading
            );
            graphics_flag(
                "classic-anti-aliasing", "Anti-Aliasing",
                "Whether the units' edges are smoothed, as the game smooths them.",
                flag::anti_alias
            );
            break;
        }
        case ClassicGroup::speeds: {
            const auto slider = [&](std::string_view id, std::string_view label,
                                    std::string_view detail, int32_t most,
                                    std::function<int32_t(const init::Preferences&)> index,
                                    std::function<void(Runtime&, int32_t)> set,
                                    std::function<std::string(int32_t)> caption) {
                ClassicRow row;
                row.id = id;
                row.label = label;
                row.detail = detail;
                row.control = SettingsMenuControl::slider;
                row.count = most + 1;
                row.index = std::move(index);
                row.set = std::move(set);
                row.caption = std::move(caption);
                rows.push_back(std::move(row));
            };
            slider(
                "classic-game-speed", "Game Speed",
                "How fast a game plays when it starts. 0 is the game's normal speed.",
                game_speed_most,
                [](const init::Preferences& preferences) {
                    return std::clamp(static_cast<int32_t>(preferences.game_speed), 1, game_speed_most);
                },
                [](Runtime& runtime, int32_t value) {
                    auto& preferences = runtime.preferences_;
                    preferences.game_speed = static_cast<uint16_t>(std::max(value, 1));
                },
                [](int32_t value) {
                    const int32_t relative = std::max(value, 1) - game_speed_normal;
                    return relative > 0 ? "+" + std::to_string(relative) : std::to_string(relative);
                }
            );
            slider(
                "classic-scroll-speed", "Scroll Speed",
                "How fast the view scrolls over the battlefield.", scroll_speed_most,
                [](const init::Preferences& preferences) {
                    return std::clamp(static_cast<int32_t>(preferences.scroll_speed), 1, scroll_speed_most);
                },
                [](Runtime& runtime, int32_t value) {
                    runtime.preferences_.scroll_speed = static_cast<uint8_t>(std::max(value, 1));
                },
                [](int32_t value) { return std::to_string(std::max(value, 1)); }
            );
            slider(
                "classic-message-lines", "Message Lines",
                "How many lines of messages show over the battlefield.", lines_most,
                [](const init::Preferences& preferences) {
                    return std::clamp(static_cast<int32_t>(preferences.text_lines), 0, lines_most);
                },
                [](Runtime& runtime, int32_t value) {
                    runtime.preferences_.text_lines = static_cast<uint32_t>(value);
                },
                [](int32_t value) {
                    return value == 0 ? worded("None") : std::to_string(value);
                }
            );
            slider(
                "classic-text-scroll", "Message Time",
                "How long a message stays before it scrolls away.", text_scroll_most,
                [](const init::Preferences& preferences) {
                    return std::clamp(static_cast<int32_t>(preferences.text_scroll), 0, text_scroll_most);
                },
                [](Runtime& runtime, int32_t value) {
                    runtime.preferences_.text_scroll = static_cast<uint32_t>(value);
                },
                [](int32_t value) {
                    return menu_paint::filled(worded("{count} secs"), "{count}", std::to_string(value));
                }
            );
            ClassicRow mouse;
            mouse.id = "classic-mouse";
            mouse.label = "Mouse Buttons";
            mouse.detail = "Which mouse button selects and which gives orders.";
            mouse.control = SettingsMenuControl::selector;
            mouse.count = stage_count("SPEEDS.GUI", "LEFTCLICK", 2);
            mouse.index = [count = mouse.count](const init::Preferences& preferences) {
                return std::clamp(preferences.interface_type, 0, count - 1);
            };
            mouse.set = [](Runtime& runtime, int32_t stage) {
                runtime.preferences_.interface_type = stage;
            };
            mouse.caption = [this](int32_t stage) {
                return stage_caption(
                    "SPEEDS.GUI", "LEFTCLICK", stage, stage == 0 ? "Left click" : "Right click"
                );
            };
            mouse.sounds = true;
            rows.push_back(std::move(mouse));

            ClassicRow chat;
            chat.id = "classic-unit-chat";
            chat.label = "Unit Chat Text";
            chat.detail = "How much of what the units say shows as text.";
            chat.control = SettingsMenuControl::selector;
            chat.count = stage_count("SPEEDS.GUI", "UNITCHAT", 2);
            chat.index = [count = chat.count](const init::Preferences& preferences) {
                return std::clamp(preferences.unit_chat_text / chat_volume_step, 0, count - 1);
            };
            chat.set = [](Runtime& runtime, int32_t stage) {
                runtime.preferences_.unit_chat_text = static_cast<uint8_t>(stage * chat_volume_step);
            };
            chat.caption = [this](int32_t stage) {
                return stage_caption("SPEEDS.GUI", "UNITCHAT", stage, stage == 0 ? "Off" : "On");
            };
            chat.sounds = true;
            rows.push_back(std::move(chat));
            break;
        }
        }
        return rows;
    }

    /// Returns how the screen shows one of the game's options.
    ///
    /// @param row the option
    /// @param defaults the game's options at their defaults
    /// @return the row
    [[nodiscard]] SettingsMenuRow shown_row(const ClassicRow& row, const init::Preferences& defaults) {
        SettingsMenuRow shown;
        shown.id = std::string(row.id);
        shown.label = worded(row.label);
        shown.control = row.control;
        shown.count = row.count;
        shown.detail = worded(row.detail);
        shown.buttons = row.buttons;
        shown.enabled = !row.usable || row.usable(runtime_.preferences_);
        if (row.index) {
            shown.index = row.index(runtime_.preferences_);
            shown.value = row.caption ? row.caption(shown.index) : std::to_string(shown.index);
            shown.default_value =
                row.caption ? row.caption(row.index(defaults)) : std::to_string(row.index(defaults));
            shown.takes_effect = worded("Takes effect immediately");
        }
        return shown;
    }

    /// Returns how the screen shows one of the dialog's rows.
    ///
    /// @param row the row
    /// @return the row
    [[nodiscard]] static SettingsMenuRow shown_row(const settings::SectionRow& row) {
        const kit::RowView& view = row.view;
        SettingsMenuRow shown;
        shown.id = view.id;
        shown.label = view.label;
        shown.detail = paragraph_of(view.hints);
        shown.lock = view.lock;
        shown.enabled = view.enabled && view.lock.empty();
        shown.index = view.index;
        shown.count = view.count;
        shown.default_value = row.default_value;
        shown.takes_effect = effect_of(view.id, view.kind);
        switch (view.kind) {
        case kit::RowKind::toggle:
            shown.control = SettingsMenuControl::selector;
            shown.index = view.on ? 1 : 0;
            shown.count = 2;
            shown.value = worded(view.on ? "On" : "Off");
            if (view.captions.size() == 2 && !row.default_value.empty())
                shown.default_value = worded(row.default_value == view.captions[1] ? "On" : "Off");
            break;
        case kit::RowKind::levels:
        case kit::RowKind::choice:
            shown.control = SettingsMenuControl::selector;
            shown.value = view.kind == kit::RowKind::choice ? view.value : std::string();
            if (view.kind == kit::RowKind::levels && view.index >= 0 &&
                static_cast<std::size_t>(view.index) < view.captions.size())
                shown.value = view.captions[static_cast<std::size_t>(view.index)];
            break;
        case kit::RowKind::slider:
            shown.control = SettingsMenuControl::slider;
            shown.value = view.value;
            break;
        case kit::RowKind::value_and_button:
        case kit::RowKind::buttons:
            shown.control = SettingsMenuControl::buttons;
            shown.value = view.value;
            shown.buttons = view.buttons;
            break;
        case kit::RowKind::link:
            shown.control = SettingsMenuControl::buttons;
            shown.label = view.buttons.empty() ? std::string() : view.buttons.front();
            shown.buttons = {worded("OPEN")};
            break;
        case kit::RowKind::text_field:
            shown.control = SettingsMenuControl::text;
            shown.value = view.text;
            break;
        case kit::RowKind::text:
            shown.control = SettingsMenuControl::text;
            shown.value = view.value;
            if (shown.detail.empty())
                shown.detail = view.value;
            break;
        }
        if (shown.label.empty() && !view.hints.empty())
            shown.label = view.hints.front();
        return shown;
    }

    Runtime& runtime_;                                  ///< the runtime whose settings it shows
    Screen opened_on_{runtime_.screen_};                ///< the game's screen it opened over
    init::OptionsEntrySnapshot entry_{};                ///< the game's options as the screen opened
    std::size_t chosen_{};                              ///< the category shown
    bool handed_over_{};                                ///< the dialog shows in its own look now
    std::vector<settings::SectionRow> engine_rows_{};   ///< the dialog's rows, as last read
    std::vector<ClassicRow> classic_now_{};             ///< the game's options' rows, as last read
    std::map<std::string, std::vector<std::string>> captions_{}; ///< the stages' captions read
    std::optional<std::size_t> language_section_{}; ///< the list of languages' section; none off it
    std::vector<LanguageRow> language_rows_{};      ///< the list of languages' rows, as last read
};

bool Runtime::push_settings_menu_screen() {
    std::optional<SettingsMenuGameArt> game_art = options_screen_art(assets_);
    if (!game_art)
        return false;
    auto host = std::make_shared<SettingsMenuHost>(*this);
    SettingsMenuActions actions;
    actions.choose = [host](std::size_t entry) { host->choose(entry); };
    actions.step = [host](std::size_t section, std::size_t row, bool up) {
        host->step(section, row, up);
    };
    actions.set_stop = [host](std::size_t section, std::size_t row, int32_t stop) {
        host->set_stop(section, row, stop);
    };
    actions.press = [host](std::size_t section, std::size_t row, std::size_t button) {
        host->press(section, row, button);
    };
    actions.restore_defaults = [host] { host->restore_defaults(); };
    actions.undo = [host] { host->undo(); };
    actions.cancel = [host] { host->cancel(true); };
    actions.accept = [host] { host->accept(); };
    oa_layer().push(make_settings_menu(
        [host] { return host->shows(); },
        [host] { return host->facts(); },
        std::move(actions),
        std::move(*game_art)
    ));
    return true;
}

} // namespace oa::app

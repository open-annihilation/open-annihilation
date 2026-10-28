// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The settings dialog's rows as one table: for each setting, in Setting's
// order, its row spec (its id, its kind, its label and hints, and its binding
// to the settings the dialog shows), the field of the dialog's locks that
// locks it, and how Restore defaults copies it. Every decision the dialog
// makes about one setting is read from here; adding a setting adds a row.
#pragma once

#include "oa/ui/engine_settings.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/kit/rows.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace oa::ui::engine_settings::geometry {

/// What a setting's row reads and sets: the settings, and the dialog that
/// shows them, for what a row shows beyond them (the unit limit slider's
/// highest stop, the Screen size slider's stops, Hardware acceleration's
/// status, the host's texts, the locks).
struct SettingsModel {
    EngineSettings* settings{}; ///< the settings the row reads and sets
    const Dialog* dialog{};     ///< the dialog the row is shown in
};

/// Returns a model that only reads settings: nothing is changed through it.
///
/// @param settings the settings
/// @param dialog the dialog they are shown in
/// @return the model
[[nodiscard]] inline SettingsModel
reading(const EngineSettings& settings, const Dialog& dialog) noexcept {
    return {const_cast<EngineSettings*>(&settings), &dialog};
}

/// What a setting's row is: the kit's row kinds, which kind_of gives.
using RowKind = oa::ui::kit::RowKind;

/// What MANAGE…, Game files' button, asks for: the Game files screen.
inline constexpr oa::ui::kit::ActionId manage_game_files_action = 0;
/// What Your files' first button asks for; each next button's is one more,
/// in FolderButton's order.
inline constexpr oa::ui::kit::ActionId first_folder_action = 1;
/// What MORE IN THE LIBRARY…, Language's link, asks for: the Library on
/// Languages. It follows Your files' buttons and Downloads' RESET and EMPTY
/// (downloads_page.hpp).
inline constexpr oa::ui::kit::ActionId open_library_action = 6;
/// What SHOW, Developer's UI kit gallery's button, asks for: the gallery.
/// It follows MORE IN THE LIBRARY… and Downloads' REGISTRIES IN THE LIBRARY…
/// (downloads_page.hpp).
inline constexpr oa::ui::kit::ActionId show_gallery_action = 8;
/// What SHOW AGAIN, System's Welcome setup, asks for: the welcome setup at
/// the next main menu, once OK is pressed. It follows SHOW.
inline constexpr oa::ui::kit::ActionId show_welcome_action = 9;
/// What EDIT KEYS…, Controls' Key layout, asks for: the key editor over
/// Settings, on every action. It follows ROLL BACK (updates_page.hpp).
/// CHECK NOW is 10.
inline constexpr oa::ui::kit::ActionId edit_keys_action = 15;
/// What CHOOSE…, Accessibility's Hold keys that latch, asks for: the key
/// editor over Settings, on the Latchable filter. It follows EDIT KEYS….
inline constexpr oa::ui::kit::ActionId choose_latch_action = 16;
/// What ARRANGE…, Accessibility's Overlay places, asks for: the picture of
/// the match where the overlays are placed. It follows CHOOSE….
inline constexpr oa::ui::kit::ActionId arrange_overlays_action = 17;
/// What Reset, User Interface's Reset window positions, asks for: every moved
/// window back in its place (Dialog::window_reset_asked). It stands clear of
/// the numbers the pages' own buttons take.
inline constexpr oa::ui::kit::ActionId reset_windows_action = 40;

/// Returns a setting's row spec.
///
/// @param setting the setting
/// @return its spec, in the table in Setting's order
[[nodiscard]] const oa::ui::kit::RowSpec<SettingsModel>& row_spec(Setting setting) noexcept;

/// Returns what a setting's row holds, as a text two models' rows can be
/// told apart by: a switch's On or Off, the item, level or stop chosen.
///
/// @param setting the setting
/// @param model the model the row reads
/// @return the text; empty for a row that holds no value
[[nodiscard]] std::string row_value_text(Setting setting, const SettingsModel& model);

/// Returns what a setting's row is: its spec's kind.
///
/// @param setting the setting
/// @return the kind
[[nodiscard]] oa::ui::kit::RowKind kind_of(Setting setting) noexcept;

/// Returns a setting's name, in kebab case: its row spec's id, as
/// vertical-sync for Setting::vertical_sync.
///
/// @param setting the setting
/// @return the name
[[nodiscard]] std::string_view setting_name(Setting setting) noexcept;

/// Returns the lock a setting's row has among a dialog's locks: the field
/// that locks it.
///
/// @param locks the dialog's locks
/// @param setting the setting
/// @return its lock; Lock::none for a row no field locks
[[nodiscard]] Lock lock_of(const Locks& locks, Setting setting) noexcept;

/// Returns a setting's lock among a dialog's locks, a check's own section's
/// when it gives one.
///
/// @param locks the dialog's locks
/// @param setting the setting
/// @param section a check's own section; null for the dialog's
/// @return why it cannot be changed now
[[nodiscard]] Lock section_lock(const Locks& locks, Setting setting, const SectionHooks* section);

/// Tells whether a setting's hint lines are its status, as a check's own
/// section has it when it says.
///
/// @param setting the setting
/// @param section a check's own section; null for the dialog's
/// @return true when they are
[[nodiscard]] bool section_hint_is_status(Setting setting, const SectionHooks* section);

/// Tells whether a setting's first hint line is a folder's path, which the
/// dialog shows as its tail that fits (path_tail): Your files'.
///
/// @param setting the setting
/// @return true when it is
[[nodiscard]] bool hint_is_path(Setting setting) noexcept;

/// Copies a setting's value from one model to another: through its own
/// fields where its stops or items cannot hold every value it may have (a
/// slider's value between two stops, a language not offered, a strip's level
/// not offered, the mod), else through its kind's get and set. A row that
/// keeps no setting copies nothing.
///
/// @param setting the setting
/// @param to the model it is copied into
/// @param from the model it is copied from
void copy_row(Setting setting, const SettingsModel& to, const SettingsModel& from);

/// Returns a text in name form: lower case, every character outside a to z,
/// 0 to 9 and hyphens a hyphen, a character of several bytes one hyphen,
/// and a dot kept or made a hyphen.
///
/// @param text the text, UTF-8
/// @param keep_dots dots stay, as between a hack's words
/// @return the word or words
[[nodiscard]] std::string word_form(std::string_view text, bool keep_dots);

/// Returns the word a row names one of its items or levels by to automation.
///
/// @param spec the row's spec
/// @param model the model
/// @param at the item or level, from 0
/// @return its id (kit::row_choice_id) in name form, else its place from 1
[[nodiscard]] std::string choice_word(
    const oa::ui::kit::RowSpec<SettingsModel>& spec, const SettingsModel& model, int32_t at
);

/// Returns a section's name in kebab case, as its entry's control is named:
/// engine-limits for Page::common_tweaks.
///
/// @param page the section
/// @return the name
[[nodiscard]] std::string_view page_word(Page page) noexcept;

} // namespace oa::ui::engine_settings::geometry

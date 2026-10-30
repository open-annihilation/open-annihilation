// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Language page: its row, the menu of installed languages and the packs
// available to download, the field's state while a pack is on its way, and
// the link to the Library's Languages.
// The page reads Dialog::available_languages. It does not call the runtime,
// the content service or the fonts.
#pragma once

#include "geometry.hpp"

#include "oa/ui/kit/looks.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace oa::ui::engine_settings::geometry {

/// Returns how many installed choices Language offers: System default and
/// every playable language.
///
/// @return the installed choices
[[nodiscard]] int32_t language_installed_count();

/// Returns Language's menu items: the installed choices, then, when packs
/// are available to download, a divider and one row for each pack.
///
/// @param model the model, whose dialog lists the packs
/// @return the items
[[nodiscard]] int32_t language_count(const SettingsModel& model);

/// Returns the menu item of the language chosen.
///
/// An offered language wins over an available pack with the same tag. A tag
/// that is neither shows System default. A tag matches whole, so "pt" is not
/// "pt-BR".
///
/// @param model the model
/// @return the item, from 0
[[nodiscard]] int32_t language_index(const SettingsModel& model);

/// Sets the language to a menu item.
///
/// System default and an installed language are stored as the preference
/// keeps them, and a language drawn in the modern fonts turns those fonts
/// on. An available pack's tag is stored and the fonts stay as they are.
/// The divider changes nothing. Choosing here does not ask for a download.
///
/// @param[in,out] model the model
/// @param index the item, held to the menu
void set_language(SettingsModel& model, int32_t index);

/// Returns a menu item's text: System default, a language named in itself,
/// AVAILABLE TO DOWNLOAD, or an available pack's endonym (its English name
/// when the endonym cannot be drawn).
///
/// @param model the model
/// @param index the item, from 0
/// @return the text; empty past the last item
[[nodiscard]] std::string language_caption(const SettingsModel& model, int32_t index);

/// Returns a menu item's automation word: System default's, a language's tag
/// or an available pack's tag, in name form. The divider has none.
///
/// @param model the model
/// @param index the item, from 0
/// @return the word; empty for the divider and past the last item
[[nodiscard]] std::string language_id(const SettingsModel& model, int32_t index);

/// Returns one line of Language's hint.
///
/// While an available pack is chosen and not yet installed, the line says
/// the download will switch to it, or why it failed. Otherwise it is the
/// page's own sentence, in English.
///
/// @param model the model
/// @param line the line, from 0
/// @return the line; empty past the last
[[nodiscard]] std::string language_hint(const SettingsModel& model, std::size_t line);

/// Returns how many lines Language's hint takes now.
///
/// @param model the model
/// @return one: the sentence, or the download while a pack is not yet installed
[[nodiscard]] std::size_t language_hint_lines(const SettingsModel& model);

/// Language's row: System default, the installed languages and the packs
/// available to download.
[[nodiscard]] inline constexpr kit::RowSpec<SettingsModel> language_row() noexcept {
    kit::RowSpec<SettingsModel> spec = kit::choice(
        "language",
        "Language",
        kit::Stepper<SettingsModel>{
            &language_count,
            &language_index,
            &set_language,
            &language_caption,
            nullptr,
            &language_id,
        },
        {"The game's own text and unit names, where its data has them in the language."}
    );
    spec.control_width = choice_width;
    spec.hint_text = &language_hint;
    spec.hint_lines = &language_hint_lines;
    return spec;
}

/// MORE IN THE LIBRARY…: a link under Language's row, its hint saying the
/// Library holds more languages. The section lists it while the dialog says
/// the Library can open over it (Dialog::library_link).
[[nodiscard]] inline constexpr kit::RowSpec<SettingsModel> more_in_library_row() noexcept {
    kit::RowSpec<SettingsModel> spec =
        kit::link<SettingsModel>("more-in-library", "More in the Library…", open_library_action);
    spec.hint = {"More languages are in the Library.", {}};
    return spec;
}

/// Moves a marked Language item by a step, skipping the divider one further
/// in the same direction.
///
/// A step of the menu's length reaches an end. A landing that cannot leave
/// the divider stays at the marked item. Home and End are a step of the
/// menu's length toward each end; Page Up and Page Down are a step of the
/// rows shown.
///
/// @param dialog the dialog
/// @param marked the marked item, from 0
/// @param step how many items to move; negative moves up
/// @return the item to mark
[[nodiscard]] int32_t language_mark_toward(const Dialog& dialog, int32_t marked, int32_t step);

/// What choosing a Language menu item asks for.
enum class LanguagePickKind : uint8_t {
    ordinary, ///< an installed language, or an item past the menu
    download, ///< an available pack: download its tag
    refused,  ///< the divider, or a pack whose registry downloads nothing
};

/// The outcome of choosing a Language menu item.
struct LanguagePick {
    LanguagePickKind kind{LanguagePickKind::ordinary}; ///< what the choice asks for
    std::string tag; ///< the pack to download, with LanguagePickKind::download
};

/// Returns what choosing a Language menu item asks for.
///
/// @param dialog the dialog
/// @param item the item, from 0
/// @return the outcome; ordinary for an item past the menu
[[nodiscard]] LanguagePick language_pick(const Dialog& dialog, int32_t item);

/// Returns the closed Language field's text.
///
/// While the chosen tag is an available pack that is not installed yet, the
/// field shows its name, and its state once a download has been asked for.
/// Otherwise the text is the one the caller already has.
///
/// @param dialog the dialog
/// @param current the field's text with nothing downloading
/// @return the text to draw
[[nodiscard]] std::string language_closed_field(const Dialog& dialog, std::string current);

/// Tells whether a menu item is Language's divider.
///
/// @param dialog the dialog
/// @param setting the drop-down
/// @param index the item, from 0
/// @return true for AVAILABLE TO DOWNLOAD
[[nodiscard]] bool language_item_divider(const Dialog& dialog, Setting setting, int32_t index);

/// Tells whether a press on a Language menu item chooses it.
///
/// @param dialog the dialog
/// @param setting the drop-down
/// @param index the item, from 0
/// @return false for the divider and for a pack whose downloads are off
[[nodiscard]] bool language_item_choosable(const Dialog& dialog, Setting setting, int32_t index);

/// Returns the aside drawn at the right of a Language menu item.
///
/// @param dialog the dialog
/// @param setting the drop-down
/// @param index the item, from 0
/// @return the aside; empty for an installed language, the divider and any other drop-down
[[nodiscard]] std::string language_item_aside(const Dialog& dialog, Setting setting, int32_t index);

/// Fills a Language menu's rows: each item's aside, id and whether it is a
/// divider or can be chosen. Another drop-down is left as it was.
///
/// @param dialog the dialog
/// @param setting the drop-down
/// @param[in,out] look the menu
void fill_language_menu(const Dialog& dialog, Setting setting, kit::ChoiceMenuLook& look);

} // namespace oa::ui::engine_settings::geometry

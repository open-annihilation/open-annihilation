// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Provisional: this header changes whenever OA's screens need it to, until the kit is declared stable (src/ui/kit/README.md).

// Declared rows: a settings page as a table. A row's spec holds its id, its
// English label and hints, its kind and its binding to a field of a model.
// The kit reads a type-free view of each row from its spec and the model,
// places a column of rows as the settings dialog places them, lists what they
// draw and where their controls are, and changes the model as a control is
// stepped, pressed, dragged, chosen or typed in. Nothing here looks a word
// up: whoever lays a page out passes the function that gives the text to show.
#pragma once

#include "oa/platform/preferences.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/theme.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <span>
#include <stdint.h>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace oa::ui::kit {

/// What a row is: the control it shows, and where. A row's kind is its
/// spec's; nothing else decides it.
enum class RowKind : uint8_t {
    toggle,           ///< an Off/On switch at the label line's right
    choice,           ///< a drop-down's field on its own line under the hints
    slider,           ///< a slider's track and its value on their own line under the hints
    levels,           ///< a strip of levels at the label line's right
    value_and_button, ///< a value on the label line and a button at its right
    buttons,          ///< up to three buttons side by side at the label line's right
    text_field,       ///< a field the player types in, on its own line under the hints
    link,             ///< a link alone on the label line, where the label would be
    text,             ///< a label and a value on the label line, with no control
};

/// What a row's button or link asks its screen to do. The screen numbers them.
using ActionId = int32_t;

/// A switch's Off half's words, in English, shown through the caller's text function.
inline constexpr std::string_view switch_off_text = "Off";
/// A switch's On half's words, in English, shown through the caller's text function.
inline constexpr std::string_view switch_on_text = "On";

/// The function that gives the text to show for an English text: the
/// interface catalogue's translation, or the text itself. An empty function
/// shows every text as it is written.
using ShownText = std::function<std::string_view(std::string_view)>;

/// Returns a text as a text function shows it, copied.
///
/// @param shown the text function; empty shows the text as written
/// @param english the text, in English
/// @return the text to show
[[nodiscard]] std::string shown_copy(const ShownText& shown, std::string_view english);

/// Tells whether a text is one word of a to z, 0 to 9 and hyphens, as a
/// row's id and its buttons' words are.
///
/// @param word the text
/// @return true when it holds one or more of those characters and no other
[[nodiscard]] constexpr bool row_word(std::string_view word) noexcept {
    if (word.empty())
        return false;
    for (const char character : word) {
        const bool letter = character >= 'a' && character <= 'z';
        const bool digit = character >= '0' && character <= '9';
        if (!letter && !digit && character != '-')
            return false;
    }
    return true;
}

/// Marks a row's id that is not one word. A spec built at compile time stops
/// there with an error. At run time it does nothing, and the row's control
/// name is reported by name_problem.
void row_id_is_not_one_word() noexcept;

/// The values a choice's items, a strip's levels or a slider's stops take,
/// read and set through functions of the model. Each function may be null.
template <class Model>
struct Stepper {
    /// Returns how many items, levels or stops there are.
    int32_t (*count)(const Model&){};
    /// Returns the item, level or stop chosen, from 0.
    int32_t (*get)(const Model&){};
    /// Sets the item, level or stop chosen, from 0.
    void (*set)(Model&, int32_t){};
    /// Returns a choice's item, a level's caption or the value text of a
    /// slider's stop, in English or already as shown.
    std::string (*caption)(const Model&, int32_t){};
    /// Returns how many levels from the left a player may choose; null
    /// offers every level.
    int32_t (*offered)(const Model&){};
    /// Returns a choice's item's, a level's or a stop's id, beside its
    /// caption: the word automation names it by, of a-z, 0-9 and hyphens;
    /// null gives none, and automation names it by its place from 1.
    std::string (*id)(const Model&, int32_t){};
};

/// One row of a page, declared: its id, its English label and hints, its kind
/// and its binding to a field of a model.
///
/// A factory (toggle, choice, slider, levels, value_and_button, buttons,
/// text_field, link, text) makes a spec and takes the row's id first; the
/// members a factory does not take (lock, enabled, label_text, hint_text, ...)
/// are set on the spec it returns. No spec is made without an id. Its
/// functions are plain function pointers, so a row that repeats for each
/// item of a list is bound to a small model of its own, such as the page and
/// the item's index.
template <class Model>
struct RowSpec {
    /// Makes a spec of a kind with an id; its other members are empty.
    ///
    /// An id that is not one word stops a spec made at compile time; at run
    /// time the control's name reports it (name_problem).
    ///
    /// @param row_kind what the row is
    /// @param row_id the row's word: one word of a to z, 0 to 9 and hyphens
    constexpr RowSpec(RowKind row_kind, std::string_view row_id) noexcept
        : kind(row_kind), id(row_id) {
        if (!row_word(row_id))
            row_id_is_not_one_word();
    }

    /// What the row is.
    RowKind kind{};
    /// The row's word. Its control is named <prefix>.<id>, and a
    /// value_and_button row's <prefix>.<id>.<button id> when the button has a
    /// word. An empty button word leaves the name as <prefix>.<id>.
    std::string_view id{};
    /// The label, in English.
    std::string_view label{};
    /// Returns a label that changes with the model, in English or already as
    /// shown; null shows label.
    std::string (*label_text)(const Model&){};
    /// The hint, one line each, in English, top to bottom. The row takes the
    /// lines up to the last one that is not empty.
    std::array<std::string_view, 2> hint{};
    /// A toggle's field.
    bool Model::* flag{};
    /// Returns a toggle's state, where flag is null.
    bool (*get_flag)(const Model&){};
    /// Sets a toggle's state, where flag is null.
    void (*set_flag)(Model&, bool){};
    /// A choice's field: the index of its item among captions.
    int32_t Model::* index{};
    /// A choice's items for index, in English. They outlive the spec.
    std::span<const std::string_view> captions{};
    /// Each of captions' items' id, beside it: the word automation names it
    /// by, of a-z, 0-9 and hyphens. Empty gives none. They outlive the spec.
    std::span<const std::string_view> choice_ids{};
    /// A choice's items, a strip's levels or a slider's stops, where index is null.
    Stepper<Model> stepper{};
    /// Returns what a value_and_button or a text row shows on its label line,
    /// in English or already as shown.
    std::string (*value)(const Model&){};
    /// A text field's text: the player's own, never looked up.
    std::string Model::* text{};
    /// The captions of a buttons row's buttons, a value_and_button row's one
    /// button or a link, in English, left to right. The row has the buttons
    /// before the first empty caption.
    std::array<std::string_view, 3> buttons{};
    /// Each button's word: one word of a to z, 0 to 9 and hyphens.
    std::array<std::string_view, 3> button_ids{};
    /// What each button, or the link, asks its screen to do.
    std::array<ActionId, 3> actions{};
    /// A value_and_button row's button is a risky action, such as Roll
    /// back… or emptying a folder: drawn in the danger look. Off draws the
    /// secondary look every row's button takes.
    bool danger{};
    /// Tells whether the row's control can be pressed now, such as a RESET
    /// with nothing to reset: a value_and_button's or a buttons row's buttons
    /// draw the disabled look, a link its idle colour, and neither a press nor
    /// the focus reaches the control. Null is always enabled.
    bool (*enabled)(const Model&){};
    /// A strip's level width, a drop-down's or a text field's field width, a
    /// value_and_button's button width or a link's width, in points; 0 takes
    /// the kind's own width.
    int32_t control_width{};
    /// Returns the lock's text, in English; empty while the row is unlocked.
    /// Null never locks.
    std::string_view (*lock)(const Model&){};
    /// The hint lines are the row's status: locked, a control on the label
    /// line gives way to the lock, and only the label line fades.
    bool hint_is_status{};
    /// Returns how many hint lines the row shows now, at most
    /// most_notice_lines; null counts hint's lines.
    std::size_t (*hint_lines)(const Model&){};
    /// Returns a hint line that changes with the model, in English or already
    /// as shown; null shows hint's line.
    std::string (*hint_text)(const Model&, std::size_t){};
    /// Tells whether a hint line is a notice, drawn as a lock's text is; null
    /// draws every line as a hint.
    bool (*hint_notice)(const Model&, std::size_t){};
    /// The preference key this row reads and writes, when set. A preference
    /// row uses it in place of a field of the model (preference_toggle and
    /// the other preference factories). Empty for a row bound to a field.
    std::string_view key{};
    /// The text stored for key when none is stored, or the stored text is
    /// not one this row keeps. Empty until a preference factory sets it.
    std::string_view default_text{};
    /// A preference slider's least value, used when key is set.
    double slider_least{};
    /// A preference slider's greatest value, used when key is set.
    double slider_most{};
    /// A preference slider's step, used when key is set. Zero is no slider.
    double slider_step{};
};

/// The values a preference-bound row reads and writes. The dialog fills
/// them from the stored preferences and changes them as the player does.
struct PreferenceModel {
    /// The stored texts, keyed by the rows' keys.
    oa::platform::preferences::Values values{};
};

/// A row bound to a preference key.
using PreferenceRow = RowSpec<PreferenceModel>;

/// The most bytes a preference text field stores.
inline constexpr std::size_t preference_text_most = 256;

/// Returns why a preference key is refused, or null when the key is kept.
///
/// A kept key is dotted words of a to z, 0 to 9 and hyphens, at least two
/// words, at most 64 bytes, and it does not start with open-annihilation.
///
/// @param key the key
/// @return a reason the key is refused, or null
[[nodiscard]] const char* preference_key_problem(std::string_view key) noexcept;

/// Makes a preference row of an Off/On switch. It stores 1 or 0.
///
/// @param id the row's word
/// @param label the label, in English
/// @param key the preference key
/// @param default_text the text read when none is stored, or the stored text is neither 1 nor 0
/// @param hint the hint's lines, in English
/// @return the spec
[[nodiscard]] PreferenceRow preference_toggle(
    std::string_view id,
    std::string_view label,
    std::string_view key,
    std::string_view default_text,
    std::array<std::string_view, 2> hint = {}
);

/// Makes a preference row of a drop-down. It stores one of the choices' ids.
///
/// @param id the row's word
/// @param label the label, in English
/// @param key the preference key
/// @param default_text the choice's id read when none is stored, or the stored text is not one of them
/// @param choice_ids the choices' ids, of a to z, 0 to 9 and hyphens; they outlive the spec
/// @param captions the choices' captions, in English, in the ids' order; they outlive the spec
/// @param hint the hint's lines, in English
/// @return the spec
[[nodiscard]] PreferenceRow preference_choice(
    std::string_view id,
    std::string_view label,
    std::string_view key,
    std::string_view default_text,
    std::span<const std::string_view> choice_ids,
    std::span<const std::string_view> captions,
    std::array<std::string_view, 2> hint = {}
);

/// Makes a preference row of a slider. It stores a decimal in its range, on its step.
///
/// @param id the row's word
/// @param label the label, in English
/// @param key the preference key
/// @param default_text the decimal read when none is stored, or the stored text is off the step
/// @param least the least value
/// @param most the greatest value
/// @param step the step, greater than zero
/// @param hint the hint's lines, in English
/// @return the spec
[[nodiscard]] PreferenceRow preference_slider(
    std::string_view id,
    std::string_view label,
    std::string_view key,
    std::string_view default_text,
    double least,
    double most,
    double step,
    std::array<std::string_view, 2> hint = {}
);

/// Makes a preference row of a text field. It stores the player's text, up to
/// preference_text_most bytes.
///
/// @param id the row's word
/// @param label the label, in English
/// @param key the preference key
/// @param default_text the text read when none is stored, or the stored text is longer than the field keeps
/// @param hint the hint's lines, in English
/// @return the spec
[[nodiscard]] PreferenceRow preference_text_field(
    std::string_view id,
    std::string_view label,
    std::string_view key,
    std::string_view default_text,
    std::array<std::string_view, 2> hint = {}
);

/// Tells whether a preference toggle is On.
///
/// @param model the values
/// @param spec the row
/// @return true for On
[[nodiscard]] bool preference_stored_on(const PreferenceModel& model, const PreferenceRow& spec);

/// Sets a preference toggle to On or Off.
///
/// @param[in,out] model the values
/// @param spec the row
/// @param on true for On
void preference_store_on(PreferenceModel& model, const PreferenceRow& spec, bool on);

/// Returns how many choices or stops a preference row offers.
///
/// @param spec the row
/// @return the count; 0 when the row is neither a choice nor a slider
[[nodiscard]] int32_t preference_stored_count(const PreferenceRow& spec);

/// Returns a preference choice's or slider's chosen index.
///
/// @param model the values
/// @param spec the row
/// @return the index, from 0
[[nodiscard]] int32_t
preference_stored_index(const PreferenceModel& model, const PreferenceRow& spec);

/// Sets a preference choice's or slider's chosen index.
///
/// @param[in,out] model the values
/// @param spec the row
/// @param chosen the index, from 0
void preference_store_index(PreferenceModel& model, const PreferenceRow& spec, int32_t chosen);

/// Returns a preference choice's caption or a slider stop's decimal.
///
/// @param spec the row
/// @param at the choice or the stop, from 0
/// @return its text; empty past the end
[[nodiscard]] std::string preference_stored_caption(const PreferenceRow& spec, int32_t at);

/// Returns a preference choice's id.
///
/// @param spec the row
/// @param at the choice, from 0
/// @return its id; empty past the end or when the row is not a choice
[[nodiscard]] std::string preference_stored_choice_id(const PreferenceRow& spec, int32_t at);

/// Returns a preference text field's text.
///
/// @param model the values
/// @param spec the row
/// @return the stored text, or the default when none is stored or it is too long
[[nodiscard]] std::string
preference_stored_text(const PreferenceModel& model, const PreferenceRow& spec);

/// Stores a preference text field's text.
///
/// @param[in,out] model the values
/// @param spec the row
/// @param text the text, at most preference_text_most bytes
void preference_store_text(PreferenceModel& model, const PreferenceRow& spec, std::string text);

/// Makes a row of an Off/On switch bound to a field.
///
/// @param id the row's word
/// @param label the label, in English
/// @param flag the switch's field
/// @param hint the hint's lines, in English
/// @return the spec
template <class Model>
[[nodiscard]] constexpr RowSpec<Model> toggle(
    std::string_view id,
    std::string_view label,
    bool Model::* flag,
    std::array<std::string_view, 2> hint = {}
) noexcept {
    RowSpec<Model> spec(RowKind::toggle, id);
    spec.label = label;
    spec.flag = flag;
    spec.hint = hint;
    return spec;
}

/// Makes a row of an Off/On switch read and set through functions.
///
/// @param id the row's word
/// @param label the label, in English
/// @param get returns the switch's state
/// @param set sets the switch's state
/// @param hint the hint's lines, in English
/// @return the spec
template <class Model>
[[nodiscard]] constexpr RowSpec<Model> toggle(
    std::string_view id,
    std::string_view label,
    bool (*get)(const Model&),
    void (*set)(Model&, bool),
    std::array<std::string_view, 2> hint = {}
) noexcept {
    RowSpec<Model> spec(RowKind::toggle, id);
    spec.label = label;
    spec.get_flag = get;
    spec.set_flag = set;
    spec.hint = hint;
    return spec;
}

/// Makes a row of a drop-down whose field holds the index of its item.
///
/// @param id the row's word
/// @param label the label, in English
/// @param index the field: the item's index among the captions
/// @param captions the items, in English; they outlive the spec
/// @param hint the hint's lines, in English
/// @return the spec
template <class Model>
[[nodiscard]] constexpr RowSpec<Model> choice(
    std::string_view id,
    std::string_view label,
    int32_t Model::* index,
    std::span<const std::string_view> captions,
    std::array<std::string_view, 2> hint = {}
) noexcept {
    RowSpec<Model> spec(RowKind::choice, id);
    spec.label = label;
    spec.index = index;
    spec.captions = captions;
    spec.hint = hint;
    return spec;
}

/// Makes a row of a drop-down whose field holds the index of its item, each
/// item with its id.
///
/// @param id the row's word
/// @param label the label, in English
/// @param index the field: the item's index among the captions
/// @param captions the items, in English; they outlive the spec
/// @param choice_ids each item's word, in the captions' order; they outlive the spec
/// @param hint the hint's lines, in English
/// @return the spec
template <class Model>
[[nodiscard]] constexpr RowSpec<Model> choice(
    std::string_view id,
    std::string_view label,
    int32_t Model::* index,
    std::span<const std::string_view> captions,
    std::span<const std::string_view> choice_ids,
    std::array<std::string_view, 2> hint = {}
) noexcept {
    RowSpec<Model> spec = choice(id, label, index, captions, hint);
    spec.choice_ids = choice_ids;
    return spec;
}

/// Makes a row of a drop-down whose items a stepper gives.
///
/// @param id the row's word
/// @param label the label, in English
/// @param stepper the items: their count, the chosen one and their texts
/// @param hint the hint's lines, in English
/// @return the spec
template <class Model>
[[nodiscard]] constexpr RowSpec<Model> choice(
    std::string_view id,
    std::string_view label,
    Stepper<Model> stepper,
    std::array<std::string_view, 2> hint = {}
) noexcept {
    RowSpec<Model> spec(RowKind::choice, id);
    spec.label = label;
    spec.stepper = stepper;
    spec.hint = hint;
    return spec;
}

/// Makes a row of a slider.
///
/// @param id the row's word
/// @param label the label, in English
/// @param stepper the stops: their count, the chosen one and the value text of each
/// @param hint the hint's lines, in English
/// @return the spec
template <class Model>
[[nodiscard]] constexpr RowSpec<Model> slider(
    std::string_view id,
    std::string_view label,
    Stepper<Model> stepper,
    std::array<std::string_view, 2> hint = {}
) noexcept {
    RowSpec<Model> spec(RowKind::slider, id);
    spec.label = label;
    spec.stepper = stepper;
    spec.hint = hint;
    return spec;
}

/// Makes a row of a strip of levels.
///
/// @param id the row's word
/// @param label the label, in English
/// @param stepper the levels: their count, the chosen one, their captions and those offered
/// @param level_width each level's columns, inside the strip's border, in points
/// @param hint the hint's lines, in English
/// @return the spec
template <class Model>
[[nodiscard]] constexpr RowSpec<Model> levels(
    std::string_view id,
    std::string_view label,
    Stepper<Model> stepper,
    int32_t level_width,
    std::array<std::string_view, 2> hint = {}
) noexcept {
    RowSpec<Model> spec(RowKind::levels, id);
    spec.label = label;
    spec.stepper = stepper;
    spec.control_width = level_width;
    spec.hint = hint;
    return spec;
}

/// Makes a row of a value and a button beside it.
///
/// @param id the row's word
/// @param label the label, in English; empty shows the value in its place
/// @param value returns the value, in English or already as shown; null shows none
/// @param caption the button's caption, in English
/// @param button_id the button's word
/// @param action what the button asks for
/// @param hint the hint's lines, in English
/// @return the spec
template <class Model>
[[nodiscard]] constexpr RowSpec<Model> value_and_button(
    std::string_view id,
    std::string_view label,
    std::string (*value)(const Model&),
    std::string_view caption,
    std::string_view button_id,
    ActionId action,
    std::array<std::string_view, 2> hint = {}
) noexcept {
    RowSpec<Model> spec(RowKind::value_and_button, id);
    spec.label = label;
    spec.value = value;
    spec.buttons[0] = caption;
    spec.button_ids[0] = button_id;
    spec.actions[0] = action;
    spec.hint = hint;
    return spec;
}

/// Makes a row of up to three buttons, such as Your files' folders.
///
/// The model is named, as in buttons<Settings>(...), since nothing here
/// binds it.
///
/// @param id the row's word
/// @param label the label, in English
/// @param captions the buttons' captions, in English, left to right; the row
///     has those before the first empty one
/// @param button_ids each button's word
/// @param actions what each button asks for
/// @param hint the hint's lines, in English
/// @return the spec
template <class Model>
[[nodiscard]] constexpr RowSpec<Model> buttons(
    std::string_view id,
    std::string_view label,
    std::array<std::string_view, 3> captions,
    std::array<std::string_view, 3> button_ids,
    std::array<ActionId, 3> actions,
    std::array<std::string_view, 2> hint = {}
) noexcept {
    RowSpec<Model> spec(RowKind::buttons, id);
    spec.label = label;
    spec.buttons = captions;
    spec.button_ids = button_ids;
    spec.actions = actions;
    spec.hint = hint;
    return spec;
}

/// Makes a row of a text field.
///
/// @param id the row's word
/// @param label the label, in English
/// @param text the field's text, the player's own
/// @param hint the hint's lines, in English
/// @return the spec
template <class Model>
[[nodiscard]] constexpr RowSpec<Model> text_field(
    std::string_view id,
    std::string_view label,
    std::string Model::* text,
    std::array<std::string_view, 2> hint = {}
) noexcept {
    RowSpec<Model> spec(RowKind::text_field, id);
    spec.label = label;
    spec.text = text;
    spec.hint = hint;
    return spec;
}

/// Makes a row of a link alone on its label line.
///
/// The model is named, as in link<Settings>(...), since nothing here binds it.
///
/// @param id the row's word
/// @param caption the link's words, in English
/// @param action what the link asks for
/// @return the spec
template <class Model>
[[nodiscard]] constexpr RowSpec<Model>
link(std::string_view id, std::string_view caption, ActionId action) noexcept {
    RowSpec<Model> spec(RowKind::link, id);
    spec.buttons[0] = caption;
    spec.actions[0] = action;
    return spec;
}

/// Makes a row of a label and a value, with no control.
///
/// @param id the row's word
/// @param label the label, in English
/// @param value returns the value, in English or already as shown; null shows none
/// @return the spec
template <class Model>
[[nodiscard]] constexpr RowSpec<Model>
text(std::string_view id, std::string_view label, std::string (*value)(const Model&)) noexcept {
    RowSpec<Model> spec(RowKind::text, id);
    spec.label = label;
    spec.value = value;
    return spec;
}

/// Returns how many buttons a row's captions give: those before the first
/// empty caption.
///
/// @param captions the captions, left to right
/// @return 0 to 3
[[nodiscard]] constexpr std::size_t
row_button_count(const std::array<std::string_view, 3>& captions) noexcept {
    std::size_t count = 0;
    while (count < captions.size() && !captions[count].empty())
        ++count;
    return count;
}

/// Tells whether a toggle row is On: its field, else its get function.
///
/// @param spec the row's spec
/// @param model the model
/// @return true for On; false when the spec binds no switch
template <class Model>
[[nodiscard]] bool row_on(const RowSpec<Model>& spec, const Model& model) {
    if constexpr (std::is_same_v<Model, PreferenceModel>) {
        if (!spec.key.empty())
            return preference_stored_on(model, spec);
    }
    if (spec.flag != nullptr)
        return model.*spec.flag;
    return spec.get_flag != nullptr && spec.get_flag(model);
}

/// Sets a toggle row: its field, else through its set function.
///
/// @param spec the row's spec
/// @param[in,out] model the model
/// @param on true for On
template <class Model>
void set_row_on(const RowSpec<Model>& spec, Model& model, bool on) {
    if constexpr (std::is_same_v<Model, PreferenceModel>) {
        if (!spec.key.empty()) {
            preference_store_on(model, spec, on);
            return;
        }
    }
    if (spec.flag != nullptr)
        model.*spec.flag = on;
    else if (spec.set_flag != nullptr)
        spec.set_flag(model, on);
}

/// Returns how many items, levels or stops a row offers: its captions for an
/// index field, else its stepper's count.
///
/// @param spec the row's spec
/// @param model the model
/// @return the count; 0 when the spec binds none
template <class Model>
[[nodiscard]] int32_t row_count(const RowSpec<Model>& spec, const Model& model) {
    if constexpr (std::is_same_v<Model, PreferenceModel>) {
        if (!spec.key.empty() && (spec.kind == RowKind::choice || spec.kind == RowKind::slider))
            return preference_stored_count(spec);
    }
    if (spec.index != nullptr)
        return static_cast<int32_t>(spec.captions.size());
    return spec.stepper.count != nullptr ? std::max(spec.stepper.count(model), int32_t{0}) : 0;
}

/// Returns a row's chosen item, level or stop: its index field, else its
/// stepper's.
///
/// @param spec the row's spec
/// @param model the model
/// @return the index, from 0; 0 when the spec binds none
template <class Model>
[[nodiscard]] int32_t row_index(const RowSpec<Model>& spec, const Model& model) {
    if constexpr (std::is_same_v<Model, PreferenceModel>) {
        if (!spec.key.empty() && (spec.kind == RowKind::choice || spec.kind == RowKind::slider))
            return preference_stored_index(model, spec);
    }
    if (spec.index != nullptr)
        return model.*spec.index;
    return spec.stepper.get != nullptr ? spec.stepper.get(model) : 0;
}

/// Sets a row's chosen item, level or stop: its index field, else through
/// its stepper.
///
/// @param spec the row's spec
/// @param[in,out] model the model
/// @param chosen the index, from 0
template <class Model>
void set_row_index(const RowSpec<Model>& spec, Model& model, int32_t chosen) {
    if constexpr (std::is_same_v<Model, PreferenceModel>) {
        if (!spec.key.empty() && (spec.kind == RowKind::choice || spec.kind == RowKind::slider)) {
            preference_store_index(model, spec, chosen);
            return;
        }
    }
    if (spec.index != nullptr)
        model.*spec.index = chosen;
    else if (spec.stepper.set != nullptr)
        spec.stepper.set(model, chosen);
}

/// Returns the text of a row's item, level or stop, as the spec writes it.
///
/// @param spec the row's spec
/// @param model the model
/// @param at the item, level or stop, from 0
/// @return its text, in English or already as shown; empty past the end
template <class Model>
[[nodiscard]] std::string row_caption(const RowSpec<Model>& spec, const Model& model, int32_t at) {
    if constexpr (std::is_same_v<Model, PreferenceModel>) {
        if (!spec.key.empty() && (spec.kind == RowKind::choice || spec.kind == RowKind::slider))
            return preference_stored_caption(spec, at);
    }
    if (spec.index != nullptr)
        return at >= 0 && static_cast<std::size_t>(at) < spec.captions.size()
                   ? std::string(spec.captions[static_cast<std::size_t>(at)])
                   : std::string();
    return spec.stepper.caption != nullptr ? spec.stepper.caption(model, at) : std::string();
}

/// Returns the id of a row's item, level or stop, as the spec gives it.
///
/// @param spec the row's spec
/// @param model the model
/// @param at the item, level or stop, from 0
/// @return its id: choice_ids' for an index field, else the stepper's; empty when it has none
template <class Model>
[[nodiscard]] std::string
row_choice_id(const RowSpec<Model>& spec, const Model& model, int32_t at) {
    if constexpr (std::is_same_v<Model, PreferenceModel>) {
        if (!spec.key.empty() && spec.kind == RowKind::choice)
            return preference_stored_choice_id(spec, at);
    }
    if (spec.index != nullptr)
        return at >= 0 && static_cast<std::size_t>(at) < spec.choice_ids.size()
                   ? std::string(spec.choice_ids[static_cast<std::size_t>(at)])
                   : std::string();
    return spec.stepper.id != nullptr ? spec.stepper.id(model, at) : std::string();
}

/// Returns how many levels from the left a strip offers.
///
/// @param spec the row's spec
/// @param model the model
/// @return the stepper's offered levels, or every level where it gives none, at most the count
template <class Model>
[[nodiscard]] int32_t row_offered(const RowSpec<Model>& spec, const Model& model) {
    const int32_t count = row_count(spec, model);
    if (spec.stepper.offered == nullptr)
        return count;
    return std::clamp(spec.stepper.offered(model), int32_t{0}, count);
}

/// Tells whether a row is locked now.
///
/// @param spec the row's spec
/// @param model the model
/// @return true while its lock function gives a text
template <class Model>
[[nodiscard]] bool row_locked(const RowSpec<Model>& spec, const Model& model) {
    return spec.lock != nullptr && !spec.lock(model).empty();
}

/// Tells whether a row's control can be pressed now.
///
/// @param spec the row's spec
/// @param model the model
/// @return its enabled function's answer, or true without one
template <class Model>
[[nodiscard]] bool row_enabled(const RowSpec<Model>& spec, const Model& model) {
    return spec.enabled == nullptr || spec.enabled(model);
}

/// Everything a row's placement and drawing read, owned: the spec and the
/// model read once, every word as shown. A caller may change it before laying
/// the rows out, as to give a repeated row's id its item's word, or a
/// choice's value a text cut to fit its field.
struct RowView {
    RowKind kind{};      ///< what the row is
    std::string id{};    ///< the row's word
    std::string label{}; ///< the label, as shown; a link's is empty
    /// The hint's lines, as shown, top to bottom.
    std::vector<std::string> hints{};
    /// For each hint line, true when it is a notice, drawn in the lock colour.
    std::vector<bool> notices{};
    std::string lock{};    ///< the lock's text, as shown; empty while unlocked
    bool hint_is_status{}; ///< the hint lines are the row's status
    bool on{};             ///< a switch is On
    int32_t index{};       ///< the chosen item, level or stop, from 0
    int32_t count{};       ///< the items, levels or stops
    int32_t offered{};     ///< the levels a player may choose, from the left
    /// A switch's OFF and ON, a choice's items or a strip's levels, as shown.
    std::vector<std::string> captions{};
    /// A choice's items' or a strip's levels' ids, in captions' order; an
    /// empty one where the spec gives none (row_choice_id).
    std::vector<std::string> choice_ids{};
    /// A slider's value text, a choice's field's text, or what a
    /// value_and_button or a text row shows, as shown.
    std::string value{};
    std::string text{};      ///< a text field's text, the player's own
    int32_t control_width{}; ///< the spec's control width; 0 for the kind's own
    /// The captions of the row's buttons or its link, as shown, left to right.
    std::vector<std::string> buttons{};
    std::vector<std::string> button_ids{}; ///< each button's word
    /// Each button's width, in captions' order. Empty keeps the class's widths.
    std::vector<int32_t> button_widths{};
    /// The lock's columns when a real font measures it. 0 keeps the class's lock.
    int32_t lock_columns{};
    /// A slider's value box when a real font measures the value wider than the
    /// class's box. 0 keeps that box.
    int32_t value_columns{};
    /// The control stands on the line under the label, because the label does
    /// not fit beside it.
    bool control_below{};
    /// The lock stands at the start of the line under the label, before the
    /// hints, because the label and the lock do not fit on the label line.
    /// A control that stays on that line keeps its place, and the label keeps
    /// the room beside it, or the whole line when nothing else is on it.
    /// When the control is also below, the lock is the line under the label
    /// and the control the line under the lock.
    bool lock_below{};
    bool enabled{true}; ///< its control can be pressed now
    bool danger{};      ///< its button is a risky action: the danger look
};

/// Reads a row's view from its spec and the model.
///
/// Every English text, the spec's and its functions', is passed through the
/// text function and copied, so the view keeps nothing of the spec or the
/// model. A text field's text is the player's own and is copied as it is. A
/// switch's captions are switch_off_text and switch_on_text.
///
/// @param spec the row's spec
/// @param model the model it is bound to
/// @param shown the text function; empty shows every text as written
/// @return the view
template <class Model>
[[nodiscard]] RowView
view_of(const RowSpec<Model>& spec, const Model& model, const ShownText& shown) {
    RowView view;
    view.kind = spec.kind;
    view.id = std::string(spec.id);
    if (spec.kind != RowKind::link)
        view.label = spec.label_text != nullptr ? shown_copy(shown, spec.label_text(model))
                                                : shown_copy(shown, spec.label);
    std::size_t lines = 0;
    if (spec.hint_lines != nullptr) {
        lines = std::min(
            spec.hint_lines(model), static_cast<std::size_t>(compact_metrics.most_notice_lines)
        );
    } else {
        for (std::size_t line = 0; line < spec.hint.size(); ++line)
            if (!spec.hint[line].empty())
                lines = line + 1;
    }
    for (std::size_t line = 0; line < lines; ++line) {
        if (spec.hint_text != nullptr)
            view.hints.push_back(shown_copy(shown, spec.hint_text(model, line)));
        else
            view.hints.push_back(
                shown_copy(shown, line < spec.hint.size() ? spec.hint[line] : std::string_view{})
            );
        view.notices.push_back(spec.hint_notice != nullptr && spec.hint_notice(model, line));
    }
    if (spec.lock != nullptr) {
        const std::string_view lock = spec.lock(model);
        if (!lock.empty())
            view.lock = shown_copy(shown, lock);
    }
    view.hint_is_status = spec.hint_is_status;
    view.control_width = spec.control_width;
    view.enabled = row_enabled(spec, model);
    view.danger = spec.danger;
    switch (spec.kind) {
    case RowKind::toggle:
        view.on = row_on(spec, model);
        view.captions = {shown_copy(shown, switch_off_text), shown_copy(shown, switch_on_text)};
        break;
    case RowKind::choice:
    case RowKind::slider:
    case RowKind::levels:
        view.count = row_count(spec, model);
        view.index = row_index(spec, model);
        view.offered = row_offered(spec, model);
        if (spec.kind == RowKind::slider) {
            view.value = shown_copy(shown, row_caption(spec, model, view.index));
            break;
        }
        for (int32_t at = 0; at < view.count; ++at) {
            view.captions.push_back(shown_copy(shown, row_caption(spec, model, at)));
            view.choice_ids.push_back(row_choice_id(spec, model, at));
        }
        if (spec.kind == RowKind::choice && view.index >= 0 && view.index < view.count)
            view.value = view.captions[static_cast<std::size_t>(view.index)];
        break;
    case RowKind::value_and_button:
    case RowKind::buttons:
    case RowKind::link: {
        const std::size_t count =
            spec.kind == RowKind::buttons ? row_button_count(spec.buttons) : std::size_t{1};
        for (std::size_t at = 0; at < count; ++at) {
            view.buttons.push_back(shown_copy(shown, spec.buttons[at]));
            view.button_ids.emplace_back(spec.button_ids[at]);
        }
        if (spec.kind == RowKind::value_and_button && spec.value != nullptr)
            view.value = shown_copy(shown, spec.value(model));
        break;
    }
    case RowKind::text_field:
        if constexpr (std::is_same_v<Model, PreferenceModel>) {
            if (!spec.key.empty()) {
                view.text = preference_stored_text(model, spec);
                break;
            }
        }
        if (spec.text != nullptr)
            view.text = model.*spec.text;
        break;
    case RowKind::text:
        if (spec.value != nullptr)
            view.value = shown_copy(shown, spec.value(model));
        break;
    }
    return view;
}

/// Where a column of rows is placed, in points.
struct RowPlacement {
    int32_t left{};      ///< the rows' first column
    int32_t right{};     ///< the column just right of the rows
    int32_t first_top{}; ///< the first row's rule
    /// The rows between a row's rule and its label line, and under its last
    /// part: compact_metrics.row_padding, or half of it for Developer's own rows.
    int32_t row_padding{};
    /// The words are drawn in the modern fonts: a hint's lines lie
    /// tall_hint_line_gap further apart.
    bool tall{};
    ControlId first_control{}; ///< the first row's control number; each next row's is one more
};

/// One row, placed. An empty rectangle is a part the row does not have.
struct PlacedRow {
    RowView view;                  ///< what the row shows
    ControlId control{no_control}; ///< its control's number
    int32_t top{};                 ///< the row of its rule
    int32_t height{};              ///< the rows from its rule to the next row's
    Rect label{};                  ///< its label; empty for a link
    Rect lock_area{};              ///< its padlock and lock text; empty while unlocked
    std::vector<Rect> hints{};     ///< its hint lines, top to bottom
    /// Its switch, strip, button or buttons, link, slider track, drop-down
    /// field or text field; empty for a text row. A lock never takes this
    /// place: the control stays, drawn faded.
    Rect control_area{};
    /// A slider's value; the label's box for a value_and_button or a text row,
    /// which shows its value there.
    Rect value{};
};

/// A column of rows, placed.
struct PlacedRows {
    std::vector<PlacedRow> rows; ///< one for each view, top to bottom
    int32_t bottom{};            ///< the row of the rule under the last row
    int32_t left{};              ///< the rows' first column
    int32_t right{};             ///< the column just right of the rows
};

/// Places a column of rows as the settings dialog places a section's rows.
///
/// From first_top, each row's rule at its top and its label line row_padding
/// under it, regular_line high. A switch (switch_width), a strip (its levels'
/// widths and its border), a button (button_row_width) or a row of buttons
/// (folder_button_widths and folder_button_gap) stands at the label line's
/// right, the label ending label_gap short of it. A locked row's lock,
/// lock_width wide (or the measured lock_columns), stands on the label line,
/// label_gap left of a control that sits there, or at the label line's right
/// when the control stands on a line of its own. When the label and the lock
/// do not fit on that line — with a control that stays there, the label's
/// estimated width, the lock, that control and the gaps; without one, the
/// label and the lock — the lock moves to the start of the line under the
/// label, before the hints. The label keeps the room beside a control that
/// stays, or the whole line when the lock was the only other part. A measured
/// view says so with lock_below; otherwise the estimate decides, and the
/// dropped lock is at least as wide as its words. The control stays where an
/// unlocked row puts it. A value row's value shares
/// its label's line, or, when the view says the control stands below, the
/// control's line, ending label_gap before the button. A link
/// stands alone at the label line's left, as wide
/// as the line or its control width, and short of its lock. The hint lines
/// follow hint_gap under the lowest of the label line, a lock that dropped
/// under it and a control that dropped under that, small_line each,
/// tall_hint_line_gap more between lines while tall. A slider's track and value, slider_gap
/// under the hints and slider_line_height high (the value slider_value_width
/// wide at the right, slider_value_gap from the track), a drop-down's field
/// (regular_line high) or a text field (field_height high), each slider_gap
/// under the hints and choice_width wide or the control width, stand on a
/// line of their own. row_padding follows the last part.
///
/// @param views the rows, top to bottom
/// @param placement where the column lies
/// @return the rows, placed
[[nodiscard]] PlacedRows place_rows(std::span<const RowView> views, const RowPlacement& placement);

/// Moves placed rows up, as their view scrolls. Empty rectangles stay empty.
///
/// @param[in,out] placed the rows
/// @param by the rows they move up, in points; negative moves them down
void scroll(PlacedRows& placed, int32_t by) noexcept;

/// Returns where a column of rows scrolls, as the settings dialog scrolls a
/// section: the content from the view's top to end_gap under the last row's
/// rule, and the limit that shows its end. While tall, the limit grows until
/// no hint line is cut by the view's top edge at the end of the scroll, and
/// the content's height is the limit and the view's.
///
/// @param placed the rows as place_rows placed them from the view's top, before any scroll
/// @param view what the rows are seen through
/// @param well the scroll bar's well
/// @param hit where a press holds the scroll bar
/// @param page_step how far Page Up and Page Down move, in points
/// @param end_gap the clear rows under the last row's rule at the end, in points
/// @param tall the words are drawn in the modern fonts
/// @return the scroll area at offset 0
[[nodiscard]] ScrollArea rows_scroll(
    const PlacedRows& placed,
    const Rect& view,
    const Rect& well,
    const Rect& hit,
    int32_t page_step,
    int32_t end_gap,
    bool tall
);

/// Returns one button of a buttons row in its control area: folder_button_widths
/// wide, folder_button_gap apart, from the area's left.
///
/// @param area the row's control area
/// @param at the button, from 0 at the left
/// @return its rectangle; empty past the third
[[nodiscard]] Rect row_button(const Rect& area, std::size_t at) noexcept;

/// Returns an Off/On switch's width for a caption as wide as `caption_width`.
///
/// The class's switch is kept when the caption fits in one half of it. A
/// wider caption widens both halves, with a few columns of air beside the
/// words, and the switch stays one piece.
///
/// @param caption_width the wider caption's width, in points
/// @return the switch's width, in points
[[nodiscard]] inline int32_t switch_width_for(int32_t caption_width) noexcept {
    constexpr int32_t pad = 4;
    const int32_t half = std::max(caption_width, int32_t{0});
    return std::max(compact_metrics.switch_width, (half + pad) * 2 + 2);
}

/// Returns the button of a buttons row under a point.
///
/// @param area the row's control area
/// @param count the row's buttons
/// @param point the point, in points
/// @return the button, from 0 at the left; count where none is, as between two
[[nodiscard]] std::size_t row_button_at(const Rect& area, std::size_t count, Point point) noexcept;

/// What a row's pointer and keys look like, and where its list is named and clipped.
struct RowsState {
    std::string name_prefix{};       ///< each control's name before the row's id
    ControlId hovered{no_control};   ///< the control under the pointer
    ControlId pressed{no_control};   ///< the control a held press is on
    ControlId focused{no_control};   ///< the control the shown focus is on
    ControlId open_menu{no_control}; ///< the drop-down whose menu is open
    std::size_t marked_button{};     ///< the button of a buttons row the keys mark
    std::size_t hovered_button{3};   ///< the button of a buttons row under the pointer; 3 for none
    std::size_t caret{};             ///< the focused text field's caret, in bytes
    int32_t group{-1};               ///< the controls' scroll area's number; -1 for none
    Rect clip{};                     ///< what the rows draw in and a press reaches; empty for all
};

/// Appends placed rows to a display list, as the settings dialog draws a
/// section's rows.
///
/// For each row, in order: its rule, label and hint lines (one row frame,
/// whose value_and_button and text rows' hint lines keep to their own
/// columns), its control in its look (a slider's value right of its track; a
/// value_and_button's or a text row's value on the label line, at its right,
/// or in the label's place when the row has no label), a locked row's fade
/// (its label line only when its hint is its status) and its lock, and the
/// focus ring round the focused control (a buttons row's marked button). Then
/// the rule under the last row. Every item takes the state's clip.
///
/// A row with a control area and no lock adds one control, named
/// <name_prefix>.<id> (a value_and_button row's <name_prefix>.<id>.<button
/// id> when the button has a word; with no prefix, the id alone), of the
/// row's kind (a value_and_button row's a button), taking steps unless it is
/// a button or a link, enabled as its view says, in the state's group and
/// clip. Its parts' words
/// (Control::parts) are a strip's levels' or a drop-down's items' ids, or a
/// row of buttons' buttons' words. The enabled controls take Tab in the
/// rows' order.
///
/// @param[in,out] list the display list
/// @param placed the rows
/// @param state the pointer, the focus, the names and the clip
void add_rows(DisplayList& list, const PlacedRows& placed, const RowsState& state);

/// What an event did to a row.
enum class RowEvent : uint8_t {
    none,      ///< nothing changed in the model
    changed,   ///< the model's value moved
    open_menu, ///< the drop-down's menu opens
    action,    ///< a button or the link asks for its action
};

/// An event's outcome.
struct RowResult {
    RowEvent event{};  ///< what happened
    ActionId action{}; ///< the action asked for, with RowEvent::action
};

/// Tells whether a row takes events now: it is unlocked and enabled.
///
/// @param spec the row's spec
/// @param model the model
/// @return true when a step, a press or a choice may act on it
template <class Model>
[[nodiscard]] bool row_takes_events(const RowSpec<Model>& spec, const Model& model) {
    return !row_locked(spec, model) && row_enabled(spec, model);
}

/// Returns changed when a value moved, and none otherwise.
///
/// @param moved the model's value moved
/// @return the outcome
[[nodiscard]] constexpr RowResult changed_if(bool moved) noexcept {
    return {moved ? RowEvent::changed : RowEvent::none, {}};
}

/// Steps a row's control with Left or Right: a switch to Off or On, a
/// drop-down one item, a slider one stop and a strip one level, each
/// stopping at its ends and a strip at its offered levels; a buttons row's
/// mark one button. Other kinds, and a locked or disabled row, take nothing.
///
/// @param spec the row's spec
/// @param[in,out] model the model
/// @param up true for Right, a step up
/// @param[in,out] marked_button the buttons row's marked button
/// @return changed only when the model's value moved
template <class Model>
RowResult step(const RowSpec<Model>& spec, Model& model, bool up, std::size_t& marked_button) {
    if (!row_takes_events(spec, model))
        return {};
    switch (spec.kind) {
    case RowKind::toggle: {
        const bool before = row_on(spec, model);
        set_row_on(spec, model, up);
        return changed_if(row_on(spec, model) != before);
    }
    case RowKind::choice:
    case RowKind::slider:
    case RowKind::levels: {
        const int32_t before = row_index(spec, model);
        const int32_t last =
            spec.kind == RowKind::levels ? row_offered(spec, model) : row_count(spec, model);
        const int32_t next = before + (up ? 1 : -1);
        if (next < 0 || next >= last)
            return {};
        set_row_index(spec, model, next);
        return changed_if(row_index(spec, model) != before);
    }
    case RowKind::buttons: {
        const std::size_t count = row_button_count(spec.buttons);
        if (count == 0)
            return {};
        marked_button = up ? std::min(marked_button + 1, count - 1)
                           : (marked_button == 0 ? 0 : std::min(marked_button, count) - 1);
        return {};
    }
    case RowKind::value_and_button:
    case RowKind::text_field:
    case RowKind::link:
    case RowKind::text:
        break;
    }
    return {};
}

/// Steps a row's control with Left or Right, as the step that takes a
/// buttons row's mark, which this one leaves where it is.
///
/// @param spec the row's spec
/// @param[in,out] model the model
/// @param up true for Right, a step up
/// @return changed only when the model's value moved
template <class Model>
RowResult step(const RowSpec<Model>& spec, Model& model, bool up) {
    std::size_t marked_button = 0;
    return step(spec, model, up, marked_button);
}

/// Acts on a row's control with Space: flips a switch, opens a drop-down's
/// menu, and asks for a button's, the marked button's or a link's action. A
/// slider, a strip, a text field and a text row take nothing, nor does a
/// locked or disabled row.
///
/// @param spec the row's spec
/// @param[in,out] model the model
/// @param marked_button a buttons row's marked button, from 0
/// @return what happened
template <class Model>
RowResult activate(const RowSpec<Model>& spec, Model& model, std::size_t marked_button) {
    if (!row_takes_events(spec, model))
        return {};
    switch (spec.kind) {
    case RowKind::toggle: {
        const bool before = row_on(spec, model);
        set_row_on(spec, model, !before);
        return changed_if(row_on(spec, model) != before);
    }
    case RowKind::choice:
        return {RowEvent::open_menu, {}};
    case RowKind::value_and_button:
    case RowKind::link:
        return {RowEvent::action, spec.actions[0]};
    case RowKind::buttons:
        if (marked_button >= row_button_count(spec.buttons))
            return {};
        return {RowEvent::action, spec.actions[marked_button]};
    case RowKind::slider:
    case RowKind::levels:
    case RowKind::text_field:
    case RowKind::text:
        break;
    }
    return {};
}

/// Acts on a press on a row's control: a switch's left half sets Off and its
/// right half On; a strip chooses the level under the point when it is
/// offered; a slider moves to the stop nearest the point; a drop-down opens
/// its menu; a button, the button under the point and a link ask for their
/// action. A text field, a text row, and a locked or disabled row take nothing.
///
/// @param spec the row's spec
/// @param[in,out] model the model
/// @param placed the row, placed as it is drawn
/// @param point the press, in points
/// @return what happened
template <class Model>
RowResult press(const RowSpec<Model>& spec, Model& model, const PlacedRow& placed, Point point) {
    if (!row_takes_events(spec, model))
        return {};
    const Rect& area = placed.control_area;
    switch (spec.kind) {
    case RowKind::toggle: {
        const bool before = row_on(spec, model);
        set_row_on(spec, model, point.x >= area.x + area.width / 2);
        return changed_if(row_on(spec, model) != before);
    }
    case RowKind::levels: {
        const int32_t before = row_index(spec, model);
        const std::size_t level = level_at(
            area,
            static_cast<std::size_t>(row_count(spec, model)),
            placed.view.control_width,
            point.x
        );
        if (level >= static_cast<std::size_t>(row_offered(spec, model)))
            return {};
        set_row_index(spec, model, static_cast<int32_t>(level));
        return changed_if(row_index(spec, model) != before);
    }
    case RowKind::slider: {
        const int32_t before = row_index(spec, model);
        set_row_index(spec, model, stop_at(area, point.x, row_count(spec, model)));
        return changed_if(row_index(spec, model) != before);
    }
    case RowKind::choice:
        return {RowEvent::open_menu, {}};
    case RowKind::value_and_button:
    case RowKind::link:
        return {RowEvent::action, spec.actions[0]};
    case RowKind::buttons: {
        const std::size_t count = row_button_count(spec.buttons);
        const std::size_t button = row_button_at(area, count, point);
        if (button >= count)
            return {};
        return {RowEvent::action, spec.actions[button]};
    }
    case RowKind::text_field:
    case RowKind::text:
        break;
    }
    return {};
}

/// Drags a slider's knob to the stop nearest a column. Other kinds, and a
/// locked or disabled row, take nothing.
///
/// @param spec the row's spec
/// @param[in,out] model the model
/// @param placed the row, placed as it is drawn
/// @param column the pointer's column, in points
/// @return changed only when the model's value moved
template <class Model>
RowResult drag(const RowSpec<Model>& spec, Model& model, const PlacedRow& placed, int32_t column) {
    if (spec.kind != RowKind::slider || !row_takes_events(spec, model))
        return {};
    const int32_t before = row_index(spec, model);
    set_row_index(spec, model, stop_at(placed.control_area, column, row_count(spec, model)));
    return changed_if(row_index(spec, model) != before);
}

/// Chooses a drop-down's item from its menu. An item past the ends, another
/// kind, and a locked or disabled row take nothing.
///
/// @param spec the row's spec
/// @param[in,out] model the model
/// @param item the item, from 0
/// @return changed only when the model's value moved
template <class Model>
RowResult choose(const RowSpec<Model>& spec, Model& model, int32_t item) {
    if (spec.kind != RowKind::choice || !row_takes_events(spec, model))
        return {};
    if (item < 0 || item >= row_count(spec, model))
        return {};
    const int32_t before = row_index(spec, model);
    set_row_index(spec, model, item);
    return changed_if(row_index(spec, model) != before);
}

/// Types text into a text field at its caret, through insert_text: text that
/// is not well-formed UTF-8 or holds a control character is refused whole.
/// Another kind, and a locked or disabled row, take nothing.
///
/// @param spec the row's spec
/// @param[in,out] model the model, whose text field's text changes
/// @param[in,out] caret the field's caret, in bytes; it moves past the text
/// @param utf8 the typed text, in UTF-8
/// @return changed only when the text changed
template <class Model>
RowResult
type(const RowSpec<Model>& spec, Model& model, std::size_t& caret, std::string_view utf8) {
    if (spec.kind != RowKind::text_field || !row_takes_events(spec, model))
        return {};
    if constexpr (std::is_same_v<Model, PreferenceModel>) {
        if (!spec.key.empty()) {
            const std::string before = preference_stored_text(model, spec);
            TextField field{before, caret};
            if (!insert_text(field, utf8) || field.text.size() > preference_text_most)
                return {};
            const bool moved = field.text != before;
            preference_store_text(model, spec, std::move(field.text));
            caret = field.caret;
            return changed_if(moved);
        }
    }
    if (spec.text == nullptr)
        return {};
    TextField field{model.*spec.text, caret};
    if (!insert_text(field, utf8))
        return {};
    const bool moved = field.text != model.*spec.text;
    model.*spec.text = std::move(field.text);
    caret = field.caret;
    return changed_if(moved);
}

/// Applies an editing key to a text field, through edit_text: Backspace and
/// Delete delete a character, Left, Right, Home and End move the caret.
/// Another kind, and a locked or disabled row, take nothing.
///
/// @param spec the row's spec
/// @param[in,out] model the model, whose text field's text changes
/// @param[in,out] caret the field's caret, in bytes
/// @param pressed the key
/// @return changed only when the text changed; a caret's move alone is none
template <class Model>
RowResult edit(const RowSpec<Model>& spec, Model& model, std::size_t& caret, Key pressed) {
    if (spec.kind != RowKind::text_field || !row_takes_events(spec, model))
        return {};
    if constexpr (std::is_same_v<Model, PreferenceModel>) {
        if (!spec.key.empty()) {
            const std::string before = preference_stored_text(model, spec);
            TextField field{before, caret};
            if (!edit_text(field, pressed) || field.text.size() > preference_text_most)
                return {};
            const bool moved = field.text != before;
            preference_store_text(model, spec, std::move(field.text));
            caret = field.caret;
            return changed_if(moved);
        }
    }
    if (spec.text == nullptr)
        return {};
    TextField field{model.*spec.text, caret};
    if (!edit_text(field, pressed))
        return {};
    const bool moved = field.text != model.*spec.text;
    model.*spec.text = std::move(field.text);
    caret = field.caret;
    return changed_if(moved);
}

} // namespace oa::ui::kit

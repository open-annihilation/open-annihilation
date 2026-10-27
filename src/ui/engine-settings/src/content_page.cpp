// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Game Content's rows: the sentence, each bundle, and the three buttons.

#include "content_page.hpp"

#include "oa/ui/kit/rows.hpp"
#include "oa/ui/kit/text.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::engine_settings::content {

namespace {

/// How wide each button is, so "Copiar otra vez desde…" fits the compact column.
constexpr int32_t kButtonWidth = 220;

/// How wide Cancel is, so "Abbrechen" fits.
constexpr int32_t kCancelWidth = 140;

/// The sentence under the heading, looked up whole.
constexpr std::string_view kIntro =
    "OA plays from its own copy in Documents ›\u00a0Open Annihilation. "
    "The installation is not used or changed.";

/// Makes a button row with an empty label, so the caption does not meet it.
///
/// @param id the row's word
/// @param button the button's word
/// @param caption the caption, in English
/// @return the view
kit::RowView button_row(std::string id, std::string button, std::string_view caption) {
    kit::RowView view;
    view.kind = kit::RowKind::value_and_button;
    view.id = std::move(id);
    view.button_ids.push_back(std::move(button));
    view.buttons.emplace_back(geometry::shown_text(caption));
    view.control_width = kButtonWidth;
    return view;
}

} // namespace

geometry::Rows place(
    const Dialog& dialog,
    const Locks& locks,
    const geometry::TextWidth& width,
    kit::PlacedRows* placed,
    const geometry::Sizes* sizes
) {
    static_cast<void>(locks);
    std::vector<kit::RowView> views;
    views.reserve(dialog.game_content.size() + 4);

    // The sentence breaks between words where the hint's font measures it,
    // as every row's hint does, so that a language whose letters are wider
    // than the estimate keeps within the section. A no-break space, as in
    // Documents ›\u00a0Open Annihilation, is no place to break, and is
    // measured with the words it joins.
    const std::string intro(geometry::shown_text(kIntro));
    // The rows lie where the dialog places every section's, in the sizes a
    // language's wide section names fit.
    const geometry::Sizes& sized = sizes != nullptr ? *sizes : geometry::sizes_of(dialog);
    const int32_t room =
        geometry::estimated_measure(width)
            ? static_cast<int32_t>(sized.hint_line_characters) * estimated_character_width
            : sized.content_width;
    std::vector<std::string> lines = kit::wrap(intro, std::max(room, int32_t{1}), width);
    if (lines.size() > 1 + geometry::most_row_lines)
        lines.resize(1 + geometry::most_row_lines);
    kit::RowView lead;
    lead.kind = kit::RowKind::text;
    lead.id = "plays-from";
    // Every line is drawn in the hint's font, which the lines were broken for.
    for (std::string& line : lines)
        lead.hints.push_back(std::move(line));
    views.push_back(std::move(lead));

    for (std::size_t index = 0; index < dialog.game_content.size(); ++index) {
        const GameContentRow& bundle = dialog.game_content[index];
        kit::RowView view;
        if (bundle.copying) {
            view.kind = kit::RowKind::value_and_button;
            view.button_ids.emplace_back("cancel-copy");
            view.buttons.emplace_back(geometry::shown_text("Cancel"));
            view.control_width = kCancelWidth;
        } else {
            view.kind = kit::RowKind::text;
        }
        if (kit::row_word(bundle.id))
            view.id = bundle.id;
        else
            view.id = "bundle-" + std::to_string(index);
        view.label = bundle.name;
        if (bundle.copying || bundle.checking)
            view.hints.push_back(bundle.check);
        else
            view.hints.push_back(bundle.size + " · " + bundle.check);
        if (!bundle.copying && !bundle.checking)
            view.notices.push_back(bundle.warn);
        views.push_back(std::move(view));
    }

    views.push_back(button_row("content-check", "check-again", "Check again"));
    views.push_back(button_row("content-copy", "copy-again", "Copy again from…"));
    views.push_back(button_row("content-open", "open-folder", "Open folder"));

    kit::RowPlacement placement;
    placement.left = sized.content_left;
    placement.right = sized.content_right;
    placement.first_top = sized.first_row_top;
    placement.row_padding = geometry::row_padding;
    placement.tall = false;
    placement.first_control = first_row_control;
    kit::PlacedRows kit_rows = kit::place_rows(views, placement);

    geometry::Rows rows;
    rows.bottom = kit_rows.bottom;
    rows.rows.reserve(kit_rows.rows.size());
    for (std::size_t index = 0; index < kit_rows.rows.size(); ++index) {
        const kit::PlacedRow& placed_row = kit_rows.rows[index];
        geometry::Row& row = rows.rows.emplace_back();
        row.content = static_cast<int16_t>(index);
        row.control = placed_row.control;
        row.top = placed_row.top;
        row.height = placed_row.height;
        row.label = placed_row.label;
        row.hint_lines = std::min(placed_row.hints.size(), geometry::most_row_lines);
        for (std::size_t line = 0; line < row.hint_lines; ++line)
            row.hints[line] = placed_row.hints[line];
        row.control_area = placed_row.control_area;
    }
    if (placed != nullptr)
        *placed = std::move(kit_rows);
    return rows;
}

DialogAction take(const Dialog& dialog, const geometry::Row& row) noexcept {
    const int32_t bundles = static_cast<int32_t>(dialog.game_content.size());
    if (row.content >= 1 && row.content <= bundles) {
        const GameContentRow& bundle =
            dialog.game_content[static_cast<std::size_t>(row.content - 1)];
        if (bundle.copying)
            return DialogAction::cancel_content_copy;
        return DialogAction::none;
    }
    if (row.content == bundles + 1)
        return DialogAction::check_content;
    if (row.content == bundles + 2)
        return DialogAction::copy_content_again;
    if (row.content == bundles + 3)
        return DialogAction::open_content_folder;
    return DialogAction::none;
}

} // namespace oa::ui::engine_settings::content

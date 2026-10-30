// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Updates: the update centre's card, or the status row a host without the
// card fills, and the policy rows: when Open Annihilation looks for a
// release, whether it downloads one by itself, how it installs one, and the
// version kept for a roll back. The rows are kit rows, built from the host's
// offer each time the page is laid out or one of them is used. The card's
// buttons, CHECK NOW and UPDATE… ask the host at once. The policies wait for OK.

#include "updates_page.hpp"

#include "cards.hpp"

#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/ui/kit/text.hpp"

#include <algorithm>
#include <array>
#include <ctime>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::engine_settings::geometry {

void fit_control(oa::ui::kit::RowView& shown, int32_t content_width, const TextWidth& measure);
void fit_measured(
    oa::ui::kit::RowView& shown,
    int32_t content_width,
    const TextWidth& small,
    const TextWidth* regular
);

} // namespace oa::ui::engine_settings::geometry

namespace oa::ui::engine_settings::updates {

namespace {

namespace kit = oa::ui::kit;

/// The check's hint.
constexpr std::string_view kModeHint =
    "OA looks for a newer Open Annihilation at every start and every hour while it runs. "
    "It never installs one until you say so.";
/// Said in place of the status while the check is Never.
constexpr std::string_view kNeverStatus = "OA never checks";
/// The check's captions, in UpdateCheck's order.
constexpr std::array<std::string_view, 3> kCaptions{
    "Automatically",
    "Only when I ask",
    "Never",
};
/// The check's ids, in the same order.
constexpr std::array<std::string_view, 3> kChoiceIds{"automatically", "when-asked", "never"};
/// The background switch's hint.
constexpr std::string_view kDownloadHint = "Off: OA asks before downloading.";
/// Install updates' hint.
constexpr std::string_view kInstallHint = "Ask each time, or install when I quit.";
/// Install updates' captions, in UpdateInstall's order.
constexpr std::array<std::string_view, 2> kInstallCaptions{
    "Ask each time",
    "When I quit",
};
/// Install updates' ids, in the same order.
constexpr std::array<std::string_view, 2> kInstallIds{"ask", "when-i-quit"};
/// Previous version's label; {version} is the kept one.
constexpr std::string_view kPreviousLabel = "Previous version \u00b7 {version}";
/// Previous version's hint; {version} is the kept one.
constexpr std::string_view kPreviousHint =
    "Roll back restores {version} and the settings you had before updating.";
/// The rows' places among the page's rows, by what each is.
constexpr std::size_t kStatusRow = 0;
constexpr std::size_t kCheckRow = 1;
constexpr std::size_t kDownloadRow = 2;
constexpr std::size_t kPreviousRow = 3;
constexpr std::size_t kInstallRow = 4;

/// One placed row: the dialog and the row's place.
struct RowModel {
    Dialog* dialog{}; ///< the dialog
    /// kStatusRow, kCheckRow, kDownloadRow, kPreviousRow or kInstallRow
    std::size_t index{};
};

/// Tells whether the status row shows a button.
///
/// Never hides it. A release shows UPDATE…. A check that can run, or a
/// status with nothing to press yet, shows CHECK NOW.
///
/// @param offer what the host filled
/// @return true when the button is drawn
bool button_shown(const UpdatesOffer& offer) noexcept {
    if (offer.show_download &&
        (offer.download == DownloadShown::ready || offer.download == DownloadShown::stopped))
        return true;
    if (offer.mode == static_cast<int32_t>(UpdateCheck::never))
        return false;
    if (offer.offered || offer.can_check)
        return true;
    return !offer.status.empty();
}

/// Tells whether the button takes a press. Checking leaves it greyed.
///
/// @param model the status row
/// @return true when it can be pressed
bool button_enabled(const RowModel& model) noexcept {
    if (model.dialog == nullptr)
        return false;
    const UpdatesOffer& offer = model.dialog->updates;
    if (offer.show_download &&
        (offer.download == DownloadShown::ready || offer.download == DownloadShown::stopped))
        return true;
    if (offer.offered)
        return true;
    return offer.can_check;
}

/// Returns the version line, already as the page shows it.
///
/// @param model the status row
/// @return the line; the product's name while none is filled
std::string version_label(const RowModel& model) {
    if (model.dialog == nullptr || model.dialog->updates.version_line.empty())
        return "Open Annihilation";
    return model.dialog->updates.version_line;
}

/// Returns the status line. Never names itself, whatever a check found.
///
/// @param model the status row
/// @param line the line, from 0
/// @return the line
std::string status_hint(const RowModel& model, std::size_t line) {
    if (line != 0 || model.dialog == nullptr)
        return {};
    const UpdatesOffer& offer = model.dialog->updates;
    if (offer.show_download) {
        switch (offer.download) {
        case DownloadShown::downloading:
            return geometry::filled(
                "Downloading {version} \u00b7 {done} of {size}",
                {{"version", offer.download_version},
                 {"done", offer.download_done},
                 {"size", offer.download_size}}
            );
        case DownloadShown::paused:
            return geometry::filled("Paused during the game", {});
        case DownloadShown::ready:
            return geometry::filled(
                "{version} is ready \u00b7 {size} downloaded",
                {{"version", offer.download_version}, {"size", offer.download_size}}
            );
        case DownloadShown::stopped:
            return geometry::filled(
                "The download stopped: {reason}", {{"reason", offer.download_reason}}
            );
        case DownloadShown::none:
            break;
        }
    }
    // Never names itself, whatever a check found. A packager's name is
    // already filled, so it is joined after the status is looked up: the
    // whole line is then not a word, and a second lookup keeps it.
    std::string line_text;
    if (offer.mode == static_cast<int32_t>(UpdateCheck::never))
        line_text = std::string(kNeverStatus);
    else
        line_text = offer.status;
    if (offer.updated_by.empty())
        return line_text;
    line_text = std::string(geometry::shown_text(line_text));
    line_text += " \u00b7 ";
    line_text += offer.updated_by;
    return line_text;
}

/// Returns how many status lines the row shows before they wrap.
///
/// @return 1
std::size_t status_hint_lines(const RowModel&) noexcept {
    return 1;
}

/// Returns the check's hint, before it wraps.
///
/// @param line the line, from 0
/// @return the hint
std::string mode_hint(const RowModel&, std::size_t line) {
    if (line != 0)
        return {};
    return std::string(kModeHint);
}

/// Returns how many hint lines the check shows before they wrap.
///
/// @return 1
std::size_t mode_hint_lines(const RowModel&) noexcept {
    return 1;
}

/// Returns the background switch. It takes effect at OK.
///
/// @param model the row
/// @return true when the switch is on
bool download_get(const RowModel& model) noexcept {
    return model.dialog != nullptr && model.dialog->chosen.update_download;
}

/// Sets the background switch. The stored preference waits for OK.
///
/// @param[in,out] model the row
/// @param on true for On
void download_set(RowModel& model, bool on) {
    if (model.dialog == nullptr)
        return;
    model.dialog->chosen.update_download = on;
}

/// Returns the switch's hint, before it wraps.
///
/// @param line the line, from 0
/// @return the hint
std::string download_hint(const RowModel&, std::size_t line) {
    if (line != 0)
        return {};
    return std::string(kDownloadHint);
}

/// Returns how many hint lines the switch shows before they wrap.
///
/// @return 1
std::size_t download_hint_lines(const RowModel&) noexcept {
    return 1;
}

/// "Previous version · {version}", already in the language shown.
///
/// @param model the row
/// @return the label
std::string previous_label(const RowModel& model) {
    if (model.dialog == nullptr)
        return {};
    return geometry::filled(kPreviousLabel, {{"version", model.dialog->updates.kept_version}});
}

/// Returns Previous version's hint, before it wraps.
///
/// @param model the row
/// @param line the line, from 0
/// @return the hint
std::string previous_hint(const RowModel& model, std::size_t line) {
    if (line != 0 || model.dialog == nullptr)
        return {};
    return geometry::filled(kPreviousHint, {{"version", model.dialog->updates.kept_version}});
}

/// Returns how many hint lines Previous version shows before they wrap.
///
/// @return 1
std::size_t previous_hint_lines(const RowModel&) noexcept {
    return 1;
}

/// Returns how many ways Install updates offers.
///
/// @return 2
int32_t install_count(const RowModel&) noexcept {
    return static_cast<int32_t>(kInstallCaptions.size());
}

/// Returns the way Install updates shows.
///
/// @param model the row
/// @return its index
int32_t install_get(const RowModel& model) noexcept {
    if (model.dialog == nullptr)
        return 0;
    return static_cast<int32_t>(model.dialog->chosen.update_install);
}

/// Sets Install updates. The stored preference waits for OK.
///
/// @param[in,out] model the row
/// @param index the way, from 0; past the last changes nothing
void install_set(RowModel& model, int32_t index) {
    if (model.dialog == nullptr || index < 0 ||
        static_cast<std::size_t>(index) >= kInstallCaptions.size())
        return;
    model.dialog->chosen.update_install = static_cast<UpdateInstall>(index);
}

/// Returns an Install updates way's caption.
///
/// @param index the way
/// @return its caption, in English
std::string install_caption(const RowModel&, int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= kInstallCaptions.size())
        return {};
    return std::string(kInstallCaptions[static_cast<std::size_t>(index)]);
}

/// Returns an Install updates way's id.
///
/// @param index the way
/// @return its id
std::string install_choice_id(const RowModel&, int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= kInstallIds.size())
        return {};
    return std::string(kInstallIds[static_cast<std::size_t>(index)]);
}

/// Returns Install updates' hint, before it wraps.
///
/// @param line the line, from 0
/// @return the hint
std::string install_hint(const RowModel&, std::size_t line) {
    if (line != 0)
        return {};
    return std::string(kInstallHint);
}

/// Returns how many hint lines Install updates shows before they wrap.
///
/// @return 1
std::size_t install_hint_lines(const RowModel&) noexcept {
    return 1;
}

/// Returns the card's heading, the status row's label while the card shows.
///
/// @param model the row
/// @return the heading, as the host gave it
std::string card_label(const RowModel& model) {
    if (model.dialog == nullptr)
        return {};
    return model.dialog->updates.card_status;
}

/// The lock ROLL BACK shows during a game. The page reads the same field
/// Downloads does: a game sets path_search, and that is what stops the row.
std::string_view previous_lock(const RowModel& model) {
    if (model.dialog == nullptr || model.dialog->locks.path_search == Lock::none)
        return {};
    return geometry::lock_text(Lock::in_game);
}

/// Returns how many ways the check offers.
///
/// @return 3
int32_t check_count(const RowModel&) noexcept {
    return static_cast<int32_t>(kCaptions.size());
}

/// Returns the check the dialog shows.
///
/// @param model the row
/// @return its index
int32_t check_get(const RowModel& model) noexcept {
    if (model.dialog == nullptr)
        return 0;
    return static_cast<int32_t>(model.dialog->chosen.update_check);
}

/// Sets the check. The stored preference waits for OK.
///
/// @param[in,out] model the row
/// @param index the way, from 0
void check_set(RowModel& model, int32_t index) {
    if (model.dialog == nullptr || index < 0 || static_cast<std::size_t>(index) >= kCaptions.size())
        return;
    model.dialog->chosen.update_check = static_cast<UpdateCheck>(index);
    follow_mode(*model.dialog);
}

/// Returns a way's caption.
///
/// @param index the way
/// @return its caption
std::string check_caption(const RowModel&, int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= kCaptions.size())
        return {};
    return std::string(kCaptions[static_cast<std::size_t>(index)]);
}

/// Returns a way's id.
///
/// @param index the way
/// @return its id
std::string check_choice_id(const RowModel&, int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= kChoiceIds.size())
        return {};
    return std::string(kChoiceIds[static_cast<std::size_t>(index)]);
}

/// Returns the spec of one row.
///
/// @param model the row
/// @return the spec
kit::RowSpec<RowModel> spec_for(const RowModel& model) {
    if (model.index == kDownloadRow) {
        kit::RowSpec<RowModel> spec = kit::toggle<RowModel>(
            "update-download", "Download automatically", download_get, download_set
        );
        spec.hint_lines = download_hint_lines;
        spec.hint_text = download_hint;
        return spec;
    }
    if (model.index == kInstallRow) {
        kit::Stepper<RowModel> stepper;
        stepper.count = install_count;
        stepper.get = install_get;
        stepper.set = install_set;
        stepper.caption = install_caption;
        stepper.id = install_choice_id;
        kit::RowSpec<RowModel> spec = kit::choice("update-install", "Install updates", stepper);
        spec.control_width = geometry::wide_choice_width;
        spec.hint_lines = install_hint_lines;
        spec.hint_text = install_hint;
        return spec;
    }
    if (model.index == kStatusRow && model.dialog != nullptr &&
        cards::update_card_shown(*model.dialog)) {
        // The card: its first button is the row's control; the page lays the
        // card over the row (place).
        const UpdatesOffer& offer = model.dialog->updates;
        if (offer.actions.empty()) {
            kit::RowSpec<RowModel> spec = kit::text<RowModel>("update-status", "", nullptr);
            spec.label_text = card_label;
            return spec;
        }
        kit::RowSpec<RowModel> spec = kit::value_and_button<RowModel>(
            "update-status",
            "",
            nullptr,
            offer.actions.front().caption,
            kit::row_word(offer.actions.front().id) ? std::string_view(offer.actions.front().id)
                                                    : std::string_view{},
            first_card_action
        );
        spec.label_text = card_label;
        return spec;
    }
    if (model.index == kStatusRow) {
        const UpdatesOffer& offer = model.dialog->updates;
        const bool restart = offer.show_download && offer.download == DownloadShown::ready;
        const bool retry = offer.show_download && offer.download == DownloadShown::stopped;
        const bool offered = offer.offered && button_shown(offer) && !restart && !retry;
        if (!button_shown(offer)) {
            kit::RowSpec<RowModel> spec = kit::text<RowModel>("update-status", "", nullptr);
            spec.label_text = version_label;
            spec.hint_lines = status_hint_lines;
            spec.hint_text = status_hint;
            return spec;
        }
        const char* caption = "Check now";
        const char* ident = "check-now";
        int32_t action = check_updates_action;
        int32_t width = geometry::manage_button_width;
        if (restart) {
            caption = "Restart to update";
            ident = "restart";
            action = restart_update_action;
            width = geometry::lock_width;
        } else if (retry) {
            caption = "Retry";
            ident = "retry";
            action = retry_update_action;
        } else if (offered) {
            caption = "Update\u2026";
            ident = "update";
            action = open_release_action;
        }
        kit::RowSpec<RowModel> spec =
            kit::value_and_button<RowModel>("update-status", "", nullptr, caption, ident, action);
        spec.label_text = version_label;
        spec.control_width = width;
        spec.enabled = button_enabled;
        spec.hint_lines = status_hint_lines;
        spec.hint_text = status_hint;
        return spec;
    }
    if (model.index == kPreviousRow) {
        kit::RowSpec<RowModel> spec = kit::value_and_button<RowModel>(
            "update-previous", "", nullptr, "Roll back\u2026", "roll-back", roll_back_action
        );
        spec.label_text = previous_label;
        spec.hint_lines = previous_hint_lines;
        spec.hint_text = previous_hint;
        spec.control_width = geometry::manage_button_width;
        spec.lock = previous_lock;
        // Rolling back is the page's risky action.
        spec.danger = true;
        return spec;
    }
    kit::Stepper<RowModel> stepper;
    stepper.count = check_count;
    stepper.get = check_get;
    stepper.set = check_set;
    stepper.caption = check_caption;
    stepper.id = check_choice_id;
    kit::RowSpec<RowModel> spec = kit::choice("update-check", "Check for updates", stepper);
    spec.control_width = geometry::wide_choice_width;
    spec.hint_lines = mode_hint_lines;
    spec.hint_text = mode_hint;
    return spec;
}

/// Builds the model of a placed row.
///
/// @param dialog the dialog
/// @param row the row
/// @return the model; a row that is not Updates' is the status row
RowModel model_of(Dialog& dialog, const geometry::Row& row) {
    const auto index = row.updates >= 0 ? static_cast<std::size_t>(row.updates) : std::size_t{0};
    return {&dialog, index};
}

/// Joins a row's looked-up hint lines and wraps them to the section.
///
/// @param[in,out] shown the row
/// @param room the hint's width
/// @param measure a text's width in the small font
void wrap_shown_hints(kit::RowView& shown, int32_t room, const geometry::TextWidth& measure) {
    std::string paragraph;
    for (const std::string& line : shown.hints) {
        if (line.empty())
            continue;
        if (!paragraph.empty())
            paragraph.push_back(' ');
        paragraph += line;
    }
    if (paragraph.empty()) {
        shown.hints.clear();
        shown.notices.clear();
        return;
    }
    std::vector<std::string> lines = kit::wrap(paragraph, std::max(room, int32_t{1}), measure);
    if (lines.empty())
        lines.push_back(std::move(paragraph));
    if (lines.size() > geometry::most_row_lines)
        lines.resize(geometry::most_row_lines);
    shown.hints = std::move(lines);
    shown.notices.assign(shown.hints.size(), false);
}

} // namespace

geometry::Rows place(
    const Dialog& dialog,
    const Locks& locks,
    const geometry::TextWidth& width,
    kit::PlacedRows* placed,
    const geometry::Sizes* sizes,
    const geometry::TextWidth* regular
) {
    const geometry::Sizes& sized = sizes != nullptr ? *sizes : geometry::sizes_of(dialog);
    const geometry::TextWidth measure =
        static_cast<bool>(width) ? width : geometry::TextWidth{&geometry::estimated_width};
    const bool estimated = measure("n") == kit::estimated_character_width &&
                           measure("W") == kit::estimated_character_width;
    const int32_t room = estimated ? static_cast<int32_t>(sized.hint_line_characters) *
                                         kit::estimated_character_width
                                   : sized.content_width;
    // The card or the status row first. Where this copy installs releases
    // (the host shows a download), Download automatically sits before the
    // check and Install updates after it. Previous version follows, and only
    // while a version is kept, so a page with nothing kept keeps the rows a
    // picture was drawn with.
    std::vector<std::size_t> order;
    order.push_back(kStatusRow);
    if (dialog.updates.show_download)
        order.push_back(kDownloadRow);
    order.push_back(kCheckRow);
    if (dialog.updates.show_download)
        order.push_back(kInstallRow);
    if (!dialog.updates.kept_version.empty())
        order.push_back(kPreviousRow);
    const bool previous_locked = locks.path_search != Lock::none;
    std::vector<kit::RowView> row_views;
    row_views.reserve(order.size());
    for (const std::size_t index : order) {
        const RowModel model{const_cast<Dialog*>(&dialog), index};
        const kit::RowSpec<RowModel> spec = spec_for(model);
        kit::RowView shown = kit::view_of(spec, model, &geometry::shown_text);
        wrap_shown_hints(shown, room, measure);
        geometry::fit_control(shown, sized.content_width, measure);
        geometry::fit_measured(shown, sized.content_width, measure, regular);
        row_views.push_back(std::move(shown));
    }

    kit::RowPlacement placement;
    placement.left = sized.content_left;
    placement.right = sized.content_right;
    placement.first_top = sized.first_row_top;
    placement.row_padding = geometry::row_padding;
    placement.tall = oa::data::languages::interface_language().needs !=
                     oa::data::languages::TextNeeds::game_fonts;
    placement.first_control = first_row_control;
    geometry::modern_control_heights(placement, dialog, sized.size_class);
    kit::PlacedRows kit_rows = kit::place_rows(row_views, placement);
    // The card stands over the first row: the row takes the card's height,
    // the rows under it move down by as much, and its control is the card's
    // first button.
    if (cards::update_card_shown(dialog) && !kit_rows.rows.empty()) {
        const geometry::TextWidth regular_measure =
            regular != nullptr && static_cast<bool>(*regular) ? *regular : measure;
        kit::PlacedRow& card_row = kit_rows.rows.front();
        const int32_t margin = kit::metrics_of(sized.size_class).space * 2;
        const cards::UpdateCard card =
            cards::update_card(dialog, sized, card_row.top + margin, regular_measure, measure);
        const int32_t height = card.height + 2 * margin;
        const int32_t moved = height - card_row.height;
        card_row.height = height;
        card_row.label = card.lines.empty() ? kit::Rect{} : card.lines.front().rect;
        card_row.lock_area = {};
        card_row.hints.clear();
        card_row.value = {};
        card_row.control_area = card.buttons.empty() ? kit::Rect{} : card.buttons.front();
        for (std::size_t later = 1; later < kit_rows.rows.size(); ++later) {
            kit::PlacedRow& following = kit_rows.rows[later];
            following.top += moved;
            for (kit::Rect* rect :
                 {&following.label,
                  &following.lock_area,
                  &following.control_area,
                  &following.value})
                if (rect->width > 0 && rect->height > 0)
                    rect->y += moved;
            for (kit::Rect& hint : following.hints)
                hint.y += moved;
        }
        kit_rows.bottom += moved;
    }

    geometry::Rows rows;
    rows.bottom = kit_rows.bottom;
    rows.rows.reserve(kit_rows.rows.size());
    for (std::size_t index = 0; index < kit_rows.rows.size(); ++index) {
        const kit::PlacedRow& placed_row = kit_rows.rows[index];
        geometry::Row& row = rows.rows.emplace_back();
        const std::size_t logical = order[index];
        row.updates = static_cast<int16_t>(logical);
        row.lock = logical == kPreviousRow && previous_locked ? Lock::in_game : Lock::none;
        row.control = placed_row.control;
        row.hint_is_status = placed_row.view.hint_is_status;
        row.top = placed_row.top;
        row.height = placed_row.height;
        row.label = placed_row.label;
        row.lock_area = placed_row.lock_area;
        row.hint_lines = std::min(placed_row.hints.size(), geometry::most_row_lines);
        for (std::size_t line = 0; line < row.hint_lines; ++line)
            row.hints[line] = placed_row.hints[line];
        row.control_area = placed_row.control_area;
    }
    if (placed != nullptr)
        *placed = std::move(kit_rows);
    return rows;
}

kit::RowKind kind(const Dialog& dialog, const geometry::Row& row) {
    if (row.updates < 0)
        return kit::RowKind::text;
    return spec_for(model_of(const_cast<Dialog&>(dialog), row)).kind;
}

bool is_choice(const geometry::Row& row) noexcept {
    return row.updates == static_cast<int16_t>(kCheckRow) ||
           row.updates == static_cast<int16_t>(kInstallRow);
}

int32_t choice_count(const geometry::Row& row) noexcept {
    return row.updates == static_cast<int16_t>(kInstallRow)
               ? static_cast<int32_t>(kInstallCaptions.size())
               : static_cast<int32_t>(kCaptions.size());
}

int32_t choice_index(const Dialog& dialog, const geometry::Row& row) noexcept {
    if (row.updates == static_cast<int16_t>(kInstallRow))
        return static_cast<int32_t>(dialog.chosen.update_install);
    return static_cast<int32_t>(dialog.chosen.update_check);
}

std::string choice_caption(const geometry::Row& row, int32_t index) {
    if (row.updates == static_cast<int16_t>(kInstallRow))
        return install_caption({}, index);
    return check_caption({}, index);
}

std::string choice_id(const geometry::Row& row, int32_t index) {
    if (row.updates == static_cast<int16_t>(kInstallRow))
        return install_choice_id({}, index);
    return check_choice_id({}, index);
}

std::string_view choice_word(const geometry::Row& row) noexcept {
    return row.updates == static_cast<int16_t>(kInstallRow) ? "update-install" : "update-check";
}

void follow_mode(Dialog& dialog) noexcept {
    const int32_t previous = dialog.updates.mode;
    const int32_t mode = static_cast<int32_t>(dialog.chosen.update_check);
    dialog.updates.mode = mode;
    if (mode == static_cast<int32_t>(UpdateCheck::never))
        dialog.updates.can_check = false;
    else if (previous == static_cast<int32_t>(UpdateCheck::never) && !dialog.updates.offered)
        dialog.updates.can_check = true;
}

void apply_choice(Dialog& dialog, const geometry::Row& row, int32_t index) {
    RowModel model{
        &dialog, row.updates == static_cast<int16_t>(kInstallRow) ? kInstallRow : kCheckRow
    };
    if (model.index == kInstallRow)
        install_set(model, index);
    else
        check_set(model, index);
}

DialogAction take_card(Dialog& dialog, std::size_t index) {
    const std::vector<UpdateCardAction>& actions = dialog.updates.actions;
    if (index >= actions.size() || index >= most_update_card_actions)
        return DialogAction::none;
    // Check now is the check CHECK NOW asks for, which lends the way the
    // dialog shows to the preference for that one check.
    if (actions[index].id == check_now_id)
        return DialogAction::check_updates;
    dialog.update_act = actions[index].act;
    return DialogAction::update_action;
}

std::optional<DialogAction> card_action(Dialog& dialog, kit::ActionId action) {
    if (action < first_card_action ||
        action >= first_card_action + static_cast<kit::ActionId>(most_update_card_actions))
        return std::nullopt;
    return take_card(dialog, static_cast<std::size_t>(action - first_card_action));
}

std::optional<std::size_t> card_button(int32_t control) noexcept {
    if (control == update_second_control)
        return 1;
    if (control == update_third_control)
        return 2;
    return std::nullopt;
}

int32_t card_control(std::size_t index, int32_t row_control) noexcept {
    if (index == 1)
        return update_second_control;
    if (index == 2)
        return update_third_control;
    return row_control;
}

kit::RowResult activate(Dialog& dialog, const geometry::Row& row) {
    RowModel model = model_of(dialog, row);
    const kit::RowSpec<RowModel> spec = spec_for(model);
    return kit::activate(spec, model, 0);
}

kit::RowResult release(Dialog& dialog, const geometry::Row& row, int32_t x, int32_t y) {
    RowModel model = model_of(dialog, row);
    const kit::RowSpec<RowModel> spec = spec_for(model);
    kit::PlacedRow placed;
    placed.control = row.control;
    placed.control_area = row.control_area;
    placed.view.kind = spec.kind;
    placed.view.control_width = spec.control_width;
    return kit::press(spec, model, placed, {x, y});
}

DialogAction step(Dialog& dialog, const geometry::Row& row, bool up) {
    const EngineSettings before = dialog.chosen;
    RowModel model = model_of(dialog, row);
    const kit::RowSpec<RowModel> spec = spec_for(model);
    static_cast<void>(kit::step(spec, model, up));
    return dialog.chosen == before ? DialogAction::redraw : DialogAction::changed;
}

/// Breaks a count of seconds into local civil time. Windows and the others
/// take the call that does not use a shared buffer.
///
/// @param seconds the count
/// @param[out] broken the time
/// @return true when it broke
bool broken_local(std::time_t seconds, std::tm& broken) noexcept {
#if defined(_WIN32)
    return localtime_s(&broken, &seconds) == 0;
#else
    return localtime_r(&seconds, &broken) != nullptr;
#endif
}

/// Breaks a count of seconds into UTC, when local time cannot.
///
/// @param seconds the count
/// @param[out] broken the time
/// @return true when it broke
bool broken_utc(std::time_t seconds, std::tm& broken) noexcept {
#if defined(_WIN32)
    return gmtime_s(&broken, &seconds) == 0;
#else
    return gmtime_r(&seconds, &broken) != nullptr;
#endif
}

/// Holds a count of seconds in a time_t, or nothing when it does not fit.
///
/// @param seconds the count
/// @return the time, or nothing
std::optional<std::time_t> as_time(int64_t seconds) noexcept {
    if (seconds < 0)
        return std::nullopt;
    const auto wide = static_cast<uintmax_t>(seconds);
    if (wide > static_cast<uintmax_t>(std::numeric_limits<std::time_t>::max()))
        return std::nullopt;
    return static_cast<std::time_t>(seconds);
}

/// Returns when a check ran, as the status line says it: today, yesterday
/// or a date. Empty when the time cannot be said.
///
/// @param checked seconds since 1970
/// @param now seconds since 1970, as the host reads the clock
/// @return the phrase, already in the language shown; empty when it cannot
std::string when_phrase(int64_t checked, int64_t now) {
    const std::optional<std::time_t> checked_time = as_time(checked);
    const std::optional<std::time_t> now_time = as_time(now);
    if (!checked_time || !now_time)
        return {};
    std::tm checked_tm{};
    std::tm now_tm{};
    const bool local = broken_local(*checked_time, checked_tm) && broken_local(*now_time, now_tm);
    if (!local && !(broken_utc(*checked_time, checked_tm) && broken_utc(*now_time, now_tm)))
        return {};
    char clock[8]{};
    char date[16]{};
    if (std::strftime(clock, sizeof clock, "%H:%M", &checked_tm) == 0)
        return {};
    if (std::strftime(date, sizeof date, "%Y-%m-%d", &checked_tm) == 0)
        return {};
    const std::string clock_text(clock);
    const std::string date_text(date);
    if (checked_tm.tm_year == now_tm.tm_year && checked_tm.tm_yday == now_tm.tm_yday)
        return geometry::filled("today at {time}", {{"time", clock_text}});
    std::tm yesterday = now_tm;
    yesterday.tm_mday -= 1;
    yesterday.tm_isdst = -1;
    const std::time_t rolled = std::mktime(&yesterday);
    if (rolled != static_cast<std::time_t>(-1) && checked_tm.tm_year == yesterday.tm_year &&
        checked_tm.tm_yday == yesterday.tm_yday)
        return geometry::filled("yesterday at {time}", {{"time", clock_text}});
    return geometry::filled("on {date}", {{"date", date_text}});
}

oa::ui::kit::Question roll_back_question(std::string_view kept, std::string_view current) {
    namespace kit = oa::ui::kit;
    kit::Question asked;
    asked.word = "update-previous";
    asked.title = geometry::filled("Roll back to {version}?", {{"version", kept}});
    // What the roll back puts back, and what it leaves as it is: the
    // preferences come back from the copy taken at the update; the player's
    // folder, with its saves, replays, mods, maps and key layouts, is not touched.
    kit::QuestionPart scope;
    scope.kind = kit::PartKind::boxes;
    kit::QuestionBox back;
    back.heading = std::string(geometry::shown_text("Comes back"));
    back.lines.push_back(
        {kit::LineMark::bullet,
         geometry::filled("Open Annihilation {version}", {{"version", kept}})}
    );
    back.lines.push_back(
        {kit::LineMark::bullet,
         std::string(geometry::shown_text("Your settings from before you updated"))}
    );
    kit::QuestionBox stays;
    stays.heading = std::string(geometry::shown_text("Stays as it is"));
    stays.lines.push_back(
        {kit::LineMark::bullet, std::string(geometry::shown_text("Saved games and replays"))}
    );
    stays.lines.push_back(
        {kit::LineMark::bullet, std::string(geometry::shown_text("Mods, maps and key layouts"))}
    );
    scope.boxes.push_back(std::move(back));
    scope.boxes.push_back(std::move(stays));
    asked.parts.push_back(std::move(scope));
    kit::QuestionPart replaced;
    replaced.kind = kit::PartKind::callout;
    replaced.tone = kit::CalloutTone::caution;
    replaced.text = std::string(
        geometry::shown_text(
            "Settings you changed after updating are replaced by the ones you had then."
        )
    );
    asked.parts.push_back(std::move(replaced));
    kit::QuestionPart restarts;
    restarts.kind = kit::PartKind::text;
    restarts.text = geometry::filled(
        "The game closes, puts {version} back and starts again. {current} is not offered again "
        "until a newer release.",
        {{"version", kept}, {"current", current}}
    );
    asked.parts.push_back(std::move(restarts));
    asked.buttons.push_back({std::string(geometry::shown_text("Cancel")), false, "cancel"});
    kit::QuestionButton roll{
        std::string(geometry::shown_text("Roll back and restart")), false, "roll-back"
    };
    roll.danger = true;
    asked.buttons.push_back(std::move(roll));
    // Cancel is marked, so Enter does not undo an update.
    asked.cancel_button = 0;
    asked.primary_button = 1;
    asked.marked = 0;
    return asked;
}

} // namespace oa::ui::engine_settings::updates

namespace oa::ui::engine_settings {

UpdateLines update_lines(const UpdateReading& reading) {
    UpdateLines lines;
    lines.version_line =
        geometry::filled("Open Annihilation {version}", {{"version", reading.version}});
    if (reading.never) {
        lines.status = "OA never checks";
        lines.can_check = false;
        lines.offered = false;
    } else {
        const std::string when = reading.checked_at.has_value()
                                     ? updates::when_phrase(*reading.checked_at, reading.now)
                                     : std::string{};
        const bool timed = !when.empty();
        switch (reading.stand) {
        case UpdateStand::checking:
            lines.status = "Checking\u2026";
            lines.can_check = false;
            break;
        case UpdateStand::up_to_date:
            if (timed)
                lines.status = geometry::filled(
                    "Up to date \u00b7 Stable \u00b7 checked {when}", {{"when", when}}
                );
            else
                lines.status = "Not checked yet";
            lines.can_check = true;
            break;
        case UpdateStand::offered:
            lines.offered = true;
            lines.can_check = true;
            if (timed)
                lines.status = geometry::filled(
                    "{version} is out \u00b7 Stable \u00b7 checked {when}",
                    {{"version", reading.offer_version}, {"when", when}}
                );
            else
                lines.status = geometry::filled(
                    "{version} is out \u00b7 Stable \u00b7 checked {when}",
                    {{"version", reading.offer_version}, {"when", std::string_view{}}}
                );
            break;
        case UpdateStand::failed:
            if (timed)
                lines.status =
                    geometry::filled("Could not check \u00b7 checked {when}", {{"when", when}});
            else
                lines.status = "Could not check";
            lines.can_check = true;
            break;
        case UpdateStand::idle:
        case UpdateStand::cancelled:
            lines.status = "Not checked yet";
            lines.can_check = true;
            break;
        }
    }
    // A copy that cannot replace itself still checks. The button stays
    // CHECK NOW, and the status names who updates it when someone is named.
    if (!reading.replaces_itself)
        lines.offered = false;
    if (!reading.replaces_itself && !reading.packager.empty())
        lines.updated_by = geometry::filled("Updated by {name}", {{"name", reading.packager}});
    return lines;
}

} // namespace oa::ui::engine_settings

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Updates: this build's version and whether a release is out, and when
// Open Annihilation looks for one. The rows are kit rows, built from the
// host's offer each time the page is laid out or one of them is used.
// CHECK NOW and UPDATE… ask the host at once. The check itself waits for OK.

#include "updates_page.hpp"

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
    "OA looks for a newer Open Annihilation once a day when it starts. "
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

/// One placed row: the dialog and the row's place.
struct RowModel {
    Dialog* dialog{};    ///< the dialog
    std::size_t index{}; ///< 0 the status, 1 the check
};

/// Tells whether the status row shows a button.
///
/// Never hides it. A release shows UPDATE…. A check that can run, or a
/// status with nothing to press yet, shows CHECK NOW.
///
/// @param offer what the host filled
/// @return true when the button is drawn
bool button_shown(const UpdatesOffer& offer) noexcept {
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
    if (model.dialog == nullptr)
        return;
    apply_choice(*model.dialog, index);
}

/// Returns a way's caption.
///
/// @param index the way
/// @return its caption
std::string check_caption(const RowModel&, int32_t index) {
    return choice_caption(index);
}

/// Returns a way's id.
///
/// @param index the way
/// @return its id
std::string check_choice_id(const RowModel&, int32_t index) {
    return choice_id(index);
}

/// Returns the spec of one row.
///
/// @param model the row
/// @return the spec
kit::RowSpec<RowModel> spec_for(const RowModel& model) {
    if (model.index == 0) {
        const UpdatesOffer& offer = model.dialog->updates;
        const bool offered = offer.offered && button_shown(offer);
        if (!button_shown(offer)) {
            kit::RowSpec<RowModel> spec = kit::text<RowModel>("update-status", "", nullptr);
            spec.label_text = version_label;
            spec.hint_lines = status_hint_lines;
            spec.hint_text = status_hint;
            return spec;
        }
        kit::RowSpec<RowModel> spec = kit::value_and_button<RowModel>(
            "update-status",
            "",
            nullptr,
            offered ? "UPDATE\u2026" : "CHECK NOW",
            offered ? "update" : "check-now",
            offered ? open_release_action : check_updates_action
        );
        spec.label_text = version_label;
        spec.control_width = geometry::manage_button_width;
        spec.enabled = button_enabled;
        spec.hint_lines = status_hint_lines;
        spec.hint_text = status_hint;
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
    const Locks& /*locks*/,
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
    constexpr std::size_t count = 2;
    std::vector<kit::RowView> row_views;
    row_views.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
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
    placement.first_top = geometry::first_row_top;
    placement.row_padding = geometry::row_padding;
    placement.tall = oa::data::languages::interface_language().needs !=
                     oa::data::languages::TextNeeds::game_fonts;
    placement.first_control = first_row_control;
    kit::PlacedRows kit_rows = kit::place_rows(row_views, placement);

    geometry::Rows rows;
    rows.bottom = kit_rows.bottom;
    rows.rows.reserve(kit_rows.rows.size());
    for (std::size_t index = 0; index < kit_rows.rows.size(); ++index) {
        const kit::PlacedRow& placed_row = kit_rows.rows[index];
        geometry::Row& row = rows.rows.emplace_back();
        row.updates = static_cast<int16_t>(index);
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
    return row.updates == 1;
}

int32_t choice_count() noexcept {
    return static_cast<int32_t>(kCaptions.size());
}

int32_t choice_index(const Dialog& dialog) noexcept {
    return static_cast<int32_t>(dialog.chosen.update_check);
}

std::string choice_caption(int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= kCaptions.size())
        return {};
    return std::string(kCaptions[static_cast<std::size_t>(index)]);
}

std::string choice_id(int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= kChoiceIds.size())
        return {};
    return std::string(kChoiceIds[static_cast<std::size_t>(index)]);
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

void apply_choice(Dialog& dialog, int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= kCaptions.size())
        return;
    dialog.chosen.update_check = static_cast<UpdateCheck>(index);
    follow_mode(dialog);
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

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Downloads: when Open Annihilation looks for content, each registry's
// install ID, and the downloaded files. On the main menu a link opens the
// Library on its Registries tab, which lists the registries content comes
// from. The rows are kit rows, built from the host's offer each time the
// page is laid out or one of them is used.

#include "downloads_page.hpp"

#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/library/text.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
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

namespace oa::ui::engine_settings::downloads {

static_assert(
    manage_registries_action != reset_install_id_action &&
        manage_registries_action != empty_downloads_action &&
        manage_registries_action != geometry::manage_game_files_action &&
        manage_registries_action != geometry::open_library_action &&
        (manage_registries_action < geometry::first_folder_action ||
         manage_registries_action >=
             geometry::first_folder_action + static_cast<int32_t>(folder_button_count)),
    "REGISTRIES IN THE LIBRARY… asks for an action no other row's button asks for"
);

namespace {

namespace kit = oa::ui::kit;

/// The check for content updates' hint.
constexpr std::string_view kCheckHint =
    "OA looks for new content and updates in the registries' lists. "
    "It never installs anything until you say so.";
/// Said once, after that hint, when this system reads https as http.
constexpr std::string_view kHttpsOverHttp = "This system reads HTTPS registries over HTTP.";
/// An install ID's hint. {name} is the registry's name.
constexpr std::string_view kInstallHint =
    "A random number that lets {name} stop automated downloads. "
    "Turned off, you can browse the Library, but downloads are off. "
    "Installed content keeps working.";
/// The gap in a shown install ID, between its first 8 and last 4 digits.
constexpr std::string_view kIdGap = " \u00b7 \u00b7 \u00b7 ";
/// What a download in progress adds to the files' size.
constexpr std::string_view kInUse = " \u00b7 in use by downloads";

/// The check's captions, in ContentCheck's order.
constexpr std::array<std::string_view, 3> kCaptions{
    "Automatically",
    "Only when I open the Library",
    "Never",
};
/// The check's ids, in the same order.
constexpr std::array<std::string_view, 3> kChoiceIds{"automatically", "library", "never"};

/// One Downloads row and the dialog it changes. A spec's functions take no
/// captures, so the row's place rides on this.
struct RowModel {
    Dialog* dialog{};     ///< the dialog
    const Locks* locks{}; ///< what cannot be changed now
    std::size_t index{};  ///< the row's place, from 0
};

/// What a row of the page is.
enum class Role : uint8_t {
    check,    ///< Check for content updates
    toggle,   ///< an install ID's switch
    id_value, ///< an install ID's value and RESET
    files,    ///< Downloaded files and EMPTY
    link,     ///< REGISTRIES IN THE LIBRARY…, on the main menu
};

const InstallIdRow* offered(const Dialog& dialog, std::size_t registry) noexcept;

/// Tells whether stored text has the 12 hex digits a shown install ID needs.
///
/// @param text the stored short form, or a whole ID
/// @return true when at least 12 hex digits are there
bool shows_an_id(std::string_view text) noexcept {
    std::size_t digits = 0;
    for (const unsigned char character : text) {
        if (std::isxdigit(character) == 0)
            continue;
        ++digits;
        if (digits >= 12)
            return true;
    }
    return false;
}

/// Tells whether this registry has an install ID the page can show.
///
/// @param dialog the dialog
/// @param registry the row's place among the offer
/// @return true when an ID is stored and long enough to show
bool id_exists(const Dialog& dialog, std::size_t registry) noexcept {
    const InstallIdRow* row = offered(dialog, registry);
    return row != nullptr && row->made && shows_an_id(row->id_text);
}

/// Returns how many rows one registry adds: its switch, and the labelled ID
/// once one exists.
///
/// @param dialog the dialog
/// @param registry the row's place among the offer
/// @return 2 when an ID shows, otherwise 1
std::size_t registry_rows(const Dialog& dialog, std::size_t registry) noexcept {
    return id_exists(dialog, registry) ? std::size_t{2} : std::size_t{1};
}

/// Returns how many rows the page has.
///
/// @param dialog the dialog
/// @return the check, each registry's switch and its labelled ID once one
///     exists, the files, and the Library link on the main menu
std::size_t row_count(const Dialog& dialog) noexcept {
    std::size_t count = 2;
    for (std::size_t registry = 0; registry < dialog.downloads.install_ids.size(); ++registry)
        count += registry_rows(dialog, registry);
    if (dialog.library_link)
        ++count;
    return count;
}

/// Returns what the row at a place is.
///
/// @param dialog the dialog
/// @param index the row's place
/// @return its role
Role role_of(const Dialog& dialog, std::size_t index) noexcept {
    if (index == 0)
        return Role::check;
    const std::size_t count = row_count(dialog);
    if (dialog.library_link && index + 1 == count)
        return Role::link;
    if (index + 1 + (dialog.library_link ? std::size_t{1} : std::size_t{0}) == count)
        return Role::files;
    std::size_t at = 1;
    for (std::size_t registry = 0; registry < dialog.downloads.install_ids.size(); ++registry) {
        if (index == at)
            return Role::toggle;
        ++at;
        if (!id_exists(dialog, registry))
            continue;
        if (index == at)
            return Role::id_value;
        ++at;
    }
    return Role::files;
}

/// Returns the install ID a row shows.
///
/// @param dialog the dialog
/// @param index the row's place
/// @return its place among the offer's rows
std::size_t registry_of(const Dialog& dialog, std::size_t index) noexcept {
    std::size_t at = 1;
    for (std::size_t registry = 0; registry < dialog.downloads.install_ids.size(); ++registry) {
        if (index == at || (id_exists(dialog, registry) && index == at + 1))
            return registry;
        at += registry_rows(dialog, registry);
    }
    return 0;
}

/// Tells whether a game is running, which locks RESET and EMPTY.
///
/// @param locks what cannot be changed now
/// @return true while pathfinding is locked, which a game does
bool game_running(const Locks& locks) noexcept {
    return locks.path_search != Lock::none;
}

/// Returns the locks a model carries, or the dialog's own.
///
/// @param model the row
/// @return the locks
const Locks& locks_of(const RowModel& model) noexcept {
    return model.locks != nullptr ? *model.locks : model.dialog->locks;
}

/// Returns whether an install-ID switch is on.
///
/// @param dialog the dialog
/// @param registry the row's place among the offer
/// @return the player's switch, or the offer's when none is chosen yet
bool switch_on(const Dialog& dialog, std::size_t registry) noexcept {
    if (registry < dialog.install_id_chosen.size())
        return dialog.install_id_chosen[registry].on;
    if (registry < dialog.downloads.install_ids.size())
        return dialog.downloads.install_ids[registry].on;
    return true;
}

/// Returns an install ID as the page shows it: its first 8 hex digits, a
/// gap, and its last 4. Fewer than 12 hex digits shows nothing.
///
/// @param text the stored short form, or a whole ID
/// @return the shown ID; empty when there are too few digits
std::string masked_id(std::string_view text) {
    std::string digits;
    digits.reserve(text.size());
    for (const unsigned char character : text) {
        if (std::isxdigit(character) != 0)
            digits.push_back(static_cast<char>(std::tolower(character)));
    }
    if (digits.size() < 12)
        return {};
    return digits.substr(0, 8) + std::string(kIdGap) + digits.substr(digits.size() - 4);
}

/// Returns the offer's row an index names, or null.
///
/// @param dialog the dialog
/// @param registry the row's place among the offer
/// @return the row; null past the end
const InstallIdRow* offered(const Dialog& dialog, std::size_t registry) noexcept {
    if (registry >= dialog.downloads.install_ids.size())
        return nullptr;
    return &dialog.downloads.install_ids[registry];
}

/// The check's hint, looked up whole, plus the https note on a system that
/// reads it as http. The page wraps the looked-up lines together.
///
/// @param model the row
/// @return one line, or two when the note shows
std::size_t check_hint_lines(const RowModel& model) {
    return model.dialog->downloads.https_over_http ? std::size_t{2} : std::size_t{1};
}

/// Returns one line of the check's hint, the whole text.
///
/// @param model the row
/// @param line the line, from 0
/// @return the line
std::string check_hint_line(const RowModel& model, std::size_t line) {
    if (line == 0)
        return std::string(kCheckHint);
    if (line == 1 && model.dialog->downloads.https_over_http)
        return std::string(kHttpsOverHttp);
    return {};
}

/// Returns an install ID's label: "Install ID", or "Install ID · {name}"
/// when more than one registry uses one.
///
/// @param model the row
/// @return the label, as shown
std::string install_label(const RowModel& model) {
    // English: view_of looks the label up once. A registry's name is its own.
    const std::string prefix("Install ID");
    if (model.dialog->downloads.install_ids.size() <= 1)
        return prefix;
    const InstallIdRow* row = offered(*model.dialog, registry_of(*model.dialog, model.index));
    const std::string name = row != nullptr ? row->registry_name : std::string();
    return prefix + " \u00b7 " + name;
}

/// Returns an install ID's hint, with the registry's name filled in.
///
/// @param model the row
/// @return the hint, as shown
std::string install_hint(const RowModel& model) {
    const InstallIdRow* row = offered(*model.dialog, registry_of(*model.dialog, model.index));
    const std::string_view name =
        row != nullptr ? std::string_view(row->registry_name) : std::string_view();
    return geometry::filled(kInstallHint, {{"name", name}});
}

/// Returns how many lines an install ID's hint takes before it is wrapped.
///
/// @param model the row
/// @return one: the whole hint
std::size_t install_hint_lines(const RowModel&) {
    return 1;
}

/// Returns the install ID's hint, the registry's name filled in.
///
/// @param model the row
/// @param line the line, from 0
/// @return the hint; empty past the first line
std::string install_hint_line(const RowModel& model, std::size_t line) {
    if (line != 0)
        return {};
    return install_hint(model);
}

/// Returns whether an install-ID switch is on.
///
/// @param model the row
/// @return true when it is on
bool toggle_get(const RowModel& model) {
    return switch_on(*model.dialog, registry_of(*model.dialog, model.index));
}

/// Sets an install-ID switch. It takes effect at OK.
///
/// @param[in,out] model the row
/// @param on true for On
void toggle_set(RowModel& model, bool on) {
    const std::size_t registry = registry_of(*model.dialog, model.index);
    if (registry < model.dialog->install_id_chosen.size())
        model.dialog->install_id_chosen[registry].on = on;
}

/// Returns the labelled row's value: "Off" while the switch is off, else
/// the shown ID. The row is placed only once an ID exists.
///
/// @param model the row
/// @return the value
std::string id_value(const RowModel& model) {
    if (!switch_on(*model.dialog, registry_of(*model.dialog, model.index)))
        return "Off";
    const InstallIdRow* row = offered(*model.dialog, registry_of(*model.dialog, model.index));
    if (row != nullptr && row->made) {
        std::string shown = masked_id(row->id_text);
        if (!shown.empty())
            return shown;
    }
    return {};
}

/// Tells whether RESET can be pressed: an ID is stored and the switch is on.
///
/// @param model the row
/// @return true when it can
bool reset_enabled(const RowModel& model) {
    if (game_running(locks_of(model)))
        return false;
    if (!switch_on(*model.dialog, registry_of(*model.dialog, model.index)))
        return false;
    const InstallIdRow* row = offered(*model.dialog, registry_of(*model.dialog, model.index));
    return row != nullptr && row->made && !masked_id(row->id_text).empty();
}

/// Returns the downloaded files' size line, or nothing while a game runs.
///
/// @param model the row
/// @return the value; empty while locked
std::string files_value(const RowModel& model) {
    // While a game is running the row shows its label, the lock and EMPTY,
    // and no size line.
    if (game_running(locks_of(model)))
        return {};
    const DownloadsOffer& offer = model.dialog->downloads;
    std::string text;
    if (offer.kept_bytes == 0)
        text = "Nothing kept";
    else if (offer.downloads_busy)
        text = oa::ui::library::size_text(offer.kept_bytes) + std::string(kInUse);
    else
        text = oa::ui::library::size_text(offer.kept_bytes) + " kept for resuming and repairs";
    if (offer.downloads_busy && offer.kept_bytes == 0)
        text += kInUse;
    return text;
}

/// Tells whether EMPTY can be pressed: files are kept and nothing is downloading.
///
/// @param model the row
/// @return true when it can
bool files_enabled(const RowModel& model) {
    if (game_running(locks_of(model)))
        return false;
    const DownloadsOffer& offer = model.dialog->downloads;
    return offer.kept_bytes > 0 && !offer.downloads_busy;
}

/// Returns the lock RESET and EMPTY show during a game: the words every
/// other locked row of the dialog shows.
///
/// @param model the row
/// @return the lock's text; empty while a game is not running
std::string_view game_lock(const RowModel& model) {
    return game_running(locks_of(model)) ? geometry::lock_text(Lock::in_game) : std::string_view();
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
    return static_cast<int32_t>(model.dialog->chosen.content_check);
}

/// Sets the check. It takes effect at OK.
///
/// @param[in,out] model the row
/// @param index the way, from 0
void check_set(RowModel& model, int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= kCaptions.size())
        return;
    model.dialog->chosen.content_check = static_cast<ContentCheck>(index);
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

/// Returns the spec of one row. Its id for an install ID is a placeholder:
/// the caller gives the view the registry's id, which holds a dot.
///
/// @param model the row
/// @return the spec
kit::RowSpec<RowModel> spec_for(const RowModel& model) {
    const Role role = role_of(*model.dialog, model.index);
    if (role == Role::check) {
        kit::Stepper<RowModel> stepper;
        stepper.count = check_count;
        stepper.get = check_get;
        stepper.set = check_set;
        stepper.caption = check_caption;
        stepper.id = check_choice_id;
        kit::RowSpec<RowModel> spec =
            kit::choice("content-updates", "Check for content updates", stepper);
        spec.control_width = geometry::wide_choice_width;
        spec.hint_lines = check_hint_lines;
        spec.hint_text = check_hint_line;
        return spec;
    }
    if (role == Role::toggle) {
        kit::RowSpec<RowModel> spec =
            kit::toggle("install-id", "Install ID", toggle_get, toggle_set);
        spec.label_text = install_label;
        spec.hint_lines = install_hint_lines;
        spec.hint_text = install_hint_line;
        return spec;
    }
    if (role == Role::id_value) {
        kit::RowSpec<RowModel> spec = kit::value_and_button(
            "install-value", "Your install ID", id_value, "Reset", "reset", reset_install_id_action
        );
        spec.control_width = geometry::manage_button_width;
        spec.enabled = reset_enabled;
        spec.lock = game_lock;
        return spec;
    }
    if (role == Role::link) {
        kit::RowSpec<RowModel> spec = kit::link<RowModel>(
            "registries-in-library", "Registries in the Library…", manage_registries_action
        );
        spec.hint = {"Registries are in the Library.", {}};
        return spec;
    }
    kit::RowSpec<RowModel> spec = kit::value_and_button(
        "downloaded-files",
        "Downloaded files",
        files_value,
        "Empty",
        "empty",
        empty_downloads_action
    );
    // Emptying the folder deletes its files: the danger look.
    spec.danger = true;
    spec.control_width = geometry::manage_button_width;
    spec.enabled = files_enabled;
    spec.lock = game_lock;
    return spec;
}

/// Names the registry a RESET is for.
///
/// @param[in,out] dialog the dialog
/// @param model the RESET row
void note_registry(Dialog& dialog, const RowModel& model) {
    const std::size_t registry = registry_of(*model.dialog, model.index);
    if (registry < dialog.install_id_chosen.size())
        dialog.install_id_registry = dialog.install_id_chosen[registry].registry_id;
    else if (const InstallIdRow* row = offered(dialog, registry))
        dialog.install_id_registry = row->registry_id;
}

/// Builds the model of a placed row.
///
/// @param dialog the dialog
/// @param row the row
/// @return the model; a row that is not Downloads' has no place
RowModel model_of(Dialog& dialog, const geometry::Row& row) {
    const auto index =
        row.downloads >= 0 ? static_cast<std::size_t>(row.downloads) : std::size_t{0};
    return {&dialog, &dialog.locks, index};
}

/// Notes a RESET's registry when a row asks for one.
///
/// @param[in,out] dialog the dialog
/// @param model the row
/// @param result what the row did
/// @return what the row asks of the dialog
kit::RowResult note_reset(Dialog& dialog, const RowModel& model, const kit::RowResult& result) {
    if (result.event == kit::RowEvent::action && result.action == reset_install_id_action)
        note_registry(dialog, model);
    return result;
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
    const bool notice = !shown.notices.empty() && shown.notices.front();
    shown.hints = std::move(lines);
    shown.notices.assign(shown.hints.size(), notice);
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
    const std::size_t count = row_count(dialog);
    std::vector<kit::RowView> row_views;
    std::vector<bool> game_locked;
    row_views.reserve(count);
    game_locked.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        const RowModel model{const_cast<Dialog*>(&dialog), &locks, index};
        const kit::RowSpec<RowModel> spec = spec_for(model);
        kit::RowView shown = kit::view_of(spec, model, &geometry::shown_text);
        wrap_shown_hints(shown, room, measure);
        // With no install ID yet, "Made at your first download" is the
        // switch's second hint line, not a row of its own.
        const Role role = role_of(dialog, index);
        if (role == Role::toggle && !id_exists(dialog, registry_of(dialog, index)) &&
            shown.hints.size() < geometry::most_row_lines) {
            shown.hints.emplace_back(
                std::string(geometry::shown_text("Made at your first download"))
            );
            shown.notices.push_back(false);
        }
        geometry::fit_control(shown, sized.content_width, measure);
        geometry::fit_measured(shown, sized.content_width, measure, regular);
        if (role == Role::toggle || role == Role::id_value) {
            const InstallIdRow* row = offered(dialog, registry_of(dialog, index));
            shown.id =
                std::string("install-id.") + (row != nullptr ? row->registry_id : std::string());
        }
        game_locked.push_back(
            (role == Role::id_value || role == Role::files) && game_running(locks)
        );
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
    kit::PlacedRows kit_rows = kit::place_rows(row_views, placement);

    geometry::Rows rows;
    rows.bottom = kit_rows.bottom;
    rows.rows.reserve(kit_rows.rows.size());
    for (std::size_t index = 0; index < kit_rows.rows.size(); ++index) {
        const kit::PlacedRow& placed_row = kit_rows.rows[index];
        geometry::Row& row = rows.rows.emplace_back();
        row.downloads = static_cast<int16_t>(index);
        row.control = placed_row.control;
        row.lock = game_locked[index] ? Lock::in_game : Lock::none;
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
    if (row.downloads < 0)
        return kit::RowKind::text;
    return spec_for(model_of(const_cast<Dialog&>(dialog), row)).kind;
}

bool is_choice(const geometry::Row& row) noexcept {
    return row.downloads == 0;
}

int32_t choice_count() noexcept {
    return static_cast<int32_t>(kCaptions.size());
}

int32_t choice_index(const Dialog& dialog) noexcept {
    return static_cast<int32_t>(dialog.chosen.content_check);
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

void apply_choice(Dialog& dialog, int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= kCaptions.size())
        return;
    dialog.chosen.content_check = static_cast<ContentCheck>(index);
}

kit::RowResult activate(Dialog& dialog, const geometry::Row& row) {
    RowModel model = model_of(dialog, row);
    const kit::RowSpec<RowModel> spec = spec_for(model);
    return note_reset(dialog, model, kit::activate(spec, model, 0));
}

kit::RowResult release(Dialog& dialog, const geometry::Row& row, int32_t x, int32_t y) {
    RowModel model = model_of(dialog, row);
    const kit::RowSpec<RowModel> spec = spec_for(model);
    kit::PlacedRow placed;
    placed.control = row.control;
    placed.control_area = row.control_area;
    placed.view.kind = spec.kind;
    placed.view.control_width = spec.control_width;
    return note_reset(dialog, model, kit::press(spec, model, placed, {x, y}));
}

DialogAction step(Dialog& dialog, const geometry::Row& row, bool up) {
    const EngineSettings before = dialog.chosen;
    RowModel model = model_of(dialog, row);
    const kit::RowSpec<RowModel> spec = spec_for(model);
    static_cast<void>(kit::step(spec, model, up));
    return dialog.chosen == before ? DialogAction::redraw : DialogAction::changed;
}

} // namespace oa::ui::engine_settings::downloads

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The settings dialog: where its parts lie, its sections and rows, switches,
// sliders and their stops, the level strip, pointer and key events, OK,
// Cancel and Restore defaults, the locks and their texts, and what it draws.
// The Graphics page's five rows scroll: Hardware acceleration, a strip of
// Off, Basic and Full whose hint lines are its status, and Vertical sync,
// each locked in its own form. Language's four switches and its Text
// size slider set the text style the drawing reads; Text size is locked
// while the modern fonts are off.
// A section of the test's own, taller than the view under the heading,
// checks scrolling: the view and its limit, the wheel, the scroll bar, the
// scroll keys, the focus brought into view, rows cut by the view, the
// control numbers, and the two forms of a locked switch. The Developer
// section: its two rows over Developer Mode's list of the standard hacks,
// the list's areas and hacks opening and closing, static while Off, each
// kind of parameter's control, the overrides it makes, Restore profile
// values, Show Active Only, its scrolling and its focus. The Touch section,
// listed only with touch controls: its place in the list, the entries'
// numbers kept with and without it, its rows and their values, and a
// finger's press taking the nearest control within reach. The Controller
// section, listed only once a gamepad has sent input: its place in the
// list after Touch, the entries' numbers kept, its rows and their strips,
// sliders, switches and drop-downs, the rows it shares with Touch, the
// Steam Input notice, and Restore defaults, a Steam Deck's included. On a
// Steam Deck, Maximum frame rate's second hint line. The Game files
// section, listed only where the host says the platform brings game files
// in: its place in the list and its entry's number, its rows (what is
// installed with MANAGE…, the backups switch with the device's name, where
// the files are), MANAGE… asking the host, and the switch. The Language
// dialog: its one section, Restore defaults restoring that section alone,
// and every text drawn in the modern fonts when its fonts hold no glyphs.
// The names the automation endpoint lists every control and part by, on
// every section.
// Those names at each size class, and at Regular and Large: every section
// inside the dialog with its parts apart, hints kept to their lines,
// Graphics showing more whole rows, the footer's buttons at their Compact
// sizes, and each control pressed where it is drawn.
// Interface size offering 150% only while Use modern fonts for game text
// shows On, and turning that switch Off taking 150% back to 100%. Graphics
// drawn on the canvas's smooth form at 1.5x, every pixel painted whole.
// With --data, its fonts from the installed game and every text fitting its
// place.

#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/platform/file_types.hpp"
#include "oa/ui/engine_settings/dialog.hpp"

#include "geometry.hpp"

#include "oa/data/mod_profile/overrides.hpp"
#include "oa/data/mod_profile/registry.hpp"
#include "oa/present/game_text.hpp"
#include "oa/ui/engine_settings/notice.hpp"
#include "oa/ui/engine_settings/prompt.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/paint/painter.hpp"
#include "oa/test/game_assets.hpp"
#include "oa/test/game_data.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <ios>
#include <iostream>
#include <map>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* expression, const char* file, int line) {
    if (condition)
        return;
    std::cerr << file << ':' << line << ": check failed: " << expression << '\n';
    ++failures;
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)

/// Installs Simplified Chinese for one test, then puts the compiled
/// available entry back.
struct InstalledSimplifiedChinese {
    InstalledSimplifiedChinese() {
        oa::data::languages::LanguageEntry entry;
        entry.tag = "zh-Hans";
        entry.endonym = "\347\256\200\344\275\223\344\270\255\346\226\207";
        entry.english_name = "Chinese (Simplified)";
        entry.word = "Chinese";
        entry.locales = {"zh-Hans", "zh-CN", "zh-SG", "zh-MY", "zh"};
        entry.needs = oa::data::languages::TextNeeds::modern_fonts;
        const std::array<oa::data::languages::LanguageEntry, 1> installed{entry};
        oa::data::languages::set_pack_languages(installed, {});
    }

    ~InstalledSimplifiedChinese() { oa::data::languages::set_pack_languages({}, {}); }

    InstalledSimplifiedChinese(const InstalledSimplifiedChinese&) = delete;
    InstalledSimplifiedChinese& operator=(const InstalledSimplifiedChinese&) = delete;
};

namespace settings = oa::ui::engine_settings;
namespace renderer = oa::ui::frontend_renderer;
namespace geometry = oa::ui::engine_settings::geometry;
namespace kit = oa::ui::kit;

/// No icon: the dialog's header and the OA button draw the OA mark.
const renderer::RgbaPicture kNoIcon{};

/// The rows Graphics' last row, Interface size, adds to its content height
/// (geometry::content_height) and so to its scroll limit at Compact: its own
/// height, a drop-down's row with two hint lines and its field under them,
/// as After zoom's.
constexpr int32_t kInterfaceSizeRow = 79;

using settings::DialogAction;
using settings::DialogKey;
using settings::HardwareAcceleration;
using settings::Lock;
using settings::Page;
using settings::Setting;

/// The engine's sections, in the order the list shows them.
constexpr std::array<Page, 10> kPages{
    Page::controls,
    Page::common_tweaks,
    Page::language,
    Page::graphics,
    Page::user_interface,
    Page::downloads,
    Page::updates,
    Page::game_content,
    Page::system,
    Page::developer,
};

/// The engine's sections while the game has touch controls: Touch between
/// User Interface and Downloads.
constexpr std::array<Page, 11> kTouchPages{
    Page::controls,
    Page::common_tweaks,
    Page::language,
    Page::graphics,
    Page::user_interface,
    Page::touch,
    Page::downloads,
    Page::updates,
    Page::game_content,
    Page::system,
    Page::developer,
};

/// The engine's sections in the main menu's dialog of a game that brings
/// game files in: Game files between User Interface and Downloads.
constexpr std::array<Page, 11> kGameFilesPages{
    Page::controls,
    Page::common_tweaks,
    Page::language,
    Page::graphics,
    Page::user_interface,
    Page::game_files,
    Page::downloads,
    Page::updates,
    Page::game_content,
    Page::system,
    Page::developer,
};

/// The engine's sections with Touch and Game files both listed.
constexpr std::array<Page, 12> kTouchGameFilesPages{
    Page::controls,
    Page::common_tweaks,
    Page::language,
    Page::graphics,
    Page::user_interface,
    Page::touch,
    Page::game_files,
    Page::downloads,
    Page::updates,
    Page::game_content,
    Page::system,
    Page::developer,
};

/// The engine's sections once a gamepad has sent input: Controller between
/// User Interface and Downloads.
constexpr std::array<Page, 11> kControllerPages{
    Page::controls,
    Page::common_tweaks,
    Page::language,
    Page::graphics,
    Page::user_interface,
    Page::controller,
    Page::downloads,
    Page::updates,
    Page::game_content,
    Page::system,
    Page::developer,
};

/// The engine's sections with Touch and Controller listed, Controller after
/// Touch.
constexpr std::array<Page, 12> kTouchControllerPages{
    Page::controls,
    Page::common_tweaks,
    Page::language,
    Page::graphics,
    Page::user_interface,
    Page::touch,
    Page::controller,
    Page::downloads,
    Page::updates,
    Page::game_content,
    Page::system,
    Page::developer,
};

/// The engine's sections with all three listed: Touch, Controller, then
/// Game files, then Downloads, then Updates, then Game Content, then System.
constexpr std::array<Page, 13> kAllPages{
    Page::controls,
    Page::common_tweaks,
    Page::language,
    Page::graphics,
    Page::user_interface,
    Page::touch,
    Page::controller,
    Page::game_files,
    Page::downloads,
    Page::updates,
    Page::game_content,
    Page::system,
    Page::developer,
};

/// Every lock state the dialog opens with.
std::vector<settings::Locks> lock_states() {
    std::vector<settings::Locks> states;
    for (const bool in_game : {false, true}) {
        for (const bool shared : {false, true}) {
            for (const bool replay : {false, true}) {
                for (const bool command_line : {false, true}) {
                    if (!in_game && (shared || replay))
                        continue;
                    for (int32_t renderer = 0; renderer < 8; ++renderer) {
                        settings::GameState state{in_game, shared, replay, command_line};
                        state.renderer_from_command_line = (renderer & 1) != 0;
                        state.acceleration_unavailable = (renderer & 2) != 0;
                        state.vertical_sync_unavailable = (renderer & 4) != 0;
                        states.push_back(settings::settings_locks(state));
                    }
                }
            }
        }
    }
    return states;
}

/// Every status Hardware acceleration's row shows: each state at each reach,
/// in a replay, with Basic or Full asked for.
std::vector<settings::AccelerationStatus> acceleration_statuses() {
    std::vector<settings::AccelerationStatus> statuses;
    for (int32_t state = 0; state <= static_cast<int32_t>(settings::AccelerationState::in_use);
         ++state)
        for (int32_t reach = 0;
             reach <= static_cast<int32_t>(settings::AccelerationReach::nearest_none);
             ++reach)
            for (const bool replay : {false, true})
                for (const auto asked : {HardwareAcceleration::basic, HardwareAcceleration::full})
                    statuses.push_back(
                        settings::AccelerationStatus{
                            static_cast<settings::AccelerationState>(state),
                            static_cast<settings::AccelerationReach>(reach),
                            replay,
                            asked,
                        }
                    );
    return statuses;
}

settings::Dialog opened(Page page, const settings::Locks& locks = {}) {
    settings::Dialog dialog;
    settings::open_dialog(
        dialog, settings::EngineSettings{}, settings::EngineSettings{}, locks, "v0.2.0", page
    );
    return dialog;
}

/// Returns a dialog of the engine's settings opened on a section, with or
/// without touch controls.
settings::Dialog opened_with_touch(Page page, bool touch = true) {
    settings::Dialog dialog;
    settings::open_dialog(
        dialog,
        settings::EngineSettings{},
        settings::EngineSettings{},
        {},
        "v0.2.0",
        page,
        {},
        settings::highest_unit_limit,
        {},
        nullptr,
        touch
    );
    return dialog;
}

/// Returns a dialog of the engine's settings opened on a section with the
/// Game files section listed, with or without touch controls, its rows
/// filled as a host fills them.
settings::Dialog opened_with_game_files(Page page, bool touch = false) {
    settings::Dialog dialog;
    settings::open_dialog(
        dialog,
        settings::EngineSettings{},
        settings::EngineSettings{},
        {},
        "v0.2.0",
        page,
        {},
        settings::highest_unit_limit,
        {},
        nullptr,
        touch,
        true
    );
    dialog.game_files_summary = "3.1c · Core Contingency · Battle Tactics · music";
    dialog.game_files_sizes = "1.1 GB · 37 GB free on this tablet";
    dialog.game_files_location = "In the file manager: Open Annihilation › Total Annihilation";
    dialog.game_files_device = "tablet";
    return dialog;
}

/// Returns a dialog of the engine's settings opened on a section, with or
/// without the Controller section listed (a gamepad has sent input), and
/// with or without Touch and Game files.
///
/// @param page the section
/// @param controller the dialog lists Controller
/// @param touch the dialog lists Touch
/// @param game_files the dialog lists Game files
/// @param current the settings in effect, which the defaults are too
/// @return the dialog
settings::Dialog opened_with_controller(
    Page page,
    bool controller = true,
    bool touch = false,
    bool game_files = false,
    const settings::EngineSettings& current = {}
) {
    settings::Dialog dialog;
    settings::open_dialog(
        dialog,
        current,
        current,
        {},
        "v0.2.0",
        page,
        {},
        settings::highest_unit_limit,
        {},
        nullptr,
        touch,
        game_files,
        controller
    );
    return dialog;
}

/// The list's entries as the dialog lists them: each entry's control and
/// its rectangle, top to bottom.
std::vector<std::pair<int32_t, renderer::SourceRect>> list_entries(const settings::Dialog& dialog) {
    std::vector<std::pair<int32_t, renderer::SourceRect>> entries;
    for (const auto& part : settings::dialog_layout(dialog))
        if (part.control >= settings::first_page_control &&
            part.control < settings::restore_control)
            entries.emplace_back(part.control, part.rect);
    return entries;
}

bool overlap(const renderer::SourceRect& a, const renderer::SourceRect& b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height &&
           b.y < a.y + a.height;
}

bool inside(const renderer::SourceRect& inner, const renderer::SourceRect& outer) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

/// The part with a text, or a control's first part.
const settings::LayoutPart*
find_part(const std::vector<settings::LayoutPart>& parts, std::string_view text, int32_t control) {
    for (const auto& part : parts) {
        if (!text.empty() && part.text == text)
            return &part;
        if (text.empty() && part.control == control)
            return &part;
    }
    return nullptr;
}

/// A copy of the part found in a list that is freed at the end of the
/// statement, as dialog_layout's is, or none. It compares with nullptr and
/// reads through -> like a pointer, but a pointer into the freed list cannot
/// be taken from it.
class FoundPart {
  public:

    FoundPart() = default;

    /// Holds a copy of a part.
    ///
    /// @param part the part
    explicit FoundPart(const settings::LayoutPart& part) : part_(part) {}

    /// The part. Only read it when there is one.
    const settings::LayoutPart* operator->() const { return &*part_; }

    /// Tells whether no part was found.
    friend bool operator==(const FoundPart& found, std::nullptr_t) { return !found.part_; }

  private:

    std::optional<settings::LayoutPart> part_;
};

/// The part with a text, or a control's first part, in a list that is freed
/// at the end of the statement: a copy of it.
FoundPart
find_part(std::vector<settings::LayoutPart>&& parts, std::string_view text, int32_t control) {
    const settings::LayoutPart* part = find_part(parts, text, control);
    return part == nullptr ? FoundPart{} : FoundPart{*part};
}

/// The room a hint is wrapped into where no font measures it: the class's
/// character budget at the estimated width of one character.
///
/// @param dialog the dialog
/// @return the room, in source pixels
int32_t hint_room(const settings::Dialog& dialog) {
    return static_cast<int32_t>(geometry::sizes_of(dialog).hint_line_characters) *
           kit::estimated_character_width;
}

/// Breaks a whole hint the way the dialog does where no font measures it.
///
/// @param text the whole hint
/// @param dialog the dialog, for its class's room
/// @return the lines; none for an empty hint
std::vector<std::string> wrapped_hint(std::string_view text, const settings::Dialog& dialog) {
    if (text.empty())
        return {};
    return kit::wrap(text, std::max(hint_room(dialog), int32_t{1}), geometry::estimated_width);
}

/// Tells whether the layout shows a whole hint as the lines it wraps to,
/// each fitting the wrap room and lying in the content column.
///
/// @param parts the dialog's layout
/// @param whole the hint, as one sentence
/// @param dialog the dialog the lines are wrapped for
/// @return true when the lines join back to the sentence and each fits
bool shows_wrapped(
    const std::vector<settings::LayoutPart>& parts,
    std::string_view whole,
    const settings::Dialog& dialog
) {
    const std::vector<std::string> lines = wrapped_hint(whole, dialog);
    if (lines.empty())
        return whole.empty();
    const geometry::Sizes& sizes = geometry::sizes_of(dialog);
    const int32_t room = hint_room(dialog);
    std::string joined;
    for (const std::string& line : lines) {
        if (!joined.empty())
            joined.push_back(' ');
        joined += line;
        const int32_t estimated = geometry::estimated_width(line);
        if (estimated > room)
            return false;
        const settings::LayoutPart* part = find_part(parts, line, settings::no_control);
        if (part == nullptr)
            return false;
        if (part->rect.width < std::min(estimated, sizes.content_width))
            return false;
        if (part->rect.x < sizes.content_left ||
            part->rect.x + part->rect.width > sizes.content_right)
            return false;
    }
    return joined == whole;
}

struct Point {
    int32_t x{};
    int32_t y{};
};

Point centre(const renderer::SourceRect& rect) {
    return {rect.x + rect.width / 2, rect.y + rect.height / 2};
}

/// Tells whether two rectangles are the same.
bool same_entry(const renderer::SourceRect& a, const renderer::SourceRect& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

DialogAction click(settings::Dialog& dialog, Point point) {
    static_cast<void>(settings::dialog_pointer_move(dialog, point.x, point.y));
    static_cast<void>(settings::dialog_pointer_down(dialog, point.x, point.y));
    return settings::dialog_pointer_up(dialog, point.x, point.y);
}

/// Every setting's whole state at each AA level, so each hint shows.
std::vector<settings::EngineSettings> level_states() {
    std::vector<settings::EngineSettings> states;
    for (const auto level : settings::anti_aliasing_levels) {
        settings::EngineSettings state{};
        state.anti_aliasing = level;
        states.push_back(state);
    }
    return states;
}

void opening_shows_the_settings_in_effect() {
    settings::EngineSettings current{};
    current.frame_stats = true;
    settings::Dialog dialog;
    settings::open_dialog(dialog, current, {}, {}, "v0.0.0", Page::graphics);
    CHECK(dialog.opened == current);
    CHECK(dialog.chosen == current);
    CHECK(dialog.page == Page::graphics);
    CHECK(!dialog.restored);
    CHECK(dialog.focused == settings::no_control);
}

void every_part_lies_inside_the_dialog_and_apart() {
    const renderer::SourceRect face{
        geometry::edge,
        geometry::edge,
        settings::dialog_width - 2 * geometry::edge,
        settings::dialog_height - 2 * geometry::edge,
    };
    const renderer::SourceRect section{
        geometry::content_left,
        geometry::body_top,
        geometry::content_width,
        geometry::footer_rule_row - geometry::body_top,
    };
    for (const Page page : kPages) {
        for (const auto& locks : lock_states()) {
            for (const auto& state : level_states()) {
                settings::Dialog dialog = opened(page, locks);
                dialog.chosen = state;
                const auto parts = settings::dialog_layout(dialog);
                CHECK(!parts.empty());
                for (std::size_t a = 0; a < parts.size(); ++a) {
                    CHECK(parts[a].rect.width > 0 && parts[a].rect.height > 0);
                    CHECK(inside(parts[a].rect, face));
                    for (std::size_t b = a + 1; b < parts.size(); ++b) {
                        if (overlap(parts[a].rect, parts[b].rect)) {
                            std::cerr << "overlap: '" << parts[a].text << "' and '" << parts[b].text
                                      << "'\n";
                            CHECK(!overlap(parts[a].rect, parts[b].rect));
                        }
                    }
                }
                // The open section stays within its columns; at its end, above
                // the footer.
                const auto rows = geometry::place_rows(page, locks);
                // Downloads' rows come from the offer, which this dialog leaves
                // empty, and it is not the main menu: the check and the
                // downloaded files, no install ID and no link to the Library.
                // Game Content with nothing listed is the sentence and three buttons.
                const std::size_t row_count = page == Page::downloads ? 2
                                              : page == Page::updates ? 2
                                              : page == Page::game_content
                                                  ? 4
                                                  : settings::page_settings(page).size();
                CHECK(rows.rows.size() == row_count);
                const auto open = geometry::open_rows(dialog);
                CHECK(
                    rows.bottom - geometry::scroll_limit(open.content_height) <
                    geometry::footer_rule_row
                );
                // Each row in its columns, at the offset that shows it.
                const auto in_columns = [&section](const renderer::SourceRect& rect) {
                    return rect.x >= section.x && rect.x + rect.width <= section.x + section.width;
                };
                for (std::size_t index = 0; index < rows.rows.size(); ++index) {
                    const auto& row = rows.rows[index];
                    CHECK(in_columns(row.label));
                    // A text row has no control: the file-type heading, the File
                    // types note, and Game Content's sentence and bundles.
                    // A locked row keeps its control.
                    if (row.control_area.width == 0) {
                        CHECK(
                            row.setting == Setting::file_type_registration ||
                            row.setting == Setting::file_types_note ||
                            row.setting == Setting::mod_limits_heading ||
                            row.setting == Setting::weapon_types || row.setting == Setting::sides ||
                            row.content >= 0 || row.updates >= 0
                        );
                        continue;
                    }
                    CHECK(in_columns(row.control_area));
                    // A focus outline two pixels out stays clear of the label and hints.
                    const renderer::SourceRect focus{
                        row.control_area.x - 2,
                        row.control_area.y - 2,
                        row.control_area.width + 4,
                        row.control_area.height + 4,
                    };
                    CHECK(!overlap(focus, row.label));
                    for (std::size_t line = 0; line < row.hint_lines; ++line)
                        CHECK(!overlap(focus, row.hints[line]));
                }
            }
        }
    }
    // The section list's entries and the footer's buttons keep apart too.
    for (const Page page : kPages)
        CHECK(!overlap(geometry::list_item(page), geometry::list_divider()));
    CHECK(!overlap(geometry::restore_button, geometry::cancel_button));
    CHECK(geometry::cancel_button.x + geometry::cancel_button.width < geometry::ok_button.x);
}

void each_section_shows_its_rows() {
    // The list names the sections in order, Developer last under a divider.
    const auto engine = settings::dialog_pages(settings::DialogKind::engine);
    CHECK(std::equal(engine.begin(), engine.end(), kPages.begin(), kPages.end()));
    // Each entry's number is its place in the list with Touch and
    // Controller, which keep their numbers listed or not: Developer is 7,
    // Downloads is 9, System is 10, Game Content is 11 and Updates is 12.
    for (std::size_t index = 0; index < kPages.size(); ++index) {
        int32_t expected = static_cast<int32_t>(index);
        if (kPages[index] == Page::downloads)
            expected = 9;
        else if (kPages[index] == Page::system)
            expected = 10;
        else if (kPages[index] == Page::game_content)
            expected = 11;
        else if (kPages[index] == Page::updates)
            expected = 12;
        else if (kPages[index] == Page::developer)
            expected = 7;
        CHECK(settings::page_control(kPages[index]) == expected);
    }
    for (std::size_t index = 1; index < kPages.size(); ++index) {
        const auto above = geometry::list_item(kPages[index - 1]);
        CHECK(above.y + above.height <= geometry::list_item(kPages[index]).y);
    }
    const auto divider = geometry::list_divider();
    const auto graphics_entry = geometry::list_item(Page::graphics);
    CHECK(graphics_entry.y + graphics_entry.height <= divider.y);
    CHECK(divider.y + divider.height <= geometry::list_item(Page::developer).y);

    // Controls: the wheel's switch with the zoom's two limits under it, how
    // far past the map's edges the view goes, middle-click panning, whether
    // a window keeps the pointer, and how firmly a sticky edge holds.
    CHECK(settings::page_settings(Page::controls).size() == 8);
    CHECK(settings::page_settings(Page::controls)[0] == Setting::wheel_zoom);
    CHECK(settings::page_settings(Page::controls)[1] == Setting::max_zoom_out);
    CHECK(settings::page_settings(Page::controls)[2] == Setting::max_zoom_in);
    CHECK(settings::page_settings(Page::controls)[3] == Setting::view_past_map_edge);
    CHECK(settings::page_settings(Page::controls)[4] == Setting::middle_click_pan);
    CHECK(settings::page_settings(Page::controls)[5] == Setting::switch_alt);
    CHECK(settings::page_settings(Page::controls)[6] == Setting::window_edge);
    CHECK(settings::page_settings(Page::controls)[7] == Setting::window_edge_stickiness);
    // Engine Limits: the unit limit, pathfinding, the skirmish player count,
    // then the heading and the two rows the mod fixes.
    CHECK(settings::page_settings(Page::common_tweaks).size() == 6);
    CHECK(settings::page_settings(Page::common_tweaks)[0] == Setting::unit_limit);
    CHECK(settings::page_settings(Page::common_tweaks)[1] == Setting::path_search);
    CHECK(settings::page_settings(Page::common_tweaks)[2] == Setting::skirmish_players);
    CHECK(settings::page_settings(Page::common_tweaks)[3] == Setting::mod_limits_heading);
    CHECK(settings::page_settings(Page::common_tweaks)[4] == Setting::weapon_types);
    CHECK(settings::page_settings(Page::common_tweaks)[5] == Setting::sides);
    // System: Your files, then Welcome setup, then the switches where the
    // game registers file types at run time, or the text row where the app
    // declares them.
    const auto system_rows = settings::page_settings(Page::system);
    CHECK(system_rows[0] == Setting::user_folder);
    if (settings::file_types_switchable) {
        CHECK(system_rows.size() == 10);
        CHECK(system_rows[1] == Setting::welcome_setup);
        CHECK(system_rows[2] == Setting::file_type_registration);
        CHECK(system_rows[3] == Setting::file_type_oamod);
        CHECK(system_rows[4] == Setting::file_type_oamap);
        CHECK(system_rows[5] == Setting::file_type_oalang);
        CHECK(system_rows[6] == Setting::file_type_oareg);
        CHECK(system_rows[7] == Setting::file_type_oamut);
        CHECK(system_rows[8] == Setting::file_type_oabrain);
        CHECK(system_rows[9] == Setting::file_type_oalaunch);
    } else {
        CHECK(system_rows.size() == 3);
        CHECK(system_rows[1] == Setting::welcome_setup);
        CHECK(system_rows[2] == Setting::file_types_note);
    }
    // Either layout can be asked for, whichever this system shows by itself.
    settings::Dialog runtime = opened(Page::system);
    runtime.file_type_switches = true;
    const auto runtime_rows = geometry::open_rows(runtime).rows.rows;
    CHECK(runtime_rows.size() == 10);
    CHECK(runtime_rows[0].setting == Setting::user_folder);
    CHECK(runtime_rows[1].setting == Setting::welcome_setup);
    CHECK(runtime_rows[2].setting == Setting::file_type_registration);
    CHECK(runtime_rows[3].setting == Setting::file_type_oamod);
    CHECK(runtime_rows[4].setting == Setting::file_type_oamap);
    CHECK(runtime_rows[5].setting == Setting::file_type_oalang);
    CHECK(runtime_rows[6].setting == Setting::file_type_oareg);
    CHECK(runtime_rows[7].setting == Setting::file_type_oamut);
    CHECK(runtime_rows[8].setting == Setting::file_type_oabrain);
    CHECK(runtime_rows[9].setting == Setting::file_type_oalaunch);
    settings::Dialog bundle = opened(Page::system);
    bundle.file_type_switches = false;
    const auto bundle_rows = geometry::open_rows(bundle).rows.rows;
    CHECK(bundle_rows.size() == 3);
    CHECK(bundle_rows[0].setting == Setting::user_folder);
    CHECK(bundle_rows[1].setting == Setting::welcome_setup);
    CHECK(bundle_rows[2].setting == Setting::file_types_note);
    CHECK(settings::page_settings(Page::language)[0] == Setting::language);
    CHECK(settings::page_settings(Page::graphics)[0] == Setting::max_frame_rate);
    CHECK(settings::page_settings(Page::graphics)[1] == Setting::anti_aliasing);
    CHECK(settings::page_settings(Page::graphics).size() == 21);
    CHECK(settings::page_settings(Page::graphics)[2] == Setting::screen_size);
    CHECK(settings::page_settings(Page::graphics)[3] == Setting::hardware_acceleration);
    CHECK(settings::page_settings(Page::graphics)[4] == Setting::vertical_sync);
    CHECK(settings::page_settings(Page::graphics)[5] == Setting::smooth_motion);
    CHECK(settings::page_settings(Page::graphics)[6] == Setting::native_density);
    CHECK(settings::page_settings(Page::graphics)[7] == Setting::explosion_flash);
    CHECK(settings::page_settings(Page::graphics)[8] == Setting::fog_of_war);
    CHECK(settings::page_settings(Page::graphics)[9] == Setting::zoomed_out_units);
    CHECK(settings::page_settings(Page::graphics)[10] == Setting::zoomed_out_after);
    CHECK(settings::page_settings(Page::graphics)[11] == Setting::allow_full_3d);
    CHECK(settings::page_settings(Page::graphics)[12] == Setting::unlocked_camera);
    CHECK(settings::page_settings(Page::graphics)[13] == Setting::weapon_lighting);
    CHECK(settings::page_settings(Page::graphics)[14] == Setting::nanolathe_lighting);
    CHECK(settings::page_settings(Page::graphics)[15] == Setting::wreck_glow);
    CHECK(settings::page_settings(Page::graphics)[16] == Setting::heat_distortion);
    CHECK(settings::page_settings(Page::graphics)[17] == Setting::water_ripples);
    CHECK(settings::page_settings(Page::graphics)[18] == Setting::coastal_waves);
    CHECK(settings::page_settings(Page::graphics)[19] == Setting::underwater_shimmer);
    CHECK(settings::page_settings(Page::graphics)[20] == Setting::naval_reflections);
    CHECK(settings::page_settings(Page::user_interface).size() == 5);
    CHECK(settings::page_settings(Page::user_interface)[0] == Setting::menu_scaling);
    CHECK(settings::page_settings(Page::user_interface)[1] == Setting::window_frame);
    CHECK(settings::page_settings(Page::user_interface)[2] == Setting::hud_scaling);
    CHECK(settings::page_settings(Page::user_interface)[3] == Setting::interface_size);
    CHECK(settings::page_settings(Page::user_interface)[4] == Setting::escape_opens_menu);
    CHECK(settings::page_settings(Page::developer).size() == 3);
    CHECK(settings::page_settings(Page::developer)[0] == Setting::developer_mode);
    CHECK(settings::page_settings(Page::developer)[1] == Setting::frame_stats);
    CHECK(settings::page_settings(Page::developer)[2] == Setting::kit_gallery);
    // Graphics' twenty-one rows are taller than the view, by 972 rows.
    const auto graphics = geometry::place_rows(Page::graphics, {});
    CHECK(graphics.rows.size() == 21);
    CHECK(geometry::scroll_limit(geometry::content_height(graphics, 0)) == 972);

    auto parts = settings::dialog_layout(opened(Page::controls));
    for (const std::string_view text :
         {"OPEN ANNIHILATION",
          "SETTINGS",
          "v0.2.0",
          "Controls",
          "Engine Limits",
          "Language & Text",
          "Graphics",
          "User Interface",
          "Developer",
          "CONTROLS",
          "Mouse wheel zoom",
          "Scroll to zoom the battlefield in and out.",
          "Maximum zoom out",
          "As far as the view always went: 1/6 of normal size",
          "with Full hardware acceleration, else 1/2.",
          "Maximum zoom in",
          "In to 4x normal size at most. The wheel, a pinch",
          "and a controller stop there.",
          "OFF",
          "ON",
          "RESTORE DEFAULTS",
          "CANCEL",
          "OK"})
        CHECK(find_part(parts, text, settings::no_control) != nullptr);
    // The limits' fields show their choices.
    const auto controls_rows = geometry::place_rows(Page::controls, {});
    for (const auto& [row, choice] :
         {std::pair{std::size_t{1}, std::string_view{"Automatic"}},
          std::pair{std::size_t{2}, std::string_view{"4x"}}})
        CHECK(
            find_part(parts, choice, settings::first_row_control + static_cast<int32_t>(row)) !=
            nullptr
        );
    CHECK(controls_rows.rows.size() == 8);
    // View past the map's edge, scrolled into view.
    settings::Dialog middle = opened(Page::controls);
    middle.scroll[static_cast<std::size_t>(Page::controls)] = 100;
    parts = settings::dialog_layout(middle);
    for (const std::string_view text :
         {"View past the map's edge",
          "Up to half the battlefield past the map's edges; a",
          "zoom keeps the ground under the pointer."})
        CHECK(find_part(parts, text, settings::no_control) != nullptr);
    // The rows from Middle-click pans down, scrolled into view.
    settings::Dialog scrolled = opened(Page::controls);
    scrolled.scroll[static_cast<std::size_t>(Page::controls)] = std::numeric_limits<int32_t>::max();
    parts = settings::dialog_layout(scrolled);
    for (const std::string_view text :
         {"Middle-click pans",
          "A middle click pans the view towards the pointer",
          "until you click or press a key.",
          "Select groups without Alt",
          "A number key selects its group on its own.",
          "Window edge",
          "The pointer stays in the window while you play.",
          "Capture",
          "Sticky",
          "Free",
          "Edge stickiness",
          "Needs Sticky",
          "1.0",
          "How firmly Sticky holds the pointer at the edge."})
        CHECK(find_part(parts, text, settings::no_control) != nullptr);
    CHECK(find_part(parts, "Shared game - still running", settings::no_control) == nullptr);
    // Each entry is its section's control.
    for (const Page page : kPages) {
        const auto* entry = find_part(parts, {}, settings::page_control(page));
        CHECK(entry != nullptr && entry->rect.y == geometry::list_item(page).y);
    }
    // Engine Limits shows its heading and its rows' labels in order.
    parts = settings::dialog_layout(opened(Page::common_tweaks));
    CHECK(find_part(parts, "ENGINE LIMITS", settings::no_control) != nullptr);
    const auto tweaks = geometry::place_rows(Page::common_tweaks, {});
    CHECK(tweaks.rows.size() == 6);
    CHECK(tweaks.rows[0].label.y < tweaks.rows[1].label.y);
    CHECK(tweaks.rows[1].label.y < tweaks.rows[2].label.y);
    CHECK(tweaks.rows[2].label.y < tweaks.rows[3].label.y);
    CHECK(tweaks.rows[3].label.y < tweaks.rows[4].label.y);
    CHECK(tweaks.rows[4].label.y < tweaks.rows[5].label.y);
    CHECK(find_part(parts, "Skirmish players", settings::no_control) != nullptr);
    CHECK(
        find_part(parts, "The skirmish setup shows that many player rows.", settings::no_control) !=
        nullptr
    );
    CHECK(find_part(parts, "10", settings::no_control) != nullptr);
}

void a_click_on_an_entry_shows_its_section() {
    settings::Dialog dialog = opened(Page::common_tweaks);
    const auto parts = settings::dialog_layout(dialog);
    const auto* entry = find_part(parts, "Graphics", settings::no_control);
    CHECK(entry != nullptr && entry->control == settings::page_control(Page::graphics));
    CHECK(click(dialog, centre(geometry::list_item(Page::graphics))) == DialogAction::redraw);
    CHECK(dialog.page == Page::graphics);
    // A release away from the press does nothing.
    const Point developer = centre(geometry::list_item(Page::developer));
    static_cast<void>(settings::dialog_pointer_down(dialog, developer.x, developer.y));
    static_cast<void>(settings::dialog_pointer_up(dialog, 300, 200));
    CHECK(dialog.page == Page::graphics);
}

void every_control_is_pressed_where_it_is_drawn() {
    for (const Page page : kPages) {
        settings::Dialog dialog = opened(page);
        for (const auto& part : settings::dialog_layout(dialog)) {
            if (part.control == settings::no_control)
                continue;
            // A disabled button is drawn and takes no press. EMPTY is disabled
            // while nothing is kept.
            const auto list = geometry::dialog_list(dialog, nullptr);
            const oa::ui::kit::Control* control = oa::ui::kit::control_of(list, part.control);
            if (control == nullptr || !control->enabled)
                continue;
            const Point point = centre(part.rect);
            static_cast<void>(settings::dialog_pointer_move(dialog, point.x, point.y));
            CHECK(dialog.hovered == part.control);
        }
        static_cast<void>(settings::dialog_pointer_move(dialog, 300, 40));
        CHECK(dialog.hovered == settings::no_control);
    }
}

void switches_take_a_click_on_either_half_and_keys() {
    settings::Dialog dialog = opened(Page::controls);
    const auto rows = geometry::place_rows(Page::controls, {});
    const auto& zoom = rows.rows[0].control_area;
    const Point off{zoom.x + 4, zoom.y + zoom.height / 2};
    const Point on{zoom.x + zoom.width - 4, zoom.y + zoom.height / 2};
    CHECK(dialog.chosen.wheel_zoom);
    CHECK(click(dialog, off) == DialogAction::changed);
    CHECK(!dialog.chosen.wheel_zoom);
    CHECK(click(dialog, off) == DialogAction::redraw);
    CHECK(!dialog.chosen.wheel_zoom);
    CHECK(click(dialog, on) == DialogAction::changed);
    CHECK(dialog.chosen.wheel_zoom);

    // Keys: the first moves the focus to the first row; Space flips it,
    // Left sets Off and Right On.
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_row_control);
    CHECK(dialog.chosen.wheel_zoom);
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::changed);
    CHECK(!dialog.chosen.wheel_zoom);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.wheel_zoom);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::redraw);
    // Down to the limits' drop-downs: Right steps a choice on, Left back.
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_row_control + 1);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.max_zoom_out == settings::ZoomOutLimit::whole_map);
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_row_control + 2);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(dialog.chosen.max_zoom_in == settings::ZoomInLimit::three_times);
    // View past the map's edge's strip: Left steps a level down from 50%.
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_row_control + 3);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(dialog.chosen.view_past_map_edge == settings::ViewPastMapEdge::one_quarter);
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_row_control + 4);
    CHECK(dialog.chosen.middle_click_pan);
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_row_control + 5);
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::changed);
    CHECK(dialog.chosen.switch_alt);
    CHECK(dialog.chosen.middle_click_pan);
    CHECK(dialog.chosen != dialog.opened);

    // Escape opens the game menu moved to User Interface, its last row.
    settings::Dialog ui_page = opened(Page::user_interface);
    ui_page.focused = settings::first_row_control + 4;
    CHECK(settings::dialog_key(ui_page, DialogKey::right) == DialogAction::changed);
    CHECK(ui_page.chosen.escape_opens_menu);

    settings::Dialog developer = opened(Page::developer);
    const auto stats = geometry::place_rows(Page::developer, {}).rows[1].control_area;
    CHECK(click(developer, {stats.x + stats.width - 4, stats.y + 4}) == DialogAction::changed);
    CHECK(developer.chosen.frame_stats && !developer.chosen.developer_mode);
}

void language_and_text_shows_a_language_five_switches_and_a_size() {
    const auto rows = settings::page_settings(Page::language);
    CHECK(rows.size() == 7);
    CHECK(rows[0] == Setting::language);
    CHECK(rows[1] == Setting::modern_fonts);
    CHECK(rows[2] == Setting::text_size);
    CHECK(rows[3] == Setting::text_outline);
    CHECK(rows[4] == Setting::text_shadow);
    CHECK(rows[5] == Setting::text_background);
    CHECK(rows[6] == Setting::unicode_chat);
    for (const Setting setting : rows)
        CHECK(
            setting == Setting::language ? geometry::kind_of(setting) == geometry::RowKind::choice
            : setting == Setting::text_size
                ? geometry::kind_of(setting) == geometry::RowKind::slider
                : geometry::kind_of(setting) == geometry::RowKind::toggle &&
                      geometry::kind_of(setting) != geometry::RowKind::slider &&
                      geometry::kind_of(setting) != geometry::RowKind::choice
        );
    // Its entry comes after Engine Limits and before Graphics, over the line
    // above Developer.
    const auto entry = geometry::list_item(Page::language);
    CHECK(settings::page_control(Page::language) == 2);
    CHECK(entry.y > geometry::list_item(Page::common_tweaks).y);
    CHECK(entry.y < geometry::list_item(Page::graphics).y);
    CHECK(entry.y + entry.height < geometry::list_divider().y);
    CHECK(geometry::list_divider().y < geometry::list_item(Page::developer).y);

    // The player's own defaults: the system's language, modern fonts,
    // outline and shadow On, the background Off, the text at 80%.
    settings::Inputs own{};
    own.players_own_profile = true;
    const auto defaults = settings::default_settings(own);
    CHECK(defaults.language == "system");
    settings::Dialog dialog;
    settings::open_dialog(dialog, defaults, defaults, {}, "v0.2.0", Page::language);
    const auto parts = settings::dialog_layout(dialog);
    for (const std::string_view text :
         {"Language & Text",
          "LANGUAGE & TEXT",
          "Language",
          "The game's own text and unit names, where its data",
          "has them in the language.",
          "System default (English)",
          "Use modern fonts for game text",
          "Modern fonts for in-game text, including",
          "internationalization.",
          "Text size",
          "The size of game text in the modern fonts. Larger",
          "sizes are easier to read.",
          "80%"})
        CHECK(find_part(parts, text, settings::no_control) != nullptr);
    // The seven are taller than the view by 188 rows: the switches under
    // Text size show as the section scrolls, under its scroll bar.
    CHECK(geometry::open_rows(dialog).limit == 188);
    CHECK(find_part(parts, {}, settings::scroll_bar_control) != nullptr);
    dialog.scroll[static_cast<std::size_t>(Page::language)] = 188;
    for (const std::string_view text :
         {"Font outline",
          "A dark edge round each letter of modern text.",
          "Font shadow",
          "A dark shadow under modern text.",
          "Game text background",
          "A shaded box behind each line of game text.",
          "Enable Unicode Multiplayer Chat",
          "Chat in any language with players who have it;",
          "others see ? for letters they lack."})
        CHECK(find_part(settings::dialog_layout(dialog), text, settings::no_control) != nullptr);
    dialog.scroll[static_cast<std::size_t>(Page::language)] = 0;
    const auto placed = geometry::place_rows(Page::language, {});
    CHECK(placed.rows[0].hint_lines == 2 && placed.rows[1].hint_lines == 2);
    CHECK(placed.rows[2].hint_lines == 2);
    for (std::size_t row = 3; row + 1 < placed.rows.size(); ++row)
        CHECK(placed.rows[row].hint_lines == 1);
    CHECK(placed.rows.back().hint_lines == 2);
    // No game locks a Language row; only the dialog's own lock on
    // Text size, while it shows the modern fonts Off, and the command
    // line's on the language.
    for (const auto& locks : lock_states())
        for (const auto& row : geometry::place_rows(Page::language, locks).rows)
            CHECK(row.lock == Lock::none);
    for (const auto& row : geometry::open_rows(dialog).rows.rows)
        CHECK(row.lock == Lock::none);

    // A click on each switch's Off half sets it Off, and its On half On;
    // the text style follows at once.
    const auto off_of = [](const renderer::SourceRect& area) {
        return Point{area.x + 4, area.y + area.height / 2};
    };
    const auto on_of = [](const renderer::SourceRect& area) {
        return Point{area.x + area.width - 4, area.y + area.height / 2};
    };
    CHECK(settings::text_style(dialog.chosen) == oa::present::TextStyle{});
    CHECK(click(dialog, off_of(placed.rows[1].control_area)) == DialogAction::changed);
    CHECK(!dialog.chosen.modern_fonts && !settings::text_style(dialog.chosen).modern_fonts);
    dialog.scroll[static_cast<std::size_t>(Page::language)] = 129;
    const auto scrolled = geometry::open_rows(dialog).rows.rows;
    CHECK(click(dialog, off_of(scrolled[3].control_area)) == DialogAction::changed);
    CHECK(!settings::text_style(dialog.chosen).outline);
    CHECK(click(dialog, off_of(scrolled[4].control_area)) == DialogAction::changed);
    CHECK(!settings::text_style(dialog.chosen).shadow);
    CHECK(click(dialog, on_of(scrolled[5].control_area)) == DialogAction::changed);
    CHECK(settings::text_style(dialog.chosen).background);
    CHECK(click(dialog, on_of(scrolled[5].control_area)) == DialogAction::redraw);
    dialog.scroll[static_cast<std::size_t>(Page::language)] = 0;
    const oa::present::TextStyle plain_text{
        .modern_fonts = false,
        .outline = false,
        .shadow = false,
        .background = true,
        .size = settings::default_text_size,
    };
    CHECK(settings::text_style(dialog.chosen) == plain_text);
    // The rest of the settings stay as they were.
    auto others = dialog.chosen;
    others.modern_fonts = defaults.modern_fonts;
    others.text_outline = defaults.text_outline;
    others.text_shadow = defaults.text_shadow;
    others.text_background = defaults.text_background;
    CHECK(others == defaults);

    // Keys: the focus starts on the language; Space flips the focused
    // switch, Left sets Off and Right On; with the modern fonts Off the
    // focus passes over Text size.
    CHECK(settings::dialog_key(dialog, DialogKey::tab) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_row_control);
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_row_control + 1);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::redraw);
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_row_control + 3);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.text_outline);
    CHECK(settings::dialog_key(dialog, DialogKey::up) == DialogAction::redraw);
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::changed);
    CHECK(dialog.chosen.modern_fonts);
    // With them On, Down reaches Text size, whose arrows step 10%.
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_row_control + 2);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.text_size == 90 && settings::text_style(dialog.chosen).size == 90);
    CHECK(find_part(settings::dialog_layout(dialog), "90%", settings::no_control) != nullptr);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(dialog.chosen.text_size == settings::default_text_size);
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.text_shadow);
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(!dialog.chosen.text_background);
    CHECK(dialog.chosen == defaults);

    // Restore defaults puts them back, and Cancel what the dialog opened with.
    settings::EngineSettings plain = defaults;
    plain.language = "it";
    plain.modern_fonts = false;
    plain.text_shadow = false;
    plain.text_background = true;
    plain.text_size = 150;
    settings::open_dialog(dialog, plain, defaults, {}, "v0.2.0", Page::language);
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.chosen == defaults);
    CHECK(click(dialog, centre(geometry::cancel_button)) == DialogAction::cancelled);
    CHECK(dialog.chosen == plain);
}

/// Opens the dialog on Language with the player's own defaults and
/// German as the operating system's language.
///
/// @return the dialog
settings::Dialog language_dialog() {
    settings::Inputs own{};
    own.players_own_profile = true;
    const auto defaults = settings::default_settings(own);
    settings::Dialog dialog;
    settings::open_dialog(
        dialog,
        defaults,
        defaults,
        {},
        "v0.2.0",
        Page::language,
        {},
        settings::highest_unit_limit,
        {},
        oa::data::languages::find_by_tag("de")
    );
    return dialog;
}

void language_drop_down_names_each_language_in_itself() {
    [[maybe_unused]] const InstalledSimplifiedChinese installed;
    // System default first, naming the system's language in itself, then
    // English and the others in the order of their own names.
    auto dialog = language_dialog();
    CHECK(geometry::choice_count(dialog, Setting::language) == 7);
    const std::array<std::string_view, 7> names{
        "System default (Deutsch)",
        "English",
        "Deutsch",
        "Espa\xC3\xB1"
        "ol",
        "Fran\xC3\xA7"
        "ais",
        "Italiano",
        "\347\256\200\344\275\223\344\270\255\346\226\207",
    };
    for (std::size_t index = 0; index < names.size(); ++index)
        CHECK(geometry::choice_text(dialog, Setting::language, index) == names[index]);
    settings::Dialog english = dialog;
    english.system_language = nullptr;
    CHECK(geometry::choice_text(english, Setting::language, 0) == "System default (English)");
    CHECK(geometry::choice_text(dialog, Setting::language, 7).empty());
    CHECK(geometry::choice_count(dialog, Setting::modern_fonts) == 0);
    // Each choice keeps its tag, and a tag not offered shows System default.
    settings::Dialog state = dialog;
    const std::array<std::string_view, 7> tags{"system", "en", "de", "es", "fr", "it", "zh-Hans"};
    for (std::size_t index = 0; index < tags.size(); ++index) {
        geometry::set_choice(state, Setting::language, index);
        CHECK(state.chosen.language == tags[index]);
        CHECK(geometry::choice_index(state, Setting::language) == index);
    }
    geometry::set_choice(state, Setting::language, 99);
    CHECK(state.chosen.language == "zh-Hans");
    state.chosen.language = "pt";
    CHECK(geometry::choice_index(state, Setting::language) == 0);

    // The row: its label and two hint lines, and the field on a line of its
    // own showing the choice, its list closed.
    const auto open = geometry::open_rows(dialog);
    const auto& row = open.rows.rows[0];
    CHECK(row.setting == Setting::language && row.control == settings::first_row_control);
    CHECK(row.control_area.x == geometry::content_left);
    CHECK(row.control_area.width == geometry::choice_width);
    CHECK(row.control_area.height == geometry::choice_line_height);
    CHECK(row.control_area.y > row.hints[1].y);
    const auto parts = settings::dialog_layout(dialog);
    const auto* field = find_part(parts, "System default (Deutsch)", settings::no_control);
    CHECK(field != nullptr && field->control == row.control);
    CHECK(find_part(parts, "Italiano", settings::no_control) == nullptr);
}

void language_drop_down_opens_marks_and_chooses() {
    [[maybe_unused]] const InstalledSimplifiedChinese installed;
    auto dialog = language_dialog();
    const auto field = geometry::open_rows(dialog).rows.rows[0].control_area;
    const auto list = geometry::choice_list(field, 6);
    // A list of six under its field, as wide, one item a line.
    CHECK(list.x == field.x && list.y == field.y + field.height && list.width == field.width);
    CHECK(list.height == 6 * geometry::choice_item_height + 2);
    CHECK(list.y + list.height <= geometry::footer_rule_row);
    const auto item = [&](int32_t shown) { return centre(geometry::choice_item(list, shown)); };

    // A click on the field opens the list, marking the choice.
    CHECK(click(dialog, centre(field)) == DialogAction::redraw);
    CHECK(dialog.open_list == settings::first_row_control);
    CHECK(dialog.list_marked == 0 && dialog.list_first == 0);
    // Open, its items are listed and what lies under them is not.
    auto parts = settings::dialog_layout(dialog);
    for (const std::string_view text : {"English", "Deutsch", "Italiano"}) {
        const auto* part = find_part(parts, text, settings::no_control);
        CHECK(part != nullptr && inside(part->rect, list));
    }
    CHECK(find_part(parts, "Use modern fonts for game text", settings::no_control) == nullptr);
    for (std::size_t a = 0; a < parts.size(); ++a)
        for (std::size_t b = a + 1; b < parts.size(); ++b)
            CHECK(!overlap(parts[a].rect, parts[b].rect));
    // The pointer marks the item under it; a press and a release on one
    // choose it, close the list and put the language in effect.
    CHECK(settings::dialog_pointer_move(dialog, item(4).x, item(4).y) == DialogAction::redraw);
    CHECK(dialog.list_marked == 4);
    CHECK(settings::dialog_pointer_move(dialog, item(4).x, item(4).y + 1) == DialogAction::none);
    CHECK(click(dialog, item(4)) == DialogAction::changed);
    CHECK(dialog.chosen.language == "fr" && dialog.open_list == settings::no_control);
    CHECK(
        find_part(
            settings::dialog_layout(dialog),
            "Fran\xC3\xA7"
            "ais",
            settings::no_control
        ) != nullptr
    );
    // A press off the list closes it and does nothing more, even on a
    // button; a release on another item than the one pressed chooses nothing.
    CHECK(click(dialog, centre(field)) == DialogAction::redraw);
    CHECK(dialog.list_marked == 4);
    CHECK(
        settings::dialog_pointer_down(
            dialog, centre(geometry::restore_button).x, centre(geometry::restore_button).y
        ) == DialogAction::redraw
    );
    CHECK(dialog.open_list == settings::no_control && !dialog.restored);
    CHECK(
        settings::dialog_pointer_up(
            dialog, centre(geometry::restore_button).x, centre(geometry::restore_button).y
        ) == DialogAction::none
    );
    CHECK(!dialog.restored && dialog.chosen.language == "fr");
    CHECK(click(dialog, centre(field)) == DialogAction::redraw);
    CHECK(settings::dialog_pointer_down(dialog, item(1).x, item(1).y) == DialogAction::redraw);
    CHECK(settings::dialog_pointer_up(dialog, item(2).x, item(2).y) == DialogAction::redraw);
    CHECK(dialog.chosen.language == "fr" && dialog.open_list == settings::first_row_control);
    // A press on the field itself closes the open list, and its release
    // does nothing.
    CHECK(click(dialog, centre(field)) == DialogAction::none);
    CHECK(dialog.open_list == settings::no_control);
    // The wheel over an open list of seven moves neither it nor the section.
    CHECK(click(dialog, centre(field)) == DialogAction::redraw);
    CHECK(settings::dialog_wheel(dialog, item(2).x, item(2).y, -1.0F) == DialogAction::none);
    CHECK(dialog.scroll[static_cast<std::size_t>(Page::language)] == 0);
    CHECK(dialog.list_first == 0);

    // Keys work the open list: Up and Down mark, Home and End the ends;
    // Escape closes it unchanged and leaves the dialog open.
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(dialog.list_marked == 5);
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(dialog.list_marked == 6);
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::none);
    CHECK(settings::dialog_key(dialog, DialogKey::home) == DialogAction::redraw);
    CHECK(dialog.list_marked == 0);
    CHECK(settings::dialog_key(dialog, DialogKey::up) == DialogAction::none);
    CHECK(settings::dialog_key(dialog, DialogKey::end) == DialogAction::redraw);
    CHECK(dialog.list_marked == 6);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::none);
    CHECK(settings::dialog_key(dialog, DialogKey::escape) == DialogAction::redraw);
    CHECK(dialog.open_list == settings::no_control && dialog.chosen.language == "fr");
    // Closed, the focused field takes Space to open, Left and Right to step,
    // and Enter is the dialog's OK.
    CHECK(settings::dialog_key(dialog, DialogKey::tab) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_row_control);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.language == "it");
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.language == "zh-Hans");
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::redraw);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(dialog.chosen.language == "fr");
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::redraw);
    CHECK(dialog.open_list == settings::first_row_control && dialog.list_marked == 4);
    CHECK(settings::dialog_key(dialog, DialogKey::up) == DialogAction::redraw);
    CHECK(settings::dialog_key(dialog, DialogKey::up) == DialogAction::redraw);
    CHECK(settings::dialog_key(dialog, DialogKey::enter) == DialogAction::changed);
    CHECK(dialog.chosen.language == "de" && dialog.open_list == settings::no_control);
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::redraw);
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::redraw);
    CHECK(dialog.chosen.language == "de" && dialog.open_list == settings::no_control);
    // Tab closes an open list and moves the focus on.
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::redraw);
    CHECK(settings::dialog_key(dialog, DialogKey::tab) == DialogAction::redraw);
    CHECK(dialog.open_list == settings::no_control);
    CHECK(dialog.focused == settings::first_row_control + 1);
    // A press on another section's entry only closes an open list; the next
    // press opens the section.
    CHECK(settings::dialog_key(dialog, DialogKey::back_tab) == DialogAction::redraw);
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::redraw);
    CHECK(click(dialog, centre(geometry::list_item(Page::graphics))) == DialogAction::none);
    CHECK(dialog.open_list == settings::no_control && dialog.page == Page::language);
    CHECK(click(dialog, centre(geometry::list_item(Page::graphics))) == DialogAction::redraw);
    CHECK(dialog.page == Page::graphics);
    CHECK(click(dialog, centre(geometry::list_item(Page::language))) == DialogAction::redraw);
    CHECK(dialog.open_list == settings::no_control);
    // Enter with the list closed keeps the choice; Restore defaults puts
    // System default back and Cancel what the dialog opened with.
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.chosen.language == "system");
    CHECK(settings::dialog_key(dialog, DialogKey::enter) == DialogAction::accepted);
    auto cancelled = language_dialog();
    CHECK(click(cancelled, centre(field)) == DialogAction::redraw);
    CHECK(click(cancelled, item(3)) == DialogAction::changed);
    CHECK(cancelled.chosen.language == "es");
    CHECK(click(cancelled, centre(geometry::cancel_button)) == DialogAction::cancelled);
    CHECK(cancelled.chosen.language == "system");
}

void language_drop_down_locks_by_the_command_line() {
    // 3.1c's command line naming a language decides it for the run: the row
    // shows its lock, takes no press and no focus, and keeps its choice.
    settings::GameState state{};
    state.language_from_command_line = true;
    auto dialog = language_dialog();
    dialog.locks = settings::settings_locks(state);
    const auto open = geometry::open_rows(dialog);
    const auto& row = open.rows.rows[0];
    CHECK(row.lock == Lock::command_line);
    CHECK(row.lock_area.width > 0 && row.lock_area.y == row.label.y);
    CHECK(
        find_part(
            settings::dialog_layout(dialog), "Set on the command line", settings::no_control
        ) != nullptr
    );
    CHECK(click(dialog, centre(row.control_area)) == DialogAction::none);
    CHECK(dialog.open_list == settings::no_control);
    CHECK(settings::dialog_key(dialog, DialogKey::tab) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_row_control + 1);
    // Restore defaults keeps a locked choice.
    dialog.chosen.language = "es";
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.chosen.language == "es");
}

/// One pack the test lists under AVAILABLE TO DOWNLOAD.
settings::AvailableLanguage
downloadable(std::string tag, std::string endonym, std::string english_name, std::string aside) {
    settings::AvailableLanguage pack;
    pack.tag = std::move(tag);
    pack.endonym = std::move(endonym);
    pack.english_name = std::move(english_name);
    pack.aside = std::move(aside);
    return pack;
}

/// The parts of an open list, in the order the layout lists them.
std::vector<const settings::LayoutPart*>
listed_in(const std::vector<settings::LayoutPart>& parts, const renderer::SourceRect& list) {
    std::vector<const settings::LayoutPart*> found;
    for (const settings::LayoutPart& part : parts)
        if (inside(part.rect, list))
            found.push_back(&part);
    return found;
}

/// Language with two packs available to download, at one size class: the
/// open list, its names, the divider, stepping, the download and the field.
void available_languages_at(oa::ui::kit::SizeClass size_class) {
    const std::string simplified = "\347\256\200\344\275\223\344\270\255\346\226\207";
    const std::string portuguese = "Portugu\u00EAs (Brasil)";
    const std::string team_aside = "4.0 MB \u21E9";
    const std::string ridge_aside = "community \u00B7 92% \u00B7 1.1 MB";
    const auto offer = [&](settings::Dialog& dialog) {
        dialog.size_class = size_class;
        dialog.available_languages = {
            downloadable("zh-Hans", simplified, "Chinese (Simplified)", team_aside),
            downloadable("pt-BR", portuguese, "Portuguese (Brazil)", ridge_aside),
        };
    };

    auto dialog = language_dialog();
    offer(dialog);
    CHECK(geometry::choice_count(dialog, Setting::language) == 9);
    CHECK(geometry::choice_text(dialog, Setting::language, 0) == "System default (Deutsch)");
    CHECK(geometry::choice_text(dialog, Setting::language, 5) == "Italiano");
    CHECK(geometry::choice_text(dialog, Setting::language, 6) == "AVAILABLE TO DOWNLOAD");
    CHECK(geometry::choice_text(dialog, Setting::language, 7) == simplified);
    CHECK(geometry::choice_text(dialog, Setting::language, 8) == portuguese);
    // A stored available tag shows that row. "pt" is not "pt-BR".
    dialog.chosen.language = "zh-Hans";
    CHECK(geometry::choice_index(dialog, Setting::language) == 7);
    dialog.chosen.language = "pt";
    CHECK(geometry::choice_index(dialog, Setting::language) == 0);
    dialog.chosen.language = "pt-BR";
    CHECK(geometry::choice_index(dialog, Setting::language) == 8);
    dialog.chosen.language = "system";

    // Nothing is downloading: the hint is the page's own sentence, wrapped
    // to the class's room.
    auto closed = settings::dialog_layout(dialog);
    CHECK(shows_wrapped(
        closed,
        "The game's own text and unit names, where its data has them in the language.",
        dialog
    ));
    CHECK(find_part(closed, "Downloads, then switches to it.", settings::no_control) == nullptr);

    const auto field = geometry::open_rows(dialog).rows.rows[0].control_area;
    const auto list = geometry::choice_list(field, 9, size_class);
    CHECK(click(dialog, centre(field)) == DialogAction::redraw);
    CHECK(dialog.download_request.empty());
    CHECK(dialog.focused == settings::no_control);
    auto parts = settings::dialog_layout(dialog);
    const auto menu = listed_in(parts, list);
    const std::array<std::string, 9> window{{
        "System default (Deutsch)",
        "English",
        "Deutsch",
        "Espa\xC3\xB1ol",
        "Fran\xC3\xA7"
        "ais",
        "Italiano",
        "AVAILABLE TO DOWNLOAD",
        simplified,
        team_aside,
    }};
    CHECK(menu.size() == window.size());
    for (std::size_t index = 0; index < menu.size() && index < window.size(); ++index)
        CHECK(menu[index]->text == window[index]);
    CHECK(menu.size() == window.size() && menu[6]->control == settings::no_control);
    CHECK(menu.size() == window.size() && menu[6]->font == settings::DialogFont::small);
    CHECK(menu.size() == window.size() && menu[6]->tracking == geometry::heading_tracking);
    CHECK(find_part(parts, portuguese, settings::no_control) == nullptr);
    CHECK(find_part(parts, ridge_aside, settings::no_control) == nullptr);
    for (std::size_t left = 0; left < parts.size(); ++left)
        for (std::size_t right = left + 1; right < parts.size(); ++right)
            CHECK(!overlap(parts[left].rect, parts[right].rect));

    dialog.list_first = 1;
    parts = settings::dialog_layout(dialog);
    CHECK(find_part(parts, ridge_aside, settings::no_control) != nullptr);
    CHECK(find_part(parts, team_aside, settings::no_control) != nullptr);
    const oa::ui::kit::DisplayList display = geometry::dialog_list(dialog, nullptr);
    const std::string problem = oa::ui::kit::name_problem(display);
    if (!problem.empty())
        std::cerr << "available languages: " << problem << '\n';
    CHECK(problem.empty());
    bool saw_simplified = false;
    bool saw_portuguese = false;
    for (const oa::ui::kit::AutomationEntry& entry : oa::ui::kit::automation_parts(display, {})) {
        CHECK(entry.text != "AVAILABLE TO DOWNLOAD");
        CHECK(entry.name != "settings.language.7");
        if (entry.name == "settings.language.zh-hans")
            saw_simplified = true;
        if (entry.name == "settings.language.pt-br")
            saw_portuguese = true;
    }
    CHECK(saw_simplified && saw_portuguese);
    for (const oa::ui::kit::Control& control : display.controls) {
        CHECK(control.text != "AVAILABLE TO DOWNLOAD");
        CHECK(control.name != "settings.language.7");
    }

    // Down from Italiano skips the divider. Home and End reach the ends.
    CHECK(dialog.list_marked == 0);
    for (int32_t step = 0; step < 5; ++step)
        CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(dialog.list_marked == 5);
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(dialog.list_marked == 7);
    CHECK(settings::dialog_key(dialog, DialogKey::up) == DialogAction::redraw);
    CHECK(dialog.list_marked == 5);
    CHECK(settings::dialog_key(dialog, DialogKey::end) == DialogAction::redraw);
    CHECK(dialog.list_marked == 8);
    CHECK(settings::dialog_key(dialog, DialogKey::home) == DialogAction::redraw);
    CHECK(dialog.list_marked == 0);
    CHECK(settings::dialog_key(dialog, DialogKey::page_down) == DialogAction::redraw);
    CHECK(dialog.list_marked == 8);
    CHECK(settings::dialog_key(dialog, DialogKey::page_up) == DialogAction::redraw);
    CHECK(dialog.list_marked == 0);
    // A key that is not a choice, and opening the list, download nothing.
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::none);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::none);
    CHECK(dialog.download_request.empty() && dialog.open_list == settings::first_row_control);
    CHECK(settings::dialog_wheel(dialog, list.x + 4, list.y + 4, -1.0F) == DialogAction::redraw);
    CHECK(dialog.list_first == 1);

    for (int32_t step = 0; step < 6; ++step)
        static_cast<void>(settings::dialog_key(dialog, DialogKey::down));
    CHECK(dialog.list_marked == 7);
    CHECK(settings::dialog_key(dialog, DialogKey::enter) == DialogAction::download_language);
    CHECK(dialog.chosen.language == "zh-Hans" && dialog.download_request == "zh-Hans");
    CHECK(dialog.open_list == settings::no_control && dialog.focused == settings::no_control);

    // Left and Right on the closed field stay among the installed languages.
    auto stepped = language_dialog();
    offer(stepped);
    CHECK(settings::dialog_key(stepped, DialogKey::tab) == DialogAction::redraw);
    for (int32_t step = 0; step < 5; ++step)
        CHECK(settings::dialog_key(stepped, DialogKey::right) == DialogAction::changed);
    CHECK(stepped.chosen.language == "it");
    CHECK(settings::dialog_key(stepped, DialogKey::right) == DialogAction::redraw);
    CHECK(stepped.chosen.language == "it" && stepped.download_request.empty());
    geometry::set_choice(stepped, Setting::language, 7);
    CHECK(stepped.chosen.language == "zh-Hans" && stepped.download_request.empty());
    CHECK(settings::dialog_key(stepped, DialogKey::right) == DialogAction::redraw);
    CHECK(stepped.chosen.language == "zh-Hans");
    CHECK(settings::dialog_key(stepped, DialogKey::left) == DialogAction::changed);
    CHECK(stepped.chosen.language == "it");

    // Choosing a failed pack again asks for the download once more.
    auto retry = language_dialog();
    offer(retry);
    retry.chosen.language = "zh-Hans";
    retry.available_languages[0].state = settings::AvailableLanguage::State::failed;
    retry.available_languages[0].failure = "the pack was refused";
    const auto retry_field = geometry::open_rows(retry).rows.rows[0].control_area;
    // The list opens on the language already chosen, so Enter retries it.
    CHECK(click(retry, centre(retry_field)) == DialogAction::redraw);
    CHECK(retry.list_marked == 7);
    CHECK(settings::dialog_key(retry, DialogKey::enter) == DialogAction::download_language);
    CHECK(retry.download_request == "zh-Hans" && retry.chosen.language == "zh-Hans");

    // The divider is not a press. A pack with downloads off refuses a click.
    auto pressed = language_dialog();
    offer(pressed);
    const auto pressed_field = geometry::open_rows(pressed).rows.rows[0].control_area;
    const auto pressed_list = geometry::choice_list(pressed_field, 9, size_class);
    CHECK(click(pressed, centre(pressed_field)) == DialogAction::redraw);
    CHECK(click(pressed, centre(geometry::choice_item(pressed_list, 6))) == DialogAction::none);
    CHECK(pressed.open_list == settings::first_row_control && pressed.chosen.language == "system");
    pressed.available_languages[1].choosable = false;
    pressed.list_first = 1;
    const auto refused = centre(geometry::choice_item(pressed_list, 7));
    CHECK(click(pressed, refused) == DialogAction::redraw);
    CHECK(pressed.open_list == settings::first_row_control && pressed.chosen.language == "system");
    CHECK(pressed.download_request.empty());

    // An endonym the fonts cannot draw shows its English name.
    auto english = language_dialog();
    offer(english);
    english.available_languages[0].endonym_drawable = false;
    CHECK(geometry::choice_text(english, Setting::language, 7) == "Chinese (Simplified)");
    english.chosen.language = "zh-Hans";
    english.available_languages[0].state = settings::AvailableLanguage::State::downloading;
    english.available_languages[0].percent = 40;
    CHECK(
        find_part(
            settings::dialog_layout(english),
            "Chinese (Simplified) \u00B7 40%",
            settings::no_control
        ) != nullptr
    );

    // Each state in the closed field, and the hint after a failure.
    struct ShownState {
        settings::AvailableLanguage::State state;
        uint8_t percent;
        std::string field;
        std::string hint;
    };

    const ShownState states[] = {
        {settings::AvailableLanguage::State::offered,
         0,
         simplified,
         "Downloads, then switches to it."},
        {settings::AvailableLanguage::State::waiting,
         0,
         simplified + " \u00B7 waiting",
         "Downloads, then switches to it."},
        {settings::AvailableLanguage::State::downloading,
         40,
         simplified + " \u00B7 40%",
         "Downloads, then switches to it."},
        {settings::AvailableLanguage::State::installing,
         0,
         simplified + " \u00B7 installing",
         "Downloads, then switches to it."},
        {settings::AvailableLanguage::State::failed,
         0,
         simplified + " \u00B7 failed",
         "The download failed: the pack was refused. Choose it again to try again."},
    };
    for (const auto& state : states) {
        auto shown = language_dialog();
        offer(shown);
        shown.chosen.language = "zh-Hans";
        shown.available_languages[0].state = state.state;
        shown.available_languages[0].percent = state.percent;
        shown.available_languages[0].failure = "the pack was refused";
        const auto laid = settings::dialog_layout(shown);
        CHECK(find_part(laid, state.field, settings::no_control) != nullptr);
        CHECK(shows_wrapped(laid, state.hint, shown));
        CHECK(find_part(laid, "data has them in the language.", settings::no_control) == nullptr);
        CHECK(find_part(laid, simplified + " \u00B7 offered", settings::no_control) == nullptr);
    }
}

void available_languages_list_download_and_show_their_state() {
    CHECK(settings::download_size_text(312000) == "312 kB");
    CHECK(settings::download_size_text(999999) == "999 kB");
    CHECK(settings::download_size_text(1000000) == "1.0 MB");
    CHECK(settings::download_size_text(4012345) == "4.0 MB");
    CHECK(settings::download_size_text(99999999) == "99.9 MB");
    CHECK(settings::download_size_text(188000000) == "188 MB");
    CHECK(
        settings::language_download_aside(true, "OA team", std::nullopt, 4012345, true) ==
        "4.0 MB \u21E9"
    );
    CHECK(
        settings::language_download_aside(true, "Ridge", 0.92, 1100000, true) ==
        "community \u00B7 92% \u00B7 1.1 MB"
    );
    CHECK(
        settings::language_download_aside(true, "Ridge", std::nullopt, 1100000, true) ==
        "community \u00B7 1.1 MB"
    );
    CHECK(
        settings::language_download_aside(false, "OA team", 0.92, 4012345, true) ==
        "not reviewed \u00B7 92% \u00B7 4.0 MB"
    );
    CHECK(
        settings::language_download_aside(true, "OA team", std::nullopt, 4012345, false) ==
        "downloads off"
    );
    // With nothing to download the page is unchanged: six choices, no divider.
    auto plain = language_dialog();
    CHECK(geometry::choice_count(plain, Setting::language) == 6);
    CHECK(
        find_part(settings::dialog_layout(plain), "AVAILABLE TO DOWNLOAD", settings::no_control) ==
        nullptr
    );
    for (const oa::ui::kit::SizeClass size_class :
         {oa::ui::kit::SizeClass::compact,
          oa::ui::kit::SizeClass::regular,
          oa::ui::kit::SizeClass::large})
        available_languages_at(size_class);
}

void drop_down_lists_scroll_and_open_over_their_field() {
    // A list of more items than it shows is as tall as most_shown_choices,
    // and opens over its field when it would reach below the footer's line.
    CHECK(geometry::shown_choices(3) == 3 && geometry::shown_choices(12) == 8);
    const renderer::SourceRect high{geometry::content_left, 80, geometry::choice_width, 16};
    const auto under = geometry::choice_list(high, 12);
    CHECK(under.y == 96 && under.height == 8 * geometry::choice_item_height + 2);
    const renderer::SourceRect low{geometry::content_left, 250, geometry::choice_width, 16};
    const auto over = geometry::choice_list(low, 12);
    CHECK(over.y + over.height == low.y && over.height == under.height);
    CHECK(over.y >= geometry::body_top);
    for (int32_t shown = 0; shown < 8; ++shown) {
        const auto item = geometry::choice_item(over, shown);
        CHECK(inside(item, over));
        CHECK(item.height == geometry::choice_item_height);
    }
}

/// Counts a text's characters, not its bytes, one source pixel each.
int32_t one_a_character(std::string_view text) {
    int32_t characters = 0;
    for (const char byte : text)
        if ((static_cast<unsigned char>(byte) & 0xC0U) != 0x80U)
            ++characters;
    return characters;
}

void path_tails_keep_the_last_components_that_fit() {
    const auto bytes = [](std::string_view text) { return static_cast<int32_t>(text.size()); };
    // A path that fits shows whole.
    CHECK(settings::path_tail("/a/b", 10, bytes) == "/a/b");
    // Else "..." and the most whole components that fit, their separator kept.
    CHECK(settings::path_tail("/Users/player/TA/mods/My Mod", 15, bytes) == ".../mods/My Mod");
    CHECK(settings::path_tail("/Users/player/TA/mods/My Mod", 14, bytes) == ".../My Mod");
    CHECK(settings::path_tail("C:\\Games\\TA\\mods\\Mod", 12, bytes) == "...\\mods\\Mod");
    // A separator at its end is dropped.
    CHECK(settings::path_tail("/a/b/", 10, bytes) == "/a/b");
    // A last component too long alone keeps as much of its end as fits,
    // whole characters only.
    CHECK(settings::path_tail("/x/abcdefghij", 8, bytes) == "...fghij");
    CHECK(
        settings::path_tail("/x/\xC3\xA9\xC3\xA9\xC3\xA9", 5, one_a_character) ==
        "...\xC3\xA9\xC3\xA9"
    );
    // Room for nothing more shows "..." alone.
    CHECK(settings::path_tail("/x/abcdefghij", 2, bytes) == "...");
    CHECK(settings::path_tail({}, 2, bytes).empty());
}

void text_size_runs_from_half_to_three_times_in_tenths() {
    // 50% to 300% of the game fonts' sizes in steps of 10%: 26 stops, the
    // default 80% the fourth.
    CHECK(settings::lowest_text_size == 50 && settings::highest_text_size == 300);
    CHECK(settings::text_size_step == 10 && settings::default_text_size == 80);
    CHECK(geometry::slider_of(Setting::text_size).stops == 26);
    settings::EngineSettings state{};
    CHECK(state.text_size == 80 && geometry::stop_of(state, Setting::text_size) == 3);
    CHECK(geometry::value_text(Setting::text_size, state) == "80%");
    const renderer::SourceRect track{100, 50, 189, geometry::slider_line_height};
    for (int32_t stop = 0; stop < 26; ++stop) {
        geometry::set_stop(state, Setting::text_size, stop);
        CHECK(state.text_size == 50 + 10 * stop);
        CHECK(geometry::stop_of(state, Setting::text_size) == stop);
        CHECK(
            geometry::value_text(Setting::text_size, state) == std::to_string(50 + 10 * stop) + "%"
        );
        CHECK(geometry::stop_at(track, geometry::knob_column(track, stop, 26), 26) == stop);
    }
    geometry::set_stop(state, Setting::text_size, 99);
    CHECK(state.text_size == 300);
    geometry::set_stop(state, Setting::text_size, -4);
    CHECK(state.text_size == 50);
    // A size off the steps, as a file may hold, shows at the nearest stop
    // and keeps its value until moved.
    state.text_size = 84;
    CHECK(geometry::stop_of(state, Setting::text_size) == 3);
    CHECK(geometry::value_text(Setting::text_size, state) == "84%");
    state.text_size = 85;
    CHECK(geometry::stop_of(state, Setting::text_size) == 4);
    // Its row: a slider under a label and two hint lines, which the
    // drag sets as every slider's does.
    settings::Inputs own{};
    own.players_own_profile = true;
    const auto defaults = settings::default_settings(own);
    settings::Dialog dialog;
    settings::open_dialog(dialog, defaults, defaults, {}, "v0.2.0", Page::language);
    const auto row = geometry::open_rows(dialog).rows.rows[2];
    CHECK(row.setting == Setting::text_size && row.control == settings::first_row_control + 2);
    CHECK(
        settings::dialog_pointer_down(
            dialog, row.control_area.x + row.control_area.width - 1, row.control_area.y + 4
        ) == DialogAction::changed
    );
    CHECK(dialog.chosen.text_size == 300);
    CHECK(
        settings::dialog_pointer_move(dialog, row.control_area.x - 30, row.control_area.y) ==
        DialogAction::changed
    );
    CHECK(dialog.chosen.text_size == 50);
    static_cast<void>(settings::dialog_pointer_up(dialog, 0, 0));
    CHECK(find_part(settings::dialog_layout(dialog), "50%", settings::no_control) != nullptr);
}

void text_size_waits_for_the_modern_fonts() {
    // With the modern fonts Off the slider is locked: faded, its padlock
    // saying it needs them, its second hint line that the game's own fonts
    // have fixed sizes, and it takes no press and no focus.
    settings::EngineSettings plain{};
    CHECK(!plain.modern_fonts);
    settings::Dialog dialog;
    settings::open_dialog(dialog, plain, plain, {}, "v0.2.0", Page::language);
    CHECK(geometry::shown_locks(dialog).text_size == Lock::needs_modern_fonts);
    const auto open = geometry::open_rows(dialog);
    const auto& row = open.rows.rows[2];
    CHECK(row.setting == Setting::text_size && row.lock == Lock::needs_modern_fonts);
    CHECK(row.lock_area.width > 0 && row.lock_area.y == row.label.y);
    const auto parts = settings::dialog_layout(dialog);
    for (const std::string_view text :
         {"Text size",
          "Needs modern fonts",
          "The size of game text in the modern fonts. The",
          "game's own fonts have fixed sizes.",
          "80%"})
        CHECK(find_part(parts, text, settings::no_control) != nullptr);
    CHECK(find_part(parts, "Larger sizes are easier to read.", settings::no_control) == nullptr);
    for (const auto& part : parts)
        CHECK(part.control != row.control);
    CHECK(geometry::lock_text(Lock::needs_modern_fonts) == "Needs modern fonts");
    CHECK(
        settings::dialog_pointer_down(
            dialog, row.control_area.x + row.control_area.width - 1, row.control_area.y + 4
        ) == DialogAction::none
    );
    static_cast<void>(settings::dialog_pointer_up(dialog, 0, 0));
    CHECK(dialog.chosen.text_size == settings::default_text_size);
    CHECK(settings::dialog_key(dialog, DialogKey::tab) == DialogAction::redraw);
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_row_control + 3);

    // Turning them On lifts the lock at once, and Off puts it back; the
    // size keeps its value through both.
    const auto modern_on = geometry::open_rows(dialog).rows.rows[1].control_area;
    CHECK(
        click(dialog, {modern_on.x + modern_on.width - 4, modern_on.y + 4}) == DialogAction::changed
    );
    CHECK(geometry::shown_locks(dialog).text_size == Lock::none);
    CHECK(geometry::open_rows(dialog).rows.rows[2].lock == Lock::none);
    CHECK(
        find_part(settings::dialog_layout(dialog), "Needs modern fonts", settings::no_control) ==
        nullptr
    );
    CHECK(
        find_part(
            settings::dialog_layout(dialog), "sizes are easier to read.", settings::no_control
        ) != nullptr
    );
    // A lock the game puts on Text size is kept, whatever the switch.
    dialog.locks.text_size = Lock::in_game;
    CHECK(geometry::shown_locks(dialog).text_size == Lock::in_game);
    dialog.locks.text_size = Lock::none;
    // Restore defaults resets the size even while it is locked by the
    // modern fonts; Cancel brings back what the dialog opened with.
    settings::EngineSettings larger = plain;
    larger.text_size = 200;
    settings::open_dialog(dialog, larger, plain, {}, "v0.2.0", Page::language);
    CHECK(geometry::open_rows(dialog).rows.rows[2].lock == Lock::needs_modern_fonts);
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.chosen.text_size == settings::default_text_size && dialog.chosen == plain);
    CHECK(click(dialog, centre(geometry::cancel_button)) == DialogAction::cancelled);
    CHECK(dialog.chosen.text_size == 200);
}

void a_slider_lock_drops_when_its_label_does_not_fit() {
    // Text size is locked while modern fonts are off. Measured in a wide
    // regular face, the label does not fit beside the lock, so the lock
    // moves under the label and the label keeps the line. A narrow face
    // still fits, and the lock stays on the label line.
    settings::Dialog dialog;
    settings::open_dialog(dialog, {}, {}, {}, "v0.2.0", Page::language);
    const geometry::TextWidth small = [](std::string_view words) {
        return static_cast<int32_t>(words.size()) * 4;
    };
    const auto text_size_row = [](const geometry::Rows& rows) -> const geometry::Row* {
        for (const geometry::Row& row : rows.rows)
            if (row.setting == Setting::text_size)
                return &row;
        return nullptr;
    };
    const auto place = [&](int32_t columns) {
        const geometry::TextWidth regular = [columns](std::string_view words) {
            return static_cast<int32_t>(words.size()) * columns;
        };
        return geometry::open_rows(dialog, &small, nullptr, &regular);
    };

    const geometry::ScrolledRows fitted = place(4);
    const geometry::Row* fits = text_size_row(fitted.rows);
    CHECK(fits != nullptr && fits->lock == Lock::needs_modern_fonts);
    if (fits != nullptr) {
        CHECK(fits->lock_area.y == fits->label.y);
        CHECK(fits->lock_area.x == fits->label.x + fits->label.width + geometry::label_gap);
        CHECK(fits->label.width >= static_cast<int32_t>(std::string_view("Text size").size()) * 4);
        CHECK(!overlap(fits->label, fits->lock_area));
        CHECK(!overlap(fits->lock_area, fits->control_area));
    }

    const geometry::ScrolledRows dropped = place(30);
    const geometry::Row* wide = text_size_row(dropped.rows);
    CHECK(wide != nullptr && wide->lock == Lock::needs_modern_fonts);
    if (wide != nullptr) {
        CHECK(wide->label.x == geometry::content_left);
        CHECK(wide->label.width == geometry::content_width);
        CHECK(wide->label.width >= static_cast<int32_t>(std::string_view("Text size").size()) * 30);
        CHECK(wide->lock_area.x == geometry::content_left);
        CHECK(
            wide->lock_area.y == wide->label.y + geometry::label_line_height + geometry::hint_gap
        );
        CHECK(!overlap(wide->label, wide->lock_area));
        CHECK(wide->hint_lines >= 1);
        CHECK(
            wide->hints[0].y == wide->lock_area.y + geometry::label_line_height + geometry::hint_gap
        );
        CHECK(wide->control_area.y >= wide->hints[0].y + geometry::hint_line_height);
    }
}

void a_language_locks_the_modern_fonts_and_unicode_chat() {
    [[maybe_unused]] const InstalledSimplifiedChinese installed;
    // Simplified Chinese draws in the modern fonts, and its pack asks for
    // chat in UTF-8: choosing it turns the fonts on, and both switches show
    // On, locked, set by the language, until another language is chosen.
    settings::EngineSettings plain{};
    settings::Dialog dialog;
    settings::open_dialog(dialog, plain, plain, {}, "v0.2.0", Page::language);
    dialog.unicode_chat_languages = {"zh-Hans"};
    CHECK(geometry::shown_locks(dialog).modern_fonts == Lock::none);
    CHECK(geometry::shown_locks(dialog).unicode_chat == Lock::none);
    const auto offered = geometry::offered_languages();
    const auto chinese = std::find_if(offered.begin(), offered.end(), [](const auto* language) {
        return language->tag == "zh-Hans";
    });
    CHECK(chinese != offered.end());
    if (chinese == offered.end())
        return;
    geometry::set_choice(
        dialog, Setting::language, static_cast<std::size_t>(chinese - offered.begin()) + 1
    );
    CHECK(dialog.chosen.language == "zh-Hans" && dialog.chosen.modern_fonts);
    CHECK(!dialog.chosen.unicode_chat);
    const auto locks = geometry::shown_locks(dialog);
    CHECK(locks.modern_fonts == Lock::set_by_language);
    CHECK(locks.unicode_chat == Lock::set_by_language);
    CHECK(locks.text_size == Lock::none);
    CHECK(geometry::lock_text(Lock::set_by_language) == "Set by the language");
    const auto rows = geometry::open_rows(dialog).rows.rows;
    const auto chat = std::find_if(rows.begin(), rows.end(), [](const auto& row) {
        return row.setting == Setting::unicode_chat;
    });
    CHECK(chat != rows.end() && chat->lock == Lock::set_by_language);
    // English lifts both locks; the fonts stay on as the player left them,
    // and the chat setting is the player's own again.
    geometry::set_choice(dialog, Setting::language, 1);
    CHECK(dialog.chosen.language == "en" && dialog.chosen.modern_fonts);
    CHECK(geometry::shown_locks(dialog).modern_fonts == Lock::none);
    CHECK(geometry::shown_locks(dialog).unicode_chat == Lock::none);
    CHECK(!dialog.chosen.unicode_chat);
}

void every_stop_maps_to_its_value_and_back() {
    struct Expected {
        settings::Setting setting;
        int32_t stops;
    };

    for (const Expected expected :
         {Expected{settings::Setting::path_search, 8},
          Expected{settings::Setting::unit_limit, 30},
          Expected{settings::Setting::skirmish_players, 9},
          Expected{settings::Setting::max_frame_rate, 19},
          Expected{settings::Setting::screen_size, 5}}) {
        CHECK(geometry::slider_of(expected.setting).stops == expected.stops);
        const renderer::SourceRect track{100, 50, 189, geometry::slider_line_height};
        for (int32_t stop = 0; stop < expected.stops; ++stop) {
            settings::EngineSettings state{};
            geometry::set_stop(state, expected.setting, stop);
            CHECK(geometry::stop_of(state, expected.setting) == stop);
            const int32_t column = geometry::knob_column(track, stop, expected.stops);
            CHECK(geometry::stop_at(track, column, expected.stops) == stop);
        }
    }
    settings::EngineSettings state{};
    geometry::set_stop(state, settings::Setting::path_search, 0);
    CHECK(state.path_search_nodes == settings::base_path_search_nodes);
    geometry::set_stop(state, settings::Setting::path_search, 7);
    CHECK(state.path_search_nodes == 8 * settings::base_path_search_nodes);
    geometry::set_stop(state, settings::Setting::path_search, 99);
    CHECK(state.path_search_nodes == 8 * settings::base_path_search_nodes);
    geometry::set_stop(state, settings::Setting::unit_limit, 0);
    CHECK(state.unit_limit == 50);
    geometry::set_stop(state, settings::Setting::unit_limit, 4);
    CHECK(state.unit_limit == 250);
    geometry::set_stop(state, settings::Setting::unit_limit, 29);
    CHECK(state.unit_limit == 1500);
    // Skirmish players: 2 to 10. Ten, the new-install count, is the last stop.
    // A stored count off the stops, such as 1, shows as it is until moved.
    geometry::set_stop(state, settings::Setting::skirmish_players, 0);
    CHECK(state.skirmish_players == settings::lowest_skirmish_players);
    geometry::set_stop(state, settings::Setting::skirmish_players, 8);
    CHECK(state.skirmish_players == settings::highest_skirmish_players);
    CHECK(geometry::value_text(settings::Setting::skirmish_players, state) == "10");
    state.skirmish_players = 1;
    CHECK(geometry::stop_of(state, settings::Setting::skirmish_players) == 0);
    CHECK(geometry::value_text(settings::Setting::skirmish_players, state) == "1");
    state.skirmish_players = settings::default_skirmish_players;
    // A mod's higher maximum adds stops past 1500, up to the largest a profile may name.
    const uint16_t highest = oa::data::limits::highest_units_per_player;
    CHECK(geometry::slider_of(settings::Setting::unit_limit, highest).stops == 131);
    geometry::set_stop(state, settings::Setting::unit_limit, 130, highest);
    CHECK(state.unit_limit == 6550);
    CHECK(geometry::stop_of(state, settings::Setting::unit_limit, highest) == 130);
    geometry::set_stop(state, settings::Setting::unit_limit, 200, 3000);
    CHECK(state.unit_limit == 3000);
    geometry::set_stop(state, settings::Setting::unit_limit, 4);
    // The frame rate from 30, a frame for each tick, to 120 in steps of 5.
    geometry::set_stop(state, settings::Setting::max_frame_rate, 0);
    CHECK(state.max_frame_rate == 30);
    CHECK(geometry::value_text(settings::Setting::max_frame_rate, state) == "30 fps");
    geometry::set_stop(state, settings::Setting::max_frame_rate, 2);
    CHECK(state.max_frame_rate == 40);
    geometry::set_stop(state, settings::Setting::max_frame_rate, 18);
    CHECK(state.max_frame_rate == 120);
    // A limit off the slider's steps, such as an installation's 20, shows at
    // the nearest stop and keeps its value until moved.
    state.unit_limit = 20;
    CHECK(geometry::stop_of(state, settings::Setting::unit_limit) == 0);
    CHECK(geometry::value_text(settings::Setting::unit_limit, state) == "20 per player");
    state.unit_limit = 275;
    CHECK(geometry::stop_of(state, settings::Setting::unit_limit) == 5);
    // The screen sizes from the desktop's up, each shown as it is.
    geometry::set_stop(state, settings::Setting::screen_size, 0);
    CHECK(state.screen_size == settings::desktop_screen_size);
    CHECK(geometry::value_text(settings::Setting::screen_size, state) == "Desktop");
    geometry::set_stop(state, settings::Setting::screen_size, 2);
    CHECK((state.screen_size == settings::ScreenSize{800, 600}));
    CHECK(geometry::value_text(settings::Setting::screen_size, state) == "800 x 600");
    geometry::set_stop(state, settings::Setting::screen_size, 99);
    CHECK((state.screen_size == settings::ScreenSize{1280, 1024}));
    CHECK(geometry::value_text(settings::Setting::screen_size, state) == "1280 x 1024");
}

void sliders_follow_the_pointer_and_the_arrows() {
    // Pathfinding cycles, Engine Limits' second row.
    settings::Dialog dialog = opened(Page::common_tweaks);
    const auto track = geometry::place_rows(Page::common_tweaks, {}).rows[1].control_area;
    const int32_t row = track.y + track.height / 2;
    CHECK(
        settings::dialog_pointer_down(dialog, track.x + track.width - 1, row) ==
        DialogAction::changed
    );
    CHECK(dialog.dragging);
    CHECK(dialog.chosen.path_search_nodes == 8 * settings::base_path_search_nodes);
    CHECK(settings::dialog_pointer_move(dialog, track.x - 40, row + 30) == DialogAction::changed);
    CHECK(dialog.chosen.path_search_nodes == settings::base_path_search_nodes);
    CHECK(
        settings::dialog_pointer_move(dialog, geometry::knob_column(track, 2, 8), row) ==
        DialogAction::changed
    );
    CHECK(dialog.chosen.path_search_nodes == 3 * settings::base_path_search_nodes);
    CHECK(settings::dialog_pointer_up(dialog, 0, 0) == DialogAction::redraw);
    CHECK(!dialog.dragging);
    CHECK(settings::dialog_pointer_move(dialog, track.x - 40, row) != DialogAction::changed);
    CHECK(dialog.chosen.path_search_nodes == 3 * settings::base_path_search_nodes);

    const auto parts = settings::dialog_layout(dialog);
    CHECK(find_part(parts, "3x", settings::no_control) != nullptr);

    // Arrows move one stop and stop at the ends.
    for (int32_t step = 0; step < 2; ++step)
        CHECK(settings::dialog_key(dialog, DialogKey::tab) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_row_control + 1);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(dialog.chosen.path_search_nodes == 2 * settings::base_path_search_nodes);
    for (int32_t press = 0; press < 10; ++press)
        static_cast<void>(settings::dialog_key(dialog, DialogKey::right));
    CHECK(dialog.chosen.path_search_nodes == 8 * settings::base_path_search_nodes);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::redraw);

    settings::Dialog graphics = opened(Page::graphics);
    CHECK(settings::dialog_key(graphics, DialogKey::down) == DialogAction::redraw);
    CHECK(settings::dialog_key(graphics, DialogKey::left) == DialogAction::changed);
    CHECK(graphics.chosen.max_frame_rate == 115);
    CHECK(find_part(settings::dialog_layout(graphics), "115 fps", settings::no_control) != nullptr);

    // The unit limit, Engine Limits' first row.
    settings::Dialog gameplay = opened(Page::common_tweaks);
    CHECK(settings::dialog_key(gameplay, DialogKey::down) == DialogAction::redraw);
    CHECK(gameplay.focused == settings::first_row_control);
    CHECK(settings::dialog_key(gameplay, DialogKey::right) == DialogAction::changed);
    CHECK(gameplay.chosen.unit_limit == 300);
    CHECK(
        find_part(settings::dialog_layout(gameplay), "300 per player", settings::no_control) !=
        nullptr
    );
}

void screen_size_offers_the_displays_sizes() {
    // A 4K monitor's sizes after Desktop, as the game offers them.
    settings::Dialog dialog = opened(Page::graphics);
    dialog.offered_screen_sizes = {
        settings::desktop_screen_size,
        {640, 480},
        {800, 600},
        {1024, 768},
        {1280, 1024},
        {1600, 1200},
        {1920, 1080},
        {2560, 1440},
        {3440, 1440},
        {3840, 2160},
    };
    const auto track = geometry::place_rows(Page::graphics, {}).rows[2].control_area;
    const int32_t row = track.y + track.height / 2;
    CHECK(
        settings::dialog_pointer_down(dialog, track.x + track.width - 1, row) ==
        DialogAction::changed
    );
    CHECK((dialog.chosen.screen_size == settings::ScreenSize{3840, 2160}));
    CHECK(
        find_part(settings::dialog_layout(dialog), "3840 x 2160", settings::no_control) != nullptr
    );
    CHECK(settings::dialog_pointer_up(dialog, 0, 0) == DialogAction::redraw);
    // The arrows step through the display's sizes and stop at the ends.
    for (int32_t press = 0; press < 8 && dialog.focused != settings::first_row_control + 2; ++press)
        static_cast<void>(settings::dialog_key(dialog, DialogKey::down));
    CHECK(dialog.focused == settings::first_row_control + 2);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK((dialog.chosen.screen_size == settings::ScreenSize{3440, 1440}));
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK((dialog.chosen.screen_size == settings::ScreenSize{2560, 1440}));
    for (int32_t press = 0; press < 12; ++press)
        static_cast<void>(settings::dialog_key(dialog, DialogKey::left));
    CHECK(dialog.chosen.screen_size == settings::desktop_screen_size);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::redraw);
    // A size the display does not offer shows on the first stop and keeps
    // its value until moved.
    dialog.chosen.screen_size = {1366, 768};
    CHECK(
        geometry::stop_of(
            dialog.chosen,
            settings::Setting::screen_size,
            settings::highest_unit_limit,
            dialog.offered_screen_sizes
        ) == 0
    );
    CHECK(geometry::value_text(settings::Setting::screen_size, dialog.chosen) == "1366 x 768");
}

void screen_size_shows_custom_for_a_window_sized_by_hand() {
    [[maybe_unused]] const InstalledSimplifiedChinese installed;
    // In a window Screen size shows the window's own size until its knob
    // moves, whatever the setting (Desktop leaves a window as it is): here a
    // window the player dragged to 1300x800, which the display offers no
    // size of, stands as a stop of its own, shown as Custom.
    settings::Dialog dialog = opened(Page::graphics);
    dialog.offered_screen_sizes = {
        settings::desktop_screen_size,
        {640, 480},
        {1280, 720},
        {1300, 800},
        {1920, 1080},
    };
    dialog.window_screen_size = settings::ScreenSize{1300, 800};
    dialog.custom_screen_size = settings::ScreenSize{1300, 800};
    CHECK(dialog.chosen.screen_size == settings::desktop_screen_size);
    CHECK(geometry::value_text(settings::Setting::screen_size, dialog) == "Custom");
    CHECK(geometry::value_text(settings::Setting::screen_size, dialog.chosen) == "Desktop");
    CHECK(find_part(settings::dialog_layout(dialog), "Custom", settings::no_control) != nullptr);
    CHECK(
        geometry::stop_of(
            geometry::slider_settings(dialog),
            settings::Setting::screen_size,
            settings::highest_unit_limit,
            dialog.offered_screen_sizes
        ) == 3
    );
    // The arrows step from the window's size to the listed sizes on each
    // side, which show as sizes, and the setting chosen follows.
    for (int32_t press = 0; press < 8 && dialog.focused != settings::first_row_control + 2; ++press)
        static_cast<void>(settings::dialog_key(dialog, DialogKey::down));
    CHECK(dialog.focused == settings::first_row_control + 2);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK((dialog.chosen.screen_size == settings::ScreenSize{1280, 720}));
    CHECK(!dialog.window_screen_size);
    CHECK(geometry::value_text(settings::Setting::screen_size, dialog) == "1280 x 720");
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK((dialog.chosen.screen_size == settings::ScreenSize{1300, 800}));
    CHECK(geometry::value_text(settings::Setting::screen_size, dialog) == "Custom");
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(geometry::value_text(settings::Setting::screen_size, dialog) == "1920 x 1080");
    // A press on the track moves the knob too.
    dialog.window_screen_size = settings::ScreenSize{1300, 800};
    const auto track = geometry::place_rows(Page::graphics, {}).rows[2].control_area;
    CHECK(
        settings::dialog_pointer_down(dialog, track.x + 1, track.y + track.height / 2) ==
        DialogAction::changed
    );
    CHECK(dialog.chosen.screen_size == settings::desktop_screen_size && !dialog.window_screen_size);
    CHECK(geometry::value_text(settings::Setting::screen_size, dialog) == "Desktop");
    CHECK(settings::dialog_pointer_up(dialog, 0, 0) == DialogAction::redraw);
    // Without a Custom stop every size shows as itself.
    dialog.custom_screen_size.reset();
    dialog.chosen.screen_size = {1300, 800};
    CHECK(geometry::value_text(settings::Setting::screen_size, dialog) == "1300 x 800");
    // The size applies when OK is pressed: one sentence, wrapped to the room.
    const std::string screen_hint =
        geometry::hint_line(settings::Setting::screen_size, dialog.chosen, dialog.acceleration, 0);
    CHECK(screen_hint == "Full screen at this size, or a window of it. Applies when you press OK.");
    CHECK(
        geometry::hint_line(settings::Setting::screen_size, dialog.chosen, dialog.acceleration, 1)
            .empty()
    );
    CHECK(shows_wrapped(settings::dialog_layout(dialog), screen_hint, dialog));
    // Custom in Simplified Chinese.
    oa::data::languages::InterfaceText catalogue;
    CHECK(catalogue.add("[Custom]\n{\nzh-Hans=\u81ea\u5b9a\u4e49;\n}\n"));
    oa::data::languages::set_interface_language(
        &catalogue, *oa::data::languages::find_by_tag("zh-Hans")
    );
    dialog.custom_screen_size = settings::ScreenSize{1300, 800};
    const std::string chinese = geometry::value_text(settings::Setting::screen_size, dialog);
    oa::data::languages::set_interface_language(nullptr, oa::data::languages::english());
    CHECK(chinese == "\u81ea\u5b9a\u4e49");
}

void the_level_strip_picks_a_level() {
    settings::Dialog dialog = opened(Page::graphics);
    std::vector<const settings::LayoutPart*> levels;
    const auto parts = settings::dialog_layout(dialog);
    for (const auto& part : parts) {
        if (part.control == settings::first_row_control + 1)
            levels.push_back(&part);
    }
    CHECK(levels.size() == settings::anti_aliasing_levels.size());
    if (levels.size() != settings::anti_aliasing_levels.size())
        return;
    CHECK(levels[0]->text == "Off");
    CHECK(levels[1]->text == "2x");
    CHECK(levels[4]->text == "16x");
    for (std::size_t index = settings::anti_aliasing_levels.size(); index-- > 0;) {
        const auto expected = settings::anti_aliasing_levels[index];
        CHECK(click(dialog, centre(levels[index]->rect)) == DialogAction::changed);
        CHECK(dialog.chosen.anti_aliasing == expected);
    }
    CHECK(
        find_part(
            settings::dialog_layout(dialog),
            "Units drawn at higher resolution and scaled down",
            settings::no_control
        ) != nullptr
    );
    dialog.chosen.anti_aliasing = settings::AntiAliasing::x8;
    CHECK(
        find_part(
            settings::dialog_layout(dialog),
            "Units drawn at 8x and scaled down. Needs a fast",
            settings::no_control
        ) != nullptr
    );
    dialog.chosen.anti_aliasing = settings::AntiAliasing::x16;
    const auto demanding = settings::dialog_layout(dialog);
    CHECK(
        find_part(
            demanding, "Units drawn at 16x and scaled down. Needs a fast", settings::no_control
        ) != nullptr
    );
    CHECK(find_part(demanding, "CPU.", settings::no_control) != nullptr);
    // While frames are drawn in Full the hint says what the level does
    // there, at the factor in use, and never asks for a fast processor.
    settings::AccelerationStatus full = dialog.acceleration;
    full.full_supersample = 4;
    CHECK(settings::set_acceleration_status(dialog, full) == DialogAction::redraw);
    const auto in_full = settings::dialog_layout(dialog);
    // The row asks for 16x: four across is fewer than asked, which the
    // second line says.
    CHECK(
        find_part(
            in_full, "In Full the card draws 4x4 samples a pixel the", settings::no_control
        ) != nullptr
    );
    CHECK(
        find_part(in_full, "most this window allows, scaled down.", settings::no_control) != nullptr
    );
    CHECK(find_part(in_full, "Needs a fast CPU.", settings::no_control) == nullptr);
    full.full_supersample = 16;
    CHECK(settings::set_acceleration_status(dialog, full) == DialogAction::redraw);
    const auto at_sixteen = settings::dialog_layout(dialog);
    CHECK(
        find_part(
            at_sixteen, "In Full the card draws 16x16 samples a pixel and", settings::no_control
        ) != nullptr
    );
    CHECK(
        find_part(at_sixteen, "scales them down for smoother edges.", settings::no_control) !=
        nullptr
    );
    full.full_supersample = 2;
    CHECK(settings::set_acceleration_status(dialog, full) == DialogAction::redraw);
    CHECK(
        find_part(
            settings::dialog_layout(dialog),
            "In Full the card draws 2x2 samples a pixel the",
            settings::no_control
        ) != nullptr
    );
    full.full_supersample = 1;
    CHECK(settings::set_acceleration_status(dialog, full) == DialogAction::redraw);
    CHECK(
        find_part(
            settings::dialog_layout(dialog),
            "In Full the graphics card draws the view 1:1; a",
            settings::no_control
        ) != nullptr
    );
    full.full_supersample = 0;
    CHECK(settings::set_acceleration_status(dialog, full) == DialogAction::redraw);
    CHECK(
        find_part(
            settings::dialog_layout(dialog),
            "Units drawn at 16x and scaled down. Needs a fast",
            settings::no_control
        ) != nullptr
    );

    // Keys step along the strip and stop at its ends.
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_row_control + 1);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::redraw);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(dialog.chosen.anti_aliasing == settings::AntiAliasing::x8);
}

void enter_keeps_and_escape_cancels() {
    settings::Dialog dialog = opened(Page::developer);
    dialog.chosen.frame_stats = true;
    CHECK(settings::dialog_key(dialog, DialogKey::enter) == DialogAction::accepted);
    CHECK(dialog.chosen.frame_stats);
    CHECK(settings::dialog_key(dialog, DialogKey::escape) == DialogAction::cancelled);
    CHECK(dialog.chosen == dialog.opened);
}

void the_footer_buttons_restore_cancel_and_keep() {
    settings::EngineSettings defaults{};
    defaults.escape_opens_menu = true;
    settings::EngineSettings current{};
    current.wheel_zoom = false;
    current.unit_limit = 600;
    current.anti_aliasing = settings::AntiAliasing::x4;
    settings::Dialog dialog;
    settings::open_dialog(dialog, current, defaults, {}, "v0.2.0", Page::controls);
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.restored);
    CHECK(dialog.chosen == defaults);
    CHECK(dialog.forget_renderer_failures == 1);
    // Every press asks the host to act, even when no setting moves.
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.chosen == defaults);
    CHECK(dialog.forget_renderer_failures == 2);
    CHECK(click(dialog, centre(geometry::cancel_button)) == DialogAction::cancelled);
    CHECK(dialog.chosen == current);
    settings::open_dialog(dialog, current, defaults, {}, "v0.2.0", Page::controls);
    CHECK(click(dialog, centre(geometry::ok_button)) == DialogAction::accepted);
    CHECK(dialog.chosen == current);

    // Left and Right move along the footer; Space presses.
    settings::open_dialog(dialog, current, defaults, {}, "v0.2.0", Page::common_tweaks);
    for (int32_t step = 0; step < 4; ++step)
        CHECK(settings::dialog_key(dialog, DialogKey::tab) == DialogAction::redraw);
    CHECK(dialog.focused == settings::restore_control);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::none);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::redraw);
    CHECK(dialog.focused == settings::cancel_control);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::redraw);
    CHECK(dialog.focused == settings::ok_control);
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::accepted);
}

void the_focus_moves_round_every_control() {
    settings::Dialog dialog = opened(Page::controls);
    const std::vector<int32_t> expected{
        settings::first_row_control,
        settings::first_row_control + 1,
        settings::first_row_control + 2,
        settings::first_row_control + 3,
        settings::first_row_control + 4,
        settings::first_row_control + 5,
        settings::first_row_control + 6,
        settings::restore_control,
        settings::cancel_control,
        settings::ok_control,
        settings::page_control(Page::controls),
        settings::page_control(Page::common_tweaks),
        settings::page_control(Page::language),
        settings::page_control(Page::graphics),
        settings::page_control(Page::user_interface),
        settings::page_control(Page::downloads),
        settings::page_control(Page::updates),
        settings::page_control(Page::game_content),
        settings::page_control(Page::system),
        settings::page_control(Page::developer),
    };
    for (const int32_t control : expected) {
        CHECK(settings::dialog_key(dialog, DialogKey::tab) == DialogAction::redraw);
        CHECK(dialog.focused == control);
    }
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(dialog.focused == settings::restore_control); // 2D focus (D30)
    CHECK(settings::dialog_key(dialog, DialogKey::up) == DialogAction::redraw);
    CHECK(dialog.focused == expected.back());
    CHECK(settings::dialog_key(dialog, DialogKey::back_tab) == DialogAction::redraw);
    CHECK(dialog.focused == settings::page_control(Page::system));
    CHECK(settings::dialog_key(dialog, DialogKey::back_tab) == DialogAction::redraw);
    CHECK(dialog.focused == settings::page_control(Page::game_content));
    CHECK(settings::dialog_key(dialog, DialogKey::back_tab) == DialogAction::redraw);
    CHECK(dialog.focused == settings::page_control(Page::updates));
    CHECK(settings::dialog_key(dialog, DialogKey::back_tab) == DialogAction::redraw);
    CHECK(dialog.focused == settings::page_control(Page::downloads));
    CHECK(settings::dialog_key(dialog, DialogKey::back_tab) == DialogAction::redraw);
    CHECK(dialog.focused == settings::page_control(Page::user_interface));
    // The title bar's close mark is last in Tab's order.
    settings::Dialog again = opened(Page::controls);
    for (const int32_t control : expected) {
        CHECK(settings::dialog_key(again, DialogKey::tab) == DialogAction::redraw);
        CHECK(again.focused == control);
    }
    CHECK(settings::dialog_key(again, DialogKey::tab) == DialogAction::redraw);
    CHECK(again.focused == settings::close_x_control);
    // Space on an entry shows its section.
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::redraw);
    CHECK(dialog.page == Page::user_interface);
    // Up from nothing lands on the last control.
    settings::Dialog fresh = opened(Page::controls);
    CHECK(settings::dialog_key(fresh, DialogKey::up) == DialogAction::redraw);
    CHECK(fresh.focused == settings::close_x_control);
}

void locks_show_their_text_and_hold_their_settings() {
    const auto in_game = settings::settings_locks(settings::GameState{true, false, false, false});
    settings::Dialog dialog = opened(Page::common_tweaks, in_game);
    auto parts = settings::dialog_layout(dialog);
    CHECK(find_part(parts, "Locked during a game", settings::no_control) != nullptr);
    CHECK(find_part(parts, "Shared game - still running", settings::no_control) == nullptr);
    const auto track = geometry::place_rows(Page::common_tweaks, in_game).rows[1].control_area;
    CHECK(
        settings::dialog_pointer_down(dialog, track.x + track.width - 1, track.y + 4) ==
        DialogAction::none
    );
    CHECK(dialog.chosen.path_search_nodes == settings::base_path_search_nodes);
    // The three sliders are locked, so the focus passes over them to the footer.
    CHECK(settings::dialog_key(dialog, DialogKey::tab) == DialogAction::redraw);
    CHECK(dialog.focused == settings::restore_control);
    // A locked slider takes no part a press can find.
    for (const auto& part : parts)
        CHECK(
            part.control != settings::first_row_control &&
            part.control != settings::first_row_control + 1 &&
            part.control != settings::first_row_control + 2
        );

    const auto shared = settings::settings_locks(settings::GameState{true, true, false, false});
    parts = settings::dialog_layout(opened(Page::common_tweaks, shared));
    CHECK(find_part(parts, "Set by the host", settings::no_control) != nullptr);
    CHECK(find_part(parts, "Shared game - still running", settings::no_control) != nullptr);
    const auto replay = settings::settings_locks(settings::GameState{true, false, true, false});
    parts = settings::dialog_layout(opened(Page::common_tweaks, replay));
    CHECK(find_part(parts, "Set by the host", settings::no_control) != nullptr);
    CHECK(find_part(parts, "Shared game - still running", settings::no_control) == nullptr);

    const auto command_line =
        settings::settings_locks(settings::GameState{false, false, false, true});
    parts = settings::dialog_layout(opened(Page::graphics, command_line));
    CHECK(find_part(parts, "Set on the command line", settings::no_control) != nullptr);
    parts = settings::dialog_layout(opened(Page::graphics));
    CHECK(find_part(parts, "Set on the command line", settings::no_control) == nullptr);
    CHECK(find_part(parts, "Locked during a game", settings::no_control) == nullptr);

    // Restore defaults leaves what is locked as it is.
    settings::EngineSettings current{};
    current.path_search_nodes = 4 * settings::base_path_search_nodes;
    current.unit_limit = 900;
    current.skirmish_players = 6;
    current.max_frame_rate = 60;
    current.frame_stats = true;
    auto all_locked = in_game;
    all_locked.max_frame_rate = Lock::command_line;
    settings::open_dialog(dialog, current, {}, all_locked, "v0.2.0", Page::developer);
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.chosen.path_search_nodes == current.path_search_nodes);
    CHECK(dialog.chosen.unit_limit == current.unit_limit);
    CHECK(dialog.chosen.skirmish_players == current.skirmish_players);
    CHECK(dialog.chosen.max_frame_rate == current.max_frame_rate);
    CHECK(!dialog.chosen.frame_stats);
}

/// A section of the test's own, shown in Graphics' place: its rows, and the
/// locks and status hints it gives them.
struct Section {
    std::vector<Setting> rows;
    std::vector<std::pair<Setting, Lock>> locks;
    std::vector<Setting> status;
};

/// Returns the test section's rows for Graphics, and the dialog's own for
/// the other sections (SectionHooks::settings).
std::span<const Setting> section_settings(void* context, Page page) {
    const auto& section = *static_cast<const Section*>(context);
    if (page != Page::graphics)
        return settings::page_settings(page);
    return section.rows;
}

/// Returns the lock the test section gives a setting, else the dialog's
/// (SectionHooks::lock).
Lock section_lock(void* context, Setting setting, Lock lock) {
    const auto& section = *static_cast<const Section*>(context);
    for (const auto& [locked, kind] : section.locks)
        if (locked == setting)
            return kind;
    return lock;
}

/// Tells whether the test section makes a setting's hint lines its status
/// (SectionHooks::hint_is_status).
bool section_status(void* context, Setting setting) {
    const auto& section = *static_cast<const Section*>(context);
    return std::find(section.status.begin(), section.status.end(), setting) != section.status.end();
}

/// Returns five rows as tall as the Graphics page the scrolling is for: a
/// slider with one hint line (65 rows), the level strip (59), a slider with
/// two (77), a switch with two (59) and a switch with one (47).
Section five_rows() {
    return {
        {Setting::max_frame_rate,
         Setting::anti_aliasing,
         Setting::screen_size,
         Setting::escape_opens_menu,
         Setting::frame_stats},
        {},
        {},
    };
}

/// Returns nine rows in one section: 543 rows from the first row's line to
/// the line under the last.
Section nine_rows() {
    return {
        {Setting::path_search,
         Setting::wheel_zoom,
         Setting::escape_opens_menu,
         Setting::switch_alt,
         Setting::unit_limit,
         Setting::max_frame_rate,
         Setting::anti_aliasing,
         Setting::screen_size,
         Setting::frame_stats},
        {},
        {},
    };
}

/// A dialog open on Graphics with a section of the test's own in its place.
struct Scrolling {
    Section section;
    settings::SectionHooks hooks{};
    settings::Dialog dialog;

    /// Opens the dialog on Graphics with a section of the test's own in its place.
    explicit Scrolling(
        Section shown,
        const settings::EngineSettings& current = {},
        const settings::Locks& locks = {}
    )
        : section(std::move(shown)) {
        hooks.context = &section;
        hooks.settings = section_settings;
        hooks.lock = section_lock;
        hooks.hint_is_status = section_status;
        open(current, locks);
    }

    Scrolling(const Scrolling&) = delete;
    Scrolling& operator=(const Scrolling&) = delete;

    /// Opens the dialog again on Graphics, with the test's section in its place.
    void open(const settings::EngineSettings& current = {}, const settings::Locks& locks = {}) {
        settings::open_dialog(dialog, current, {}, locks, "v0.2.0", Page::graphics);
        dialog.section_hooks = &hooks;
    }

    /// Returns Graphics' stored offset.
    int32_t scroll() const { return dialog.scroll[static_cast<std::size_t>(Page::graphics)]; }

    /// Returns the open section's rows, placed at its offset.
    geometry::ScrolledRows rows() const { return geometry::open_rows(dialog); }
};

/// Tells whether two rectangles are the same.
bool same_rect(const renderer::SourceRect& a, const renderer::SourceRect& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

/// Turns the wheel at a point of the section.
DialogAction wheel(settings::Dialog& dialog, float notches, Point at = {300, 150}) {
    return settings::dialog_wheel(dialog, at.x, at.y, notches);
}

void the_view_and_the_scroll_bar_keep_their_places() {
    CHECK(same_rect(geometry::view, {158, 54, 309, 236}));
    CHECK(same_rect(geometry::view_clip, {156, 54, 313, 236}));
    CHECK(same_rect(geometry::scroll_well, {470, 54, 7, 236}));
    CHECK(same_rect(geometry::scroll_hit, {467, 54, 12, 236}));
    CHECK(geometry::wheel_step == 24);
    CHECK(geometry::page_step == 200);
    CHECK(geometry::least_thumb_height == 16);
    CHECK(geometry::end_gap == 8);
    // The view runs from the first row's line to the row above the footer's.
    CHECK(geometry::view.y == geometry::first_row_top);
    CHECK(geometry::view.y + geometry::view.height == geometry::footer_rule_row);
    // Columns 477 and 478 stay clear before the dark edge; the hit area ends there.
    CHECK(geometry::scroll_well.x + geometry::scroll_well.width == 477);
    CHECK(geometry::scroll_hit.x + geometry::scroll_hit.width == settings::dialog_width - 1);
}

void sections_that_fit_do_not_scroll() {
    // Each section's content: its rows, and the end gap under the last.
    // Controls' three switches, the zoom's two drop-downs, View past the
    // map's edge's strip, Window edge's strip and Edge stickiness's slider
    // are 491. Engine Limits' three sliders, the mod's heading and its two
    // rows are 369. Graphics' twenty-one rows are 1208, Language's
    // drop-down, five switches and slider 424, and User Interface's four
    // strips and switches with Interface size's row are 324. All of them
    // scroll. Developer's list scrolls in a view of its own (developer_*).
    const std::array<Page, 5> pages{
        Page::controls, Page::common_tweaks, Page::graphics, Page::language, Page::user_interface
    };
    const std::array<int32_t, 5> content{491, 369, 1208, 424, 324};
    const std::array<int32_t, 5> limits{255, 133, 972, 188, 88};
    for (std::size_t index = 0; index < content.size(); ++index) {
        const Page page = pages[index];
        if (limits[index] != 0) {
            for (const auto& locks : lock_states()) {
                const auto at_top = geometry::place_rows(page, locks);
                // Hardware acceleration's lock drops under its label when
                // the label, the lock and the strip do not fit, so Graphics
                // grows by that line while the row is locked.
                const int32_t grown =
                    page == Page::graphics && locks.hardware_acceleration != Lock::none ? 18 : 0;
                const int32_t height = content[index] + grown;
                CHECK(geometry::content_height(at_top, 0) == height);
                CHECK(geometry::scroll_limit(height) == limits[index] + grown);
            }
            continue;
        }
        for (const auto& locks : lock_states()) {
            settings::Dialog dialog = opened(page, locks);
            const auto at_top = geometry::place_rows(page, locks);
            CHECK(geometry::content_height(at_top, 0) == content[index]);
            CHECK(geometry::scroll_limit(content[index]) == 0);
            // A stored offset is clamped to the limit: the rows stay put.
            dialog.scroll[static_cast<std::size_t>(page)] = 100;
            const auto open = geometry::open_rows(dialog);
            CHECK(open.scroll == 0 && open.limit == 0);
            CHECK(open.rows.rows.size() == at_top.rows.size());
            CHECK(open.rows.bottom == at_top.bottom);
            for (std::size_t row = 0; row < at_top.rows.size(); ++row) {
                CHECK(open.rows.rows[row].top == at_top.rows[row].top);
                CHECK(same_rect(open.rows.rows[row].control_area, at_top.rows[row].control_area));
                CHECK(same_rect(open.rows.rows[row].label, at_top.rows[row].label));
            }
            for (const auto& part : settings::dialog_layout(dialog))
                CHECK(part.control != settings::scroll_bar_control);
            // Neither the wheel, the keys nor a press in the margin scroll it.
            CHECK(wheel(dialog, -1.0F) == DialogAction::none);
            for (const DialogKey key :
                 {DialogKey::page_down, DialogKey::end, DialogKey::page_up, DialogKey::home})
                CHECK(settings::dialog_key(dialog, key) == DialogAction::none);
            CHECK(dialog.focused == settings::no_control);
            CHECK(settings::dialog_pointer_down(dialog, 473, 150) == DialogAction::none);
            CHECK(settings::dialog_pointer_up(dialog, 473, 150) == DialogAction::none);
            CHECK(dialog.hovered == settings::no_control);
            CHECK(geometry::open_rows(dialog).scroll == 0);
        }
    }
}

/// The part whose rectangle is a row's value, when the layout lists it.
const settings::LayoutPart*
value_part(const std::vector<settings::LayoutPart>& parts, const renderer::SourceRect& value) {
    for (const settings::LayoutPart& part : parts)
        if (same_rect(part.rect, value))
            return &part;
    return nullptr;
}

void the_mod_limits_rows_show_the_counts_and_take_no_input() {
    const auto at_end = [](settings::Dialog& dialog) {
        dialog.scroll[static_cast<std::size_t>(Page::common_tweaks)] =
            settings::scroll_limit(dialog);
    };
    settings::Dialog dialog = opened(Page::common_tweaks);
    at_end(dialog);
    auto parts = settings::dialog_layout(dialog);
    CHECK(find_part(parts, "Set by the mod", settings::no_control) != nullptr);
    CHECK(find_part(parts, "Weapon types", settings::no_control) != nullptr);
    CHECK(find_part(parts, "Sides", settings::no_control) != nullptr);
    CHECK(shows_wrapped(parts, "Set by the mod. Every player in a game has the same.", dialog));
    auto open = geometry::open_rows(dialog);
    const settings::LayoutPart* weapons = value_part(parts, open.rows.rows[4].value);
    const settings::LayoutPart* sides = value_part(parts, open.rows.rows[5].value);
    CHECK(weapons != nullptr && weapons->text == "256");
    CHECK(sides != nullptr && sides->text == "5");
    CHECK(find_part(parts, "256 of 256", settings::no_control) == nullptr);
    CHECK(find_part(parts, "5 of 5", settings::no_control) == nullptr);

    dialog.mod_limits = {true, 1024, 10, std::nullopt, std::nullopt};
    at_end(dialog);
    parts = settings::dialog_layout(dialog);
    open = geometry::open_rows(dialog);
    weapons = value_part(parts, open.rows.rows[4].value);
    sides = value_part(parts, open.rows.rows[5].value);
    CHECK(weapons != nullptr && weapons->text == "1024");
    CHECK(sides != nullptr && sides->text == "10");

    dialog.mod_limits = {true, 1024, 10, 312, 7};
    at_end(dialog);
    parts = settings::dialog_layout(dialog);
    open = geometry::open_rows(dialog);
    weapons = value_part(parts, open.rows.rows[4].value);
    sides = value_part(parts, open.rows.rows[5].value);
    CHECK(weapons != nullptr && weapons->text == "312 of 1024");
    CHECK(sides != nullptr && sides->text == "7 of 10");

    const oa::ui::kit::DisplayList list = geometry::dialog_list(dialog, nullptr);
    for (int32_t index = 3; index <= 5; ++index) {
        CHECK(kit::control_of(list, settings::first_row_control + index) == nullptr);
        CHECK(
            std::find(
                list.tab_order.begin(), list.tab_order.end(), settings::first_row_control + index
            ) == list.tab_order.end()
        );
    }
    settings::Dialog keys = opened(Page::common_tweaks);
    for (int32_t step = 0; step < 3; ++step)
        CHECK(settings::dialog_key(keys, DialogKey::tab) == DialogAction::redraw);
    CHECK(keys.focused == settings::first_row_control + 2);
    CHECK(settings::dialog_key(keys, DialogKey::tab) == DialogAction::redraw);
    CHECK(keys.focused == settings::restore_control);

    const settings::EngineSettings chosen = dialog.chosen;
    const settings::ModLimitsShown limits = dialog.mod_limits;
    const Page page = dialog.page;
    const int32_t scroll = dialog.scroll[static_cast<std::size_t>(Page::common_tweaks)];
    for (const std::string_view text :
         {"Set by the mod", "Weapon types", "312 of 1024", "Sides", "7 of 10"}) {
        const settings::LayoutPart* part = find_part(parts, text, settings::no_control);
        CHECK(part != nullptr);
        CHECK(click(dialog, centre(part->rect)) == DialogAction::none);
    }
    CHECK(dialog.chosen == chosen);
    CHECK(dialog.page == page);
    CHECK(dialog.scroll[static_cast<std::size_t>(Page::common_tweaks)] == scroll);
    CHECK(dialog.mod_limits.mod == limits.mod);
    CHECK(dialog.mod_limits.weapon_types == limits.weapon_types);
    CHECK(dialog.mod_limits.sides == limits.sides);
    CHECK(dialog.mod_limits.loaded_weapon_types == limits.loaded_weapon_types);
    CHECK(dialog.mod_limits.loaded_sides == limits.loaded_sides);
    CHECK(dialog.focused == settings::no_control);
}

void a_long_section_scrolls_by_its_overflow() {
    Scrolling five(five_rows());
    const auto open = five.rows();
    CHECK(open.rows.rows.size() == 5);
    const std::array<int32_t, 5> tops{54, 119, 178, 255, 314};
    const std::array<int32_t, 5> heights{65, 59, 77, 59, 47};
    for (std::size_t index = 0; index < open.rows.rows.size() && index < tops.size(); ++index) {
        const auto& row = open.rows.rows[index];
        CHECK(row.top == tops[index]);
        CHECK(row.height == heights[index]);
        CHECK(row.control == settings::first_row_control + static_cast<int32_t>(index));
        // No row is taller than the view, so the focus can always show one whole.
        CHECK(row.height < geometry::view.height);
    }
    CHECK(open.rows.bottom == 361);
    CHECK(open.content_height == 316);
    CHECK(open.limit == 80);
    CHECK(open.scroll == 0);

    // At its end every row is 80 rows higher, the closing line at 281.
    five.dialog.scroll[static_cast<std::size_t>(Page::graphics)] = 500;
    const auto end = five.rows();
    CHECK(end.scroll == 80);
    CHECK(end.rows.bottom == 281);
    CHECK(end.rows.rows[0].top == -26);
    CHECK(end.rows.rows[1].control_area.y == 48);
    CHECK(end.rows.rows[4].top == 234);
    // Every part a row has moves with it; a part it lacks stays empty.
    const auto lifted = [](const renderer::SourceRect& at_end, const renderer::SourceRect& at_top) {
        if (at_top.width == 0 || at_top.height == 0)
            return same_rect(at_end, at_top);
        return same_rect(at_end, {at_top.x, at_top.y - 80, at_top.width, at_top.height});
    };
    for (std::size_t index = 0; index < open.rows.rows.size(); ++index) {
        const auto& top_row = open.rows.rows[index];
        const auto& end_row = end.rows.rows[index];
        CHECK(lifted(end_row.label, top_row.label));
        CHECK(lifted(end_row.lock_area, top_row.lock_area));
        for (std::size_t line = 0; line < top_row.hints.size(); ++line)
            CHECK(lifted(end_row.hints[line], top_row.hints[line]));
        CHECK(lifted(end_row.control_area, top_row.control_area));
        CHECK(lifted(end_row.value, top_row.value));
    }

    // The thumb: 174 rows of the well's 234, its top 55 at the top, 85
    // half-way and 115 at the end; a dragged top gives the offset back.
    CHECK(same_rect(geometry::scroll_thumb(0, 80, 316), {471, 55, 5, 174}));
    CHECK(geometry::scroll_thumb(40, 80, 316).y == 85);
    CHECK(geometry::scroll_thumb(80, 80, 316).y == 115);
    CHECK(geometry::scroll_at(55, 80, 316) == 0);
    CHECK(geometry::scroll_at(85, 80, 316) == 40);
    CHECK(geometry::scroll_at(115, 80, 316) == 80);
    CHECK(geometry::scroll_at(10, 80, 316) == 0);
    CHECK(geometry::scroll_at(400, 80, 316) == 80);
    // To the nearest row: two rows down the thumb's 60 is 2.67 of the 80.
    CHECK(geometry::scroll_at(57, 80, 316) == 3);
    CHECK(geometry::scroll_at(56, 80, 316) == 1);
    for (int32_t scroll = 0; scroll <= 80; ++scroll) {
        const auto thumb = geometry::scroll_thumb(scroll, 80, 316);
        CHECK(thumb.y >= 55 && thumb.y + thumb.height <= 289);
        // The offset a thumb's top gives puts the thumb back there.
        CHECK(geometry::scroll_thumb(geometry::scroll_at(thumb.y, 80, 316), 80, 316).y == thumb.y);
    }

    // Nine rows: 543 rows, a limit of 316 and a thumb of 100.
    Scrolling all(nine_rows());
    CHECK(all.rows().content_height == 552);
    CHECK(all.rows().limit == 316);
    CHECK(geometry::scroll_thumb(0, 316, 552).height == 100);
    // A section far taller than the view keeps the thumb's least height.
    CHECK(geometry::scroll_thumb(0, 10000, 10236).height == geometry::least_thumb_height);
}

void control_numbers_put_the_rows_after_every_fixed_control() {
    // Each entry is its place in the list with Touch and Controller,
    // whether or not the dialog lists them: the five sections before Touch
    // are 0 to 4, Touch 5 and Controller 6. Developer is 7, Game files 8,
    // Downloads 9, System 10, Game Content 11 and Updates 12.
    for (std::size_t index = 0; index + 5 < kTouchControllerPages.size(); ++index)
        CHECK(settings::page_control(kTouchControllerPages[index]) == static_cast<int32_t>(index));
    CHECK(settings::page_control(Page::touch) == 5);
    CHECK(settings::page_control(Page::controller) == 6);
    CHECK(settings::page_control(Page::developer) == 7);
    CHECK(settings::page_control(Page::game_files) == 8);
    CHECK(settings::page_control(Page::downloads) == 9);
    CHECK(settings::page_control(Page::system) == 10);
    CHECK(settings::page_control(Page::game_content) == 11);
    CHECK(settings::page_control(Page::updates) == 12);
    CHECK(settings::most_listed_pages == 13);
    CHECK(settings::page_count == 19);
    CHECK(settings::restore_control == 13);
    CHECK(settings::cancel_control == 14);
    CHECK(settings::ok_control == 15);
    CHECK(settings::scroll_bar_control == 16);
    CHECK(settings::first_row_control == 17);
    // Developer's three rows come first, Enable Developer Mode, Show
    // performance statistics and UI kit gallery, then its footer's switch
    // and button, then its list's rows.
    CHECK(settings::developer_mode_control == 17);
    CHECK(settings::active_only_control == 20);
    CHECK(settings::restore_profile_control == 21);
    CHECK(settings::first_hack_list_control == 22);

    // Every row's number comes after every fixed control's, and each is its own.
    Scrolling all(nine_rows());
    std::set<int32_t> rows;
    for (const auto& row : all.rows().rows.rows) {
        CHECK(row.control > settings::scroll_bar_control);
        CHECK(rows.insert(row.control).second);
    }
    CHECK(rows.size() == 9);
    CHECK(*rows.rbegin() == settings::first_row_control + 8);
    // The focus moves through the rows, the footer's buttons and the
    // sections' entries, never the scroll bar.
    std::vector<int32_t> expected;
    for (int32_t row = 0; row < 9; ++row)
        expected.push_back(settings::first_row_control + row);
    for (const int32_t control :
         {settings::restore_control, settings::cancel_control, settings::ok_control})
        expected.push_back(control);
    for (const Page page : kPages)
        expected.push_back(settings::page_control(page));
    expected.push_back(settings::close_x_control);
    for (const int32_t control : expected) {
        CHECK(settings::dialog_key(all.dialog, DialogKey::tab) == DialogAction::redraw);
        CHECK(all.dialog.focused == control);
    }
    CHECK(settings::dialog_key(all.dialog, DialogKey::tab) == DialogAction::redraw);
    CHECK(all.dialog.focused == settings::first_row_control);
    // Space on the last row of nine flips its switch: it is a row, not a button.
    settings::Dialog& dialog = all.dialog;
    for (int32_t press = 0; press < 8; ++press)
        static_cast<void>(settings::dialog_key(dialog, DialogKey::down));
    CHECK(dialog.focused == settings::first_row_control + 8);
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::changed);
    CHECK(dialog.chosen.frame_stats);
}

/// Checks a dialog open on Graphics at one offset: every part inside the
/// dialog and apart, every part over the view wholly in it, the scroll bar
/// once and right of every focus outline, and every listed control pressed
/// where it is drawn.
void check_dialog_layout_at(settings::Dialog& dialog, int32_t scroll) {
    const renderer::SourceRect face{
        geometry::edge,
        geometry::edge,
        settings::dialog_width - 2 * geometry::edge,
        settings::dialog_height - 2 * geometry::edge,
    };
    dialog.scroll[static_cast<std::size_t>(Page::graphics)] = scroll;
    const auto parts = settings::dialog_layout(dialog);
    int32_t bars = 0;
    for (std::size_t a = 0; a < parts.size(); ++a) {
        const auto& part = parts[a];
        CHECK(part.rect.width > 0 && part.rect.height > 0);
        CHECK(inside(part.rect, face));
        if (overlap(part.rect, geometry::view) && !inside(part.rect, geometry::view)) {
            std::cerr << "at " << scroll << ": '" << part.text << "' is cut by the view\n";
            CHECK(inside(part.rect, geometry::view));
        }
        for (std::size_t b = a + 1; b < parts.size(); ++b)
            CHECK(!overlap(part.rect, parts[b].rect));
        if (part.control == settings::scroll_bar_control) {
            ++bars;
            CHECK(same_rect(part.rect, geometry::scroll_well));
        }
    }
    CHECK(bars == 1);
    for (const auto& row : geometry::open_rows(dialog).rows.rows)
        CHECK(
            row.control_area.x + row.control_area.width + geometry::focus_inset <
            geometry::scroll_well.x
        );
    for (const auto& part : parts) {
        if (part.control == settings::no_control)
            continue;
        const Point point = centre(part.rect);
        static_cast<void>(settings::dialog_pointer_move(dialog, point.x, point.y));
        CHECK(dialog.hovered == part.control);
    }
    CHECK(dialog.scroll[static_cast<std::size_t>(Page::graphics)] == scroll);
}

/// Checks the layout of a section of the test's own at one offset
/// (check_dialog_layout_at).
void check_layout_at(Scrolling& scrolling, int32_t scroll) {
    check_dialog_layout_at(scrolling.dialog, scroll);
}

void the_layout_lists_the_parts_wholly_in_the_view() {
    for (Section section : {five_rows(), nine_rows()}) {
        Scrolling scrolling(std::move(section));
        const int32_t limit = scrolling.rows().limit;
        std::map<std::string, renderer::SourceRect> seen;
        for (int32_t scroll = 0; scroll <= limit; ++scroll) {
            check_layout_at(scrolling, scroll);
            for (const auto& part : settings::dialog_layout(scrolling.dialog))
                seen.emplace(part.text, part.rect);
        }
        // Every row's label is listed, and each whole hint is listed as the
        // lines it wraps to, each of those lines fitting the content column.
        const geometry::Sizes& sizes = geometry::sizes_of(scrolling.dialog);
        const int32_t room = hint_room(scrolling.dialog);
        for (const Setting setting : scrolling.section.rows) {
            CHECK(seen.contains(std::string(geometry::label_of(setting))));
            for (std::size_t line = 0; line < geometry::hint_line_count(setting); ++line) {
                const std::string source = geometry::hint_line(
                    setting, scrolling.dialog.chosen, scrolling.dialog.acceleration, line
                );
                if (source.empty())
                    continue;
                const std::vector<std::string> lines = wrapped_hint(source, scrolling.dialog);
                std::string joined;
                for (const std::string& wrapped_line : lines) {
                    if (!joined.empty())
                        joined.push_back(' ');
                    joined += wrapped_line;
                    const auto found = seen.find(wrapped_line);
                    CHECK(found != seen.end());
                    if (found == seen.end())
                        continue;
                    const int32_t estimated = geometry::estimated_width(wrapped_line);
                    CHECK(estimated <= room);
                    CHECK(found->second.width >= std::min(estimated, sizes.content_width));
                    CHECK(
                        found->second.x >= sizes.content_left &&
                        found->second.x + found->second.width <= sizes.content_right
                    );
                }
                CHECK(joined == source);
            }
        }
    }
}

void the_wheel_scrolls_by_notches_and_carries_fractions() {
    Scrolling five(five_rows());
    // Towards the player scrolls down, 24 rows a notch, to the end in four.
    for (const int32_t expected : {24, 48, 72, 80}) {
        CHECK(wheel(five.dialog, -1.0F) == DialogAction::redraw);
        CHECK(five.scroll() == expected);
    }
    CHECK(wheel(five.dialog, -1.0F) == DialogAction::none);
    CHECK(five.scroll() == 80);
    CHECK(five.dialog.wheel_rows == 0.0F);
    // Away from the player scrolls up, and stops at the top.
    for (const int32_t expected : {56, 32, 8, 0}) {
        CHECK(wheel(five.dialog, 1.0F) == DialogAction::redraw);
        CHECK(five.scroll() == expected);
    }
    CHECK(wheel(five.dialog, 2.0F) == DialogAction::none);
    CHECK(five.scroll() == 0);
    // Fractions of a notch carry over until they make whole rows: 0.75 rows
    // a turn.
    const float fine = -1.0F / 32.0F;
    CHECK(wheel(five.dialog, fine) == DialogAction::none);
    CHECK(five.scroll() == 0 && five.dialog.wheel_rows == 0.75F);
    CHECK(wheel(five.dialog, fine) == DialogAction::redraw);
    CHECK(five.scroll() == 1 && five.dialog.wheel_rows == 0.5F);
    CHECK(wheel(five.dialog, fine) == DialogAction::redraw);
    CHECK(five.scroll() == 2 && five.dialog.wheel_rows == 0.25F);
    CHECK(wheel(five.dialog, fine) == DialogAction::redraw);
    CHECK(five.scroll() == 3 && five.dialog.wheel_rows == 0.0F);
    // A turn far larger than the section reaches its end, and the carry
    // towards that end is dropped there.
    CHECK(wheel(five.dialog, -1.0e9F) == DialogAction::redraw);
    CHECK(five.scroll() == 80 && five.dialog.wheel_rows == 0.0F);
    CHECK(wheel(five.dialog, fine) == DialogAction::none);
    CHECK(five.dialog.wheel_rows == 0.0F);
    // A carry is dropped when another section shows.
    CHECK(wheel(five.dialog, 1.0F / 32.0F) == DialogAction::none);
    CHECK(five.dialog.wheel_rows == -0.75F);
    CHECK(click(five.dialog, centre(geometry::list_item(Page::controls))) == DialogAction::redraw);
    CHECK(five.dialog.wheel_rows == 0.0F);
    CHECK(click(five.dialog, centre(geometry::list_item(Page::graphics))) == DialogAction::redraw);
    CHECK(five.scroll() == 80);

    // Anywhere over the dialog, the section list and the footer included;
    // nowhere outside it.
    CHECK(wheel(five.dialog, 1.0F, {20, 100}) == DialogAction::redraw);
    CHECK(wheel(five.dialog, 1.0F, {300, 310}) == DialogAction::redraw);
    CHECK(five.scroll() == 32);
    for (const Point outside :
         {Point{-1, 100},
          Point{settings::dialog_width, 100},
          Point{300, -1},
          Point{300, settings::dialog_height}})
        CHECK(wheel(five.dialog, 1.0F, outside) == DialogAction::none);
    CHECK(five.scroll() == 32);
    // Not a number, nor an endless turn, scrolls.
    CHECK(
        settings::dialog_wheel(five.dialog, 300, 150, std::numeric_limits<float>::quiet_NaN()) ==
        DialogAction::none
    );
    CHECK(
        settings::dialog_wheel(five.dialog, 300, 150, std::numeric_limits<float>::infinity()) ==
        DialogAction::none
    );
    CHECK(five.scroll() == 32);

    // Nothing moves while a press is held: on a footer button or a slider.
    const Point restore = centre(geometry::restore_button);
    CHECK(settings::dialog_pointer_down(five.dialog, restore.x, restore.y) == DialogAction::redraw);
    CHECK(wheel(five.dialog, -1.0F) == DialogAction::none);
    static_cast<void>(settings::dialog_pointer_up(five.dialog, 0, 0));
    const auto track = five.rows().rows.rows[2].control_area;
    const Point last_stop{track.x + track.width - 1, track.y + 4};
    CHECK(
        settings::dialog_pointer_down(five.dialog, last_stop.x, last_stop.y) ==
        DialogAction::changed
    );
    CHECK(five.dialog.dragging);
    CHECK(wheel(five.dialog, -1.0F) == DialogAction::none);
    static_cast<void>(settings::dialog_pointer_up(five.dialog, last_stop.x, last_stop.y));
    CHECK(five.scroll() == 32);
    // A scroll changes no setting.
    const auto chosen = five.dialog.chosen;
    CHECK(wheel(five.dialog, -1.0F) == DialogAction::redraw);
    CHECK(five.dialog.chosen == chosen);
}

void the_scroll_keys_scroll_whatever_has_the_focus() {
    Scrolling all(nine_rows());
    // Page Down and Page Up by 200 rows, stopping at the ends; End and Home.
    for (const auto& [key, expected] :
         {std::pair{DialogKey::page_down, 200},
          std::pair{DialogKey::page_down, 316},
          std::pair{DialogKey::page_up, 116},
          std::pair{DialogKey::page_up, 0},
          std::pair{DialogKey::end, 316},
          std::pair{DialogKey::home, 0}}) {
        CHECK(settings::dialog_key(all.dialog, key) == DialogAction::redraw);
        CHECK(all.scroll() == expected);
    }
    CHECK(settings::dialog_key(all.dialog, DialogKey::home) == DialogAction::none);
    CHECK(settings::dialog_key(all.dialog, DialogKey::page_up) == DialogAction::none);
    // They never show the focus, nor change a setting.
    CHECK(all.dialog.focused == settings::no_control);
    CHECK(all.dialog.chosen == settings::EngineSettings{});

    // With the focus on the unit limit's slider, Home and End scroll and
    // leave the slider and the focus alone.
    for (int32_t press = 0; press < 5; ++press)
        static_cast<void>(settings::dialog_key(all.dialog, DialogKey::tab));
    CHECK(all.dialog.focused == settings::first_row_control + 4);
    const auto chosen = all.dialog.chosen;
    CHECK(settings::dialog_key(all.dialog, DialogKey::end) == DialogAction::redraw);
    CHECK(all.scroll() == 316);
    CHECK(settings::dialog_key(all.dialog, DialogKey::home) == DialogAction::redraw);
    CHECK(all.scroll() == 0);
    CHECK(all.dialog.focused == settings::first_row_control + 4);
    CHECK(all.dialog.chosen == chosen);

    // One Page Down reaches the end of five rows.
    Scrolling five(five_rows());
    CHECK(settings::dialog_key(five.dialog, DialogKey::page_down) == DialogAction::redraw);
    CHECK(five.scroll() == 80);
    // Nothing moves while a press is held.
    const Point ok = centre(geometry::ok_button);
    static_cast<void>(settings::dialog_pointer_down(five.dialog, ok.x, ok.y));
    CHECK(settings::dialog_key(five.dialog, DialogKey::home) == DialogAction::none);
    CHECK(five.scroll() == 80);
}

void the_focus_scrolls_its_row_into_view() {
    Scrolling five(five_rows());
    // Tab from no focus: the first three rows at 0, the fourth at 25 (its
    // lower line at 289), the last at the end.
    const std::array<std::pair<int32_t, int32_t>, 6> forward{{
        {settings::first_row_control, 0},
        {settings::first_row_control + 1, 0},
        {settings::first_row_control + 2, 0},
        {settings::first_row_control + 3, 25},
        {settings::first_row_control + 4, 80},
        {settings::restore_control, 80},
    }};
    for (const auto& [control, expected] : forward) {
        CHECK(settings::dialog_key(five.dialog, DialogKey::tab) == DialogAction::redraw);
        CHECK(five.dialog.focused == control);
        CHECK(five.scroll() == expected);
    }
    // Back up: the third row shows whole at 80, the second scrolls to 65 and
    // the first to 0. A button or an entry never scrolls.
    const std::array<std::pair<int32_t, int32_t>, 5> back{{
        {settings::first_row_control + 4, 80},
        {settings::first_row_control + 3, 80},
        {settings::first_row_control + 2, 80},
        {settings::first_row_control + 1, 65},
        {settings::first_row_control, 0},
    }};
    for (const auto& [control, expected] : back) {
        CHECK(settings::dialog_key(five.dialog, DialogKey::back_tab) == DialogAction::redraw);
        CHECK(five.dialog.focused == control);
        CHECK(five.scroll() == expected);
    }
    CHECK(settings::dialog_key(five.dialog, DialogKey::up) == DialogAction::redraw);
    CHECK(five.dialog.focused == settings::close_x_control); // the title bar, above the row
    CHECK(five.scroll() == 0);

    // Scrolling never moves the focus; a key that acts on the focused row
    // brings it back into view first.
    five.open();
    for (int32_t press = 0; press < 5; ++press)
        static_cast<void>(settings::dialog_key(five.dialog, DialogKey::down));
    CHECK(five.dialog.focused == settings::first_row_control + 4);
    CHECK(settings::dialog_key(five.dialog, DialogKey::home) == DialogAction::redraw);
    CHECK(five.dialog.focused == settings::first_row_control + 4);
    CHECK(five.scroll() == 0);
    CHECK(settings::dialog_key(five.dialog, DialogKey::right) == DialogAction::changed);
    CHECK(five.dialog.chosen.frame_stats);
    CHECK(five.scroll() == 80);
    // A key that only brings the row back, changing nothing, still redraws.
    CHECK(settings::dialog_key(five.dialog, DialogKey::home) == DialogAction::redraw);
    CHECK(settings::dialog_key(five.dialog, DialogKey::right) == DialogAction::redraw);
    CHECK(five.scroll() == 80);
    // Space on a slider does nothing but bring it into view.
    for (int32_t press = 0; press < 4; ++press)
        static_cast<void>(settings::dialog_key(five.dialog, DialogKey::up));
    CHECK(five.dialog.focused == settings::first_row_control);
    CHECK(five.scroll() == 0);
    CHECK(settings::dialog_key(five.dialog, DialogKey::end) == DialogAction::redraw);
    CHECK(settings::dialog_key(five.dialog, DialogKey::space) == DialogAction::redraw);
    CHECK(five.scroll() == 0);
    CHECK(settings::dialog_key(five.dialog, DialogKey::space) == DialogAction::none);
}

void the_scroll_bar_follows_a_drag() {
    Scrolling five(five_rows());
    // The pointer over the bar hovers it.
    CHECK(settings::dialog_pointer_move(five.dialog, 473, 150) == DialogAction::redraw);
    CHECK(five.dialog.hovered == settings::scroll_bar_control);
    CHECK(settings::dialog_pointer_move(five.dialog, 467, 289) == DialogAction::none);
    CHECK(settings::dialog_pointer_move(five.dialog, 478, 54) == DialogAction::none);
    // A press on the thumb grabs it where it is pressed and moves nothing.
    CHECK(settings::dialog_pointer_down(five.dialog, 473, 100) == DialogAction::redraw);
    CHECK(five.dialog.pressed == settings::scroll_bar_control && five.dialog.dragging);
    CHECK(five.dialog.scroll_grab == 45);
    CHECK(five.scroll() == 0);
    // The thumb follows the pointer's row only, wherever the pointer goes,
    // and the offset follows the thumb to the nearest row.
    CHECK(settings::dialog_pointer_move(five.dialog, 300, 102) == DialogAction::redraw);
    CHECK(five.scroll() == 3);
    CHECK(settings::dialog_pointer_move(five.dialog, 300, 130) == DialogAction::redraw);
    CHECK(five.scroll() == 40);
    CHECK(settings::dialog_pointer_move(five.dialog, 10, 130) == DialogAction::none);
    CHECK(settings::dialog_pointer_move(five.dialog, 10, 400) == DialogAction::redraw);
    CHECK(five.scroll() == 80);
    CHECK(settings::dialog_pointer_move(five.dialog, 600, -100) == DialogAction::redraw);
    CHECK(five.scroll() == 0);
    CHECK(five.dialog.chosen == settings::EngineSettings{});
    // While it is held the wheel and the scroll keys do nothing.
    CHECK(wheel(five.dialog, -1.0F) == DialogAction::none);
    CHECK(settings::dialog_key(five.dialog, DialogKey::end) == DialogAction::none);
    CHECK(five.scroll() == 0);
    // The release ends the drag, wherever it happens, and acts on nothing.
    CHECK(settings::dialog_pointer_up(five.dialog, 10, 10) == DialogAction::redraw);
    CHECK(!five.dialog.dragging && five.dialog.pressed == settings::no_control);
    CHECK(settings::dialog_pointer_move(five.dialog, 10, 400) == DialogAction::none);
    CHECK(five.scroll() == 0);

    // A press on the well below the thumb brings the thumb's middle to it,
    // and above it likewise; the drag starts there.
    CHECK(settings::dialog_pointer_down(five.dialog, 473, 260) == DialogAction::redraw);
    CHECK(five.scroll() == 80);
    CHECK(five.dialog.scroll_grab == 87);
    static_cast<void>(settings::dialog_pointer_up(five.dialog, 473, 260));
    CHECK(settings::dialog_pointer_down(five.dialog, 473, 70) == DialogAction::redraw);
    CHECK(five.scroll() == 0);
    CHECK(settings::dialog_pointer_move(five.dialog, 473, 172) == DialogAction::redraw);
    CHECK(five.scroll() == 40);
    static_cast<void>(settings::dialog_pointer_up(five.dialog, 473, 172));

    // A press on the bar leaves the keyboard focus where it was, and the
    // next Tab moves on from there: a press on the thumb,
    five.open();
    CHECK(settings::dialog_key(five.dialog, DialogKey::tab) == DialogAction::redraw);
    CHECK(five.dialog.focused == settings::first_row_control);
    static_cast<void>(settings::dialog_pointer_down(five.dialog, 473, 100));
    CHECK(five.dialog.focused == settings::first_row_control);
    static_cast<void>(settings::dialog_pointer_up(five.dialog, 473, 100));
    CHECK(five.dialog.focused == settings::first_row_control);
    CHECK(settings::dialog_key(five.dialog, DialogKey::tab) == DialogAction::redraw);
    CHECK(five.dialog.focused == settings::first_row_control + 1);
    // and a press on the well below it, which scrolls to the end.
    five.open();
    for (int32_t press = 0; press < 3; ++press)
        static_cast<void>(settings::dialog_key(five.dialog, DialogKey::tab));
    CHECK(five.dialog.focused == settings::first_row_control + 2);
    CHECK(five.scroll() == 0);
    CHECK(settings::dialog_pointer_down(five.dialog, 473, 260) == DialogAction::redraw);
    CHECK(five.scroll() == 80);
    CHECK(five.dialog.focused == settings::first_row_control + 2);
    static_cast<void>(settings::dialog_pointer_up(five.dialog, 473, 260));
    CHECK(five.dialog.focused == settings::first_row_control + 2);
    CHECK(settings::dialog_key(five.dialog, DialogKey::tab) == DialogAction::redraw);
    CHECK(five.dialog.focused == settings::first_row_control + 3);
    CHECK(five.scroll() == 80);
}

void a_cut_row_answers_only_where_it_shows() {
    Scrolling five(five_rows());
    // At 40 the last row's switch shows its top seven rows, 283 to 289: a
    // press there acts, a press under the view does not.
    five.dialog.scroll[static_cast<std::size_t>(Page::graphics)] = 40;
    const auto stats = five.rows().rows.rows[4].control_area;
    CHECK(stats.y == 283);
    const int32_t on = stats.x + stats.width - 4;
    CHECK(click(five.dialog, {on, 292}) == DialogAction::none);
    CHECK(!five.dialog.chosen.frame_stats);
    CHECK(click(five.dialog, {on, 286}) == DialogAction::changed);
    CHECK(five.dialog.chosen.frame_stats);
    // Its parts are drawn but not listed.
    for (const auto& part : settings::dialog_layout(five.dialog))
        CHECK(part.control != settings::first_row_control + 4);

    // At 50 the first row's slider line shows from 54: a press on its
    // visible part drags it, a press above the view does not.
    five.dialog.scroll[static_cast<std::size_t>(Page::graphics)] = 50;
    const auto track = five.rows().rows.rows[0].control_area;
    CHECK(track.y == 47);
    CHECK(settings::dialog_pointer_down(five.dialog, track.x + 1, 50) == DialogAction::none);
    CHECK(five.dialog.chosen.max_frame_rate == settings::highest_frame_rate);
    CHECK(settings::dialog_pointer_down(five.dialog, track.x + 1, 56) == DialogAction::changed);
    CHECK(five.dialog.chosen.max_frame_rate == settings::lowest_frame_rate);
    // The drag follows the pointer's column wherever the line is.
    CHECK(
        settings::dialog_pointer_move(five.dialog, track.x + track.width, 20) ==
        DialogAction::changed
    );
    CHECK(five.dialog.chosen.max_frame_rate == settings::highest_frame_rate);
    static_cast<void>(settings::dialog_pointer_up(five.dialog, 0, 0));

    // At the end the strip's top is cut at 54; it still answers below it.
    five.dialog.scroll[static_cast<std::size_t>(Page::graphics)] = 80;
    const auto strip = five.rows().rows.rows[1].control_area;
    CHECK(strip.y == 48);
    const int32_t second = strip.x + 1 + geometry::level_width + geometry::level_width / 2;
    CHECK(click(five.dialog, {second, 50}) == DialogAction::none);
    CHECK(click(five.dialog, {second, 58}) == DialogAction::changed);
    CHECK(five.dialog.chosen.anti_aliasing == settings::AntiAliasing::x2);
    // A press never scrolls.
    CHECK(five.scroll() == 80);
}

void offsets_are_kept_for_each_section_until_the_dialog_opens_again() {
    Scrolling five(five_rows());
    CHECK(wheel(five.dialog, -1.0F) == DialogAction::redraw);
    CHECK(five.scroll() == 24);
    CHECK(click(five.dialog, centre(geometry::list_item(Page::controls))) == DialogAction::redraw);
    CHECK(geometry::open_rows(five.dialog).scroll == 0);
    CHECK(click(five.dialog, centre(geometry::list_item(Page::graphics))) == DialogAction::redraw);
    CHECK(five.scroll() == 24);
    CHECK(geometry::open_rows(five.dialog).rows.rows[0].top == geometry::first_row_top - 24);
    // Restore defaults is no scroll: the offset stays.
    CHECK(click(five.dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(five.scroll() == 24);
    // Each opening starts every section at its top.
    five.open();
    CHECK(five.dialog.scroll == (std::array<int32_t, settings::page_count>{}));
    CHECK(five.scroll() == 0);
}

void the_hover_follows_the_rows_under_a_still_pointer() {
    Scrolling five(five_rows());
    // Over the first row's value: no control. A notch brings the level strip
    // under the pointer, and it is hovered without the pointer moving.
    CHECK(settings::dialog_pointer_move(five.dialog, 400, 104) == DialogAction::none);
    CHECK(five.dialog.hovered == settings::no_control);
    CHECK(wheel(five.dialog, -1.0F, {400, 104}) == DialogAction::redraw);
    CHECK(five.dialog.hovered == settings::first_row_control + 1);
    // The keys move the rows under it too.
    CHECK(settings::dialog_key(five.dialog, DialogKey::home) == DialogAction::redraw);
    CHECK(five.dialog.hovered == settings::no_control);
}

void locked_switch_rows_keep_their_value_in_sight() {
    // The fourth row, a switch whose hint lines are its status, locked
    // during a game; the fifth, any other switch, set on the command line.
    // Neither label fits beside its lock and its switch, so each keeps the
    // switch and puts the lock at the start of the line under the label.
    Section section = five_rows();
    section.locks = {
        {Setting::escape_opens_menu, Lock::in_game},
        {Setting::frame_stats, Lock::command_line},
    };
    section.status = {Setting::escape_opens_menu};
    settings::EngineSettings current{};
    current.escape_opens_menu = true;
    current.frame_stats = true;
    current.unit_limit = 900;
    Scrolling five(std::move(section), current);
    const auto rows = five.rows().rows.rows;
    // The switch kept on the label line, the label the room beside it, and
    // the lock, as wide as its words, on the next line.
    const auto& status = rows[3];
    CHECK(status.hint_is_status);
    CHECK(same_rect(status.control_area, {415, 264, 52, 16}));
    CHECK(same_rect(status.lock_area, {158, 282, 148, 16}));
    CHECK(same_rect(status.label, {158, 264, 249, 16}));
    const auto& kept = rows[4];
    CHECK(!kept.hint_is_status);
    CHECK(same_rect(kept.control_area, {415, 341, 52, 16}));
    CHECK(same_rect(kept.lock_area, {158, 359, 169, 16}));
    CHECK(same_rect(kept.label, {158, 341, 249, 16}));
    for (const auto& row : {status, kept}) {
        CHECK(!overlap(row.label, row.lock_area));
        CHECK(!overlap(row.lock_area, row.control_area));
        CHECK(row.height == (row.hint_lines == 2 ? 77 : 65));
    }
    // Each dropped lock adds a line, so the limit grows by 36.
    CHECK(five.rows().limit == 116);

    // At the end both show whole: the padlock and lock text, and each
    // switch's Off and On with no control. The dropped locks sit lower
    // than the old end, so the scroll is the new limit.
    five.dialog.scroll[static_cast<std::size_t>(Page::graphics)] = five.rows().limit;
    const auto parts = settings::dialog_layout(five.dialog);
    CHECK(find_part(parts, "Locked during a game", settings::no_control) != nullptr);
    CHECK(find_part(parts, "Set on the command line", settings::no_control) != nullptr);
    int32_t status_captions = 0;
    int32_t kept_captions = 0;
    for (const auto& part : parts) {
        CHECK(part.control != settings::first_row_control + 3);
        CHECK(part.control != settings::first_row_control + 4);
        if (part.text != "OFF" && part.text != "ON")
            continue;
        if (part.rect.y == 149) {
            CHECK(part.control == settings::no_control);
            ++status_captions;
        }
        if (part.rect.y == 226) {
            CHECK(part.control == settings::no_control);
            ++kept_captions;
        }
    }
    CHECK(status_captions == 2);
    CHECK(kept_captions == 2);
    bool status_padlock = false;
    bool kept_padlock = false;
    for (const auto& part : parts) {
        if (!part.text.empty() || part.control != settings::no_control)
            continue;
        status_padlock = status_padlock || same_rect(part.rect, {158, 166, 5, 16});
        kept_padlock = kept_padlock || same_rect(part.rect, {158, 243, 5, 16});
    }
    CHECK(status_padlock && kept_padlock);

    // Neither takes a press or the focus.
    CHECK(settings::dialog_pointer_down(five.dialog, 460, 233) == DialogAction::none);
    CHECK(settings::dialog_pointer_up(five.dialog, 460, 233) == DialogAction::none);
    CHECK(settings::dialog_pointer_down(five.dialog, 460, 156) == DialogAction::none);
    CHECK(settings::dialog_pointer_up(five.dialog, 460, 156) == DialogAction::none);
    CHECK(five.dialog.chosen == current);
    five.open(current);
    for (const int32_t expected :
         {settings::first_row_control,
          settings::first_row_control + 1,
          settings::first_row_control + 2,
          settings::restore_control}) {
        CHECK(settings::dialog_key(five.dialog, DialogKey::tab) == DialogAction::redraw);
        CHECK(five.dialog.focused == expected);
    }
    CHECK(five.scroll() == 0);

    // Restore defaults keeps every locked value, the switches' included.
    CHECK(click(five.dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(five.dialog.chosen.escape_opens_menu);
    CHECK(five.dialog.chosen.frame_stats);
    CHECK(five.dialog.chosen.unit_limit == settings::default_unit_limit);
}

/// Returns a dialog open on the Graphics page with its own rows.
settings::Dialog graphics_page(
    const settings::EngineSettings& current = {},
    const settings::Locks& locks = {},
    const settings::AccelerationStatus& acceleration = {}
) {
    settings::Dialog dialog;
    settings::open_dialog(dialog, current, {}, locks, "v0.2.0", Page::graphics, acceleration);
    return dialog;
}

/// Returns Graphics' stored offset.
int32_t graphics_scroll(const settings::Dialog& dialog) {
    return dialog.scroll[static_cast<std::size_t>(Page::graphics)];
}

/// Graphics' offset at which Smooth motion shows whole with its hint, as
/// the focus scrolls to it.
constexpr int32_t smooth_motion_in_view = 131;
/// Graphics' offset at which Explosion flash shows whole with its hint.
constexpr int32_t explosion_flash_in_view = 395;
/// Graphics' offset at which Fog of War shows whole with its hint.
constexpr int32_t fog_of_war_in_view = 454;
/// Graphics' offset at which Zoomed out units and After zoom show whole
/// with their hints.
constexpr int32_t zoomed_out_units_in_view = 454;

void the_graphics_page_scrolls_its_rows() {
    settings::Dialog dialog = graphics_page();
    const auto open = geometry::open_rows(dialog);
    CHECK(open.rows.rows.size() == 21);
    const std::array<int32_t, 21> tops{54,  119, 178, 255, 314, 361, 420,  479,  538,  597, 656,
                                       735, 782, 829, 882, 935, 988, 1041, 1094, 1147, 1200};
    const std::array<int32_t, 21> heights{65, 59, 77, 59, 47, 59, 59, 59, 59, 59, 79,
                                          47, 47, 53, 53, 53, 53, 53, 53, 53, 53};
    for (std::size_t index = 0; index < open.rows.rows.size() && index < tops.size(); ++index) {
        const auto& row = open.rows.rows[index];
        CHECK(row.top == tops[index]);
        CHECK(row.height == heights[index]);
        CHECK(row.control == settings::first_row_control + static_cast<int32_t>(index));
    }
    CHECK(open.rows.bottom == 1253);
    CHECK(open.limit == 972);
    // Hardware acceleration: a strip of Off, Basic and Full, 34 columns a
    // level inside its border, with two status lines; Vertical sync a
    // switch with one hint line.
    const auto& acceleration = open.rows.rows[3];
    CHECK(acceleration.setting == Setting::hardware_acceleration);
    CHECK(acceleration.hint_is_status);
    CHECK(same_rect(acceleration.label, {158, 264, 197, 16}));
    CHECK(same_rect(acceleration.control_area, {363, 264, 104, 16}));
    CHECK(acceleration.hint_lines == 2);
    CHECK(acceleration.hints[0].y == 282 && acceleration.hints[1].y == 294);
    const auto& vsync = open.rows.rows[4];
    CHECK(vsync.setting == Setting::vertical_sync);
    CHECK(!vsync.hint_is_status);
    CHECK(same_rect(vsync.label, {158, 323, 249, 16}));
    CHECK(same_rect(vsync.control_area, {415, 323, 52, 16}));
    CHECK(vsync.hint_lines == 1 && vsync.hints[0].y == 341);
    // Smooth motion: a switch whose hint, two lines either way, says what
    // On and Off draw.
    const auto& smooth = open.rows.rows[5];
    CHECK(smooth.setting == Setting::smooth_motion);
    CHECK(!smooth.hint_is_status);
    CHECK(same_rect(smooth.label, {158, 370, 249, 16}));
    CHECK(same_rect(smooth.control_area, {415, 370, 52, 16}));
    CHECK(smooth.hint_lines == 2);
    CHECK(smooth.hints[0].y == 388 && smooth.hints[1].y == 400);
    // Native pixel density, where Menu scaling was: a switch with two hint
    // lines.
    const auto& density = open.rows.rows[6];
    CHECK(density.setting == Setting::native_density);
    CHECK(same_rect(density.label, {158, 429, 249, 16}));
    CHECK(same_rect(density.control_area, {415, 429, 52, 16}));
    CHECK(density.hint_lines == 2);
    CHECK(density.hints[0].y == 447 && density.hints[1].y == 459);
    // Explosion flash: a strip of Off, Reduced and Full, 50 columns a level
    // inside its border, with two hint lines.
    const auto& flash = open.rows.rows[7];
    CHECK(flash.setting == Setting::explosion_flash);
    CHECK(!flash.hint_is_status);
    CHECK(same_rect(flash.label, {158, 488, 149, 16}));
    CHECK(same_rect(flash.control_area, {315, 488, 152, 16}));
    CHECK(flash.hint_lines == 2);
    CHECK(flash.hints[0].y == 506 && flash.hints[1].y == 518);
    // Fog of War: a strip of Classic and Smooth, 56 columns a level inside
    // its border, with two hint lines.
    const auto& fog = open.rows.rows[8];
    CHECK(fog.setting == Setting::fog_of_war);
    CHECK(!fog.hint_is_status);
    CHECK(same_rect(fog.label, {158, 547, 187, 16}));
    CHECK(same_rect(fog.control_area, {353, 547, 114, 16}));
    CHECK(fog.hint_lines == 2);
    CHECK(fog.hints[0].y == 565 && fog.hints[1].y == 577);
    // Zoomed out units: a strip of Rendered, Dots and Icons, 56 columns a
    // level inside its border, with two hint lines; After zoom a drop-down
    // under its label, with two. The camera's switches and the effect
    // sliders follow it.
    const auto& zoomed_out = open.rows.rows[9];
    CHECK(zoomed_out.setting == Setting::zoomed_out_units);
    CHECK(same_rect(zoomed_out.label, {158, 606, 131, 16}));
    CHECK(same_rect(zoomed_out.control_area, {297, 606, 170, 16}));
    CHECK(zoomed_out.hint_lines == 2);
    CHECK(zoomed_out.hints[0].y == 624 && zoomed_out.hints[1].y == 636);
    const auto& after = open.rows.rows[10];
    CHECK(after.setting == Setting::zoomed_out_after);
    CHECK(same_rect(after.label, {158, 665, 153, 16}));
    CHECK(same_rect(after.control_area, {158, 711, 200, 16}));
    CHECK(after.hint_lines == 2);
    CHECK(after.hints[0].y == 683 && after.hints[1].y == 695);

    // At the top the first three rows keep their places and Hardware
    // acceleration's label and strip show whole; at the end the closing
    // line is at 281.
    const auto top = settings::dialog_layout(dialog);
    CHECK(find_part(top, "Hardware acceleration", settings::no_control) != nullptr);
    CHECK(find_part(top, "Vertical sync", settings::no_control) == nullptr);
    CHECK(find_part(top, {}, settings::first_row_control + 3) != nullptr);
    dialog.scroll[static_cast<std::size_t>(Page::graphics)] = 1200;
    const auto end = geometry::open_rows(dialog);
    CHECK(end.scroll == 972);
    CHECK(end.rows.bottom == 281);
    CHECK(end.rows.rows[1].control_area.y == -844);
    CHECK(end.rows.rows[7].label.y == -484);
    CHECK(end.rows.rows[8].label.y == -425);
    CHECK(end.rows.rows[9].label.y == -366);
    CHECK(end.rows.rows[10].label.y == -307);
    CHECK(end.rows.rows[10].control_area.y == -261);
    // At the end, the effect sliders show, down to Naval reflections.
    // Menu scaling, Window frame, HUD scaling and Interface size are on
    // User Interface.
    const auto last = settings::dialog_layout(dialog);
    CHECK(find_part(last, "After zoom", settings::no_control) == nullptr);
    CHECK(find_part(last, "Window frame", settings::no_control) == nullptr);
    CHECK(find_part(last, "Interface size", settings::no_control) == nullptr);
    CHECK(find_part(last, "Naval reflections", settings::no_control) != nullptr);
    CHECK(find_part(last, "Needs Dots", settings::no_control) == nullptr);

    // Tab from no focus: the first three rows at 0, Hardware acceleration at
    // 25, Vertical sync at 72, Smooth motion at 131, Native pixel density at
    // 190, Explosion flash at 249, Fog of War at 308 and Zoomed out units at
    // 367. After zoom, its field locked, takes no focus. Back up, the rows
    // that still show whole stay at 367, Native pixel density, a pixel above
    // the view's top there, shows at 366, then Smooth motion at 307,
    // Vertical sync at 260, Hardware acceleration at 201, Screen size at 124
    // and Enhanced anti-aliasing at 65.
    settings::Dialog keys = graphics_page();
    const std::array<std::pair<int32_t, int32_t>, 10> forward{{
        {settings::first_row_control, 0},
        {settings::first_row_control + 1, 0},
        {settings::first_row_control + 2, 0},
        {settings::first_row_control + 3, 25},
        {settings::first_row_control + 4, 72},
        {settings::first_row_control + 5, 131},
        {settings::first_row_control + 6, 190},
        {settings::first_row_control + 7, 249},
        {settings::first_row_control + 8, 308},
        {settings::first_row_control + 9, 367},
    }};
    for (const auto& [control, expected] : forward) {
        CHECK(settings::dialog_key(keys, DialogKey::tab) == DialogAction::redraw);
        CHECK(keys.focused == control);
        CHECK(graphics_scroll(keys) == expected);
    }
    const std::array<std::pair<int32_t, int32_t>, 9> back{{
        {settings::first_row_control + 8, 367},
        {settings::first_row_control + 7, 367},
        {settings::first_row_control + 6, 366},
        {settings::first_row_control + 5, 307},
        {settings::first_row_control + 4, 260},
        {settings::first_row_control + 3, 201},
        {settings::first_row_control + 2, 124},
        {settings::first_row_control + 1, 65},
        {settings::first_row_control, 0},
    }};
    for (const auto& [control, expected] : back) {
        CHECK(settings::dialog_key(keys, DialogKey::back_tab) == DialogAction::redraw);
        CHECK(keys.focused == control);
        CHECK(graphics_scroll(keys) == expected);
    }

    // Every part apart and in its place every 18 pixels down the scroll, the
    // end included, under every lock: each row is taller than the step, so
    // it is checked at several offsets.
    for (const auto& locks : lock_states()) {
        settings::Dialog locked = graphics_page({}, locks);
        for (int32_t scroll = 0; scroll <= 972; scroll += 18)
            check_dialog_layout_at(locked, scroll);
    }

    // User Interface holds the rows that left Graphics, then Escape, and
    // Interface size keeps a drop-down's height.
    settings::Dialog ui_page = opened(Page::user_interface);
    const auto ui = geometry::open_rows(ui_page);
    CHECK(ui.rows.rows.size() == 5);
    const std::array<int32_t, 5> interface_tops{54, 113, 172, 231, 310};
    const std::array<int32_t, 5> interface_heights{59, 59, 59, kInterfaceSizeRow, 59};
    const std::array<Setting, 5> interface_settings{
        Setting::menu_scaling,
        Setting::window_frame,
        Setting::hud_scaling,
        Setting::interface_size,
        Setting::escape_opens_menu,
    };
    for (std::size_t index = 0; index < ui.rows.rows.size(); ++index) {
        CHECK(ui.rows.rows[index].top == interface_tops[index]);
        CHECK(ui.rows.rows[index].height == interface_heights[index]);
        CHECK(ui.rows.rows[index].setting == interface_settings[index]);
    }
    CHECK(ui.rows.bottom == 369);
    CHECK(ui.limit == 88);
}

/// Returns Interface size's field on User Interface, scrolled to the end.
///
/// @param[in,out] dialog the dialog
/// @return the field's part; null when it does not show
const settings::LayoutPart* interface_size_field(settings::Dialog& dialog) {
    dialog.page = Page::user_interface;
    dialog.scroll[static_cast<std::size_t>(Page::user_interface)] = 900;
    static thread_local std::vector<settings::LayoutPart> parts;
    parts = settings::dialog_layout(dialog);
    return find_part(parts, {}, settings::first_row_control + 3);
}

/// Returns the automation words of Interface size's open drop-down's items.
///
/// @param dialog the dialog, its drop-down open
/// @return the words, top to bottom
std::vector<std::string> interface_size_words(const settings::Dialog& dialog) {
    const auto list = geometry::dialog_list(dialog, nullptr);
    for (const auto& control : list.controls)
        if (control.id == settings::first_row_control + 3)
            return control.parts;
    return {};
}

/// Interface size offers 150% only while the dialog shows Use modern fonts
/// for game text On: its open drop-down lists Auto, 100%, 200%, 300% and
/// 400% with them Off, and 150% between 100% and 200% with them On, its
/// items named by their stored words; a 150% the dialog does not offer
/// shows as 100%; and turning the switch Off while 150% is chosen sets
/// Interface size to 100%, which turning it On again keeps.
void interface_size_offers_150_with_the_modern_fonts() {
    using settings::InterfaceSize;
    // Off: five sizes, no 150%.
    settings::Dialog off = graphics_page();
    CHECK(!off.chosen.modern_fonts);
    const auto* field = interface_size_field(off);
    CHECK(field != nullptr);
    if (field == nullptr)
        return;
    CHECK(click(off, centre(field->rect)) == DialogAction::redraw);
    CHECK(off.open_list == settings::first_row_control + 3);
    auto parts = settings::dialog_layout(off);
    for (const std::string_view caption : {"Auto", "100%", "200%", "300%", "400%"})
        CHECK(find_part(parts, caption, settings::no_control) != nullptr);
    CHECK(find_part(parts, "150%", settings::no_control) == nullptr);
    CHECK(
        interface_size_words(off) ==
        (std::vector<std::string>{"automatic", "100", "200", "300", "400"})
    );

    // On: six sizes, 150% between 100% and 200%.
    settings::EngineSettings modern{};
    modern.modern_fonts = true;
    settings::Dialog on = graphics_page(modern);
    field = interface_size_field(on);
    CHECK(field != nullptr && click(on, centre(field->rect)) == DialogAction::redraw);
    parts = settings::dialog_layout(on);
    const auto* hundred = find_part(parts, "100%", settings::no_control);
    const auto* fraction = find_part(parts, "150%", settings::no_control);
    const auto* two_hundred = find_part(parts, "200%", settings::no_control);
    CHECK(hundred != nullptr && fraction != nullptr && two_hundred != nullptr);
    if (hundred == nullptr || fraction == nullptr || two_hundred == nullptr)
        return;
    CHECK(hundred->rect.y < fraction->rect.y && fraction->rect.y < two_hundred->rect.y);
    CHECK(
        interface_size_words(on) ==
        (std::vector<std::string>{"automatic", "100", "150", "200", "300", "400"})
    );
    // A click on 150% chooses it, and the field shows it.
    CHECK(click(on, centre(fraction->rect)) == DialogAction::changed);
    CHECK(on.chosen.interface_size == InterfaceSize::size_150);
    CHECK(on.open_list == settings::no_control);
    field = interface_size_field(on);
    CHECK(field != nullptr && field->text == "150%");

    // A 150% the dialog does not offer, as when the modern fonts are Off,
    // shows as 100%.
    settings::EngineSettings stale{};
    stale.interface_size = InterfaceSize::size_150;
    settings::Dialog unoffered = graphics_page(stale);
    field = interface_size_field(unoffered);
    CHECK(field != nullptr && field->text == "100%");

    // Turning Use modern fonts for game text Off sets 150% to 100% in the
    // dialog; On again keeps 100%.
    const auto language_entry =
        find_part(settings::dialog_layout(on), {}, settings::page_control(Page::language));
    CHECK(language_entry != nullptr);
    if (language_entry == nullptr)
        return;
    static_cast<void>(click(on, centre(language_entry->rect)));
    CHECK(on.page == Page::language);
    const auto switch_area = geometry::open_rows(on).rows.rows[1].control_area;
    CHECK(geometry::open_rows(on).rows.rows[1].setting == Setting::modern_fonts);
    CHECK(click(on, {switch_area.x + 4, switch_area.y + 4}) == DialogAction::changed);
    CHECK(!on.chosen.modern_fonts);
    CHECK(on.chosen.interface_size == InterfaceSize::size_100);
    CHECK(
        click(on, {switch_area.x + switch_area.width - 4, switch_area.y + 4}) ==
        DialogAction::changed
    );
    CHECK(on.chosen.modern_fonts);
    CHECK(on.chosen.interface_size == InterfaceSize::size_100);
    // Any other size stays as it is when the switch turns Off.
    on.chosen.interface_size = InterfaceSize::size_200;
    CHECK(click(on, {switch_area.x + 4, switch_area.y + 4}) == DialogAction::changed);
    CHECK(on.chosen.interface_size == InterfaceSize::size_200);
}

void every_switch_reads_and_sets_through_one_table() {
    int32_t switches = 0;
    for (int32_t value = 0; value <= static_cast<int32_t>(Setting::text_background); ++value) {
        const auto setting = static_cast<Setting>(value);
        const settings::EngineSettings before{};
        settings::EngineSettings state = before;
        if (geometry::kind_of(setting) != geometry::RowKind::toggle) {
            // A slider or the level strip is no switch, and set_switch leaves it.
            geometry::set_switch(state, setting, true);
            CHECK(state == before);
            CHECK(!geometry::switch_on(state, setting));
            continue;
        }
        ++switches;
        // Each switch reads and sets its own value, whichever its default.
        const bool was = geometry::switch_on(state, setting);
        geometry::set_switch(state, setting, !was);
        CHECK(geometry::switch_on(state, setting) == !was);
        CHECK(state != before);
        geometry::set_switch(state, setting, was);
        CHECK(geometry::switch_on(state, setting) == was);
        CHECK(state == before);
    }
    CHECK(switches == 11);
    // Hardware acceleration is a strip of Off, Basic and Full, no switch:
    // set_switch leaves it, and the strip reads and sets it by level, as
    // Enhanced anti-aliasing's strip does.
    settings::EngineSettings both{};
    geometry::set_switch(both, Setting::hardware_acceleration, true);
    CHECK(both.hardware_acceleration == HardwareAcceleration::off);
    CHECK(geometry::kind_of(Setting::hardware_acceleration) == geometry::RowKind::levels);
    CHECK(geometry::kind_of(Setting::anti_aliasing) == geometry::RowKind::levels);
    CHECK(geometry::kind_of(Setting::vertical_sync) != geometry::RowKind::levels);
    CHECK(geometry::strip_of(Setting::hardware_acceleration).levels == 3);
    CHECK(
        geometry::strip_of(Setting::hardware_acceleration).level_width ==
        geometry::acceleration_level_width
    );
    CHECK(
        geometry::strip_of(Setting::anti_aliasing).levels == settings::anti_aliasing_levels.size()
    );
    CHECK(geometry::strip_of(Setting::vertical_sync).levels == 0);
    CHECK(geometry::strip_caption(Setting::hardware_acceleration, 0) == "Off");
    CHECK(geometry::strip_caption(Setting::hardware_acceleration, 1) == "Basic");
    CHECK(geometry::strip_caption(Setting::hardware_acceleration, 2) == "Full");
    CHECK(geometry::strip_caption(Setting::hardware_acceleration, 3).empty());
    CHECK(geometry::strip_caption(Setting::anti_aliasing, 1) == "2x");
    geometry::set_strip_level(both, Setting::hardware_acceleration, 2);
    CHECK(both.hardware_acceleration == HardwareAcceleration::full);
    CHECK(geometry::strip_level(both, Setting::hardware_acceleration) == 2);
    geometry::set_strip_level(both, Setting::hardware_acceleration, 9);
    CHECK(both.hardware_acceleration == HardwareAcceleration::full);
    geometry::set_strip_level(both, Setting::hardware_acceleration, 1);
    CHECK(both.hardware_acceleration == HardwareAcceleration::basic);
    CHECK(geometry::strip_level(both, Setting::hardware_acceleration) == 1);
    geometry::set_strip_level(both, Setting::vertical_sync, 1);
    CHECK(!both.vertical_sync);
    geometry::set_switch(both, Setting::vertical_sync, true);
    CHECK(both.hardware_acceleration == HardwareAcceleration::basic && both.vertical_sync);
    // The column under each level, and the nearest end outside the strip.
    const renderer::SourceRect strip_area{363, 264, 104, 16};
    const auto strip = geometry::strip_of(Setting::hardware_acceleration);
    CHECK(geometry::level_at(strip_area, strip, 364) == 0);
    CHECK(geometry::level_at(strip_area, strip, 397) == 0);
    CHECK(geometry::level_at(strip_area, strip, 398) == 1);
    CHECK(geometry::level_at(strip_area, strip, 431) == 1);
    CHECK(geometry::level_at(strip_area, strip, 432) == 2);
    CHECK(geometry::level_at(strip_area, strip, 466) == 2);
    CHECK(geometry::level_at(strip_area, strip, 0) == 0);
    CHECK(geometry::level_at(strip_area, strip, 900) == 2);

    // Vertical sync's switch takes a click on either half and the keys;
    // Hardware acceleration's strip a click on each level.
    settings::Dialog dialog = graphics_page();
    dialog.scroll[static_cast<std::size_t>(Page::graphics)] = 80;
    const auto rows = geometry::open_rows(dialog).rows.rows;
    const auto on_half = [](const renderer::SourceRect& area) {
        return Point{area.x + area.width - 4, area.y + area.height / 2};
    };
    const auto off_half = [](const renderer::SourceRect& area) {
        return Point{area.x + 4, area.y + area.height / 2};
    };
    const auto level_centre = [](const renderer::SourceRect& area, int32_t level) {
        return Point{
            area.x + 1 + level * geometry::acceleration_level_width +
                geometry::acceleration_level_width / 2,
            area.y + area.height / 2
        };
    };
    CHECK(click(dialog, on_half(rows[4].control_area)) == DialogAction::changed);
    CHECK(dialog.chosen.vertical_sync);
    CHECK(click(dialog, off_half(rows[4].control_area)) == DialogAction::changed);
    CHECK(!dialog.chosen.vertical_sync);
    CHECK(dialog.forget_renderer_failures == 0);

    // Hardware acceleration passing to a higher level, Off to Basic or
    // Full or Basic to Full, asks for the graphics card to be tried afresh,
    // once each time; a level kept, and a lower one, keep the count.
    CHECK(click(dialog, level_centre(rows[3].control_area, 1)) == DialogAction::changed);
    CHECK(dialog.chosen.hardware_acceleration == HardwareAcceleration::basic);
    CHECK(dialog.forget_renderer_failures == 1);
    CHECK(click(dialog, level_centre(rows[3].control_area, 1)) == DialogAction::redraw);
    CHECK(dialog.forget_renderer_failures == 1);
    CHECK(click(dialog, level_centre(rows[3].control_area, 2)) == DialogAction::changed);
    CHECK(dialog.chosen.hardware_acceleration == HardwareAcceleration::full);
    CHECK(dialog.forget_renderer_failures == 2);
    CHECK(click(dialog, level_centre(rows[3].control_area, 1)) == DialogAction::changed);
    CHECK(dialog.chosen.hardware_acceleration == HardwareAcceleration::basic);
    CHECK(dialog.forget_renderer_failures == 2);
    CHECK(click(dialog, level_centre(rows[3].control_area, 0)) == DialogAction::changed);
    CHECK(dialog.chosen.hardware_acceleration == HardwareAcceleration::off);
    CHECK(dialog.forget_renderer_failures == 2);
    CHECK(click(dialog, level_centre(rows[3].control_area, 2)) == DialogAction::changed);
    CHECK(dialog.chosen.hardware_acceleration == HardwareAcceleration::full);
    CHECK(dialog.forget_renderer_failures == 3);
    CHECK(click(dialog, level_centre(rows[3].control_area, 0)) == DialogAction::changed);
    CHECK(dialog.chosen.hardware_acceleration == HardwareAcceleration::off);
    // Through the keys: Tab to it, Right a level up to Full and no further,
    // Left back down to Off, Space nothing on a strip.
    for (int32_t press = 0; press < 4; ++press)
        static_cast<void>(settings::dialog_key(dialog, DialogKey::tab));
    CHECK(dialog.focused == settings::first_row_control + 3);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.hardware_acceleration == HardwareAcceleration::basic);
    CHECK(dialog.forget_renderer_failures == 4);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.hardware_acceleration == HardwareAcceleration::full);
    CHECK(dialog.forget_renderer_failures == 5);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::redraw);
    CHECK(dialog.chosen.hardware_acceleration == HardwareAcceleration::full);
    CHECK(dialog.forget_renderer_failures == 5);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(dialog.chosen.hardware_acceleration == HardwareAcceleration::basic);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(dialog.chosen.hardware_acceleration == HardwareAcceleration::off);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::redraw);
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::none);
    CHECK(dialog.chosen.hardware_acceleration == HardwareAcceleration::off);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.forget_renderer_failures == 6);
    CHECK(settings::dialog_key(dialog, DialogKey::tab) == DialogAction::redraw);
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::changed);
    CHECK(dialog.chosen.vertical_sync);
    CHECK(dialog.forget_renderer_failures == 6);
    // Cancel puts both back.
    CHECK(settings::dialog_key(dialog, DialogKey::escape) == DialogAction::cancelled);
    CHECK(
        dialog.chosen.hardware_acceleration == HardwareAcceleration::off &&
        !dialog.chosen.vertical_sync
    );

    // Restore defaults with the player's own defaults, Hardware acceleration
    // Full: every press asks once more, and reports a change though none
    // moved.
    settings::EngineSettings own{};
    own.hardware_acceleration = HardwareAcceleration::full;
    settings::open_dialog(dialog, own, own, {}, "v0.2.0", Page::graphics);
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.chosen == own);
    CHECK(dialog.forget_renderer_failures == 1);
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.forget_renderer_failures == 2);
    // From Off to its default Full, Restore defaults asks once; from Basic
    // it asks too, since each press has the card tried afresh.
    settings::open_dialog(dialog, {}, own, {}, "v0.2.0", Page::graphics);
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.chosen.hardware_acceleration == HardwareAcceleration::full);
    CHECK(dialog.forget_renderer_failures == 1);
    settings::EngineSettings basic{};
    basic.hardware_acceleration = HardwareAcceleration::basic;
    settings::open_dialog(dialog, basic, own, {}, "v0.2.0", Page::graphics);
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.chosen.hardware_acceleration == HardwareAcceleration::full);
    CHECK(dialog.forget_renderer_failures == 1);
}

void the_new_rows_lock_in_their_own_forms() {
    // Either flag locks Hardware acceleration; nothing that could help locks
    // it Not available here; a game never does.
    settings::GameState state{};
    state.renderer_from_command_line = true;
    state.acceleration_unavailable = true;
    CHECK(settings::settings_locks(state).hardware_acceleration == Lock::command_line);
    state.renderer_from_command_line = false;
    CHECK(settings::settings_locks(state).hardware_acceleration == Lock::unavailable);
    for (const bool shared : {false, true})
        for (const bool replay : {false, true}) {
            const auto locks = settings::settings_locks(settings::GameState{true, shared, replay});
            CHECK(locks.hardware_acceleration == Lock::none);
            CHECK(locks.vertical_sync == (shared || replay ? Lock::in_game : Lock::none));
        }
    state = {};
    state.vertical_sync_unavailable = true;
    CHECK(settings::settings_locks(state).vertical_sync == Lock::unavailable);
    state.in_game = true;
    state.shared_game = true;
    CHECK(settings::settings_locks(state).vertical_sync == Lock::unavailable);

    // Hardware acceleration locked: its strip kept and its label the room
    // beside the strip; the lock does not fit there, so it stands at the
    // start of the line under the label. Vertical sync's label does fit
    // beside its lock and its switch, so the lock stays 8 columns left of
    // the switch and the label 93 wide.
    settings::Locks locks{};
    locks.hardware_acceleration = Lock::unavailable;
    locks.vertical_sync = Lock::unavailable;
    settings::EngineSettings current{};
    current.hardware_acceleration = HardwareAcceleration::basic;
    current.vertical_sync = true;
    settings::Dialog dialog =
        graphics_page(current, locks, {settings::AccelerationState::no_usable_card, {}, false});
    const auto rows = geometry::open_rows(dialog).rows.rows;
    CHECK(same_rect(rows[3].control_area, {363, 264, 104, 16}));
    CHECK(same_rect(rows[3].lock_area, {158, 282, 148, 16}));
    CHECK(same_rect(rows[3].label, {158, 264, 197, 16}));
    CHECK(same_rect(rows[4].control_area, {415, 341, 52, 16}));
    CHECK(same_rect(rows[4].lock_area, {259, 341, 148, 16}));
    CHECK(same_rect(rows[4].label, {158, 341, 93, 16}));
    // The extra lock line adds 18 pixels to Graphics' 972-pixel scroll range.
    CHECK(geometry::open_rows(dialog).limit == 990);

    // Scrolled down to Native pixel density: both lock texts, the status,
    // the strip's captions and the kept switch's, each with no control.
    dialog.scroll[static_cast<std::size_t>(Page::graphics)] = 198;
    const auto parts = settings::dialog_layout(dialog);
    int32_t lock_texts = 0;
    for (const auto& part : parts) {
        lock_texts += part.text == "Not available here" ? 1 : 0;
        CHECK(part.control != settings::first_row_control + 3);
        CHECK(part.control != settings::first_row_control + 4);
    }
    CHECK(lock_texts == 2);
    CHECK(
        find_part(parts, "Not in use: no usable graphics card was found.", settings::no_control) !=
        nullptr
    );
    CHECK(find_part(parts, "Basic", settings::no_control) != nullptr);
    // A press where either control is, and every key, leaves them; the
    // keys pass from the first three rows to Smooth motion, Native pixel
    // density, Explosion flash, Fog of War and Zoomed out units. After zoom stays locked while
    // units are Rendered, and the rows that moved are on User Interface.
    CHECK(click(dialog, {460, 72}) == DialogAction::none);
    CHECK(click(dialog, {380, 72}) == DialogAction::none);
    CHECK(click(dialog, {460, 151}) == DialogAction::none);
    CHECK(dialog.chosen == current);
    for (const int32_t expected :
         {settings::first_row_control,
          settings::first_row_control + 1,
          settings::first_row_control + 2,
          settings::first_row_control + 5,
          settings::first_row_control + 6,
          settings::first_row_control + 7,
          settings::first_row_control + 8,
          settings::first_row_control + 9,
          settings::first_row_control + 11,
          settings::first_row_control + 12,
          settings::first_row_control + 13,
          settings::first_row_control + 14,
          settings::first_row_control + 15,
          settings::first_row_control + 16,
          settings::restore_control}) {
        CHECK(settings::dialog_key(dialog, DialogKey::tab) == DialogAction::redraw);
        CHECK(dialog.focused == expected);
    }
    // Restore defaults keeps both locked values.
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(
        dialog.chosen.hardware_acceleration == HardwareAcceleration::basic &&
        dialog.chosen.vertical_sync
    );

    // Under a flag the lock says so, and in a shared game Vertical sync is
    // locked during the game while Hardware acceleration can still be set.
    settings::GameState flagged{true, true, false, false};
    flagged.renderer_from_command_line = true;
    settings::Dialog under_flag = graphics_page({}, settings::settings_locks(flagged));
    under_flag.scroll[static_cast<std::size_t>(Page::graphics)] = 80;
    const auto flag_parts = settings::dialog_layout(under_flag);
    CHECK(find_part(flag_parts, "Set on the command line", settings::no_control) != nullptr);
    CHECK(find_part(flag_parts, "Locked during a game", settings::no_control) != nullptr);
    settings::Dialog shared =
        graphics_page({}, settings::settings_locks(settings::GameState{true, true, false, false}));
    shared.scroll[static_cast<std::size_t>(Page::graphics)] = 80;
    const auto shared_rows = geometry::open_rows(shared).rows.rows;
    CHECK(shared_rows[3].lock == Lock::none);
    CHECK(shared_rows[4].lock == Lock::in_game);
    CHECK(
        click(shared, {shared_rows[3].control_area.x + 48, shared_rows[3].control_area.y + 8}) ==
        DialogAction::changed
    );
    CHECK(shared.chosen.hardware_acceleration == HardwareAcceleration::basic);
    CHECK(
        click(shared, {shared_rows[3].control_area.x + 90, shared_rows[3].control_area.y + 8}) ==
        DialogAction::changed
    );
    CHECK(shared.chosen.hardware_acceleration == HardwareAcceleration::full);
}

void hardware_acceleration_shows_its_status() {
    using settings::AccelerationReach;
    using settings::AccelerationState;

    struct Expected {
        AccelerationState state;
        std::string_view first;
        std::string_view second;
    };

    constexpr std::string_view off = "Off: the processor draws and scales the view.";
    constexpr std::string_view processor = "The processor draws and scales the view.";
    constexpr std::string_view retry = "Set it to Off and back, or restore defaults.";
    constexpr std::string_view needs_memory = "Not in use: it needs at least 2 GB of memory.";
    constexpr std::string_view takes_effect = "takes effect from the next game.";
    const std::array<Expected, 18> fixed{{
        {AccelerationState::off_driver_skipped, off, "A failed graphics driver is skipped."},
        {AccelerationState::needs_memory_driver_skipped,
         needs_memory,
         "A failed graphics driver is skipped."},
        {AccelerationState::needs_memory, needs_memory, processor},
        {AccelerationState::off_by_setting, off, "Basic lets the graphics card scale it evenly."},
        {AccelerationState::off_by_command_line, off, "For this run only. The setting is kept."},
        {AccelerationState::environment_driver,
         "Not in use: the environment names a driver.",
         processor},
        {AccelerationState::too_little_memory,
         "Not in use: there is too little memory.",
         processor},
        {AccelerationState::waiting_for_game_end,
         "Off for this game: in a shared game, Basic",
         "takes effect from the next game."},
        {AccelerationState::engine_error, "Not in use: an error stopped it for this run.", retry},
        {AccelerationState::driver_failed, "Not in use: the graphics driver failed.", retry},
        {AccelerationState::game_stopped, "Not in use: the game stopped while using it.", retry},
        {AccelerationState::no_usable_card,
         "Not in use: no usable graphics card was found.",
         processor},
        {AccelerationState::lacks_feature,
         "Not in use: the graphics card lacks a feature.",
         processor},
        {AccelerationState::cannot_save, "Not in use: the game cannot save its files.", processor},
        {AccelerationState::next_start,
         "Takes effect from the next start.",
         "The processor draws and scales the view until then."},
        {AccelerationState::full_stopped, "Basic in use: Full stopped for this run.", retry},
        {AccelerationState::full_failed_before,
         "Basic in use: Full failed before on this driver.",
         retry},
        {AccelerationState::full_waiting_for_game_end,
         "Basic for this game: in a shared game, Full",
         takes_effect},
    }};
    for (const auto& expected : fixed) {
        for (const auto& status : acceleration_statuses()) {
            // The wait names the level asked for: Basic here, Full below.
            if (status.state != expected.state || status.replay ||
                status.asked == HardwareAcceleration::full)
                continue;
            CHECK(geometry::status_line(status, 0) == expected.first);
            CHECK(geometry::status_line(status, 1) == expected.second);
            CHECK(geometry::status_line(status, 2).empty());
        }
    }
    // In a replay the wait names it, and Full where Full was asked for.
    settings::AccelerationStatus replay{AccelerationState::waiting_for_game_end, {}, true};
    CHECK(geometry::status_line(replay, 0) == "Off for this game: in a replay, Basic");
    CHECK(geometry::status_line(replay, 1) == "takes effect from the next game.");
    settings::AccelerationStatus full_wait{
        AccelerationState::waiting_for_game_end, {}, false, HardwareAcceleration::full
    };
    CHECK(geometry::status_line(full_wait, 0) == "Off for this game: in a shared game, Full");
    CHECK(geometry::status_line(full_wait, 1) == "takes effect from the next game.");
    full_wait.replay = true;
    CHECK(geometry::status_line(full_wait, 0) == "Off for this game: in a replay, Full");
    CHECK(geometry::status_line(full_wait, 1) == "takes effect from the next game.");
    // Basic running while Full waits names the match too.
    settings::AccelerationStatus basic_wait{
        AccelerationState::full_waiting_for_game_end, {}, true, HardwareAcceleration::full
    };
    CHECK(geometry::status_line(basic_wait, 0) == "Basic for this game: in a replay, Full");
    CHECK(geometry::status_line(basic_wait, 1) == takes_effect);
    // Every other state reads the same whichever level was asked for.
    for (const auto& status : acceleration_statuses()) {
        if (status.state == AccelerationState::waiting_for_game_end ||
            status.state == AccelerationState::full_waiting_for_game_end)
            continue;
        settings::AccelerationStatus other = status;
        other.asked = HardwareAcceleration::off;
        CHECK(geometry::status_line(status, 0) == geometry::status_line(other, 0));
        CHECK(geometry::status_line(status, 1) == geometry::status_line(other, 1));
    }
    // While it is in use the first line names Basic, the tier that runs, and
    // the second says what it does here; Full, which the game cannot draw
    // yet, or which stopped, says Basic is in use in its place.
    const std::array<std::pair<AccelerationState, std::string_view>, 6> in_use{{
        {AccelerationState::full_cannot_save, "Basic in use: the game cannot save its files."},
        {AccelerationState::full_too_little_memory,
         "Basic in use: there is too little memory for Full."},
        {AccelerationState::full_lacks_feature,
         "Basic in use: the card lacks a feature Full needs."},
        {AccelerationState::in_use_on_another_driver,
         "Basic in use, on another driver: one failed."},
        {AccelerationState::in_use_no_smoothing,
         "Basic in use; no smoothing when zoomed out here."},
        {AccelerationState::in_use, "Basic in use."},
    }};
    const std::array<std::pair<AccelerationReach, std::string_view>, 5> reaches{{
        {AccelerationReach::menus, "It scales the menus and the interface evenly."},
        {AccelerationReach::zoomed_in, "It scales the interface and zoomed-in view evenly."},
        {AccelerationReach::zoomed_out, "It scales evenly and smooths the zoomed-out view."},
        {AccelerationReach::nearest_zoomed_out, "It smooths the zoomed-out view."},
        {AccelerationReach::nearest_none, "Here the view is drawn as when it is off."},
    }};
    for (const auto& [state, first] : in_use)
        for (const auto& [reach, second] : reaches) {
            const settings::AccelerationStatus status{state, reach, false};
            CHECK(geometry::status_line(status, 0) == first);
            CHECK(geometry::status_line(status, 1) == second);
        }
    // Full in use names its anti-aliasing on the second line, whatever the
    // reach: none, 2x or 4x; a count between reads as the one below it.
    const std::array<std::pair<AccelerationState, std::string_view>, 1> full_in_use{{
        {AccelerationState::full_in_use, "Full in use: the graphics card draws the view."},
    }};
    const std::array<std::pair<uint8_t, std::string_view>, 7> anti_aliasing{{
        {1, "Smoothed at every zoom."},
        {2, "Smoothed at every zoom; 2x2 samples a pixel."},
        {3, "Smoothed at every zoom; 2x2 samples a pixel."},
        {4, "Smoothed at every zoom; 4x4 samples a pixel."},
        {8, "Smoothed at every zoom; 8x8 samples a pixel."},
        {16, "Smoothed at every zoom; 16x16 samples a pixel."},
        {0, "Smoothed at every zoom."},
    }};
    for (const auto& [state, first] : full_in_use)
        for (const auto& [reach, second] : reaches)
            for (const auto& [supersample, line] : anti_aliasing) {
                settings::AccelerationStatus status{state, reach, false};
                status.supersample = supersample;
                CHECK(geometry::status_line(status, 0) == first);
                CHECK(geometry::status_line(status, 1) == line);
            }
    // Every status has two lines, none empty.
    for (const auto& status : acceleration_statuses()) {
        CHECK(!geometry::status_line(status, 0).empty());
        CHECK(!geometry::status_line(status, 1).empty());
        CHECK(
            geometry::hint_line(Setting::hardware_acceleration, {}, status, 0) ==
            geometry::status_line(status, 0)
        );
    }
    CHECK(geometry::hint_line_count(Setting::hardware_acceleration) == 2);
    CHECK(geometry::hint_line_count(Setting::vertical_sync) == 1);
    CHECK(geometry::hint_is_status(Setting::hardware_acceleration));
    CHECK(!geometry::hint_is_status(Setting::vertical_sync));
    CHECK(geometry::lock_text(Lock::unavailable) == "Not available here");

    // The host gives the status at opening and each frame after; only a
    // change asks for a redraw, and the row shows it.
    settings::Dialog dialog = graphics_page({}, {}, {AccelerationState::off_by_setting, {}, false});
    dialog.scroll[static_cast<std::size_t>(Page::graphics)] = 80;
    CHECK(
        settings::set_acceleration_status(
            dialog, {AccelerationState::off_by_setting, AccelerationReach::menus, false}
        ) == DialogAction::none
    );
    CHECK(
        settings::set_acceleration_status(
            dialog, {AccelerationState::in_use, AccelerationReach::zoomed_out, false}
        ) == DialogAction::redraw
    );
    const auto parts = settings::dialog_layout(dialog);
    CHECK(find_part(parts, "Basic in use.", settings::no_control) != nullptr);
    CHECK(
        find_part(
            parts, "It scales evenly and smooths the zoomed-out view.", settings::no_control
        ) != nullptr
    );
    CHECK(
        settings::set_acceleration_status(
            dialog, {AccelerationState::waiting_for_game_end, AccelerationReach::zoomed_out, true}
        ) == DialogAction::redraw
    );
    CHECK(
        find_part(
            settings::dialog_layout(dialog),
            "Off for this game: in a replay, Basic",
            settings::no_control
        ) != nullptr
    );
    CHECK(
        settings::set_acceleration_status(
            dialog,
            {AccelerationState::waiting_for_game_end,
             AccelerationReach::zoomed_out,
             true,
             HardwareAcceleration::full}
        ) == DialogAction::redraw
    );
    CHECK(
        find_part(
            settings::dialog_layout(dialog),
            "Off for this game: in a replay, Full",
            settings::no_control
        ) != nullptr
    );
    CHECK(
        settings::set_acceleration_status(
            dialog,
            {AccelerationState::full_lacks_feature,
             AccelerationReach::menus,
             false,
             HardwareAcceleration::full}
        ) == DialogAction::redraw
    );
    const auto full_parts = settings::dialog_layout(dialog);
    CHECK(
        find_part(
            full_parts, "Basic in use: the card lacks a feature Full needs.", settings::no_control
        ) != nullptr
    );
    CHECK(
        find_part(
            full_parts, "It scales the menus and the interface evenly.", settings::no_control
        ) != nullptr
    );
}

struct Canvas {
    renderer::Surface surface;

    renderer::Rgb at(int32_t x, int32_t y) const {
        const std::size_t offset = (static_cast<std::size_t>(y) * surface.width + x) * 3;
        return {surface.rgb[offset], surface.rgb[offset + 1], surface.rgb[offset + 2]};
    }
};

Canvas blank(uint32_t width, uint32_t height) {
    Canvas canvas;
    canvas.surface.width = width;
    canvas.surface.height = height;
    canvas.surface.rgb.assign(std::size_t{width} * height * 3, 0);
    return canvas;
}

/// A 40 by 40 icon of one opaque colour, but for its clear top left 2 by 2
/// pixels.
struct IconPicture {
    std::vector<uint8_t> pixels;

    renderer::RgbaPicture picture() const { return {kIconSide, kIconSide, pixels}; }

    static constexpr uint32_t kIconSide = 40;
};

/// The test icon's colour.
constexpr renderer::Rgb kIconColor{0xd4, 0xa0, 0x30};

IconPicture solid_icon() {
    IconPicture icon;
    for (uint32_t y = 0; y < IconPicture::kIconSide; ++y)
        for (uint32_t x = 0; x < IconPicture::kIconSide; ++x) {
            icon.pixels.insert(icon.pixels.end(), kIconColor.begin(), kIconColor.end());
            icon.pixels.push_back(x < 2 && y < 2 ? 0 : 0xff);
        }
    return icon;
}

constexpr renderer::Rgb kPanel{0x1b, 0x1e, 0x19};
constexpr renderer::Rgb kBand{0x14, 0x16, 0x12};
constexpr renderer::Rgb kList{0x17, 0x1a, 0x15};
constexpr renderer::Rgb kAccent{0x9c, 0xcc, 0x3c};
constexpr renderer::Rgb kOffSelected{0x2c, 0x32, 0x26};

void the_dialog_draws_its_faces_and_accents(const settings::DialogFonts& fonts) {
    Canvas canvas = blank(settings::dialog_width, settings::dialog_height);
    settings::Dialog dialog = opened(Page::controls);
    dialog.chosen.escape_opens_menu = false;
    settings::draw_dialog(canvas.surface, {0, 0, 1}, dialog, fonts, kNoIcon);
    CHECK(canvas.at(300, 3) == kBand);                           // the header
    CHECK(canvas.at(300, geometry::footer_top + 2) == kBand);    // the footer
    CHECK(canvas.at(4, 250) == kList);                           // the section list
    CHECK(canvas.at(geometry::content_left + 2, 270) == kPanel); // under the rows
    const auto marker = geometry::list_item(Page::controls);
    CHECK(
        canvas.at(marker.x + geometry::list_marker_offset, marker.y + marker.height / 2) == kAccent
    );
    CHECK(canvas.at(geometry::ok_button.x + 2, geometry::ok_button.y + 2) == kAccent);
    const auto rows = geometry::place_rows(Page::controls, {});
    const auto& zoom = rows.rows[0].control_area;
    CHECK(canvas.at(zoom.x + zoom.width - 3, zoom.y + 2) == kAccent); // On
    Canvas off = blank(settings::dialog_width, settings::dialog_height);
    settings::Dialog zoom_off = dialog;
    zoom_off.chosen.wheel_zoom = false;
    settings::draw_dialog(off.surface, {0, 0, 1}, zoom_off, fonts, kNoIcon);
    CHECK(off.at(zoom.x + 2, zoom.y + 2) == kOffSelected); // Off

    // At twice the size, offset into a larger surface.
    Canvas larger = blank(1200, 800);
    settings::draw_dialog(larger.surface, {100, 50, 2}, dialog, fonts, kNoIcon);
    CHECK(larger.at(99, 49) == (renderer::Rgb{0, 0, 0}));
    CHECK(larger.at(100 + 2 * 300, 50 + 2 * 3) == kBand);
    CHECK(
        larger.at(100 + 2 * (geometry::ok_button.x + 2), 50 + 2 * (geometry::ok_button.y + 2)) ==
        kAccent
    );

    // A slider's filled track runs to its knob.
    Canvas graphics = blank(settings::dialog_width, settings::dialog_height);
    settings::Dialog shown = opened(Page::graphics);
    settings::draw_dialog(graphics.surface, {0, 0, 1}, shown, fonts, kNoIcon);
    const auto track = geometry::place_rows(Page::graphics, {}).rows[0].control_area;
    CHECK(graphics.at(track.x + 2, track.y + geometry::track_offset + 1) == kAccent);

    // Without the icon, the header shows the OA mark's green outlined
    // square, 13 pixels a side in the middle of the icon's place.
    const auto mark = geometry::header_mark;
    const int32_t square_left = mark.x + (mark.width - geometry::header_mark_square) / 2;
    const int32_t square_top = mark.y + (mark.height - geometry::header_mark_square) / 2;
    CHECK(canvas.at(square_left, square_top) == kAccent);
    CHECK(canvas.at(square_left + geometry::header_mark_square - 1, square_top + 6) == kAccent);
    CHECK(canvas.at(square_left - 1, square_top) == kBand);

    // With the icon, the icon fills its 20 by 20 place in the header's
    // middle rows, and the band shows round it.
    const IconPicture icon = solid_icon();
    Canvas iconic = blank(settings::dialog_width, settings::dialog_height);
    settings::draw_dialog(iconic.surface, {0, 0, 1}, dialog, fonts, icon.picture());
    CHECK(mark.width == 20 && mark.height == 20);
    CHECK(
        mark.y - geometry::header_top ==
        geometry::header_top + geometry::header_height - (mark.y + mark.height)
    );
    CHECK(iconic.at(mark.x, mark.y + 5) == kIconColor);
    CHECK(iconic.at(mark.x + mark.width - 1, mark.y + mark.height - 1) == kIconColor);
    CHECK(iconic.at(mark.x - 1, mark.y + 5) == kBand);
    CHECK(iconic.at(mark.x + mark.width, mark.y + 5) == kBand);
    CHECK(iconic.at(mark.x + 5, mark.y - 1) == kBand);
    CHECK(iconic.at(mark.x + 5, mark.y + mark.height) == kBand);
    CHECK(iconic.at(mark.x, mark.y) == kBand); // the icon's clear corner
    CHECK(iconic.at(mark.x + 1, mark.y) == kIconColor);
    CHECK(iconic.at(square_left, square_top) != kAccent);
    // Twice as large, the icon covers 40 by 40 surface pixels, one of the
    // icon's pixels each: its clear corner is 2 by 2 surface pixels.
    Canvas twice = blank(2 * settings::dialog_width, 2 * settings::dialog_height);
    settings::draw_dialog(twice.surface, {0, 0, 2}, dialog, fonts, icon.picture());
    CHECK(twice.at(2 * mark.x + 1, 2 * mark.y + 1) == kBand);
    CHECK(twice.at(2 * mark.x + 2, 2 * mark.y) == kIconColor);
    CHECK(
        twice.at(2 * mark.x + 2 * mark.width - 1, 2 * mark.y + 2 * mark.height - 1) == kIconColor
    );
    CHECK(twice.at(2 * mark.x + 2 * mark.width, 2 * mark.y + 5) == kBand);
    CHECK(twice.at(2 * mark.x - 1, 2 * mark.y + 5) == kBand);

    // The OA button's face.
    Canvas button = blank(settings::menu_button_side, settings::menu_button_side);
    settings::draw_oa_button(
        button.surface,
        {0, 0, 1},
        settings::menu_button_side,
        settings::ButtonLook::idle,
        fonts,
        kNoIcon
    );
    CHECK(button.at(3, 3) == kPanel);
    Canvas pressed = blank(settings::ingame_button_side, settings::ingame_button_side);
    settings::draw_oa_button(
        pressed.surface,
        {0, 0, 1},
        settings::ingame_button_side,
        settings::ButtonLook::pressed,
        fonts,
        kNoIcon
    );
    CHECK(pressed.at(2, 2) == kBand);
}

constexpr std::array<Page, 5> kModPages{
    Page::mod_keys,
    Page::mod_patrol,
    Page::mod_guard,
    Page::mod_tools,
    Page::mod_chat,
};

/// A mod's options as a profile offers them: snap radii up to 6 and 4.
settings::EngineSettings mod_settings() {
    settings::EngineSettings state{};
    auto& options = state.mod_options;
    options.snap_override_key = settings::option_keys[0].code;
    options.autoclick_key = settings::option_keys[8].code;
    options.rotate_build_key = settings::option_keys[15].code;
    options.patrol = {0, 1, 2};
    options.guard = {0, 1, 2};
    options.mex_snap_most = 6;
    options.wreck_snap_most = 4;
    options.mex_snap_radius = 3;
    options.wreck_snap_radius = 2;
    return state;
}

/// The mod options dialog opened over mod_settings, with defaults that
/// differ from them.
settings::Dialog opened_mod_options(Page page, const settings::Locks& locks = {}) {
    settings::EngineSettings defaults = mod_settings();
    defaults.mod_options.mex_snap_radius = 6;
    defaults.mod_options.wreck_snap_radius = 4;
    defaults.mod_options.full_rings = true;
    defaults.wheel_zoom = !defaults.wheel_zoom;
    settings::Dialog dialog;
    settings::open_mod_options_dialog(dialog, mod_settings(), defaults, locks, "v0.2.0", page);
    return dialog;
}

constexpr renderer::Rgb kRule{0x2b, 0x30, 0x27};
constexpr renderer::Rgb kText{0xe7, 0xe8, 0xdf};
constexpr renderer::Rgb kHint{0x9a, 0xa1, 0x90};
constexpr renderer::Rgb kWell{0x12, 0x14, 0x10};
constexpr renderer::Rgb kControlBorder{0x3a, 0x40, 0x34};
constexpr renderer::Rgb kControlHover{0x5b, 0x63, 0x52};
constexpr renderer::Rgb kSwitchIdle{0x7d, 0x84, 0x74};
constexpr renderer::Rgb kLock{0xe0, 0xb0, 0x4f};

/// Returns a colour faded into the panel as a locked row is: 115 of 256
/// parts panel.
renderer::Rgb faded(renderer::Rgb color) {
    constexpr uint32_t fade = 115;
    renderer::Rgb mixed{};
    for (std::size_t channel = 0; channel < mixed.size(); ++channel)
        mixed[channel] = static_cast<uint8_t>(
            (color[channel] * (256U - fade) + kPanel[channel] * fade + 128U) / 256U
        );
    return mixed;
}

/// Returns a font whose every printable glyph is a solid block 3 columns
/// wide and 8 rows high, so that every text draws pixels in its whole colour.
settings::DialogFonts block_fonts() {
    renderer::TextFont font;
    font.font.nominal_height = 8;
    constexpr uint8_t ink_index = 1;
    for (int32_t byte = ' '; byte < 0x7f; ++byte) {
        const bool space = byte == ' ';
        font.font.glyphs[static_cast<std::size_t>(byte)] = oa::formats::fnt::Glyph{
            3,
            8,
            0,
            0,
            std::vector<uint8_t>(24, ink_index),
            std::vector<uint8_t>(24, space ? 0 : 1),
        };
    }
    font.ink[ink_index] = renderer::blend_opaque;
    return {font, font};
}

/// The rows as draw_dialog places them with these fonts: a real font wraps
/// hints to the section and may move a row, so a pixel probe follows them.
///
/// @param dialog the dialog
/// @param fonts the fonts the dialog is drawn with
/// @return the open section
geometry::ScrolledRows
rows_as_drawn(const settings::Dialog& dialog, const settings::DialogFonts& fonts) {
    const geometry::TextWidth regular = [&fonts](std::string_view text) {
        return settings::dialog_text_width(fonts, settings::DialogFont::regular, text);
    };
    const geometry::TextWidth small = [&fonts](std::string_view text) {
        return settings::dialog_text_width(fonts, settings::DialogFont::small, text);
    };
    const geometry::Sizes sized = geometry::fitted_sizes(dialog, &regular, &small);
    return geometry::open_rows(dialog, &small, &sized);
}

/// Tells whether a colour is painted anywhere in a rectangle.
///
/// @param canvas the picture
/// @param rect the rectangle
/// @param colour the colour
/// @return true when some pixel of it is that colour
bool rect_has(const Canvas& canvas, const renderer::SourceRect& rect, renderer::Rgb colour) {
    if (rect.width <= 0 || rect.height <= 0)
        return false;
    const int32_t left = std::max(rect.x, int32_t{0});
    const int32_t top_row = std::max(rect.y, int32_t{0});
    const int32_t right = std::min(rect.x + rect.width, static_cast<int32_t>(canvas.surface.width));
    const int32_t bottom =
        std::min(rect.y + rect.height, static_cast<int32_t>(canvas.surface.height));
    for (int32_t y = top_row; y < bottom; ++y)
        for (int32_t x = left; x < right; ++x)
            if (canvas.at(x, y) == colour)
                return true;
    return false;
}

/// The row of a setting in an open section, or null.
///
/// @param open the section
/// @param setting the setting
/// @return the row
const geometry::Row* row_of(const geometry::ScrolledRows& open, Setting setting) {
    for (const geometry::Row& row : open.rows.rows)
        if (row.setting == setting)
            return &row;
    return nullptr;
}

void the_dialog_draws_the_scroll_bar_and_clips_the_rows() {
    const auto fonts = block_fonts();
    // A section draws a scroll bar when its rows are taller than the view,
    // and none when they fit, whatever offset it holds. The height is the
    // dialog's own, so a section that grows on one system is checked as that
    // system lays it out. System is checked both ways: the file-type
    // switches, and the text row. Developer's list scrolls in its own view,
    // and that view's bar is the one this checks.
    for (const Page page : kPages) {
        const int layouts = page == Page::system ? 2 : 1;
        for (int layout = 0; layout < layouts; ++layout) {
            settings::Dialog dialog = opened(page);
            if (page == Page::system)
                dialog.file_type_switches = layout == 0;
            // The thumb follows the rows as the fonts lay them out.
            const auto placed = rows_as_drawn(dialog, fonts);
            Canvas canvas = blank(settings::dialog_width, settings::dialog_height);
            settings::draw_dialog(canvas.surface, {0, 0, 1}, dialog, fonts, kNoIcon);
            CHECK(canvas.at(300, geometry::first_row_top) == kRule);
            if (placed.limit == 0) {
                CHECK(canvas.at(473, 150) == kPanel);
                Canvas steady = blank(settings::dialog_width, settings::dialog_height);
                dialog.scroll[static_cast<std::size_t>(page)] = 50;
                settings::draw_dialog(steady.surface, {0, 0, 1}, dialog, fonts, kNoIcon);
                CHECK(steady.surface.rgb == canvas.surface.rgb);
                continue;
            }
            const auto thumb = geometry::scroll_thumb(
                placed.area, placed.scroll, placed.limit, placed.content_height
            );
            const int32_t sample_x = placed.area.well.x + placed.area.well.width / 2;
            const int32_t sample_y = placed.area.well.y + placed.area.well.height / 2;
            const bool on_thumb = sample_y >= thumb.y && sample_y < thumb.y + thumb.height;
            CHECK(canvas.at(sample_x, sample_y) == (on_thumb ? kControlHover : kWell));
            Canvas moved = blank(settings::dialog_width, settings::dialog_height);
            dialog.scroll[static_cast<std::size_t>(page)] = placed.limit;
            settings::draw_dialog(moved.surface, {0, 0, 1}, dialog, fonts, kNoIcon);
            CHECK(moved.surface.rgb != canvas.surface.rgb);
        }
    }

    // At the top: the well with its border, the thumb in its top 174 rows.
    // On a canvas taller than the dialog, nothing of the rows below the view
    // is drawn under the dialog.
    Scrolling five(five_rows());
    Canvas top = blank(settings::dialog_width, 400);
    settings::draw_dialog(top.surface, {0, 0, 1}, five.dialog, fonts, kNoIcon);
    CHECK(top.at(470, 100) == kControlBorder);
    CHECK(top.at(476, 100) == kControlBorder);
    CHECK(top.at(473, 54) == kControlBorder);
    CHECK(top.at(473, 289) == kControlBorder);
    CHECK(top.at(473, 100) == kControlHover);
    CHECK(top.at(473, 228) == kControlHover);
    // The thumb grows when a real font wraps the hints onto fewer lines.
    // The pixel under it is the well, unless the thumb fills the well.
    {
        const auto drawn = rows_as_drawn(five.dialog, fonts);
        const auto thumb =
            geometry::scroll_thumb(drawn.area, drawn.scroll, drawn.limit, drawn.content_height);
        const int32_t under = thumb.y + thumb.height;
        const int32_t mid_x = thumb.x + thumb.width / 2;
        if (under < drawn.area.well.y + drawn.area.well.height - 1)
            CHECK(top.at(mid_x, under) == kWell);
        else
            CHECK(top.at(mid_x, thumb.y + thumb.height / 2) == kControlHover);
    }
    CHECK(top.at(469, 100) == kPanel);
    CHECK(top.at(477, 100) == kPanel);
    CHECK(top.at(300, geometry::first_row_top) == kRule);
    CHECK(top.at(440, 330) == (renderer::Rgb{0, 0, 0}));
    // A hint in the view is drawn; one that would cross the view's bottom is
    // not drawn below it.
    {
        const auto drawn = rows_as_drawn(five.dialog, fonts);
        const renderer::SourceRect& seen_rows = drawn.area.view;
        bool saw_hint = false;
        for (const geometry::Row& row : drawn.rows.rows) {
            for (std::size_t line = 0; line < row.hint_lines; ++line) {
                renderer::SourceRect box = row.hints[line];
                if (box.y >= seen_rows.y + seen_rows.height || box.y + box.height <= seen_rows.y)
                    continue;
                if (box.y < seen_rows.y) {
                    box.height -= seen_rows.y - box.y;
                    box.y = seen_rows.y;
                }
                if (box.y + box.height > seen_rows.y + seen_rows.height)
                    box.height = seen_rows.y + seen_rows.height - box.y;
                box.width = std::min(box.width, int32_t{48});
                if (rect_has(top, box, kHint))
                    saw_hint = true;
            }
        }
        CHECK(saw_hint);
        CHECK(top.at(geometry::content_left + 1, seen_rows.y + seen_rows.height) != kHint);
    }
    CHECK(top.at(159, 291) != kHint);

    // Scrolled to 40 with the pointer on the bar: the bar lighter, the thumb
    // at 85, a fixed line at the view's top, and nothing of the rows above
    // the view drawn over the header or beside the heading.
    five.dialog.scroll[static_cast<std::size_t>(Page::graphics)] = 40;
    static_cast<void>(settings::dialog_pointer_move(five.dialog, 473, 150));
    Canvas scrolled = blank(settings::dialog_width, settings::dialog_height);
    settings::draw_dialog(scrolled.surface, {0, 0, 1}, five.dialog, fonts, kNoIcon);
    CHECK(scrolled.at(470, 100) == kControlHover);
    CHECK(scrolled.at(473, 84) == kWell);
    // The thumb, held, is the idle switch colour; the well shows past its ends.
    {
        const auto drawn = rows_as_drawn(five.dialog, fonts);
        const auto thumb =
            geometry::scroll_thumb(drawn.area, drawn.scroll, drawn.limit, drawn.content_height);
        const int32_t mid_x = thumb.x + thumb.width / 2;
        CHECK(scrolled.at(mid_x, thumb.y + thumb.height / 2) == kSwitchIdle);
        if (thumb.y + thumb.height < drawn.area.well.y + drawn.area.well.height - 1)
            CHECK(scrolled.at(mid_x, thumb.y + thumb.height) == kWell);
        if (thumb.y > drawn.area.well.y + 1)
            CHECK(scrolled.at(mid_x, thumb.y - 1) == kWell);
    }
    CHECK(scrolled.at(300, geometry::first_row_top) == kRule);
    CHECK(scrolled.at(300, 14) == kBand);
    CHECK(scrolled.at(160, 30) == kPanel);
    CHECK(scrolled.at(220, 45) == kPanel);
    // The first row's slider line shows from 57, its track filled to the knob.
    CHECK(scrolled.at(160, 57 + geometry::track_offset + 1) == kAccent);

    // Locked: the status row fades its label line only, its status at full
    // strength; the other switch fades whole, its On without the accent,
    // and each padlock is drawn over the fade.
    Section section = five_rows();
    section.locks = {
        {Setting::escape_opens_menu, Lock::in_game},
        {Setting::frame_stats, Lock::command_line},
    };
    section.status = {Setting::escape_opens_menu};
    settings::EngineSettings current{};
    current.frame_stats = true;
    // The check below is the On half's faded control colour. The switch
    // defaults Off, and an Off switch fills that half with the off colour.
    current.escape_opens_menu = true;
    Scrolling locked(std::move(section), current);
    locked.dialog.scroll[static_cast<std::size_t>(Page::graphics)] = 80;
    Canvas canvas = blank(settings::dialog_width, settings::dialog_height);
    settings::draw_dialog(canvas.surface, {0, 0, 1}, locked.dialog, fonts, kNoIcon);
    const auto locked_rows = rows_as_drawn(locked.dialog, fonts);
    const geometry::Row* status = row_of(locked_rows, Setting::escape_opens_menu);
    const geometry::Row* other = row_of(locked_rows, Setting::frame_stats);
    CHECK(status != nullptr && other != nullptr);
    if (status != nullptr && other != nullptr) {
        CHECK(rect_has(canvas, status->label, faded(kText)));
        CHECK(status->hint_lines >= 1 && rect_has(canvas, status->hints[0], kHint));
        CHECK(rect_has(canvas, status->lock_area, kLock));
        CHECK(status->control_area.width > 0);
        CHECK(rect_has(canvas, status->control_area, faded(kControlHover)));
        CHECK(!rect_has(canvas, status->control_area, kControlHover));
        CHECK(!rect_has(canvas, status->control_area, kAccent));
        CHECK(rect_has(canvas, other->label, faded(kText)));
        CHECK(other->hint_lines >= 1 && rect_has(canvas, other->hints[0], faded(kHint)));
        CHECK(rect_has(canvas, other->control_area, faded(kControlHover)));
        CHECK(!rect_has(canvas, other->control_area, kAccent));
        CHECK(rect_has(canvas, other->control_area, faded(kControlBorder)));
    }
    locked.dialog.chosen.frame_stats = false;
    Canvas off = blank(settings::dialog_width, settings::dialog_height);
    settings::draw_dialog(off.surface, {0, 0, 1}, locked.dialog, fonts, kNoIcon);
    const auto off_rows = rows_as_drawn(locked.dialog, fonts);
    const geometry::Row* off_switch = row_of(off_rows, Setting::frame_stats);
    CHECK(off_switch != nullptr && rect_has(off, off_switch->control_area, faded(kOffSelected)));
    CHECK(off_switch != nullptr && rect_has(off, off_switch->control_area, faded(kWell)));

    // With the focus shown on a row, a press held on the bar leaves its
    // outline drawn.
    Scrolling focused(five_rows());
    CHECK(settings::dialog_key(focused.dialog, DialogKey::tab) == DialogAction::redraw);
    CHECK(settings::dialog_pointer_down(focused.dialog, 473, 100) == DialogAction::redraw);
    Canvas held = blank(settings::dialog_width, settings::dialog_height);
    settings::draw_dialog(held.surface, {0, 0, 1}, focused.dialog, fonts, kNoIcon);
    const auto track = focused.rows().rows.rows[0].control_area;
    CHECK(held.at(track.x - geometry::focus_inset, track.y + 4) == kAccent);
    CHECK(held.at(470, 100) == kControlHover);
    CHECK(held.at(473, 100) == kSwitchIdle);
    // So does a press held on the well, which scrolled to the end: the third
    // row's outline is drawn where its slider now lies.
    Scrolling jumped(five_rows());
    for (int32_t press = 0; press < 3; ++press)
        static_cast<void>(settings::dialog_key(jumped.dialog, DialogKey::tab));
    CHECK(settings::dialog_pointer_down(jumped.dialog, 473, 260) == DialogAction::redraw);
    CHECK(jumped.scroll() == 80);
    Canvas well = blank(settings::dialog_width, settings::dialog_height);
    settings::draw_dialog(well.surface, {0, 0, 1}, jumped.dialog, fonts, kNoIcon);
    const auto size_track = rows_as_drawn(jumped.dialog, fonts).rows.rows[2].control_area;
    CHECK(well.at(size_track.x - geometry::focus_inset, size_track.y + 4) == kAccent);
    CHECK(well.at(473, 200) == kSwitchIdle);
}

void the_graphics_page_draws_its_locked_rows() {
    const auto fonts = block_fonts();
    // At the top: the scroll bar, Hardware acceleration's strip whole, Off
    // chosen with the accent and Basic in the well, and its status cut at
    // 289.
    settings::Dialog top =
        graphics_page({}, {}, {settings::AccelerationState::off_by_setting, {}, false});
    Canvas at_top = blank(settings::dialog_width, settings::dialog_height);
    settings::draw_dialog(at_top.surface, {0, 0, 1}, top, fonts, kNoIcon);
    CHECK(at_top.at(473, 100) == kControlHover);
    {
        const auto drawn = rows_as_drawn(top, fonts);
        const geometry::Row* acceleration = row_of(drawn, Setting::hardware_acceleration);
        CHECK(acceleration != nullptr && acceleration->control_area.width > 0);
        if (acceleration != nullptr && acceleration->control_area.width > 0) {
            // Each level is 34 wide. The caption sits in its middle, so the
            // fill is the pixel just inside the segment.
            const renderer::SourceRect& strip = acceleration->control_area;
            constexpr int32_t level_width = 34;
            const int32_t mid = strip.y + strip.height / 2;
            const int32_t origin = strip.x + 1;
            CHECK(at_top.at(origin + 2, mid) == kAccent);
            CHECK(at_top.at(origin + level_width + 2, mid) == kWell);
            CHECK(at_top.at(origin + 2 * level_width + 2, mid) == kWell);
        }
        bool saw_hint = false;
        for (const geometry::Row& row : drawn.rows.rows)
            for (std::size_t line = 0; line < row.hint_lines; ++line) {
                renderer::SourceRect box = row.hints[line];
                box.width = std::min(box.width, int32_t{48});
                if (box.y >= drawn.area.view.y &&
                    box.y + box.height <= drawn.area.view.y + drawn.area.view.height &&
                    rect_has(at_top, box, kHint))
                    saw_hint = true;
            }
        CHECK(saw_hint);
        CHECK(
            at_top.at(geometry::content_left + 1, drawn.area.view.y + drawn.area.view.height) !=
            kHint
        );
    }
    CHECK(at_top.at(159, 291) != kHint);

    // At the end, both locked Not available here: Hardware acceleration's
    // label line faded, its strip kept and faded, its status at full
    // strength; Vertical sync faded whole, its switch kept and On without
    // the accent.
    settings::Locks locks{};
    locks.hardware_acceleration = Lock::unavailable;
    locks.vertical_sync = Lock::unavailable;
    settings::EngineSettings current{};
    current.hardware_acceleration = HardwareAcceleration::full;
    current.vertical_sync = true;
    settings::Dialog dialog =
        graphics_page(current, locks, {settings::AccelerationState::no_usable_card, {}, false});
    dialog.scroll[static_cast<std::size_t>(Page::graphics)] = 80;
    Canvas canvas = blank(settings::dialog_width, settings::dialog_height);
    settings::draw_dialog(canvas.surface, {0, 0, 1}, dialog, fonts, kNoIcon);
    const auto drawn = rows_as_drawn(dialog, fonts);
    const geometry::Row* acceleration = row_of(drawn, Setting::hardware_acceleration);
    const geometry::Row* sync = row_of(drawn, Setting::vertical_sync);
    CHECK(acceleration != nullptr && sync != nullptr);
    if (acceleration != nullptr && sync != nullptr) {
        CHECK(rect_has(canvas, acceleration->label, faded(kText)));
        CHECK(acceleration->hint_lines >= 1 && rect_has(canvas, acceleration->hints[0], kHint));
        CHECK(acceleration->control_area.width > 0);
        CHECK(rect_has(canvas, acceleration->control_area, faded(kControlHover)));
        CHECK(rect_has(canvas, acceleration->control_area, faded(kWell)));
        CHECK(!rect_has(canvas, acceleration->control_area, kAccent));
        CHECK(!rect_has(canvas, acceleration->control_area, kWell));
        CHECK(rect_has(canvas, sync->label, faded(kText)));
        CHECK(sync->hint_lines >= 1 && rect_has(canvas, sync->hints[0], faded(kHint)));
        CHECK(rect_has(canvas, sync->control_area, faded(kControlHover)));
        CHECK(rect_has(canvas, sync->control_area, faded(kControlBorder)));
    }
    // Unlocked, Full and On show the accent, Full's level alone of the strip.
    settings::Dialog open =
        graphics_page(current, {}, {settings::AccelerationState::in_use, {}, false});
    open.scroll[static_cast<std::size_t>(Page::graphics)] = 80;
    Canvas unlocked = blank(settings::dialog_width, settings::dialog_height);
    settings::draw_dialog(unlocked.surface, {0, 0, 1}, open, fonts, kNoIcon);
    const auto unlocked_rows = rows_as_drawn(open, fonts);
    const geometry::Row* full = row_of(unlocked_rows, Setting::hardware_acceleration);
    const geometry::Row* on = row_of(unlocked_rows, Setting::vertical_sync);
    CHECK(full != nullptr && full->control_area.width > 0);
    if (full != nullptr && full->control_area.width > 0) {
        const renderer::SourceRect& strip = full->control_area;
        constexpr int32_t level_width = 34;
        const int32_t mid = strip.y + strip.height / 2;
        const int32_t origin = strip.x + 1;
        CHECK(unlocked.at(origin + 2, mid) == kWell);
        CHECK(unlocked.at(origin + level_width + 2, mid) == kWell);
        CHECK(unlocked.at(origin + 2 * level_width + 2, mid) == kAccent);
    }
    CHECK(on != nullptr && rect_has(unlocked, on->control_area, kAccent));
}

/// Draws Graphics at Large on the smooth form at 1.5x, with no modern
/// fonts so that only its shapes draw: the dialog's 960 by 600 points fill
/// exactly 1440 by 900 pixels, every one of them painted whole, so that no
/// rectangle's edge is blended across two pixels.
void the_dialog_draws_on_the_smooth_form_on_whole_pixels() {
    const auto fonts = block_fonts();
    settings::Dialog dialog = graphics_page();
    dialog.size_class = oa::ui::kit::SizeClass::large;
    auto pixels = oa::ui::paint::make_canvas(1440 + 6, 900 + 6);
    oa::ui::paint::Painter painter(pixels);
    oa::ui::kit::Canvas canvas{};
    canvas.fonts = &fonts;
    canvas.smooth.painter = &painter;
    canvas.smooth.scale_percent = 150;
    settings::draw_dialog(canvas, dialog);
    int64_t whole = 0;
    int64_t partial = 0;
    for (int32_t y = 0; y < pixels.height; ++y)
        for (int32_t x = 0; x < pixels.width; ++x) {
            const auto alpha = oa::ui::paint::pixel_at(pixels, x, y).a;
            whole += alpha == 255 ? 1 : 0;
            partial += alpha != 0 && alpha != 255 ? 1 : 0;
            if (x >= 1440 || y >= 900)
                CHECK(alpha == 0);
        }
    CHECK(whole == int64_t{1440} * 900);
    CHECK(partial == 0);
    // The window's raised edge is two pixels of the light colour at the top
    // left, the panel inside it.
    CHECK(oa::ui::paint::pixel_at(pixels, 1, 1).r == oa::ui::kit::colour::edge_light.r);
    CHECK(oa::ui::paint::pixel_at(pixels, 1439, 899).r == oa::ui::kit::colour::edge_dark.r);
}

// ---- Developer, and Developer Mode's list

namespace profiles = oa::data::mod_profile;

/// The section's place among Dialog::scroll.
constexpr std::size_t kDeveloperScroll = static_cast<std::size_t>(Page::developer);

/// Returns a hack's place among the standard hacks.
///
/// @param id the hack's id
/// @return its place
std::size_t hack_index(std::string_view id) {
    const auto index = profiles::standard_hack_index(id);
    CHECK(index.has_value());
    return index.value_or(0);
}

/// Returns an area's place among developer_areas.
///
/// @param name the area
/// @return its place
std::size_t area_index(std::string_view name) {
    const auto areas = settings::developer_areas();
    for (std::size_t index = 0; index < areas.size(); ++index)
        if (areas[index].name == name)
            return index;
    CHECK(false);
    return 0;
}

/// Opens the dialog on Developer, Developer Mode On or Off, over a
/// profile's hacks.
///
/// @param on Developer Mode is on
/// @param overrides the overrides the settings hold
/// @param profile the profile's hacks; empty for 3.1c's
/// @return the dialog
settings::Dialog developer_dialog(
    bool on,
    std::vector<profiles::HackOverride> overrides = {},
    const std::vector<profiles::HackState>& profile = {}
) {
    settings::EngineSettings current{};
    current.developer_mode = on;
    current.hack_overrides = std::move(overrides);
    settings::Dialog dialog;
    settings::open_dialog(
        dialog,
        current,
        settings::EngineSettings{},
        {},
        "v0.2.0",
        Page::developer,
        {},
        settings::highest_unit_limit,
        profile
    );
    return dialog;
}

/// Opens an area and one of its hacks.
///
/// @param[in,out] dialog the dialog
/// @param hack the hack's id
void open_hack(settings::Dialog& dialog, std::string_view hack) {
    const std::size_t index = hack_index(hack);
    dialog.developer.areas_open[area_index(profiles::standard_hacks()[index]->area)] = 1;
    dialog.developer.hacks_open[index] = 1;
}

/// Returns the list's row that matches, scrolled so that the row's top is
/// the view's first row, as far as the list scrolls.
///
/// @param[in,out] dialog the dialog
/// @param matches tells the row
/// @return the row as placed at the new offset; an empty row when none matches
template <typename Matches>
geometry::ListRow shown_row(settings::Dialog& dialog, Matches matches) {
    const auto open = geometry::open_rows(dialog);
    for (const auto& row : open.list.rows) {
        if (!matches(row))
            continue;
        dialog.scroll[kDeveloperScroll] = row.top + open.scroll - open.area.view.y;
        for (const auto& moved : geometry::open_rows(dialog).list.rows)
            if (matches(moved))
                return moved;
    }
    CHECK(false);
    return {};
}

/// Returns the header of a hack, shown.
///
/// @param[in,out] dialog the dialog
/// @param hack the hack's id
/// @return the row
geometry::ListRow hack_header(settings::Dialog& dialog, std::string_view hack) {
    const std::size_t index = hack_index(hack);
    return shown_row(dialog, [&](const geometry::ListRow& row) {
        return row.kind == geometry::ListRowKind::hack && row.hack == index;
    });
}

/// Returns a parameter's row of a hack, shown.
///
/// @param[in,out] dialog the dialog
/// @param hack the hack's id
/// @param parameter the parameter's index within the hack
/// @param item a list's item or a set's value, list_length, or whole_parameter
/// @return the row
geometry::ListRow parameter_row(
    settings::Dialog& dialog,
    std::string_view hack,
    int32_t parameter,
    int32_t item = geometry::whole_parameter
) {
    const std::size_t index = hack_index(hack);
    return shown_row(dialog, [&](const geometry::ListRow& row) {
        return (row.kind == geometry::ListRowKind::slider ||
                row.kind == geometry::ListRowKind::toggle) &&
               row.hack == index && row.parameter == parameter && row.item == item;
    });
}

/// Returns the point of a switch's half.
///
/// @param area the switch
/// @param on true for its On half
/// @return the point
Point switch_half(const renderer::SourceRect& area, bool on) {
    return {on ? area.x + area.width - 4 : area.x + 4, area.y + area.height / 2};
}

/// Returns a hack's override among the chosen settings'.
///
/// @param dialog the dialog
/// @param hack the hack's id
/// @return the override; null for none
const profiles::HackOverride*
chosen_override(const settings::Dialog& dialog, std::string_view hack) {
    return profiles::find_override(dialog.chosen.hack_overrides, hack);
}

/// Returns the value an override sets for a parameter.
///
/// @param override the override; null for none
/// @param name the parameter's name
/// @return its canonical text; "absent" when it sets none
std::string override_value(const profiles::HackOverride* override, std::string_view name) {
    if (override == nullptr)
        return "absent";
    for (const auto& parameter : override->parameters)
        if (parameter.name == name)
            return profiles::canonical_json(parameter.value);
    return "absent";
}

/// Focuses a row's control and takes a key on it.
///
/// @param[in,out] dialog the dialog
/// @param control the row's control
/// @param key the key
/// @return what it asks of the host
DialogAction key_on(settings::Dialog& dialog, int32_t control, DialogKey key) {
    dialog.focused = control;
    return settings::dialog_key(dialog, key);
}

void developer_mode_lists_every_hack_by_area_closed_at_first() {
    const auto areas = settings::developer_areas();
    const auto hacks = profiles::standard_hacks();
    std::size_t listed = 0;
    for (const auto& area : areas) {
        CHECK(!area.hacks.empty());
        for (const std::size_t hack : area.hacks) {
            CHECK(hacks[hack]->area == area.name);
            ++listed;
        }
    }
    CHECK(listed == hacks.size());
    CHECK(areas.front().name == "ai");
    settings::Dialog dialog = developer_dialog(false);
    CHECK(dialog.developer.profile.size() == hacks.size());
    CHECK(dialog.developer.areas_open.size() == areas.size());
    CHECK(dialog.developer.hacks_open.size() == hacks.size());
    const auto open = geometry::open_rows(dialog);
    CHECK(open.list.rows.size() == areas.size());
    for (std::size_t index = 0; index < open.list.rows.size(); ++index) {
        const auto& row = open.list.rows[index];
        CHECK(row.kind == geometry::ListRowKind::area && !row.open);
        CHECK(row.control == settings::first_hack_list_control + static_cast<int32_t>(index));
        CHECK(
            row.top == open.area.view.y + static_cast<int32_t>(index) * geometry::list_header_height
        );
    }
    CHECK(open.list.rows[0].text == "AI" && open.list.rows[1].text == "Aircraft");
    CHECK(open.list.rows[0].shown == "0 of " + std::to_string(areas[0].hacks.size()) + " on");
    // The closed areas are taller than the list's view: the list scrolls.
    // A hint that wraps past the character budget lowers the list, so the
    // view starts lower than the rule and ends on the same row.
    CHECK(
        open.limit == static_cast<int32_t>(areas.size()) * geometry::list_header_height +
                          geometry::end_gap - open.area.view.height
    );
    CHECK(open.area.view.x == geometry::developer_view.x);
    CHECK(open.area.view.width == geometry::developer_view.width);
    CHECK(open.area.view.y >= geometry::developer_view.y);
    CHECK(
        open.area.view.y + open.area.view.height ==
        geometry::developer_view.y + geometry::developer_view.height
    );
    const auto parts = settings::dialog_layout(dialog);
    const std::string label = geometry::active_only_text(0, hacks.size());
    CHECK(label == "Show Active Only (0/" + std::to_string(hacks.size()) + ")");
    for (const std::string_view text :
         {"Developer",
          "DEVELOPER",
          "Enable Developer Mode",
          "Show performance statistics",
          "Frame and tick times over the battlefield."})
        CHECK(find_part(parts, text, settings::no_control) != nullptr);
    CHECK(shows_wrapped(parts, "Your changes to the profile's hacks apply while on.", dialog));
    CHECK(find_part(parts, label, settings::no_control) != nullptr);
    const auto* restore = find_part(parts, "RESTORE PROFILE VALUES", settings::no_control);
    CHECK(restore != nullptr && restore->control == settings::no_control);
    int32_t bars = 0;
    for (const auto& part : parts)
        if (part.control == settings::scroll_bar_control) {
            ++bars;
            CHECK(same_rect(part.rect, open.area.well));
        }
    CHECK(bars == 1);
    // The section's parts keep their places: its three rows at its top,
    // closer than a section's, Enable Developer Mode first and UI kit
    // gallery last, whose hint takes two lines at Compact; the list between
    // the line under them and its footer's line.
    const auto rows = open.rows.rows;
    CHECK(rows.size() == 3);
    CHECK(rows[0].setting == Setting::developer_mode && rows[1].setting == Setting::frame_stats);
    CHECK(rows[2].setting == Setting::kit_gallery);
    CHECK(rows[0].control == settings::developer_mode_control);
    CHECK(rows[1].control == settings::first_row_control + 1);
    CHECK(rows[2].control == settings::first_row_control + 2);
    // Enable Developer Mode's hint is one character over Compact's budget, so
    // that row is one line taller and the rows under it sit that much lower.
    const int32_t mode_extra =
        static_cast<int32_t>(
            wrapped_hint("Your changes to the profile's hacks apply while on.", dialog).size() - 1
        ) *
        geometry::hint_line_height;
    CHECK(
        rows[0].top == geometry::first_row_top && rows[1].top == 93 + mode_extra &&
        rows[2].top == 132 + mode_extra
    );
    CHECK(same_rect(rows[0].control_area, {415, 59, 52, 16}));
    CHECK(same_rect(rows[1].control_area, {415, 98 + mode_extra, 52, 16}));
    CHECK(rows[1].hint_lines >= 1);
    CHECK(same_rect(rows[1].hints[0], {158, 116 + mode_extra, 309, 12}));
    CHECK(open.rows.bottom == geometry::developer_list_rule + mode_extra);
    CHECK(same_rect(geometry::developer_view, {158, 184, 309, 63}));
    CHECK(same_rect(geometry::active_only_switch, {415, 251, 52, 16}));
    CHECK(same_rect(geometry::restore_profile_button, {158, 271, 142, 17}));
    CHECK(geometry::developer_list_rule == 183 && geometry::developer_footer_rule == 247);
    CHECK(
        geometry::restore_profile_button.y + geometry::restore_profile_button.height <
        geometry::footer_rule_row
    );
}

/// Returns a digest of a dialog's parts, in their order: FNV-1a over each
/// part's rectangle, control, font, tracking and text.
///
/// @param parts the parts
/// @return the digest
uint64_t parts_digest(const std::vector<settings::LayoutPart>& parts) {
    uint64_t digest = 0xcbf29ce484222325ULL;
    const auto fold = [&digest](uint64_t value) {
        for (int byte = 0; byte < 8; ++byte) {
            digest ^= (value >> (8 * byte)) & 0xffU;
            digest *= 0x100000001b3ULL;
        }
    };
    for (const settings::LayoutPart& part : parts) {
        fold(static_cast<uint64_t>(static_cast<uint32_t>(part.rect.x)));
        fold(static_cast<uint64_t>(static_cast<uint32_t>(part.rect.y)));
        fold(static_cast<uint64_t>(static_cast<uint32_t>(part.rect.width)));
        fold(static_cast<uint64_t>(static_cast<uint32_t>(part.rect.height)));
        fold(static_cast<uint64_t>(static_cast<uint32_t>(part.control)));
        fold(static_cast<uint64_t>(part.font));
        fold(static_cast<uint64_t>(static_cast<uint32_t>(part.tracking)));
        for (const char character : part.text)
            fold(static_cast<uint8_t>(character));
        fold(part.text.size());
    }
    return digest;
}

/// UI kit gallery, Developer's third row: its label, its hint (two lines at
/// Compact, one at Regular and Large) and SHOW, which asks for the gallery;
/// the list under the rows moved down by exactly that row's height at every
/// class; and every other section's Compact parts as they were before the
/// row was added.
void the_kit_gallery_row_moves_only_developers_list() {
    // The row's parts at Compact.
    settings::Dialog dialog = opened(Page::developer);
    const auto parts = settings::dialog_layout(dialog);
    const auto placed = geometry::open_rows(dialog);
    const auto& gallery_row = placed.rows.rows[2];
    const int32_t show_control = settings::first_row_control + 2;
    const auto* label = find_part(parts, "UI kit gallery", settings::no_control);
    CHECK(label != nullptr && same_rect(label->rect, gallery_row.label));
    const auto gallery_lines = geometry::break_lines(
        geometry::kit_gallery_hint_text,
        geometry::sizes_of(dialog).hint_line_characters,
        geometry::kit_gallery_hint_most_lines
    );
    CHECK(gallery_lines.size() == gallery_row.hint_lines);
    for (std::size_t line = 0; line < gallery_lines.size() && line < gallery_row.hint_lines;
         ++line) {
        const auto* shown = find_part(parts, gallery_lines[line], settings::no_control);
        CHECK(shown != nullptr && same_rect(shown->rect, gallery_row.hints[line]));
    }
    CHECK(shows_wrapped(parts, geometry::kit_gallery_hint_text, dialog));
    const auto* show = find_part(parts, "SHOW", show_control);
    CHECK(show != nullptr && same_rect(show->rect, gallery_row.control_area));
    // Its hint breaks as the Developer rule assumes at every class.
    for (const auto size_class :
         {oa::ui::kit::SizeClass::compact,
          oa::ui::kit::SizeClass::regular,
          oa::ui::kit::SizeClass::large}) {
        const geometry::Sizes& sized = geometry::sizes_of(size_class);
        const auto lines = geometry::break_lines(
            geometry::kit_gallery_hint_text,
            sized.hint_line_characters,
            geometry::kit_gallery_hint_most_lines
        );
        CHECK(
            lines.size() == geometry::lines_between_words(
                                geometry::kit_gallery_hint_text, sized.hint_line_characters
                            )
        );
        CHECK(lines.size() == (size_class == oa::ui::kit::SizeClass::compact ? 2U : 1U));
    }
    // The list's first part lies exactly the row's height lower than it
    // did with two rows, when the line over the list was at 132, at every
    // class: 51 at Compact, where the hint takes two lines, 39 at the others.
    constexpr int32_t kTwoRowsRule = 132;
    for (const auto size_class :
         {oa::ui::kit::SizeClass::compact,
          oa::ui::kit::SizeClass::regular,
          oa::ui::kit::SizeClass::large}) {
        settings::Dialog sized_dialog = opened(Page::developer);
        sized_dialog.size_class = size_class;
        const auto open = geometry::open_rows(sized_dialog);
        CHECK(open.rows.rows.size() == 3);
        const auto& gallery = open.rows.rows[2];
        const int32_t row_height =
            geometry::developer_row_height +
            (size_class == oa::ui::kit::SizeClass::compact ? geometry::hint_line_height : 0);
        // Enable Developer Mode wraps onto a second line only at Compact.
        const int32_t mode_extra = static_cast<int32_t>(
                                       geometry::lines_between_words(
                                           "Your changes to the profile's hacks apply while on.",
                                           geometry::sizes_of(size_class).hint_line_characters
                                       ) -
                                       1
                                   ) *
                                   geometry::hint_line_height;
        CHECK(gallery.setting == Setting::kit_gallery && gallery.top == kTwoRowsRule + mode_extra);
        CHECK(gallery.height == row_height);
        CHECK(
            !open.list.rows.empty() &&
            open.list.rows[0].top == kTwoRowsRule + 1 + row_height + mode_extra
        );
        CHECK(open.area.view.y == kTwoRowsRule + 1 + row_height + mode_extra);
        CHECK(geometry::sizes_of(size_class).developer_list_rule == kTwoRowsRule + row_height);
    }
    // SHOW asks for the gallery, pressed or by Space, and changes nothing.
    CHECK(click(dialog, centre(show->rect)) == DialogAction::show_gallery);
    CHECK(dialog.chosen == dialog.opened);
    settings::Dialog keyed = opened(Page::developer);
    for (int presses = 0; presses < 3; ++presses)
        CHECK(settings::dialog_key(keyed, DialogKey::tab) == DialogAction::redraw);
    CHECK(keyed.focused == show_control);
    CHECK(settings::dialog_key(keyed, DialogKey::space) == DialogAction::show_gallery);
    // Its button is settings.kit-gallery.show to automation.
    const auto list = geometry::dialog_list(dialog, nullptr);
    CHECK(std::any_of(list.controls.begin(), list.controls.end(), [](const auto& control) {
        return control.name == "settings.kit-gallery.show";
    }));
    // Every other section's Compact parts are those before the row: their
    // digests, as the dialog laid them out without it. A digest that differs
    // is printed, so that a change meant to move these parts can pin them
    // again.
    const std::array<std::pair<Page, uint64_t>, 11> kBefore{{
        {Page::controls, 0x1353c5107226492b},
        {Page::common_tweaks, 0x574d80f303b6ba2c},
        {Page::language, 0x4b044c1ccf4c14cf},
        {Page::graphics, 0xe142c80554355985},
        {Page::user_interface, 0xfb1f3f430da1a9e6},
        {Page::touch, 0x06e04c8c7adeaedf},
        {Page::controller, 0x33bfab27a238bf5b},
        {Page::game_files, 0x91919fcbfa7bd9a4},
        {Page::downloads, 0xe8c219c70e49efd6},
        {Page::updates, 0x44e95e2faa63e655},
        {Page::game_content, 0x8ff6dbe85ecb0582},
    }};
    for (const auto& [page, before] : kBefore) {
        const settings::Dialog other = opened_with_controller(page, true, true, true);
        const uint64_t digest = parts_digest(settings::dialog_layout(other));
        if (digest != before)
            std::cerr << "section " << static_cast<int>(page) << "'s parts digest is 0x" << std::hex
                      << digest << std::dec << '\n';
        CHECK(digest == before);
    }
}

void areas_and_hacks_open_and_close() {
    settings::Dialog dialog = developer_dialog(false);
    auto open = geometry::open_rows(dialog);
    CHECK(click(dialog, centre(open.list.rows[0].label)) == DialogAction::redraw);
    CHECK(dialog.developer.areas_open[0] == 1);
    open = geometry::open_rows(dialog);
    const auto& area = settings::developer_areas()[0];
    CHECK(open.list.rows.size() == settings::developer_areas().size() + area.hacks.size());
    const auto& entry = *profiles::standard_hacks()[area.hacks[0]];
    const auto first = open.list.rows[1];
    CHECK(first.kind == geometry::ListRowKind::hack && first.hack == area.hacks[0]);
    CHECK(first.text == entry.title && first.text == "Attack Wave Size");
    CHECK(!first.on && first.locked && !first.open);
    CHECK(first.control == settings::first_hack_list_control + 1);
    // A press on its header opens it: its id, its summary, its scope and
    // its note.
    CHECK(click(dialog, centre(first.label)) == DialogAction::redraw);
    CHECK(dialog.developer.hacks_open[area.hacks[0]] == 1);
    open = geometry::open_rows(dialog);
    CHECK(open.list.rows[2].kind == geometry::ListRowKind::id);
    CHECK(open.list.rows[2].text == entry.id && open.list.rows[2].control == settings::no_control);
    const auto lines = geometry::summary_lines(entry.summary);
    CHECK(!lines.empty());
    for (std::size_t line = 0; line < lines.size(); ++line) {
        CHECK(open.list.rows[3 + line].kind == geometry::ListRowKind::text);
        CHECK(open.list.rows[3 + line].text == lines[line]);
    }
    CHECK(open.list.rows[3 + lines.size()].kind == geometry::ListRowKind::scope);
    CHECK(open.list.rows[3 + lines.size()].text == "Applies at next match");
    CHECK(open.list.rows[4 + lines.size()].text == "Off: it plays as 3.1c.");
    // A display (view-scope) hack says nothing of the next match.
    open_hack(dialog, "ui.whiteboard");
    open = geometry::open_rows(dialog);
    const std::size_t whiteboard = hack_index("ui.whiteboard");
    for (const auto& row : open.list.rows)
        if (row.hack == whiteboard)
            CHECK(row.kind != geometry::ListRowKind::scope);
    dialog.developer.areas_open[area_index("ui")] = 0;
    // The keys: the focus goes to Enable Developer Mode, Show performance
    // statistics, UI kit gallery's SHOW, then the list's headers; Left
    // closes an open area, Right opens it, Space flips it.
    CHECK(settings::dialog_key(dialog, DialogKey::tab) == DialogAction::redraw);
    CHECK(dialog.focused == settings::developer_mode_control);
    CHECK(settings::dialog_key(dialog, DialogKey::tab) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_row_control + 1);
    CHECK(settings::dialog_key(dialog, DialogKey::tab) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_row_control + 2);
    CHECK(settings::dialog_key(dialog, DialogKey::tab) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_hack_list_control);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::none);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::redraw);
    CHECK(dialog.developer.areas_open[0] == 0);
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::redraw);
    CHECK(dialog.developer.areas_open[0] == 1);
    // Down reaches the open hack's header, whose Space closes it.
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_hack_list_control + 1);
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::redraw);
    CHECK(dialog.developer.hacks_open[area.hacks[0]] == 0);
    // Opening and closing changes no setting.
    CHECK(dialog.chosen == dialog.opened);
}

/// Returns a text with A to Z lowered.
///
/// @param text the text
/// @return it lowered
std::string lowered(std::string_view text) {
    std::string low(text);
    for (char& letter : low)
        if (letter >= 'A' && letter <= 'Z')
            letter = static_cast<char>(letter - 'A' + 'a');
    return low;
}

/// Tells whether titles are in alphabetical order, without regard to case.
///
/// @param titles the titles
/// @return true when each comes before the next
bool alphabetical(const std::vector<std::string>& titles) {
    return std::is_sorted(titles.begin(), titles.end(), [](const auto& left, const auto& right) {
        return lowered(left) < lowered(right);
    });
}

/// Returns the titles the list's area headers show, top to bottom, and
/// each area's hack titles, with every area open.
///
/// @param[out] hacks each area's hack titles, in the list's order
/// @return the areas' titles, in the list's order
std::vector<std::string> listed_titles(std::vector<std::vector<std::string>>& hacks) {
    settings::Dialog dialog = developer_dialog(false);
    dialog.developer.areas_open.assign(dialog.developer.areas_open.size(), 1);
    std::vector<std::string> areas;
    hacks.clear();
    for (const auto& row : geometry::open_rows(dialog).list.rows) {
        if (row.kind == geometry::ListRowKind::area) {
            areas.push_back(row.text);
            hacks.emplace_back();
        } else if (row.kind == geometry::ListRowKind::hack) {
            CHECK(!hacks.empty() && row.text == profiles::standard_hacks()[row.hack]->title);
            if (!hacks.empty())
                hacks.back().push_back(row.text);
        }
    }
    return areas;
}

void hacks_show_their_titles_alphabetically() {
    namespace registry = profiles::registry;
    // Every hack has a title of its own, and every area a name.
    std::set<std::string> titles;
    for (const auto* hack : profiles::standard_hacks()) {
        CHECK(!hack->title.empty() && hack->title != hack->id);
        CHECK(titles.insert(lowered(hack->title)).second);
        CHECK(!registry::area_title(hack->area).empty());
        CHECK(registry::area_title(hack->area) != hack->area);
    }
    CHECK(registry::find_entry("economy.deterministic-wind")->title == "Deterministic Wind");
    CHECK(registry::find_entry("ui.megamap")->title == "Megamap");
    CHECK(registry::find_entry("ui.build-tools")->title == "Build Tools");
    CHECK(registry::find_entry("limits.units-per-player")->title.empty());
    CHECK(registry::area_title("ui") == "Interface" && registry::area_title("ai") == "AI");
    CHECK(registry::area_title("nowhere") == "nowhere");
    CHECK(registry::table().area_titles.size() == settings::developer_areas().size());
    // The areas, and the hacks within each, follow their titles
    // alphabetically, in developer_areas and in the list.
    std::vector<std::string> area_titles;
    for (const auto& area : settings::developer_areas()) {
        CHECK(area.title == registry::area_title(area.name));
        area_titles.emplace_back(area.title);
        std::vector<std::string> hack_titles;
        for (const std::size_t hack : area.hacks)
            hack_titles.emplace_back(profiles::standard_hacks()[hack]->title);
        CHECK(alphabetical(hack_titles));
    }
    CHECK(alphabetical(area_titles));
    std::vector<std::vector<std::string>> hacks;
    const auto areas = listed_titles(hacks);
    CHECK(areas == area_titles);
    CHECK(areas.size() >= 3 && areas[0] == "AI" && areas[1] == "Aircraft");
    CHECK(!areas.empty() && areas.back() == "Weapons");
    for (const auto& titles_of_area : hacks)
        CHECK(!titles_of_area.empty() && alphabetical(titles_of_area));
    // In another language the list follows the names as that language
    // shows them, and the areas keep their places among developer_areas.
    oa::data::languages::InterfaceText catalogue;
    CHECK(catalogue.add(
        "[Weapons]\n{\nde=Aaa Waffen;\n}\n[Whiteboard]\n{\nde=Aaa Tafel;\n}\n"
        "[Build Preview]\n{\nde=Zzz Vorschau;\n}\n"
    ));
    oa::data::languages::set_interface_language(
        &catalogue, *oa::data::languages::find_by_tag("de")
    );
    const auto german = listed_titles(hacks);
    oa::data::languages::set_interface_language(nullptr, oa::data::languages::english());
    CHECK(german.size() == areas.size() && german.front() == "Weapons" && german[1] == "AI");
    const auto interface = static_cast<std::size_t>(
        std::find(german.begin(), german.end(), "Interface") - german.begin()
    );
    CHECK(interface < hacks.size());
    if (interface < hacks.size()) {
        CHECK(hacks[interface].front() == "Whiteboard");
        CHECK(hacks[interface].back() == "Build Preview");
    }
    CHECK(settings::developer_areas().back().name == "weapons");
    CHECK(listed_titles(hacks) == area_titles);
}

void developer_mode_off_shows_the_profile_and_takes_no_change() {
    const std::size_t wave = hack_index("ai.attack-wave-size");
    settings::Dialog dialog =
        developer_dialog(false, {profiles::HackOverride{"ai.attack-wave-size", true, {}}});
    // Off, the kept override is not laid over the profile.
    CHECK(!settings::shown_hacks(dialog)[wave].on);
    CHECK(settings::active_hack_count(dialog) == 0);
    open_hack(dialog, "ai.attack-wave-size");
    auto header = hack_header(dialog, "ai.attack-wave-size");
    CHECK(header.locked && !header.on && header.open);
    CHECK(click(dialog, switch_half(header.toggle, true)) == DialogAction::redraw);
    CHECK(key_on(dialog, header.control, DialogKey::right) == DialogAction::redraw);
    CHECK(dialog.chosen == dialog.opened);
    // Restore profile values takes no press and no focus.
    const Point restore = centre(geometry::restore_profile_button);
    CHECK(settings::dialog_pointer_down(dialog, restore.x, restore.y) == DialogAction::none);
    CHECK(settings::dialog_pointer_up(dialog, restore.x, restore.y) == DialogAction::none);
    dialog.focused = settings::active_only_control;
    CHECK(settings::dialog_key(dialog, DialogKey::tab) == DialogAction::redraw);
    CHECK(dialog.focused == settings::restore_control);
    // Show performance statistics takes a change whatever Developer Mode is.
    const auto developer_rows = geometry::open_rows(dialog).rows.rows;
    const auto& enable = developer_rows[0].control_area;
    const auto& stats = developer_rows[1].control_area;
    CHECK(click(dialog, switch_half(stats, true)) == DialogAction::changed);
    CHECK(dialog.chosen.frame_stats && !dialog.chosen.developer_mode);
    CHECK(click(dialog, switch_half(stats, false)) == DialogAction::changed);
    CHECK(dialog.chosen == dialog.opened);
    // On, the override applies and the count shows it; Off again keeps it.
    CHECK(click(dialog, switch_half(enable, true)) == DialogAction::changed);
    CHECK(dialog.chosen.developer_mode);
    CHECK(settings::shown_hacks(dialog)[wave].on && settings::active_hack_count(dialog) == 1);
    CHECK(
        find_part(
            settings::dialog_layout(dialog),
            geometry::active_only_text(1, profiles::standard_hacks().size()),
            settings::no_control
        ) != nullptr
    );
    const auto profile_parts = settings::dialog_layout(dialog);
    const auto* button = find_part(profile_parts, "RESTORE PROFILE VALUES", 0);
    CHECK(button != nullptr && button->control == settings::restore_profile_control);
    // Show performance statistics, on, stays as it is.
    CHECK(click(dialog, switch_half(stats, true)) == DialogAction::changed);
    CHECK(dialog.chosen.frame_stats && settings::active_hack_count(dialog) == 1);
    CHECK(click(dialog, switch_half(enable, false)) == DialogAction::changed);
    CHECK(dialog.chosen.frame_stats);
    CHECK(!dialog.chosen.developer_mode && dialog.chosen.hack_overrides.size() == 1);
    // The keys set it too.
    CHECK(
        key_on(dialog, settings::developer_mode_control, DialogKey::right) == DialogAction::changed
    );
    CHECK(dialog.chosen.developer_mode);
    CHECK(
        key_on(dialog, settings::developer_mode_control, DialogKey::space) == DialogAction::changed
    );
    CHECK(!dialog.chosen.developer_mode);
}

void hacks_turn_on_and_off_and_their_overrides_follow_the_profile() {
    // A profile with ai.attack-wave-size on at 25.
    auto profile = profiles::base_hack_states();
    const std::size_t wave = hack_index("ai.attack-wave-size");
    profile[wave].on = true;
    profile[wave].values[0] = profiles::make_integer(25);
    settings::Dialog dialog = developer_dialog(true, {}, profile);
    CHECK(settings::active_hack_count(dialog) == 1);
    open_hack(dialog, "ai.attack-wave-size");
    auto header = hack_header(dialog, "ai.attack-wave-size");
    CHECK(header.on && !header.locked);
    // Off: an override that turns it off.
    CHECK(click(dialog, switch_half(header.toggle, false)) == DialogAction::changed);
    const auto* off = chosen_override(dialog, "ai.attack-wave-size");
    CHECK(off != nullptr && !off->on && off->parameters.empty());
    CHECK(!settings::shown_hacks(dialog)[wave].on);
    // On again: the profile's own state, and no override.
    header = hack_header(dialog, "ai.attack-wave-size");
    CHECK(click(dialog, switch_half(header.toggle, true)) == DialogAction::changed);
    CHECK(dialog.chosen.hack_overrides.empty());
    // Its parameter's slider: 1 to 1500 units, at the profile's 25.
    auto units = parameter_row(dialog, "ai.attack-wave-size", 0);
    CHECK(units.kind == geometry::ListRowKind::slider && !units.locked);
    CHECK(units.stops == 1500 && units.stop == 24 && units.shown == "25 units");
    CHECK(key_on(dialog, units.control, DialogKey::right) == DialogAction::changed);
    CHECK(override_value(chosen_override(dialog, "ai.attack-wave-size"), "units") == "26");
    // Back to the profile's value drops the override.
    CHECK(key_on(dialog, units.control, DialogKey::left) == DialogAction::changed);
    CHECK(dialog.chosen.hack_overrides.empty());
    // A press at the track's right end, and a drag back to its left.
    units = parameter_row(dialog, "ai.attack-wave-size", 0);
    const auto& track = units.control_area;
    CHECK(
        settings::dialog_pointer_down(dialog, track.x + track.width - 1, track.y + 5) ==
        DialogAction::changed
    );
    CHECK(override_value(chosen_override(dialog, "ai.attack-wave-size"), "units") == "1500");
    CHECK(
        settings::dialog_pointer_move(dialog, track.x - 20, track.y + 5) == DialogAction::changed
    );
    CHECK(override_value(chosen_override(dialog, "ai.attack-wave-size"), "units") == "1");
    CHECK(settings::dialog_pointer_up(dialog, track.x - 20, track.y + 5) == DialogAction::redraw);

    // A hack the profile has off: turned on, its defaults, and the override
    // holds only what differs from them.
    open_hack(dialog, "ai.patrol-group-size");
    header = hack_header(dialog, "ai.patrol-group-size");
    CHECK(!header.on);
    CHECK(click(dialog, switch_half(header.toggle, true)) == DialogAction::changed);
    const auto* patrol = chosen_override(dialog, "ai.patrol-group-size");
    CHECK(patrol != nullptr && patrol->on && patrol->parameters.empty());
    auto group = parameter_row(dialog, "ai.patrol-group-size", 0);
    CHECK(group.shown == "15 units");
    CHECK(key_on(dialog, group.control, DialogKey::right) == DialogAction::changed);
    CHECK(override_value(chosen_override(dialog, "ai.patrol-group-size"), "units") == "16");
    // Off drops the parameters it set; the override is gone with the profile's state.
    header = hack_header(dialog, "ai.patrol-group-size");
    CHECK(key_on(dialog, header.control, DialogKey::left) == DialogAction::changed);
    CHECK(chosen_override(dialog, "ai.patrol-group-size") == nullptr);
    CHECK(settings::active_hack_count(dialog) == 1);
}

void every_kind_of_parameter_has_its_control() {
    std::vector<profiles::HackOverride> on;
    for (const std::string_view hack :
         {"orders.build-site-kickout",
          "orders.weapons-free-while-busy",
          "veterancy.model",
          "ai.difficulty-names",
          "console.atm-amount",
          "ai.income-multipliers",
          "console.game-speed-range",
          "setup.ai-player-name-format"})
        on.push_back(profiles::HackOverride{std::string{hack}, true, {}});
    settings::Dialog dialog = developer_dialog(true, on);
    for (const auto& override : on)
        open_hack(dialog, override.hack);

    // A boolean: a switch, set by its halves.
    auto kickout = parameter_row(dialog, "orders.build-site-kickout", 1);
    CHECK(kickout.kind == geometry::ListRowKind::toggle && kickout.on && kickout.text == "kickout");
    CHECK(click(dialog, switch_half(kickout.control_area, false)) == DialogAction::changed);
    CHECK(
        override_value(chosen_override(dialog, "orders.build-site-kickout"), "kickout") == "false"
    );
    auto retry = parameter_row(dialog, "orders.build-site-kickout", 2);
    CHECK(retry.stops == 1000 && retry.shown == "20 attempts");

    // A set: a switch for each value, the set kept in the registry's order.
    auto repair = parameter_row(dialog, "orders.weapons-free-while-busy", 0, 4);
    CHECK(repair.kind == geometry::ListRowKind::toggle && repair.text == "repair" && !repair.on);
    CHECK(click(dialog, switch_half(repair.control_area, true)) == DialogAction::changed);
    CHECK(
        override_value(chosen_override(dialog, "orders.weapons-free-while-busy"), "states") ==
        "[\"nanolathe\",\"repair\"]"
    );

    // An ascending list of whole numbers: its length, and each item between
    // its neighbours.
    auto length = parameter_row(dialog, "veterancy.model", 1, geometry::list_length);
    CHECK(length.kind == geometry::ListRowKind::slider && length.text == "Items");
    CHECK(length.stops == 32 && length.stop == 4 && length.shown == "5");
    CHECK(key_on(dialog, length.control, DialogKey::right) == DialogAction::changed);
    CHECK(
        override_value(chosen_override(dialog, "veterancy.model"), "default-thresholds") ==
        "[5,10,15,20,25,26]"
    );
    auto second = parameter_row(dialog, "veterancy.model", 1, 1);
    CHECK(second.text == "Item 2" && second.stops == 9 && second.stop == 4);
    CHECK(second.shown == "10 kills");
    CHECK(key_on(dialog, second.control, DialogKey::left) == DialogAction::changed);
    CHECK(
        override_value(chosen_override(dialog, "veterancy.model"), "default-thresholds") ==
        "[5,9,15,20,25,26]"
    );
    // An int-or-none: none at the first stop.
    auto cap = parameter_row(dialog, "veterancy.model", 6);
    CHECK(cap.stops == 1002 && cap.stop == 0 && cap.shown == "none");
    CHECK(key_on(dialog, cap.control, DialogKey::right) == DialogAction::changed);
    CHECK(override_value(chosen_override(dialog, "veterancy.model"), "damage-dealt-cap") == "0");
    // An enumeration: a slider of its words.
    auto source = parameter_row(dialog, "veterancy.model", 0);
    CHECK(source.stops == 2 && source.stop == 1 && source.shown == "thresholds");

    // A list of distinct words: a word moved in swaps with the item that held it.
    auto easiest = parameter_row(dialog, "ai.difficulty-names", 0, 0);
    CHECK(easiest.shown == "hard" && easiest.stops == 3);
    CHECK(key_on(dialog, easiest.control, DialogKey::left) == DialogAction::changed);
    CHECK(
        override_value(chosen_override(dialog, "ai.difficulty-names"), "names") ==
        "[\"medium\",\"hard\",\"easy\"]"
    );

    // A decimal over a wide range: its stops hold the registry's values.
    auto amount = parameter_row(dialog, "console.atm-amount", 0);
    CHECK(amount.shown == "4294967296000 metal and energy");
    CHECK(amount.stops == 1003 && amount.stop == 6);
    for (int32_t press = 0; press < 5; ++press)
        static_cast<void>(key_on(dialog, amount.control, DialogKey::left));
    CHECK(override_value(chosen_override(dialog, "console.atm-amount"), "amount") == "1000");
    // A list of decimals: each item in tenths.
    auto production = parameter_row(dialog, "ai.income-multipliers", 0, 2);
    CHECK(production.stops == 1001 && production.stop == 40 && production.shown == "4 x income");
    CHECK(key_on(dialog, production.control, DialogKey::right) == DialogAction::changed);
    CHECK(
        override_value(chosen_override(dialog, "ai.income-multipliers"), "production") ==
        "[0.5,1,4.1]"
    );

    // Two parameters a constraint ties: min goes no higher than max.
    auto most = parameter_row(dialog, "console.game-speed-range", 1);
    CHECK(most.stops == 21 && most.stop == 20);
    for (int32_t press = 0; press < 10; ++press)
        static_cast<void>(key_on(dialog, most.control, DialogKey::left));
    auto least = parameter_row(dialog, "console.game-speed-range", 0);
    CHECK(least.stops == 11 && least.stop == 0);
    CHECK(least.shown == "0 speed steps (10 = normal)");
    most = parameter_row(dialog, "console.game-speed-range", 1);
    CHECK(most.stops == 21 && most.stop == 10);

    // A string: a slider of the values the registry gives it.
    auto format = parameter_row(dialog, "setup.ai-player-name-format", 0);
    CHECK(format.stops == 2 && format.stop == 1 && format.shown == "AI:%s %d");
    CHECK(key_on(dialog, format.control, DialogKey::left) == DialogAction::changed);
    CHECK(
        override_value(chosen_override(dialog, "setup.ai-player-name-format"), "format") ==
        "\"AI:%s\""
    );
    // Every override the dialog made is one the resolver lays.
    for (const auto& override : dialog.chosen.hack_overrides) {
        const std::size_t index = hack_index(override.hack);
        const auto state = profiles::overridden_state(
            *profiles::standard_hacks()[index], dialog.developer.profile[index], &override
        );
        const auto parameters =
            oa::data::mod_profile::registry::parameters_of(*profiles::standard_hacks()[index]);
        for (std::size_t at = 0; at < parameters.size(); ++at) {
            profiles::Value normal{};
            CHECK(!oa::data::mod_profile::registry::check_value(
                parameters[at].value, state.values[at], {}, normal
            ));
        }
    }
}

void restore_profile_values_and_show_active_only() {
    settings::Dialog dialog = developer_dialog(
        true,
        {profiles::HackOverride{"ai.attack-wave-size", true, {}},
         profiles::HackOverride{"ui.whiteboard", true, {}}}
    );
    CHECK(settings::active_hack_count(dialog) == 2);
    // Show Active Only lists only the areas and hacks that are on.
    CHECK(click(dialog, switch_half(geometry::active_only_switch, true)) == DialogAction::redraw);
    CHECK(dialog.developer.active_only);
    auto open = geometry::open_rows(dialog);
    CHECK(
        open.list.rows.size() == 2 && open.list.rows[0].text == "AI" &&
        open.list.rows[1].text == "Interface"
    );
    CHECK(
        open.list.rows[0].shown ==
        "1 of " + std::to_string(settings::developer_areas()[0].hacks.size()) + " on"
    );
    CHECK(open.limit == 0);
    dialog.developer.areas_open.assign(dialog.developer.areas_open.size(), 1);
    open = geometry::open_rows(dialog);
    CHECK(open.list.rows.size() == 4);
    CHECK(open.list.rows[1].text == "Attack Wave Size" && open.list.rows[3].text == "Whiteboard");
    // Restore profile values clears every override, at once.
    CHECK(click(dialog, centre(geometry::restore_profile_button)) == DialogAction::changed);
    CHECK(dialog.chosen.hack_overrides.empty() && settings::active_hack_count(dialog) == 0);
    CHECK(geometry::open_rows(dialog).list.rows.empty());
    CHECK(click(dialog, centre(geometry::restore_profile_button)) == DialogAction::redraw);
    // The keys: Space on the filter's switch, and on the button.
    CHECK(key_on(dialog, settings::active_only_control, DialogKey::space) == DialogAction::redraw);
    CHECK(!dialog.developer.active_only);
    CHECK(key_on(dialog, settings::active_only_control, DialogKey::right) == DialogAction::redraw);
    CHECK(dialog.developer.active_only);
    dialog.chosen.hack_overrides.push_back(profiles::HackOverride{"ui.megamap", true, {}});
    CHECK(
        key_on(dialog, settings::restore_profile_control, DialogKey::space) == DialogAction::changed
    );
    CHECK(dialog.chosen.hack_overrides.empty());
    // Cancel puts the overrides the dialog opened with back.
    CHECK(settings::dialog_key(dialog, DialogKey::escape) == DialogAction::cancelled);
    CHECK(dialog.chosen.hack_overrides.size() == 2);
}

void restore_defaults_turns_developer_mode_off_and_keeps_the_overrides() {
    settings::Dialog dialog =
        developer_dialog(true, {profiles::HackOverride{"ai.attack-wave-size", true, {}}});
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(!dialog.chosen.developer_mode);
    CHECK(dialog.chosen.hack_overrides.size() == 1);
}

void the_list_scrolls_and_shows_the_focused_row() {
    settings::Dialog dialog = developer_dialog(true);
    // Page Down and End move the list by its own view.
    const auto first = geometry::open_rows(dialog);
    CHECK(settings::dialog_key(dialog, DialogKey::page_down) == DialogAction::redraw);
    CHECK(dialog.scroll[kDeveloperScroll] == std::min(first.limit, first.area.page_step));
    CHECK(settings::dialog_key(dialog, DialogKey::end) == DialogAction::redraw);
    CHECK(geometry::open_rows(dialog).scroll == first.limit);
    CHECK(settings::dialog_key(dialog, DialogKey::home) == DialogAction::redraw);
    // The wheel over the dialog too.
    CHECK(wheel(dialog, -1.0F) == DialogAction::redraw);
    CHECK(dialog.scroll[kDeveloperScroll] == geometry::wheel_step);
    CHECK(settings::dialog_key(dialog, DialogKey::home) == DialogAction::redraw);
    // The focus brings the last area into view, and back up to the first.
    const int32_t last = settings::first_hack_list_control +
                         static_cast<int32_t>(settings::developer_areas().size()) - 1;
    dialog.focused = settings::developer_mode_control;
    while (dialog.focused != last)
        CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(geometry::open_rows(dialog).scroll == first.limit);
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(dialog.focused == settings::active_only_control);
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(dialog.focused == settings::ok_control);                              // 2D focus (D30)
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::none); // 2D focus (D30)
    CHECK(dialog.focused == settings::ok_control);                              // 2D focus (D30)
    dialog.focused = settings::first_hack_list_control;
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::redraw);
    CHECK(geometry::open_rows(dialog).scroll == 0);
    // The scroll bar takes a drag in the list's own margin.
    const auto rested = geometry::open_rows(dialog);
    const int32_t bar_x = rested.area.well.x + rested.area.well.width / 2;
    CHECK(
        settings::dialog_pointer_down(dialog, bar_x, rested.area.well.y + 2) == DialogAction::redraw
    );
    CHECK(
        settings::dialog_pointer_move(
            dialog, bar_x, rested.area.well.y + rested.area.well.height + 400
        ) == DialogAction::redraw
    );
    CHECK(geometry::open_rows(dialog).scroll == geometry::open_rows(dialog).limit);
    CHECK(
        settings::dialog_pointer_up(
            dialog, bar_x, rested.area.well.y + rested.area.well.height + 400
        ) == DialogAction::redraw
    );
}

void summaries_break_into_lines_the_fonts_hold() {
    for (const auto* hack : profiles::standard_hacks()) {
        const auto lines = geometry::summary_lines(hack->summary);
        CHECK(!lines.empty());
        std::string joined;
        for (const auto& line : lines) {
            CHECK(!line.empty() && line.size() <= geometry::summary_line_characters);
            for (const char letter : line)
                CHECK(letter >= ' ' && letter < 0x7f);
            joined += line;
        }
        std::string expected = geometry::ascii_text(hack->summary);
        std::erase(expected, ' ');
        std::erase(joined, ' ');
        CHECK(joined == expected);
    }
    CHECK(geometry::ascii_text("above 45\xC2\xB0") == "above 45 degrees");
    CHECK(
        geometry::ascii_text(
            "4\xC3\x97 work, a\xC2\xB7"
            "b, \xC2\xB1"
            "100"
        ) == "4x work, a*b, +/-100"
    );
    CHECK(geometry::ascii_text("caf\xC3\xA9!") == "caf?!");
    // Chinese stays, for the modern fonts to draw.
    CHECK(geometry::ascii_text("指挥官 ok") == "指挥官 ok");
    // A word longer than a line breaks after its last slash that fits.
    const auto broken = geometry::summary_lines(
        "Commands +sharemetal/+shareenergy/+setshare*/+shootall/+noshake work."
    );
    CHECK(broken.size() == 2);
    CHECK(broken[0] == "Commands +sharemetal/+shareenergy/+setshare*/");
    CHECK(broken[1] == "+shootall/+noshake work.");
    // A summary in Chinese breaks between whole characters, as the line
    // breaker lets it: 43 bytes, then the six characters left.
    std::string hanzi_summary = "a";
    for (int count = 0; count < 20; ++count)
        hanzi_summary += "\xE4\xB8\xAD";
    const auto hanzi_lines = geometry::summary_lines(hanzi_summary);
    CHECK(hanzi_lines.size() == 2);
    CHECK(hanzi_lines.size() == 2 && hanzi_lines[0] == hanzi_summary.substr(0, 43));
    CHECK(hanzi_lines.size() == 2 && hanzi_lines[1] == hanzi_summary.substr(43));
    CHECK(geometry::list_value_text(profiles::make_string(""), {}) == "\"\"");
}

/// Opens Developer Mode On with every hack on and every area and hack open.
///
/// @return the dialog
settings::Dialog everything_open() {
    std::vector<profiles::HackOverride> on;
    for (const auto* hack : profiles::standard_hacks())
        if (hack->implemented)
            on.push_back(profiles::HackOverride{std::string{hack->id}, true, {}});
    settings::Dialog dialog = developer_dialog(true, on);
    dialog.developer.areas_open.assign(dialog.developer.areas_open.size(), 1);
    dialog.developer.hacks_open.assign(dialog.developer.hacks_open.size(), 1);
    return dialog;
}

void the_open_list_keeps_its_parts_apart() {
    settings::Dialog dialog = everything_open();
    const auto open = geometry::open_rows(dialog);
    const renderer::SourceRect face{
        geometry::edge,
        geometry::edge,
        settings::dialog_width - 2 * geometry::edge,
        settings::dialog_height - 2 * geometry::edge,
    };
    // Every row in the section's columns; a control's focus outline clear of the scroll bar.
    for (const auto& row : open.list.rows) {
        CHECK(row.label.x >= geometry::content_left);
        CHECK(row.label.x + row.label.width <= geometry::content_right);
        if (row.control_area.width > 0)
            CHECK(
                row.control_area.x + row.control_area.width + geometry::focus_inset <
                geometry::developer_scroll.well.x
            );
        if (row.kind == geometry::ListRowKind::slider)
            CHECK(row.stops >= 1 && row.stop >= 0 && row.stop < row.stops);
    }
    // At offsets through the whole list: every part inside the dialog and
    // apart, every part over the list's view wholly in it, and each listed
    // control pressed where it is drawn.
    for (int32_t scroll = 0; scroll <= open.limit + 300; scroll += 300) {
        dialog.scroll[kDeveloperScroll] = std::min(scroll, open.limit);
        const auto parts = settings::dialog_layout(dialog);
        const renderer::SourceRect list_view = geometry::open_rows(dialog).area.view;
        for (std::size_t a = 0; a < parts.size(); ++a) {
            CHECK(parts[a].rect.width > 0 && parts[a].rect.height > 0);
            CHECK(inside(parts[a].rect, face));
            if (overlap(parts[a].rect, list_view))
                CHECK(inside(parts[a].rect, list_view));
            for (std::size_t b = a + 1; b < parts.size(); ++b)
                if (overlap(parts[a].rect, parts[b].rect)) {
                    std::cerr << "overlap: '" << parts[a].text << "' and '" << parts[b].text
                              << "'\n";
                    CHECK(!overlap(parts[a].rect, parts[b].rect));
                }
        }
        for (const auto& part : parts) {
            if (part.control == settings::no_control)
                continue;
            const Point point = centre(part.rect);
            static_cast<void>(settings::dialog_pointer_move(dialog, point.x, point.y));
            CHECK(dialog.hovered == part.control);
        }
    }
}

void the_developer_section_draws_its_parts() {
    const auto fonts = block_fonts();
    // Closed, an area's arrow points right.
    settings::Dialog closed_list = developer_dialog(true);
    Canvas closed_canvas = blank(settings::dialog_width, settings::dialog_height);
    settings::draw_dialog(closed_canvas.surface, {0, 0, 1}, closed_list, fonts, kNoIcon);
    // The list starts at the view the fonts leave, which an estimate of the
    // same rows can place lower. The probe follows the rows that were drawn.
    const auto closed_arrow = rows_as_drawn(closed_list, fonts).list.rows[0].arrow;
    CHECK(closed_canvas.at(closed_arrow.x + 1, closed_arrow.y) == kHint);
    CHECK(closed_canvas.at(closed_arrow.x, closed_arrow.y + 1) == kPanel);
    settings::Dialog dialog = developer_dialog(true);
    dialog.developer.areas_open[0] = 1;
    Canvas canvas = blank(settings::dialog_width, settings::dialog_height);
    settings::draw_dialog(canvas.surface, {0, 0, 1}, dialog, fonts, kNoIcon);
    // Its four lines, Enable Developer Mode's On with the accent, Show
    // performance statistics' and Show Active Only's Off, and the list's
    // scroll bar.
    const auto rows = rows_as_drawn(dialog, fonts).rows.rows;
    CHECK(canvas.at(300, geometry::first_row_top) == kRule);
    CHECK(canvas.at(300, rows[1].top) == kRule);
    CHECK(canvas.at(300, geometry::developer_list_rule) == kRule);
    CHECK(canvas.at(300, geometry::developer_footer_rule) == kRule);
    const auto& enable = rows[0].control_area;
    CHECK(canvas.at(enable.x + enable.width - 3, enable.y + 2) == kAccent);
    const auto& stats = rows[1].control_area;
    CHECK(canvas.at(stats.x + 2, stats.y + 2) == kOffSelected);
    const auto& active = geometry::active_only_switch;
    CHECK(canvas.at(active.x + 2, active.y + 2) == kOffSelected);
    CHECK(canvas.at(473, geometry::developer_view.y + 2) == kControlHover);
    CHECK(canvas.at(470, geometry::developer_view.y + 20) == kControlBorder);
    // Open, it points down. The same drawn rows as the closed arrow above.
    const auto open = rows_as_drawn(dialog, fonts);
    const auto& first = open.list.rows[0];
    CHECK(canvas.at(first.arrow.x, first.arrow.y + 1) == kHint);
    // Nothing of the list is drawn below its view.
    for (const auto& row : open.list.rows)
        if (row.top >= geometry::developer_footer_rule)
            CHECK(canvas.at(row.label.x + 1, geometry::developer_footer_rule + 2) == kPanel);
    // Off, a hack's switch fades, its On without the accent.
    settings::Dialog off = developer_dialog(false, {}, [] {
        auto profile = profiles::base_hack_states();
        profile[settings::developer_areas()[0].hacks[0]].on = true;
        return profile;
    }());
    off.developer.areas_open[0] = 1;
    Canvas still = blank(settings::dialog_width, settings::dialog_height);
    settings::draw_dialog(still.surface, {0, 0, 1}, off, fonts, kNoIcon);
    const auto hack = geometry::open_rows(off).list.rows[1];
    CHECK(hack.kind == geometry::ListRowKind::hack && hack.on && hack.locked);
    CHECK(
        still.at(hack.toggle.x + hack.toggle.width - 3, hack.toggle.y + 2) == faded(kControlHover)
    );
}

/// Checks that every row of Developer Mode's list, with everything open and
/// every hack on, fits its places in the game's fonts.
///
/// @param fonts the dialog's fonts
void developer_texts_fit(const settings::DialogFonts& fonts) {
    settings::Dialog dialog = everything_open();
    const auto width = [](const renderer::TextFont& font, std::string_view text) {
        return static_cast<int32_t>(oa::formats::fnt::measure_text(font.font, text));
    };
    const auto fits = [&](const renderer::TextFont& font, std::string_view text, int32_t room) {
        if (width(font, text) <= room)
            return;
        std::cerr << "'" << text << "' is " << width(font, text) << " wide in " << room << '\n';
        CHECK(width(font, text) <= room);
    };
    for (const auto& row : geometry::open_rows(dialog).list.rows) {
        const bool area = row.kind == geometry::ListRowKind::area;
        fits(area ? fonts.regular : fonts.small, row.text, row.label.width);
        if (row.kind == geometry::ListRowKind::slider || area)
            fits(fonts.small, row.shown, row.value.width);
    }
    // Each slider of a hack at each end of its scale.
    for (const auto& row : geometry::open_rows(dialog).list.rows) {
        if (row.kind != geometry::ListRowKind::slider)
            continue;
        const auto same = [&row](const geometry::ListRow& candidate) {
            return candidate.control == row.control;
        };
        for (const bool high : {false, true}) {
            settings::Dialog trial = dialog;
            const auto shown = shown_row(trial, same);
            const auto track = shown.control_area;
            const int32_t x = high ? track.x + track.width - 1 : track.x;
            static_cast<void>(settings::dialog_pointer_down(trial, x, track.y + 5));
            static_cast<void>(settings::dialog_pointer_up(trial, x, track.y + 5));
            const auto placed = shown_row(trial, same);
            fits(fonts.small, placed.shown, placed.value.width);
        }
    }
    const auto hacks = profiles::standard_hacks().size();
    fits(
        fonts.regular, geometry::active_only_text(hacks, hacks), geometry::active_only_label.width
    );
    fits(fonts.small, geometry::restore_profile_text, geometry::restore_profile_button.width - 8);
    for (const auto& area : settings::developer_areas())
        fits(
            fonts.small,
            std::to_string(area.hacks.size()) + " of " + std::to_string(area.hacks.size()) + " on",
            geometry::area_count_width
        );
}

/// Checks that MANAGE\u2026 draws as the other buttons do, in the game's small
/// font with its ellipsis as three full stops, and centred inside its box with
/// clear columns each side at every scale.
///
/// @param fonts the dialog's fonts
void manage_draws_in_the_game_font_inside_its_button(const settings::DialogFonts& fonts) {
    const int32_t width =
        settings::dialog_text_width(fonts, settings::DialogFont::small, "MANAGE…");
    const auto stops =
        static_cast<int32_t>(oa::formats::fnt::measure_text(fonts.small.font, "MANAGE..."));
    CHECK(width == stops);
    CHECK(width + 2 * 3 <= geometry::manage_button_width);
    // The mark between a location's folders, as a greater-than sign.
    CHECK(
        settings::dialog_text_width(
            fonts, settings::DialogFont::small, "On My iPhone › Open Annihilation"
        ) ==
        static_cast<int32_t>(
            oa::formats::fnt::measure_text(fonts.small.font, "On My iPhone > Open Annihilation")
        )
    );
    std::cout << "'MANAGE…' is " << width << " columns in " << geometry::manage_button_width
              << '\n';

    constexpr renderer::Rgb kAccentLight{0xb6, 0xe0, 0x5a};
    constexpr renderer::Rgb kOnAccent{0x10, 0x12, 0x0d};
    for (const bool touch : {false, true}) {
        const settings::Dialog dialog = opened_with_game_files(Page::game_files, touch);
        const auto rows = geometry::open_rows(dialog).rows;
        CHECK(!rows.rows.empty());
        if (rows.rows.empty())
            continue;
        const renderer::SourceRect button = rows.rows.front().control_area;
        CHECK(button.width == geometry::manage_button_width);
        for (const int32_t scale : {1, 2, 3}) {
            Canvas canvas = blank(
                static_cast<uint32_t>(settings::dialog_width * scale),
                static_cast<uint32_t>(settings::dialog_height * scale)
            );
            settings::draw_dialog(canvas.surface, {0, 0, scale}, dialog, fonts, kNoIcon);
            // Inside the button only its face, its edge and its caption, in the
            // text's colour on the face, show.
            int32_t left = button.width * scale;
            int32_t right = -1;
            int32_t top = button.height * scale;
            int32_t bottom = -1;
            bool clean = true;
            for (int32_t y = 0; y < button.height * scale; ++y)
                for (int32_t x = 0; x < button.width * scale; ++x) {
                    const renderer::Rgb seen =
                        canvas.at(button.x * scale + x, button.y * scale + y);
                    if (seen == kAccent || seen == kAccentLight)
                        continue;
                    // The caption's pixels, its letters' edges mixed into the face.
                    left = std::min(left, x);
                    right = std::max(right, x);
                    top = std::min(top, y);
                    bottom = std::max(bottom, y);
                    for (std::size_t channel = 0; channel < seen.size(); ++channel)
                        if (seen[channel] < kOnAccent[channel] ||
                            seen[channel] > kAccent[channel]) {
                            if (clean)
                                std::cerr << "MANAGE… at scale " << scale << ": colour "
                                          << int{seen[0]} << ',' << int{seen[1]} << ','
                                          << int{seen[2]} << " at " << x << ',' << y << '\n';
                            clean = false;
                        }
                }
            CHECK(clean);
            CHECK(right >= left);
            // Clear columns each side, the caption centred as every caption is,
            // by its letters' advances: within the font's two columns of bearing.
            const int32_t before = left;
            const int32_t after = button.width * scale - 1 - right;
            CHECK(before >= 3 * scale && after >= 3 * scale);
            CHECK(std::abs(before - after) <= 2 * scale);
            CHECK(top >= scale && button.height * scale - 1 - bottom >= scale);
            // Nothing of the caption spills past the button's right edge.
            for (int32_t y = 0; y < button.height * scale; ++y)
                for (int32_t x = 0; x < 4 * scale; ++x) {
                    const int32_t column = (button.x + button.width) * scale + x;
                    if (column < static_cast<int32_t>(canvas.surface.width))
                        CHECK(canvas.at(column, button.y * scale + y) != kOnAccent);
                }
            if (!clean || before < 3 * scale || after < 3 * scale ||
                std::abs(before - after) > 2 * scale)
                std::cerr << "MANAGE… at scale " << scale << ": " << before << " and " << after
                          << " clear columns\n";
        }
    }
}

/// The player's own folder as a host gives it to the dialog.
constexpr std::string_view kUserFolder = "/home/player/Documents/Open Annihilation";

/// Returns System with the player's own folder. The text row stands in for
/// the switches, so Your files is the first row on every system.
settings::Dialog your_files_dialog() {
    settings::Dialog dialog = opened(Page::system);
    dialog.file_type_switches = false;
    dialog.user_folder = std::string(kUserFolder);
    return dialog;
}

void your_files_shows_the_folder_and_opens_its_folders() {
    settings::Dialog dialog = your_files_dialog();
    const auto open = geometry::open_rows(dialog);
    CHECK(open.rows.rows.size() == 3);
    const geometry::Row row = open.rows.rows[0];
    CHECK(row.setting == Setting::user_folder);
    CHECK(row.lock == Lock::none);
    CHECK(geometry::kind_of(Setting::user_folder) == geometry::RowKind::buttons);
    CHECK(geometry::kind_of(Setting::user_folder) != geometry::RowKind::toggle);
    // Its three buttons stand on its label line at the right, apart.
    CHECK(row.control_area.width == geometry::folder_buttons_width);
    CHECK(row.control_area.x + row.control_area.width == geometry::content_right);
    CHECK(row.control_area.y == row.label.y);
    CHECK(row.label.x + row.label.width < row.control_area.x);
    const auto parts = settings::dialog_layout(dialog);
    const std::array<std::string_view, 3> captions{"SAVES", "SCREENSHOTS", "MODS"};
    for (std::size_t index = 0; index < captions.size(); ++index) {
        const auto* part = find_part(parts, captions[index], settings::no_control);
        CHECK(part != nullptr && part->control == row.control);
        CHECK(
            part != nullptr &&
            same_rect(part->rect, geometry::folder_button(row.control_area, index))
        );
    }
    CHECK(find_part(parts, "Your files", settings::no_control) != nullptr);
    CHECK(find_part(parts, kUserFolder, settings::no_control) != nullptr);
    CHECK(find_part(parts, geometry::user_folder_hint_text, settings::no_control) != nullptr);
    // A click on each button asks for its folder and marks it.
    for (std::size_t index = 0; index < settings::folder_button_count; ++index) {
        const auto button = geometry::folder_button(row.control_area, index);
        CHECK(click(dialog, centre(button)) == DialogAction::open_folder);
        CHECK(dialog.folder_to_open == static_cast<settings::FolderButton>(index));
        CHECK(dialog.folder_marked == static_cast<settings::FolderButton>(index));
    }
    // A press on one button released on another opens nothing, nor a click
    // between two buttons.
    const auto saves = centre(geometry::folder_button(row.control_area, 0));
    const auto mods = centre(geometry::folder_button(row.control_area, 2));
    static_cast<void>(settings::dialog_pointer_down(dialog, saves.x, saves.y));
    CHECK(settings::dialog_pointer_up(dialog, mods.x, mods.y) == DialogAction::redraw);
    const auto first = geometry::folder_button(row.control_area, 0);
    CHECK(
        click(dialog, {first.x + first.width + geometry::folder_button_gap / 2, first.y + 2}) ==
        DialogAction::redraw
    );
    // The pointer lights the button under it.
    CHECK(settings::dialog_pointer_move(dialog, mods.x, mods.y) != DialogAction::open_folder);
    CHECK(dialog.hovered == row.control && dialog.folder_hovered == 2);
    CHECK(settings::dialog_pointer_move(dialog, saves.x, saves.y) == DialogAction::redraw);
    CHECK(dialog.folder_hovered == 0);
    // Keys: Space presses the marked button; Left and Right move the mark
    // and stop at the ends.
    dialog.folder_marked = settings::FolderButton::saves;
    dialog.focused = row.control;
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::open_folder);
    CHECK(dialog.folder_to_open == settings::FolderButton::saves);
    CHECK(settings::dialog_key(dialog, DialogKey::left) != DialogAction::open_folder);
    CHECK(dialog.folder_marked == settings::FolderButton::saves);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::redraw);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::redraw);
    CHECK(dialog.folder_marked == settings::FolderButton::mods);
    CHECK(settings::dialog_key(dialog, DialogKey::right) != DialogAction::redraw);
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::open_folder);
    CHECK(dialog.folder_to_open == settings::FolderButton::mods);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::redraw);
    CHECK(dialog.folder_marked == settings::FolderButton::screenshots);
    // Enter still keeps the dialog's settings; the row changes none.
    CHECK(dialog.chosen == dialog.opened);
    // A folder that cannot be opened is said on the second hint line, in
    // amber, until a folder opens.
    CHECK(
        settings::set_folder_notice(dialog, "The file manager could not open it.") ==
        DialogAction::redraw
    );
    auto hint = geometry::row_hint(dialog, Setting::user_folder, 1);
    CHECK(hint.notice && hint.text == "The file manager could not open it.");
    CHECK(
        settings::set_folder_notice(dialog, "The file manager could not open it.") ==
        DialogAction::none
    );
    CHECK(settings::set_folder_notice(dialog, {}) == DialogAction::redraw);
    hint = geometry::row_hint(dialog, Setting::user_folder, 1);
    CHECK(!hint.notice && hint.text == geometry::user_folder_hint_text);
    // A long folder shows its tail that fits the hint line.
    dialog.user_folder = "/home/player/" + std::string(30, 'd') + "/" + std::string(30, 'e') + "/" +
                         std::string(kUserFolder.substr(1));
    const auto long_parts = settings::dialog_layout(dialog);
    const settings::LayoutPart* tail = nullptr;
    for (const auto& part : long_parts)
        if (part.text.starts_with(geometry::path_ellipsis) &&
            part.text.ends_with("Open Annihilation"))
            tail = &part;
    CHECK(tail != nullptr);
    CHECK(
        tail != nullptr &&
        one_a_character(tail->text) * settings::estimated_character_width <= tail->rect.width
    );
    // Restore defaults leaves the folder and the mark alone.
    dialog.focused = settings::no_control;
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.folder_marked == settings::FolderButton::screenshots);
}

/// While the dialog's words are drawn in the modern fonts, whose ideographs
/// stand as tall as a hint line, a hint's two lines lie three rows further
/// apart, so that one line's letters, outline and shadow keep clear of the
/// next's; a one-line hint, and every hint in a language the game's fonts
/// draw, keeps its place.
void hint_lines_part_while_the_modern_fonts_draw_the_words() {
    const auto game_fonts = geometry::place_rows(Page::controls, {});
    static oa::data::languages::Language modern{};
    modern.tag = "en-XA";
    modern.needs = oa::data::languages::TextNeeds::modern_fonts;
    oa::data::languages::set_interface_language(nullptr, modern);
    const auto tall = geometry::place_rows(Page::controls, {});
    oa::data::languages::set_interface_language(nullptr, oa::data::languages::english());
    CHECK(game_fonts.rows.size() == 8 && tall.rows.size() == 8);
    if (game_fonts.rows.size() != 8 || tall.rows.size() != 8)
        return;
    // Mouse wheel zoom: one line, in the same place.
    CHECK(tall.rows[0].hint_lines == 1 && tall.rows[0].height == game_fonts.rows[0].height);
    // Maximum zoom out: two lines, 12 rows apart beside the game's fonts
    // and 15 beside the modern fonts; the row is 3 rows taller.
    const auto& escape = game_fonts.rows[1];
    const auto& parted = tall.rows[1];
    CHECK(escape.hint_lines == 2 && parted.hint_lines == 2);
    CHECK(escape.hints[1].y == escape.hints[0].y + 12);
    CHECK(parted.hints[0].y == escape.hints[0].y && parted.hints[1].y == parted.hints[0].y + 15);
    CHECK(parted.height == escape.height + 3 && tall.rows[2].top == game_fonts.rows[2].top + 3);
}

/// While the dialog's words are drawn in the modern fonts, the view's top
/// edge cuts no hint line at the end of a section's scroll: ideographs fill
/// a hint line from the row over it to its last row, so a cut line would
/// leave a sliver of them under the edge.
void hint_lines_stay_whole_at_the_end_of_a_section() {
    static oa::data::languages::Language modern{};
    modern.tag = "en-XA";
    modern.needs = oa::data::languages::TextNeeds::modern_fonts;
    oa::data::languages::set_interface_language(nullptr, modern);
    for (const Page page :
         {Page::controls,
          Page::common_tweaks,
          Page::language,
          Page::graphics,
          Page::touch,
          Page::controller}) {
        settings::Dialog dialog = opened_with_controller(page, true, true);
        dialog.scroll[static_cast<std::size_t>(page)] = std::numeric_limits<int32_t>::max();
        const auto open = geometry::open_rows(dialog);
        const int32_t edge = open.area.view.y;
        for (const auto& row : open.rows.rows)
            for (std::size_t line = 0; line < row.hint_lines; ++line) {
                const auto& box = row.hints[line];
                CHECK(box.y + box.height <= edge || box.y - 1 >= edge);
            }
    }
    oa::data::languages::set_interface_language(nullptr, oa::data::languages::english());
}

/// A notice as the main menu shows it after the saved games moved.
settings::Notice moved_notice() {
    settings::Notice notice;
    notice.title = "SAVED GAMES MOVED";
    notice.paragraphs = {
        {"3 saved games moved to:", false},
        {"/home/player/Documents/Open Annihilation/Saves", true},
        {"Screenshots, films and mods now go in the same Open Annihilation folder.", false},
    };
    notice.open_caption = "OPEN FOLDER";
    return notice;
}

void the_notice_wraps_its_text_and_places_its_buttons() {
    settings::Notice notice = moved_notice();
    const int32_t height = settings::notice_height(notice);
    CHECK(height >= settings::least_notice_height && height <= settings::greatest_notice_height);
    const auto parts = settings::notice_layout(notice);
    const renderer::SourceRect whole{0, 0, settings::notice_width, height};
    for (std::size_t a = 0; a < parts.size(); ++a) {
        CHECK(inside(parts[a].rect, whole));
        for (std::size_t b = a + 1; b < parts.size(); ++b)
            CHECK(!overlap(parts[a].rect, parts[b].rect));
    }
    CHECK(find_part(parts, "SAVED GAMES MOVED", settings::no_control) != nullptr);
    CHECK(find_part(parts, "3 saved games moved to:", settings::no_control) != nullptr);
    CHECK(find_part(parts, {}, settings::notice_ok_control) != nullptr);
    CHECK(find_part(parts, {}, settings::notice_open_control) != nullptr);
    // The path's lines, put together, give the path whole, each in the
    // regular font.
    std::string path;
    for (const auto& part : parts)
        if (part.font == settings::DialogFont::regular && part.control == settings::no_control &&
            !part.text.empty() && part.text != "SAVED GAMES MOVED")
            path += part.text;
    CHECK(path == "/home/player/Documents/Open Annihilation/Saves");
    // A failure adds its amber line and the notice grows.
    notice.failure = "The file manager could not be opened.";
    CHECK(settings::notice_height(notice) > height);
    CHECK(
        find_part(settings::notice_layout(notice), notice.failure, settings::no_control) != nullptr
    );

    // A long path breaks after its separators, a long component within it,
    // and a long text between its words.
    const auto width = [](std::string_view text) { return one_a_character(text); };
    const std::string deep = "/home/player/" + std::string(25, 'x') + "/Open Annihilation/Saves";
    const auto lines = oa::ui::kit::wrap_path(deep, 20, width);
    std::string joined;
    for (const auto& line : lines) {
        CHECK(one_a_character(line) <= 20);
        joined += line;
    }
    CHECK(joined == deep);
    CHECK(lines.size() >= 4 && lines[0] == "/home/player/");
    const auto words = oa::ui::kit::wrap("Screenshots, films and mods now go here.", 16, width);
    CHECK(words.size() == 3);
    CHECK(words[0] == "Screenshots," && words[1] == "films and mods" && words[2] == "now go here.");
    CHECK(oa::ui::kit::wrap("", 16, width).empty());
    CHECK(oa::ui::kit::wrap(std::string(40, 'w'), 16, width).size() == 3);
    // Chinese breaks between its characters, never before a full-width comma.
    CHECK(
        (oa::ui::kit::wrap("建造完成，单位已就绪", 4, width) ==
         std::vector<std::string>{"建造完", "成，单位", "已就绪"})
    );
}

void the_notice_answers_its_buttons_and_keys() {
    settings::Notice notice = moved_notice();
    const int32_t height = settings::notice_height(notice);
    const auto parts = settings::notice_layout(notice);
    const auto ok = centre(find_part(parts, {}, settings::notice_ok_control)->rect);
    const auto opener = centre(find_part(parts, {}, settings::notice_open_control)->rect);
    const auto press = [&](Point at) {
        static_cast<void>(settings::notice_pointer_move(notice, at.x, at.y, height));
        static_cast<void>(settings::notice_pointer_down(notice, at.x, at.y, height));
        return settings::notice_pointer_up(notice, at.x, at.y, height);
    };
    CHECK(press(opener) == settings::NoticeAction::open_folder);
    CHECK(press(ok) == settings::NoticeAction::closed);
    // A press on one button released on the other does nothing; a press
    // off the buttons holds nothing.
    static_cast<void>(settings::notice_pointer_down(notice, opener.x, opener.y, height));
    CHECK(
        settings::notice_pointer_up(notice, ok.x, ok.y, height) == settings::NoticeAction::redraw
    );
    CHECK(settings::notice_pointer_down(notice, 5, 5, height) == settings::NoticeAction::none);
    CHECK(settings::notice_pointer_up(notice, 5, 5, height) == settings::NoticeAction::none);
    // A finger just under OK takes it, and its release where it landed
    // closes the notice; a finger far from both buttons holds nothing.
    const auto ok_rect = find_part(parts, {}, settings::notice_ok_control)->rect;
    const Point under_ok{ok.x, ok_rect.y + ok_rect.height - 1 + 5};
    static_cast<void>(settings::notice_finger_down(notice, under_ok.x, under_ok.y, height, 22));
    CHECK(notice.pressed == settings::notice_ok_control);
    CHECK(
        settings::notice_pointer_up(notice, under_ok.x, under_ok.y, height) ==
        settings::NoticeAction::closed
    );
    CHECK(settings::notice_finger_down(notice, 5, 5, height, 22) == settings::NoticeAction::none);
    CHECK(settings::notice_pointer_up(notice, 5, 5, height) == settings::NoticeAction::none);
    // The pointer lights the button under it.
    CHECK(
        settings::notice_pointer_move(notice, ok.x, ok.y, height) == settings::NoticeAction::redraw
    );
    CHECK(notice.hovered == settings::notice_ok_control);
    CHECK(
        settings::notice_pointer_move(notice, ok.x, ok.y, height) == settings::NoticeAction::none
    );
    // Enter and Escape close it; Space presses the marked button, OK at
    // first; the arrows and Tab move the mark.
    CHECK(settings::notice_key(notice, DialogKey::enter) == settings::NoticeAction::closed);
    CHECK(settings::notice_key(notice, DialogKey::escape) == settings::NoticeAction::closed);
    CHECK(notice.marked == settings::notice_ok_control);
    CHECK(settings::notice_key(notice, DialogKey::space) == settings::NoticeAction::closed);
    CHECK(settings::notice_key(notice, DialogKey::tab) == settings::NoticeAction::redraw);
    CHECK(notice.marked == settings::notice_open_control);
    CHECK(settings::notice_key(notice, DialogKey::space) == settings::NoticeAction::open_folder);
    CHECK(settings::notice_key(notice, DialogKey::left) == settings::NoticeAction::redraw);
    CHECK(notice.marked == settings::notice_ok_control);
    CHECK(settings::notice_key(notice, DialogKey::yes) == settings::NoticeAction::none);
    CHECK(settings::notice_key(notice, DialogKey::page_down) == settings::NoticeAction::none);
}

void the_notice_draws_in_the_dialogs_colours(const settings::DialogFonts& fonts) {
    const settings::Notice notice = moved_notice();
    const int32_t height = settings::notice_height(notice, &fonts);
    Canvas canvas = blank(settings::notice_width, static_cast<uint32_t>(height));
    settings::draw_notice(canvas.surface, {0, 0, 1}, notice, fonts, kNoIcon);
    CHECK(canvas.at(300, 3) == kBand);          // the header
    CHECK(canvas.at(300, height - 4) == kBand); // the footer
    CHECK(canvas.at(3, 30 + 1) == kPanel);      // the text's panel
    const auto parts = settings::notice_layout(notice, &fonts);
    const auto ok = find_part(parts, {}, settings::notice_ok_control);
    CHECK(canvas.at(ok->rect.x + 2, ok->rect.y + 2) == kAccent);
    // OK is marked: a green ring round it.
    CHECK(canvas.at(ok->rect.x - 2, ok->rect.y + 5) == kAccent);
}

void fonts_load_and_every_text_fits_its_place() {
    auto assets = oa::test::require_game_assets("the settings dialog's fonts");
    const auto fonts = settings::load_dialog_fonts(assets);
    CHECK(oa::formats::fnt::measure_text(fonts.regular.font, "OK") > 0);
    CHECK(oa::formats::fnt::measure_text(fonts.small.font, "OK") > 0);
    CHECK(fonts.small.font.nominal_height < fonts.regular.font.nominal_height);

    std::vector<settings::EngineSettings> states = level_states();
    settings::EngineSettings widest{};
    widest.unit_limit = settings::highest_unit_limit;
    widest.path_search_nodes = 8 * settings::base_path_search_nodes;
    states.push_back(widest);
    for (const Page page : kPages) {
        for (const auto& locks : lock_states()) {
            for (const auto& state : states) {
                settings::Dialog dialog = opened(page, locks);
                dialog.chosen = state;
                for (const auto& part : settings::dialog_layout(dialog)) {
                    if (part.text.empty())
                        continue;
                    const auto& font = part.font == settings::DialogFont::regular
                                           ? fonts.regular.font
                                           : fonts.small.font;
                    const auto width =
                        static_cast<int32_t>(oa::formats::fnt::measure_text(font, part.text)) +
                        part.tracking * static_cast<int32_t>(part.text.size() - 1);
                    if (width > part.rect.width || font.nominal_height > part.rect.height) {
                        std::cerr << "'" << part.text << "' is " << width << " wide in a box "
                                  << part.rect.width << " wide\n";
                        CHECK(width <= part.rect.width);
                        CHECK(font.nominal_height <= part.rect.height);
                    }
                }
            }
        }
    }
    for (const Page page : kModPages) {
        settings::Locks locks{};
        locks.mex_snap = Lock::set_by_mod;
        locks.wreck_snap = Lock::set_by_mod;
        for (const auto& lock_state : {settings::Locks{}, locks}) {
            settings::Dialog dialog = opened_mod_options(page, lock_state);
            for (const auto& part : settings::dialog_layout(dialog)) {
                if (part.text.empty())
                    continue;
                const auto& font = part.font == settings::DialogFont::regular ? fonts.regular.font
                                                                              : fonts.small.font;
                const auto width =
                    static_cast<int32_t>(oa::formats::fnt::measure_text(font, part.text)) +
                    part.tracking * static_cast<int32_t>(part.text.size() - 1);
                if (width > part.rect.width) {
                    std::cerr << "'" << part.text << "' is " << width << " wide in a box "
                              << part.rect.width << " wide\n";
                    CHECK(width <= part.rect.width);
                }
            }
        }
    }
    the_dialog_draws_its_faces_and_accents(fonts);

    // The Touch section, with each way of its strips and at every offset,
    // and the list with Touch in it: every text fits its place.
    for (const auto drag : settings::touch_drag_choices)
        for (const auto latches : settings::touch_latches_choices) {
            settings::Dialog dialog = opened_with_touch(Page::touch);
            dialog.chosen.touch_drag = drag;
            dialog.chosen.touch_latches = latches;
            dialog.chosen.touch_hold_ms = settings::highest_touch_hold_ms;
            const int32_t limit = geometry::open_rows(dialog).limit;
            for (int32_t scroll = 0; scroll <= limit; ++scroll) {
                dialog.scroll[static_cast<std::size_t>(Page::touch)] = scroll;
                for (const auto& part : settings::dialog_layout(dialog)) {
                    if (part.text.empty())
                        continue;
                    const auto& font = part.font == settings::DialogFont::regular
                                           ? fonts.regular.font
                                           : fonts.small.font;
                    const auto width =
                        static_cast<int32_t>(oa::formats::fnt::measure_text(font, part.text)) +
                        part.tracking * static_cast<int32_t>(part.text.size() - 1);
                    if (width > part.rect.width || font.nominal_height > part.rect.height) {
                        std::cerr << "'" << part.text << "' is " << width << " wide in a box "
                                  << part.rect.width << " wide\n";
                        CHECK(width <= part.rect.width);
                        CHECK(font.nominal_height <= part.rect.height);
                    }
                }
            }
        }
    for (const std::string_view text : {"Automatic", "Scroll", "Stay on", "One action"})
        std::cout << "'" << text << "' is "
                  << oa::formats::fnt::measure_text(fonts.small.font, text) << " columns\n";

    // The Controller section, with each way of its strips and drop-downs,
    // with and without the Steam Input notice and its drop-down lists open,
    // at every offset; and Graphics on a Steam Deck: every text fits its place.
    const auto every_text_fits = [&fonts](const settings::Dialog& dialog) {
        for (const auto& part : settings::dialog_layout(dialog)) {
            if (part.text.empty())
                continue;
            const auto& font =
                part.font == settings::DialogFont::regular ? fonts.regular.font : fonts.small.font;
            const auto width =
                static_cast<int32_t>(oa::formats::fnt::measure_text(font, part.text)) +
                part.tracking * static_cast<int32_t>(part.text.size() - 1);
            if (width > part.rect.width || font.nominal_height > part.rect.height) {
                std::cerr << "'" << part.text << "' is " << width << " wide in a box "
                          << part.rect.width << " wide\n";
                CHECK(width <= part.rect.width);
                CHECK(font.nominal_height <= part.rect.height);
            }
        }
    };
    {
        namespace pad = oa::ui::pad_controls;
        std::vector<settings::EngineSettings> pad_states;
        for (const auto scheme : {pad::Scheme::trackpads, pad::Scheme::sticks})
            for (const auto stick :
                 {pad::RightStick::zoom_and_pages,
                  pad::RightStick::pointer,
                  pad::RightStick::nothing})
                for (const auto trackpad :
                     {pad::RightTrackpad::relative, pad::RightTrackpad::absolute}) {
                    settings::EngineSettings state{};
                    state.pad_scheme = scheme;
                    state.pad_right_stick = stick;
                    state.pad_right_trackpad = trackpad;
                    state.pad_gyro = pad::Gyro::right_stick_touched;
                    state.pad_prompts = pad::Prompts::playstation;
                    state.pad_pointer_speed = pad::highest_pointer_speed;
                    state.pad_gyro_speed = pad::highest_gyro_speed;
                    state.touch_hold_ms = settings::highest_touch_hold_ms;
                    state.touch_latches = settings::TouchLatches::one_action;
                    pad_states.push_back(state);
                }
        for (const auto& state : pad_states)
            for (const bool steam_input : {false, true}) {
                settings::Dialog dialog =
                    opened_with_controller(Page::controller, true, true, true);
                dialog.chosen = state;
                static_cast<void>(settings::set_controller_section(dialog, true, steam_input));
                const int32_t limit = geometry::open_rows(dialog).limit;
                for (int32_t scroll = 0; scroll <= limit; ++scroll) {
                    dialog.scroll[static_cast<std::size_t>(Page::controller)] = scroll;
                    every_text_fits(dialog);
                }
            }
        // Each drop-down's list, open over the section.
        for (const Setting setting : {Setting::pad_gyro, Setting::pad_prompts}) {
            settings::Dialog dialog = opened_with_controller(Page::controller);
            const auto open = geometry::open_rows(dialog);
            for (const auto& row : open.rows.rows)
                if (row.setting == setting) {
                    dialog.scroll[static_cast<std::size_t>(Page::controller)] =
                        geometry::scroll_showing(
                            open,
                            static_cast<std::size_t>(row.control - settings::first_row_control)
                        );
                    dialog.focused = row.control;
                }
            static_cast<void>(settings::dialog_key(dialog, DialogKey::space));
            CHECK(dialog.open_list != settings::no_control);
            every_text_fits(dialog);
        }
        for (const uint32_t rate : {60U, 90U}) {
            settings::Dialog deck = opened(Page::graphics);
            deck.steam_deck_panel_hz = rate;
            every_text_fits(deck);
        }
        // Smooth motion each way, scrolled into view with its hint.
        for (const bool smooth : {true, false}) {
            settings::Dialog motion = opened(Page::graphics);
            motion.chosen.smooth_motion = smooth;
            motion.scroll[static_cast<std::size_t>(Page::graphics)] = smooth_motion_in_view;
            every_text_fits(motion);
        }
        // Explosion flash at each level, scrolled into view with its hint.
        for (const auto level : settings::explosion_flash_choices) {
            settings::Dialog flash = opened(Page::graphics);
            flash.chosen.explosion_flash = level;
            flash.scroll[static_cast<std::size_t>(Page::graphics)] = explosion_flash_in_view;
            every_text_fits(flash);
        }
        // Fog of War each way, scrolled into view with its hint.
        for (const auto way : settings::fog_of_war_choices) {
            settings::Dialog fog = opened(Page::graphics);
            fog.chosen.fog_of_war = way;
            fog.scroll[static_cast<std::size_t>(Page::graphics)] = fog_of_war_in_view;
            every_text_fits(fog);
        }
        // Window frame each way, on User Interface with its hint.
        for (const auto way : settings::window_frame_choices) {
            settings::Dialog frame = opened(Page::user_interface);
            frame.chosen.window_frame = way;
            frame.scroll[static_cast<std::size_t>(Page::user_interface)] = 0;
            every_text_fits(frame);
        }
        // HUD scaling each way, on User Interface with its hint.
        for (const bool scaled : {true, false}) {
            settings::Dialog hud = opened(Page::user_interface);
            hud.chosen.hud_scaling = scaled;
            hud.scroll[static_cast<std::size_t>(Page::user_interface)] = 0;
            every_text_fits(hud);
        }
    }

    // A section that scrolls: every text it lists fits its place at every
    // offset, and each lock's text fits beside a kept switch as on a slider.
    Scrolling all(nine_rows());
    const int32_t limit = all.rows().limit;
    for (int32_t scroll = 0; scroll <= limit; ++scroll) {
        all.dialog.scroll[static_cast<std::size_t>(Page::graphics)] = scroll;
        for (const auto& part : settings::dialog_layout(all.dialog)) {
            if (part.text.empty())
                continue;
            const auto& font =
                part.font == settings::DialogFont::regular ? fonts.regular.font : fonts.small.font;
            const auto width =
                static_cast<int32_t>(oa::formats::fnt::measure_text(font, part.text));
            CHECK(width <= part.rect.width);
        }
    }
    // Every status line, for every state, reach and replay, fits a hint
    // line's 309 columns; Hardware acceleration's label fits the 153 beside
    // its lock, and Vertical sync's the 93 beside its lock and kept switch.
    const auto small_width = [&](std::string_view text) {
        return static_cast<int32_t>(oa::formats::fnt::measure_text(fonts.small.font, text));
    };
    for (const auto& status : acceleration_statuses())
        for (std::size_t line = 0; line < 2; ++line) {
            const auto text = geometry::status_line(status, line);
            if (small_width(text) > geometry::content_width) {
                std::cerr << "'" << text << "' is " << small_width(text) << " wide\n";
                CHECK(small_width(text) <= geometry::content_width);
            }
        }
    CHECK(
        static_cast<int32_t>(
            oa::formats::fnt::measure_text(fonts.regular.font, "Hardware acceleration")
        ) <= 153
    );
    CHECK(
        static_cast<int32_t>(oa::formats::fnt::measure_text(fonts.regular.font, "Vertical sync")) <=
        93
    );
    for (const std::string_view text :
         {"Not in use: the game cannot save its files.",
          "Not in use: it needs at least 2 GB of memory.",
          "Basic in use, on another driver: one failed.",
          "Basic in use, with less smoothing: frames were slow.",
          "Basic in use; no smoothing when zoomed out here.",
          "Basic in use.",
          "Off for this game: in a shared game, Basic",
          "It smooths the zoomed-out view.",
          "Here the view is drawn as when it is off.",
          "Each frame waits for the display: no tearing.",
          "Not available here",
          "Off",
          "Basic",
          "Full"})
        std::cout << "'" << text << "' is " << small_width(text) << " columns\n";
    // The Graphics page at every offset, under every lock and with every
    // status: every text it lists fits its place.
    const auto fits = [&](const settings::Dialog& dialog) {
        for (const auto& part : settings::dialog_layout(dialog)) {
            if (part.text.empty())
                continue;
            const auto& font =
                part.font == settings::DialogFont::regular ? fonts.regular.font : fonts.small.font;
            const auto width =
                static_cast<int32_t>(oa::formats::fnt::measure_text(font, part.text)) +
                part.tracking * static_cast<int32_t>(part.text.size() - 1);
            if (width > part.rect.width) {
                std::cerr << "'" << part.text << "' is " << width << " wide in a box "
                          << part.rect.width << " wide\n";
                CHECK(width <= part.rect.width);
            }
        }
    };
    for (const auto& locks : lock_states()) {
        settings::Dialog dialog = graphics_page({}, locks);
        for (int32_t scroll = 0; scroll <= 80; ++scroll) {
            dialog.scroll[static_cast<std::size_t>(Page::graphics)] = scroll;
            fits(dialog);
        }
    }
    for (const auto& status : acceleration_statuses()) {
        settings::GameState unavailable{};
        unavailable.acceleration_unavailable = true;
        unavailable.vertical_sync_unavailable = true;
        for (const auto& locks : {settings::Locks{}, settings::settings_locks(unavailable)}) {
            settings::Dialog dialog = graphics_page({}, locks, status);
            dialog.scroll[static_cast<std::size_t>(Page::graphics)] = 80;
            fits(dialog);
        }
    }
    // Your files: a long folder's tail and the notice an open that failed
    // gives fit their hint lines; the buttons' captions fit the buttons.
    {
        settings::Dialog dialog = your_files_dialog();
        dialog.user_folder = "C:\\Documents and Settings\\A Rather Long User Name\\"
                             "My Documents\\Open Annihilation";
        const auto fits_drawn = [&](const settings::Dialog& shown) {
            for (const auto& part : settings::dialog_layout(shown, &fonts)) {
                if (part.text.empty())
                    continue;
                const auto width = settings::dialog_text_width(fonts, part.font, part.text) +
                                   part.tracking * static_cast<int32_t>(part.text.size() - 1);
                if (width > part.rect.width) {
                    std::cerr << "'" << part.text << "' is " << width << " wide in a box "
                              << part.rect.width << " wide\n";
                    CHECK(width <= part.rect.width);
                }
            }
        };
        fits_drawn(dialog);
        for (const std::string_view notice :
             {"The file manager could not open it.",
              "No file manager is there to open it.",
              "The folder cannot be made."}) {
            dialog.folder_notice = std::string(notice);
            fits_drawn(dialog);
            std::cout << "'" << notice << "' is " << small_width(notice) << " columns\n";
        }
    }
    // The notice of saved games that moved: every line it lists fits the
    // notice in its fonts, and a long path wraps into it whole.
    {
        settings::Notice notice = moved_notice();
        notice.paragraphs[1].text = "C:\\Documents and Settings\\A Rather Long User Name\\"
                                    "My Documents\\Open Annihilation\\Saves";
        notice.failure = "No file manager is there to open it.";
        const int32_t height = settings::notice_height(notice, &fonts);
        CHECK(height < settings::greatest_notice_height);
        std::string path;
        for (const auto& part : settings::notice_layout(notice, &fonts)) {
            if (part.text.empty())
                continue;
            const auto width = settings::dialog_text_width(fonts, part.font, part.text) +
                               part.tracking * static_cast<int32_t>(part.text.size() - 1);
            CHECK(width <= part.rect.width);
            if (part.font == settings::DialogFont::regular &&
                part.control == settings::no_control && part.text != notice.title)
                path += part.text;
        }
        CHECK(path == notice.paragraphs[1].text);
        Canvas canvas = blank(settings::notice_width, static_cast<uint32_t>(height));
        settings::draw_notice(canvas.surface, {0, 0, 1}, notice, fonts, kNoIcon);
        CHECK(canvas.at(300, 3) == kBand);
    }

    // Enhanced anti-aliasing's hint in Full, at each factor and each level.
    for (const uint8_t supersample : {uint8_t{1}, uint8_t{2}, uint8_t{4}, uint8_t{8}, uint8_t{16}})
        for (const auto& state : level_states()) {
            settings::AccelerationStatus in_full{};
            in_full.state = settings::AccelerationState::in_use;
            in_full.asked = HardwareAcceleration::full;
            in_full.full_supersample = supersample;
            settings::Dialog dialog = graphics_page(state, {}, in_full);
            for (const int32_t scroll : {0, 80}) {
                dialog.scroll[static_cast<std::size_t>(Page::graphics)] = scroll;
                fits(dialog);
            }
        }

    for (const Lock lock :
         {Lock::in_game, Lock::set_by_host, Lock::command_line, Lock::unavailable}) {
        Section section = five_rows();
        section.locks = {{Setting::escape_opens_menu, lock}, {Setting::frame_stats, lock}};
        section.status = {Setting::escape_opens_menu};
        Scrolling locked(std::move(section));
        // The locks sit on the line under the labels, past the old end.
        locked.dialog.scroll[static_cast<std::size_t>(Page::graphics)] = locked.rows().limit;
        const auto parts = settings::dialog_layout(locked.dialog);
        int32_t lock_texts = 0;
        for (const auto& part : parts) {
            if (part.text != geometry::lock_text(lock))
                continue;
            ++lock_texts;
            const auto width =
                static_cast<int32_t>(oa::formats::fnt::measure_text(fonts.small.font, part.text));
            CHECK(width <= part.rect.width);
        }
        CHECK(lock_texts == 2);
    }
    developer_texts_fit(fonts);

    // The Game files section with a host's usual texts, and the Language &
    // Text dialog alone: every text fits its place, as the dialog measures
    // it (the characters the game fonts lack in the modern fonts).
    const auto dialog_fits = [&](const settings::Dialog& dialog) {
        for (const auto& part : settings::dialog_layout(dialog)) {
            if (part.text.empty())
                continue;
            const int32_t width = settings::dialog_text_width(fonts, part.font, part.text) +
                                  part.tracking * static_cast<int32_t>(part.text.size() - 1);
            if (width > part.rect.width) {
                std::cerr << "'" << part.text << "' is " << width << " wide in a box "
                          << part.rect.width << " wide\n";
                CHECK(width <= part.rect.width);
            }
        }
    };
    for (const bool touch : {false, true}) {
        settings::Dialog files = opened_with_game_files(Page::game_files, touch);
        files.game_files_summary = "3.1c · Core Contingency · Battle Tactics · music · 1 mod";
        files.game_files_sizes = "1.1 GB · 11 GB free on this tablet";
        files.game_files_location = "In the file manager: On My tablet › Open Annihilation › Total "
                                    "Annihilation";
        dialog_fits(files);
        files.game_files_device.clear();
        dialog_fits(files);
    }
    settings::Dialog language;
    settings::open_language_text_dialog(language, {}, {}, {}, "v0.6.0");
    dialog_fits(language);
    manage_draws_in_the_game_font_inside_its_button(fonts);
}

void the_mod_options_dialog_lists_its_own_sections() {
    const auto pages = settings::dialog_pages(settings::DialogKind::mod_options);
    CHECK(std::equal(pages.begin(), pages.end(), kModPages.begin(), kModPages.end()));
    const auto engine = settings::dialog_pages(settings::DialogKind::engine);
    CHECK(std::equal(engine.begin(), engine.end(), kPages.begin(), kPages.end()));
    // An engine section asked of it opens its first section instead.
    const settings::Dialog fallback = opened_mod_options(Page::graphics);
    CHECK(fallback.kind == settings::DialogKind::mod_options);
    CHECK(fallback.page == Page::mod_keys);
    settings::Dialog dialog = opened_mod_options(Page::mod_keys);
    const auto parts = settings::dialog_layout(dialog);
    CHECK(find_part(parts, "Keys", settings::page_control(Page::mod_keys)) != nullptr);
    CHECK(find_part(parts, "Snap & chat", settings::no_control) != nullptr);
    CHECK(find_part(parts, "Graphics", settings::no_control) == nullptr);
    CHECK(find_part(parts, "Snap override key", settings::no_control) != nullptr);
    CHECK(find_part(parts, "Alt", settings::no_control) != nullptr);
    for (const Page page : kModPages) {
        CHECK(click(dialog, centre(geometry::list_item(page))) == DialogAction::redraw);
        CHECK(dialog.page == page);
        const auto rows = geometry::place_rows(page, {});
        CHECK(rows.rows.size() == settings::page_settings(page).size());
        CHECK(rows.bottom < geometry::footer_rule_row);
        const auto shown = settings::dialog_layout(dialog);
        for (std::size_t a = 0; a < shown.size(); ++a) {
            for (std::size_t b = a + 1; b < shown.size(); ++b)
                CHECK(!overlap(shown[a].rect, shown[b].rect));
        }
    }
}

void the_mod_options_change_only_the_mod_options() {
    settings::Dialog dialog = opened_mod_options(Page::mod_keys);
    // The key sliders step through option_keys.
    dialog.focused = settings::first_row_control;
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.mod_options.snap_override_key == settings::option_keys[1].code);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::redraw);
    CHECK(dialog.chosen.mod_options.snap_override_key == settings::option_keys[0].code);
    CHECK(geometry::value_text(settings::Setting::rotate_build_key, dialog.chosen) == "\\");
    // A three-way choice names its choices.
    settings::EngineSettings state = mod_settings();
    CHECK(geometry::value_text(settings::Setting::patrol_hold, state) == "Reclaim only");
    CHECK(geometry::value_text(settings::Setting::patrol_roam, state) == "Assist only");
    CHECK(geometry::value_text(settings::Setting::guard_maneuver, state) == "Normal");
    CHECK(geometry::value_text(settings::Setting::panel_background, state) == "None");
    // A snap radius runs to the mod's most and no further.
    CHECK(geometry::stops_of(state, settings::Setting::mex_snap_radius) == 7);
    CHECK(geometry::stops_of(state, settings::Setting::wreck_snap_radius) == 5);
    geometry::set_stop(state, settings::Setting::mex_snap_radius, 9);
    CHECK(state.mod_options.mex_snap_radius == 6);
    CHECK(geometry::value_text(settings::Setting::mex_snap_radius, state) == "6 cells");
    state.mod_options.wreck_snap_most = 0;
    CHECK(geometry::stops_of(state, settings::Setting::wreck_snap_radius) == 2);
    geometry::set_stop(state, settings::Setting::wreck_snap_radius, 1);
    CHECK(state.mod_options.wreck_snap_radius == 0);
    // A switch flips with a click.
    CHECK(click(dialog, centre(geometry::list_item(Page::mod_tools))) == DialogAction::redraw);
    const auto rows = geometry::place_rows(Page::mod_tools, {});
    CHECK(click(dialog, centre(rows.rows[1].control_area)) == DialogAction::changed);
    CHECK(dialog.chosen.mod_options.full_rings != dialog.opened.mod_options.full_rings);
    CHECK(geometry::switch_on(dialog.chosen, settings::Setting::full_rings));
    // Restore defaults restores the mod options and keeps the engine's settings.
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.chosen.mod_options.mex_snap_radius == 6);
    CHECK(dialog.chosen.mod_options.wreck_snap_radius == 4);
    CHECK(dialog.chosen.wheel_zoom == dialog.opened.wheel_zoom);
    // Cancel puts back what it opened with.
    CHECK(settings::dialog_key(dialog, DialogKey::escape) == DialogAction::cancelled);
    CHECK(dialog.chosen == dialog.opened);
}

void a_mod_set_snap_radius_is_locked() {
    settings::Locks locks{};
    locks.mex_snap = Lock::set_by_mod;
    settings::Dialog dialog = opened_mod_options(Page::mod_tools, locks);
    const auto parts = settings::dialog_layout(dialog);
    CHECK(find_part(parts, "Set by the mod", settings::no_control) != nullptr);
    const auto rows = geometry::place_rows(Page::mod_tools, locks);
    CHECK(rows.rows[2].lock == Lock::set_by_mod);
    static_cast<void>(click(dialog, centre(rows.rows[2].control_area)));
    CHECK(dialog.chosen.mod_options.mex_snap_radius == 3);
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.chosen.mod_options.mex_snap_radius == 3);
    CHECK(dialog.chosen.mod_options.wreck_snap_radius == 4);
}

/// Walks the focus with Tab once round every control of a dialog that takes
/// it, from the first Tab's.
///
/// @param[in,out] dialog the dialog
/// @return the sections' entries the focus met, in order
std::vector<int32_t> entries_walked(settings::Dialog& dialog) {
    std::vector<int32_t> walked;
    static_cast<void>(settings::dialog_key(dialog, DialogKey::tab));
    const int32_t first = dialog.focused;
    for (int32_t press = 0; press < 64; ++press) {
        if (dialog.focused >= settings::first_page_control &&
            dialog.focused < settings::restore_control)
            walked.push_back(dialog.focused);
        static_cast<void>(settings::dialog_key(dialog, DialogKey::tab));
        if (dialog.focused == first)
            break;
    }
    return walked;
}

void touch_is_listed_only_with_touch_controls() {
    // Without touch controls the engine's settings list their ten sections,
    // with Touch eleven; a mod's options list their five either way.
    const auto engine = settings::dialog_pages(settings::DialogKind::engine);
    CHECK(std::equal(engine.begin(), engine.end(), kPages.begin(), kPages.end()));
    const auto touch = settings::dialog_pages(settings::DialogKind::engine, true);
    CHECK(std::equal(touch.begin(), touch.end(), kTouchPages.begin(), kTouchPages.end()));
    const auto mods = settings::dialog_pages(settings::DialogKind::mod_options, true);
    CHECK(std::equal(mods.begin(), mods.end(), kModPages.begin(), kModPages.end()));

    // The list without Touch lies where it always has: 21 rows an entry from
    // row 36, the line at 256 and Developer under it at the list's foot, at
    // 262.
    constexpr std::array<int32_t, 5> kTops{36, 57, 78, 99, 120};
    for (std::size_t index = 0; index < kTops.size(); ++index)
        CHECK(geometry::list_item(kPages[index]).y == kTops[index]);
    CHECK(geometry::list_divider().y == 256);
    CHECK(geometry::list_item(Page::developer).y == 262);
    CHECK(geometry::list_item(Page::mod_chat).y == 120);
    // With Touch, Touch takes the place after User Interface; the line and
    // Developer stay at the list's foot.
    for (std::size_t index = 0; index < kTops.size(); ++index)
        CHECK(geometry::list_item(kTouchPages[index], true).y == kTops[index]);
    CHECK(geometry::list_item(Page::touch, true).y == 141);
    CHECK(geometry::list_item(Page::developer, true).y == 262);
    CHECK(geometry::list_item(Page::mod_chat, true).y == 120);

    // Each section's entry keeps its number with or without Touch listed;
    // a dialog without Touch has no entry numbered as Touch.
    for (const Page page : kPages) {
        const auto plain = list_entries(opened_with_touch(page, false));
        const auto with_touch = list_entries(opened_with_touch(page));
        CHECK(plain.size() == kPages.size());
        CHECK(with_touch.size() == kTouchPages.size());
        for (std::size_t index = 0; index < plain.size(); ++index) {
            CHECK(plain[index].first == settings::page_control(kPages[index]));
            CHECK(plain[index].first != settings::page_control(Page::touch));
        }
        for (std::size_t index = 0; index < with_touch.size(); ++index)
            CHECK(with_touch[index].first == settings::page_control(kTouchPages[index]));
    }
    const auto parts = settings::dialog_layout(opened_with_touch(Page::common_tweaks, false));
    CHECK(find_part(parts, "Touch", settings::no_control) == nullptr);
    const auto touch_parts = settings::dialog_layout(opened_with_touch(Page::common_tweaks));
    const auto* entry = find_part(touch_parts, "Touch", settings::no_control);
    CHECK(entry != nullptr && entry->control == settings::page_control(Page::touch));

    // Touch opens only where it is listed.
    CHECK(opened_with_touch(Page::touch, false).page == Page::controls);
    CHECK(opened_with_touch(Page::touch).page == Page::touch);
    CHECK(opened(Page::developer).page == Page::developer && !opened(Page::developer).touch);

    // A click on Touch's entry shows it; Developer's entry still shows
    // Developer, where it stands with Touch listed.
    settings::Dialog dialog = opened_with_touch(Page::common_tweaks);
    CHECK(click(dialog, centre(geometry::list_item(Page::touch, true))) == DialogAction::redraw);
    CHECK(dialog.page == Page::touch);
    CHECK(
        click(dialog, centre(geometry::list_item(Page::developer, true))) == DialogAction::redraw
    );
    CHECK(dialog.page == Page::developer);

    // The keyboard focus walks the entries in the list's order, Touch among
    // them.
    settings::Dialog keys = opened_with_touch(Page::common_tweaks);
    const std::vector<int32_t> walked = entries_walked(keys);
    CHECK(walked.size() == kTouchPages.size());
    for (std::size_t index = 0; index < walked.size() && index < kTouchPages.size(); ++index)
        CHECK(walked[index] == settings::page_control(kTouchPages[index]));

    // The touch controls coming on list Touch at once; going off, a dialog
    // showing Touch shows its first section.
    settings::Dialog later = opened_with_touch(Page::graphics, false);
    CHECK(settings::set_touch_controls(later, false) == DialogAction::none);
    CHECK(settings::set_touch_controls(later, true) == DialogAction::redraw);
    CHECK(later.touch && list_entries(later).size() == kTouchPages.size());
    CHECK(click(later, centre(geometry::list_item(Page::touch, true))) == DialogAction::redraw);
    later.focused = settings::first_row_control;
    CHECK(settings::set_touch_controls(later, false) == DialogAction::redraw);
    CHECK(later.page == Page::controls && later.focused == settings::no_control);
    CHECK(list_entries(later).size() == kPages.size());
}

void game_files_is_listed_only_where_the_host_says() {
    // With the flag the engine's settings list Game files after User
    // Interface, or after Touch, and before Developer; a mod's options and
    // Language & Text alone never do.
    const auto plain = settings::dialog_pages(settings::DialogKind::engine, false, false);
    CHECK(std::equal(plain.begin(), plain.end(), kPages.begin(), kPages.end()));
    const auto files = settings::dialog_pages(settings::DialogKind::engine, false, true);
    CHECK(std::equal(files.begin(), files.end(), kGameFilesPages.begin(), kGameFilesPages.end()));
    const auto both = settings::dialog_pages(settings::DialogKind::engine, true, true);
    CHECK(
        std::equal(
            both.begin(), both.end(), kTouchGameFilesPages.begin(), kTouchGameFilesPages.end()
        )
    );
    const auto mods = settings::dialog_pages(settings::DialogKind::mod_options, true, true);
    CHECK(std::equal(mods.begin(), mods.end(), kModPages.begin(), kModPages.end()));
    CHECK(settings::dialog_pages(settings::DialogKind::language_text, true, true).size() == 1);

    // Game files alone keeps the gap. Touch and Game files together drop it:
    // eleven flowing entries fill the room above the divider exactly.
    constexpr std::array<int32_t, 5> kTops{36, 57, 78, 99, 120};
    constexpr std::array<int32_t, 5> kGapless{36, 56, 76, 96, 116};
    for (std::size_t index = 0; index < kTops.size(); ++index) {
        CHECK(geometry::list_item(kGameFilesPages[index], false, true).y == kTops[index]);
        CHECK(geometry::list_item(kTouchGameFilesPages[index], true, true).y == kGapless[index]);
    }
    CHECK(geometry::list_item(Page::game_files, false, true).y == 141);
    CHECK(geometry::list_item(Page::developer, false, true).y == 262);
    CHECK(geometry::list_item(Page::touch, true, true).y == 136);
    CHECK(geometry::list_item(Page::game_files, true, true).y == 156);
    CHECK(
        geometry::list_item(Page::game_files, true, true).y + geometry::list_item_height <
        geometry::list_divider().y
    );
    CHECK(geometry::list_item(Page::developer, true, true).y == 262);
    CHECK(
        geometry::list_item(Page::developer, true, true).y + geometry::list_item_height <
        geometry::footer_rule_row
    );

    // Touch is 5, Controller 6, Developer 7 and Game files 8 in every
    // dialog: a dialog without Touch has no control 5, one without Game
    // files no control 8.
    CHECK(settings::page_control(Page::touch) == 5);
    CHECK(settings::page_control(Page::controller) == 6);
    CHECK(settings::page_control(Page::developer) == 7);
    CHECK(settings::page_control(Page::game_files) == 8);
    for (const bool touch : {false, true}) {
        const auto entries = list_entries(opened_with_game_files(Page::common_tweaks, touch));
        const auto& order = touch ? std::span<const Page>(kTouchGameFilesPages)
                                  : std::span<const Page>(kGameFilesPages);
        CHECK(entries.size() == order.size());
        for (std::size_t index = 0; index < entries.size() && index < order.size(); ++index) {
            CHECK(entries[index].first == settings::page_control(order[index]));
            CHECK(entries[index].second.y == geometry::list_item(order[index], touch, true).y);
        }
    }
    for (const auto& [control, rect] : list_entries(opened_with_touch(Page::common_tweaks)))
        CHECK(control != settings::page_control(Page::game_files));
    const auto parts = settings::dialog_layout(opened_with_game_files(Page::common_tweaks));
    const auto* entry = find_part(parts, "Game files", settings::no_control);
    CHECK(entry != nullptr && entry->control == settings::page_control(Page::game_files));
    CHECK(
        find_part(settings::dialog_layout(opened(Page::common_tweaks)), "Game files", 0) == nullptr
    );

    // Game files opens only where it is listed.
    CHECK(opened(Page::game_files).page == Page::controls);
    CHECK(opened_with_game_files(Page::game_files).page == Page::game_files);
    CHECK(opened_with_game_files(Page::game_files).game_files);

    // A click on its entry shows it; the focus walks the entries in the
    // list's order, Game files among them.
    settings::Dialog dialog = opened_with_game_files(Page::common_tweaks, true);
    CHECK(
        click(dialog, centre(geometry::list_item(Page::game_files, true, true))) ==
        DialogAction::redraw
    );
    CHECK(dialog.page == Page::game_files);
    CHECK(
        click(dialog, centre(geometry::list_item(Page::developer, true, true))) ==
        DialogAction::redraw
    );
    CHECK(dialog.page == Page::developer);
    settings::Dialog keys = opened_with_game_files(Page::common_tweaks, true);
    const std::vector<int32_t> walked = entries_walked(keys);
    CHECK(walked.size() == kTouchGameFilesPages.size());
    for (std::size_t index = 0; index < walked.size() && index < kTouchGameFilesPages.size();
         ++index)
        CHECK(walked[index] == settings::page_control(kTouchGameFilesPages[index]));
    // Touch going off keeps Game files listed.
    CHECK(settings::set_touch_controls(keys, false) == DialogAction::redraw);
    CHECK(list_entries(keys).size() == kGameFilesPages.size());
}

void game_files_shows_what_is_installed_the_backups_and_the_folder() {
    const auto rows = settings::page_settings(Page::game_files);
    CHECK(rows.size() == 3);
    CHECK(rows.size() == 3 && rows[0] == Setting::game_files_summary);
    CHECK(rows.size() == 3 && rows[1] == Setting::game_files_backed_up);
    CHECK(rows.size() == 3 && rows[2] == Setting::game_files_location);
    CHECK(geometry::kind_of(Setting::game_files_summary) == geometry::RowKind::value_and_button);
    CHECK(geometry::kind_of(Setting::game_files_location) == geometry::RowKind::text);
    CHECK(geometry::kind_of(Setting::game_files_backed_up) == geometry::RowKind::toggle);
    CHECK(geometry::kind_of(Setting::game_files_summary) != geometry::RowKind::toggle);
    CHECK(geometry::kind_of(Setting::game_files_location) != geometry::RowKind::toggle);

    // MANAGE… stands at the label line's right; the switch where every
    // switch stands; where the files are has no control. Every row fits the
    // view.
    const auto placed = geometry::place_rows(Page::game_files, {});
    CHECK(placed.rows.size() == 3);
    const auto& summary = placed.rows[0];
    CHECK(summary.control == settings::first_row_control);
    CHECK(summary.control_area.width == geometry::manage_button_width);
    CHECK(summary.control_area.x + summary.control_area.width == geometry::content_right);
    CHECK(summary.control_area.y == summary.label.y);
    CHECK(summary.label.x + summary.label.width + geometry::label_gap == summary.control_area.x);
    // No host text: the summary and the location have nothing to wrap.
    // The backups hint names "device" and wraps to the room.
    CHECK(summary.hint_lines == 0);
    const auto& backups = placed.rows[1];
    CHECK(backups.control_area.width == geometry::switch_width);
    CHECK(backups.hint_lines >= 1);
    const auto& location = placed.rows[2];
    CHECK(location.control_area.width == 0 && location.hint_lines == 0);
    CHECK(geometry::scroll_limit(geometry::content_height(placed, 0)) == 0);

    // The layout: the host's texts, MANAGE… as the first row's control, the
    // switch's halves as the second's, nothing pressable for the third.
    settings::Dialog dialog = opened_with_game_files(Page::game_files);
    const auto hosted = geometry::open_rows(dialog).rows.rows;
    CHECK(hosted.size() == 3);
    CHECK(hosted[0].hint_lines == 2);
    CHECK(hosted[2].hint_lines == 2);
    const auto parts = settings::dialog_layout(dialog);
    for (const std::string_view text :
         {"GAME FILES",
          "Installed",
          "3.1c · Core Contingency · Battle Tactics · music",
          "1.1 GB · 37 GB free on this tablet",
          "Include in device backups",
          "Where the files are",
          "In the file manager: Open Annihilation ›",
          "Total Annihilation"})
        CHECK(find_part(parts, text, settings::no_control) != nullptr);
    CHECK(shows_wrapped(
        parts, "After restoring this tablet from a backup, add the game files again.", dialog
    ));
    const auto* manage = find_part(parts, "MANAGE…", settings::no_control);
    CHECK(manage != nullptr && manage->control == settings::first_row_control);
    CHECK(manage != nullptr && same_entry(manage->rect, summary.control_area));
    const auto* annihilation = find_part(parts, "Total Annihilation", settings::no_control);
    CHECK(annihilation != nullptr && annihilation->control == settings::no_control);
    const auto* off = find_part(parts, "OFF", settings::no_control);
    CHECK(off != nullptr && off->control == settings::first_row_control + 1);
    for (const auto& part : parts)
        CHECK(part.control != settings::first_row_control + 2);
    // No two parts overlap, and each lies inside the dialog.
    const renderer::SourceRect face{
        geometry::edge,
        geometry::edge,
        settings::dialog_width - 2 * geometry::edge,
        settings::dialog_height - 2 * geometry::edge,
    };
    for (std::size_t a = 0; a < parts.size(); ++a) {
        CHECK(inside(parts[a].rect, face));
        for (std::size_t b = a + 1; b < parts.size(); ++b)
            CHECK(!overlap(parts[a].rect, parts[b].rect));
    }
    // Every control is pressed where it is drawn.
    for (const auto& part : parts) {
        if (part.control == settings::no_control)
            continue;
        const Point point = centre(part.rect);
        static_cast<void>(settings::dialog_pointer_move(dialog, point.x, point.y));
        CHECK(dialog.hovered == part.control);
    }
    // A focus outline round MANAGE… keeps clear of the label and its lines.
    const renderer::SourceRect focus{
        hosted[0].control_area.x - geometry::focus_inset,
        hosted[0].control_area.y - geometry::focus_inset,
        hosted[0].control_area.width + 2 * geometry::focus_inset,
        hosted[0].control_area.height + 2 * geometry::focus_inset,
    };
    CHECK(!overlap(focus, hosted[0].label));
    for (std::size_t line = 0; line < hosted[0].hint_lines; ++line)
        CHECK(!overlap(focus, hosted[0].hints[line]));

    // Without the device's name the hint says "device"; the host's empty
    // texts list nothing.
    settings::Dialog neutral = opened_with_game_files(Page::game_files);
    neutral.game_files_device.clear();
    neutral.game_files_sizes.clear();
    const auto neutral_parts = settings::dialog_layout(neutral);
    CHECK(shows_wrapped(
        neutral_parts,
        "After restoring this device from a backup, add the game files again.",
        neutral
    ));
    CHECK(geometry::open_rows(neutral).rows.rows[0].hint_lines == 1);
    CHECK(
        find_part(neutral_parts, "1.1 GB · 37 GB free on this tablet", settings::no_control) ==
        nullptr
    );
    CHECK(geometry::row_hint(neutral, Setting::game_files_summary, 1).text.empty());

    // A long location breaks between words into two lines at most.
    const auto lines = geometry::break_lines(
        "Shown as: On My tablet › Open Annihilation › Total Annihilation", 50, 2
    );
    CHECK(lines.size() == 2);
    CHECK(lines.size() == 2 && lines[0] == "Shown as: On My tablet › Open Annihilation ›");
    CHECK(lines.size() == 2 && lines[1] == "Total Annihilation");
    // The mark in the line's second half is taken over a later space.
    const auto early = geometry::break_lines(
        "In the file manager: On My tablet › Open Annihilation › Total Annihilation", 50, 2
    );
    CHECK(early.size() == 2 && early[0] == "In the file manager: On My tablet ›");
    CHECK(early.size() == 2 && early[1] == "Open Annihilation › Total Annihilation");
    // Without a mark in the line's second half it breaks at its last space.
    const auto plain_words = geometry::break_lines(
        "In the file manager: Open Annihilation's own folder for games", 50, 2
    );
    CHECK(plain_words.size() == 2);
    CHECK(plain_words.size() == 2 && plain_words[1] == "folder for games");
    CHECK(geometry::break_lines("", 50, 2).empty());
    CHECK(geometry::break_lines("short", 50, 2) == std::vector<std::string>{"short"});
    const auto cut = geometry::break_lines(std::string(120, 'x'), 50, 2);
    CHECK(cut.size() == 2 && cut[0].size() == 50 && cut[1].size() == 50);
    const auto words = geometry::break_lines("one two three four", 9, 5);
    CHECK(words == (std::vector<std::string>{"one two", "three", "four"}));
    // Lines break where the line breaker lets them, as every wrap does:
    // Korean at its spaces, each word whole, though "반드시 살" fits five;
    // Chinese between its characters, a full-width comma never starting a
    // line.
    CHECK(
        geometry::break_lines("반드시 살아남아야 합니다", 5, 5) ==
        (std::vector<std::string>{"반드시", "살아남아야", "합니다"})
    );
    CHECK(
        geometry::break_lines("建造完成，单位已就绪", 4, 5) ==
        (std::vector<std::string>{"建造完", "成，单位", "已就绪"})
    );
}

void manage_asks_the_host_and_the_backups_switch_changes_at_once() {
    settings::Dialog dialog = opened_with_game_files(Page::game_files);
    const auto hosted = geometry::open_rows(dialog).rows.rows;
    const auto manage = hosted[0].control_area;
    const auto backups = hosted[1].control_area;
    // A click on MANAGE… asks the host to open the Game files screen and
    // changes no setting; a press released elsewhere asks nothing.
    const auto before = dialog.chosen;
    CHECK(click(dialog, centre(manage)) == DialogAction::manage_game_files);
    CHECK(dialog.chosen == before && dialog.pressed == settings::no_control);
    static_cast<void>(settings::dialog_pointer_down(dialog, manage.x + 2, manage.y + 2));
    CHECK(dialog.pressed == settings::first_row_control);
    CHECK(settings::dialog_pointer_up(dialog, 300, 290) == DialogAction::redraw);
    // A press on where the files are takes nothing.
    const auto location = hosted[2].hints[0];
    CHECK(
        settings::dialog_pointer_down(dialog, location.x + 4, location.y + 4) == DialogAction::none
    );
    CHECK(dialog.pressed == settings::no_control);
    static_cast<void>(settings::dialog_pointer_up(dialog, location.x + 4, location.y + 4));

    // The switch, Off at first, turns on by either half and by the keys, and
    // each change asks the host to put it in effect at once.
    CHECK(!dialog.chosen.game_files_backed_up);
    CHECK(click(dialog, {backups.x + backups.width - 4, backups.y + 4}) == DialogAction::changed);
    CHECK(dialog.chosen.game_files_backed_up);
    CHECK(click(dialog, {backups.x + 4, backups.y + 4}) == DialogAction::changed);
    CHECK(!dialog.chosen.game_files_backed_up);

    // The keys: the focus takes MANAGE…, then the switch, then the footer,
    // never where the files are. Space on MANAGE… asks the host; Left and
    // Right do nothing to it.
    settings::Dialog keys = opened_with_game_files(Page::game_files);
    CHECK(settings::dialog_key(keys, DialogKey::tab) == DialogAction::redraw);
    CHECK(keys.focused == settings::first_row_control);
    CHECK(settings::dialog_key(keys, DialogKey::space) == DialogAction::manage_game_files);
    CHECK(settings::dialog_key(keys, DialogKey::right) == DialogAction::none);
    CHECK(settings::dialog_key(keys, DialogKey::left) == DialogAction::redraw);  // 2D focus (D30)
    CHECK(keys.focused == settings::page_control(Page::common_tweaks));          // 2D focus (D30)
    CHECK(settings::dialog_key(keys, DialogKey::right) == DialogAction::redraw); // 2D focus (D30)
    CHECK(keys.focused == settings::first_row_control);                          // 2D focus (D30)
    CHECK(keys.chosen == before);
    CHECK(settings::dialog_key(keys, DialogKey::tab) == DialogAction::redraw);
    CHECK(keys.focused == settings::first_row_control + 1);
    CHECK(settings::dialog_key(keys, DialogKey::right) == DialogAction::changed);
    CHECK(keys.chosen.game_files_backed_up);
    CHECK(settings::dialog_key(keys, DialogKey::space) == DialogAction::changed);
    CHECK(!keys.chosen.game_files_backed_up);
    CHECK(settings::dialog_key(keys, DialogKey::tab) == DialogAction::redraw);
    CHECK(keys.focused == settings::restore_control);

    // Restore defaults turns the switch Off; Cancel puts back what it
    // opened with.
    settings::Dialog restore = opened_with_game_files(Page::game_files);
    restore.chosen.game_files_backed_up = true;
    CHECK(click(restore, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(!restore.chosen.game_files_backed_up);
    settings::Dialog opened_on;
    settings::EngineSettings on{};
    on.game_files_backed_up = true;
    settings::open_dialog(
        opened_on,
        on,
        {},
        {},
        "v0.2.0",
        Page::game_files,
        {},
        settings::highest_unit_limit,
        {},
        nullptr,
        false,
        true
    );
    const auto opened_switch = geometry::open_rows(opened_on).rows.rows[1].control_area;
    CHECK(click(opened_on, {opened_switch.x + 4, opened_switch.y + 4}) == DialogAction::changed);
    CHECK(click(opened_on, centre(geometry::cancel_button)) == DialogAction::cancelled);
    CHECK(opened_on.chosen.game_files_backed_up);
    // A dialog that does not list Game files keeps the switch through
    // Restore defaults.
    settings::Dialog in_game;
    settings::open_dialog(in_game, on, {}, {}, "v0.2.0", Page::controls);
    CHECK(click(in_game, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(in_game.chosen.game_files_backed_up);

    // A finger beside MANAGE… takes it within reach, and its release where
    // it landed asks the host.
    settings::Dialog finger = opened_with_game_files(Page::game_files);
    const Point below{manage.x + manage.width / 2, manage.y + manage.height + 6};
    static_cast<void>(settings::dialog_finger_down(finger, below.x, below.y, 22));
    CHECK(finger.pressed == settings::first_row_control);
    CHECK(settings::dialog_pointer_up(finger, below.x, below.y) == DialogAction::manage_game_files);
}

/// What the scripted modern fonts drew: every line they were asked for.
std::vector<std::string> modern_lines;

/// Draws a line in the scripted modern fonts: each character a block 5
/// columns wide and 9 rows high over a baseline 9 rows down, the pen
/// moving 6 columns a character.
std::shared_ptr<const oa::present::TextMask>
scripted_modern_draw(void*, std::string_view text, oa::present::TextFace, int32_t scale, int32_t) {
    modern_lines.emplace_back(text);
    auto mask = std::make_shared<oa::present::TextMask>();
    std::size_t characters = 0;
    for (std::size_t at = 0; at < text.size(); ++at)
        if ((static_cast<unsigned char>(text[at]) & 0xc0) != 0x80)
            ++characters;
    const int32_t advance = 6 * scale;
    mask->width = std::max<int32_t>(1, static_cast<int32_t>(characters) * advance);
    mask->height = 12 * scale;
    mask->baseline = 9 * scale;
    mask->advance = static_cast<int32_t>(characters) * advance;
    mask->alpha.assign(static_cast<std::size_t>(mask->width * mask->height), 0);
    for (std::size_t character = 0; character < characters; ++character) {
        for (int32_t row = 0; row < 9 * scale; ++row)
            for (int32_t column = 0; column < 5 * scale; ++column)
                mask->alpha[static_cast<std::size_t>(
                    row * mask->width + static_cast<int32_t>(character) * advance + column
                )] = 255;
        mask->character_ends.push_back(static_cast<int32_t>(character + 1) * advance);
    }
    return mask;
}

/// The settings the scripted modern fonts draw with: no outline, shadow or
/// background, so that only the letters' colour is laid.
oa::present::TextSettings scripted_text_settings(void*) {
    oa::present::TextSettings settings;
    settings.style.outline = false;
    settings.style.shadow = false;
    settings.style.background = false;
    return settings;
}

void language_text_lists_one_section_and_draws_without_the_game_fonts() {
    settings::Dialog dialog;
    settings::EngineSettings current{};
    current.unit_limit = settings::highest_unit_limit;
    current.text_size = settings::highest_text_size;
    settings::EngineSettings defaults{};
    settings::open_language_text_dialog(dialog, current, defaults, {}, "v0.6");
    CHECK(dialog.kind == settings::DialogKind::language_text);
    CHECK(dialog.page == Page::language);
    CHECK(!dialog.touch && !dialog.game_files);
    // One entry, at the top of the list, keeping its number.
    const auto entries = list_entries(dialog);
    CHECK(entries.size() == 1);
    CHECK(entries.size() == 1 && entries[0].first == settings::page_control(Page::language));
    CHECK(entries.size() == 1 && entries[0].second.y == 36);
    const auto parts = settings::dialog_layout(dialog);
    for (const std::string_view absent : {"Graphics", "Developer", "Game files", "Touch"})
        CHECK(find_part(parts, absent, settings::no_control) == nullptr);
    for (const std::string_view present :
         {"Language & Text", "LANGUAGE & TEXT", "Language", "RESTORE DEFAULTS", "CANCEL", "OK"})
        CHECK(find_part(parts, present, settings::no_control) != nullptr);
    // Where Graphics' entry would be, nothing is pressed.
    const auto graphics = centre(geometry::list_item(Page::graphics));
    CHECK(click(dialog, graphics) == DialogAction::none);
    CHECK(dialog.page == Page::language);
    // The focus walks its rows, the footer and its one entry.
    settings::Dialog keys = dialog;
    std::vector<int32_t> walked;
    for (int32_t press = 0; press < 20; ++press) {
        static_cast<void>(settings::dialog_key(keys, DialogKey::tab));
        if (keys.focused >= settings::first_page_control &&
            keys.focused < settings::restore_control)
            walked.push_back(keys.focused);
    }
    CHECK(!walked.empty());
    for (const int32_t control : walked)
        CHECK(control == settings::page_control(Page::language));
    // Restore defaults restores Language's settings alone.
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.chosen.text_size == defaults.text_size);
    CHECK(dialog.chosen.unit_limit == settings::highest_unit_limit);
    CHECK(dialog.restored);
    // Restore defaults keeps a locked language.
    settings::Dialog locked;
    settings::EngineSettings french{};
    french.language = "fr";
    settings::Locks locks{};
    locks.language = Lock::command_line;
    settings::open_language_text_dialog(locked, french, defaults, locks, "v0.6");
    CHECK(click(locked, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(locked.chosen.language == "fr");

    // With fonts that hold no glyphs, every text is drawn in the modern
    // fonts and measured by them.
    const settings::DialogFonts empty{};
    oa::present::GameTextHooks hooks{};
    hooks.draw = scripted_modern_draw;
    hooks.settings = scripted_text_settings;
    oa::present::set_game_text_hooks(hooks);
    CHECK(settings::dialog_text_width(empty, settings::DialogFont::regular, "Language") == 48);
    CHECK(settings::dialog_text_width(empty, settings::DialogFont::small, "OK") == 12);
    modern_lines.clear();
    Canvas canvas = blank(settings::dialog_width, settings::dialog_height);
    settings::draw_dialog(canvas.surface, {0, 0, 1}, dialog, empty, kNoIcon);
    const auto drew = [](std::string_view text) {
        return std::find(modern_lines.begin(), modern_lines.end(), text) != modern_lines.end();
    };
    CHECK(drew("OPEN ANNIHILATION") || drew("O"));
    CHECK(drew("Language & Text"));
    CHECK(drew("LANGUAGE & TEXT"));
    CHECK(drew("Language"));
    CHECK(drew("OK"));
    CHECK(drew("v0.6"));
    // The label's letters are drawn in the text colour inside its box, the
    // capitals centred as a game font's are.
    const auto label = geometry::place_rows(Page::language, {}).rows[0].label;
    bool letters = false;
    for (int32_t y = label.y; y < label.y + label.height; ++y)
        for (int32_t x = label.x; x < label.x + 48; ++x)
            letters = letters || canvas.at(x, y) == kText;
    CHECK(letters);
    CHECK(canvas.at(label.x + 2, label.y - 1) != kText);
    oa::present::set_game_text_hooks({});
    // Without the modern fonts nothing draws the texts, and nothing fails.
    Canvas bare = blank(settings::dialog_width, settings::dialog_height);
    settings::draw_dialog(bare.surface, {0, 0, 1}, dialog, empty, kNoIcon);
    CHECK(settings::dialog_text_width(empty, settings::DialogFont::regular, "Language") == 0);
}

/// The settings the scripted modern fonts draw with in an outline and a
/// shadow, as the game's text settings choose by default.
oa::present::TextSettings outlined_text_settings(void*) {
    oa::present::TextSettings settings;
    settings.style.outline = true;
    settings.style.shadow = true;
    settings.style.background = false;
    return settings;
}

/// Letters as dark as their outline, as OK's caption on the accent, are
/// drawn bare in the modern fonts: only the accent's face, its lighter edge
/// and the letters lie on the button. Lighter letters keep their outline.
void dark_letters_on_the_accent_are_drawn_bare() {
    settings::Dialog dialog;
    settings::open_language_text_dialog(dialog, {}, {}, {}, "v0.6");
    const settings::DialogFonts empty{};
    oa::present::GameTextHooks hooks{};
    hooks.draw = scripted_modern_draw;
    hooks.settings = outlined_text_settings;
    oa::present::set_game_text_hooks(hooks);
    Canvas canvas = blank(settings::dialog_width, settings::dialog_height);
    settings::draw_dialog(canvas.surface, {0, 0, 1}, dialog, empty, kNoIcon);
    oa::present::set_game_text_hooks({});
    constexpr renderer::Rgb on_accent{0x10, 0x12, 0x0d};
    constexpr renderer::Rgb accent_edge{0xb6, 0xe0, 0x5a};
    const auto ok = geometry::footer_button(settings::ok_control);
    std::size_t letters = 0;
    std::size_t others = 0;
    for (int32_t y = ok.y; y < ok.y + ok.height; ++y)
        for (int32_t x = ok.x; x < ok.x + ok.width; ++x) {
            const auto shown = canvas.at(x, y);
            letters += shown == on_accent ? 1 : 0;
            others += shown != on_accent && shown != kAccent && shown != accent_edge ? 1 : 0;
        }
    CHECK(letters > 0);
    CHECK(others == 0);
    // The light label beside it keeps its dark outline.
    const auto label = geometry::place_rows(Page::language, {}).rows[0].label;
    bool outline = false;
    for (int32_t y = label.y - 2; y < label.y + label.height + 2; ++y)
        for (int32_t x = label.x - 2; x < label.x + 50; ++x) {
            const auto shown = canvas.at(x, y);
            outline = outline || shown == oa::present::text_outline_color ||
                      shown == oa::present::text_dark_outline_color;
        }
    CHECK(outline);
}

/// Clicks a strip's caption where the dialog draws it.
///
/// @param[in,out] dialog the dialog
/// @param control the strip's row's control
/// @param caption the caption
/// @return what the click asks of the host; DialogAction::none when the caption is not shown
DialogAction click_caption_at(settings::Dialog& dialog, int32_t control, std::string_view caption) {
    const auto shown = settings::dialog_layout(dialog);
    for (const auto& part : shown)
        if (part.text == caption && part.control == control)
            return click(dialog, centre(part.rect));
    std::cerr << "no caption '" << caption << "' on control " << control << '\n';
    CHECK(false);
    return DialogAction::none;
}

void touch_shows_its_rows_and_their_values() {
    const auto rows = settings::page_settings(Page::touch);
    CHECK(rows.size() == 6);
    constexpr std::array<Setting, 6> kTouchRows{
        Setting::touch_drag,
        Setting::touch_hold_delay,
        Setting::touch_latches,
        Setting::touch_haptics,
        Setting::touch_left_handed,
        Setting::touch_control_size,
    };
    CHECK(std::equal(rows.begin(), rows.end(), kTouchRows.begin(), kTouchRows.end()));
    CHECK(geometry::kind_of(Setting::touch_drag) == geometry::RowKind::levels);
    CHECK(geometry::kind_of(Setting::touch_latches) == geometry::RowKind::levels);
    CHECK(geometry::kind_of(Setting::touch_hold_delay) == geometry::RowKind::slider);
    CHECK(geometry::kind_of(Setting::touch_haptics) == geometry::RowKind::toggle);
    CHECK(geometry::kind_of(Setting::touch_left_handed) == geometry::RowKind::toggle);
    CHECK(geometry::kind_of(Setting::touch_control_size) == geometry::RowKind::levels);

    settings::Dialog dialog = opened_with_touch(Page::touch);
    // Every part inside the dialog and apart, at every offset; no game lock
    // reaches a Touch row.
    // Its six rows are taller than the view, by 121 rows.
    const auto open = geometry::open_rows(dialog);
    CHECK(open.limit == 121);
    const renderer::SourceRect face{
        geometry::edge,
        geometry::edge,
        settings::dialog_width - 2 * geometry::edge,
        settings::dialog_height - 2 * geometry::edge,
    };
    for (const auto& locks : lock_states()) {
        const auto placed = geometry::place_rows(Page::touch, locks);
        for (const auto& row : placed.rows)
            CHECK(row.lock == Lock::none);
    }
    for (int32_t scroll = 0; scroll <= open.limit; ++scroll) {
        dialog.scroll[static_cast<std::size_t>(Page::touch)] = scroll;
        const auto parts = settings::dialog_layout(dialog);
        for (std::size_t a = 0; a < parts.size(); ++a) {
            CHECK(inside(parts[a].rect, face));
            for (std::size_t b = a + 1; b < parts.size(); ++b)
                CHECK(!overlap(parts[a].rect, parts[b].rect));
        }
    }
    dialog.scroll = {};

    // At its top: the heading, the rows' labels, hints, captions and values.
    auto parts = settings::dialog_layout(dialog);
    for (const std::string_view text :
         {"TOUCH",
          "One-finger drag",
          "Automatic: a selection box on a tablet, scrolling",
          "on a phone.",
          "Automatic",
          "Box",
          "Scroll",
          "Hold delay",
          "How long a finger or button is held for a hold.",
          "350 ms",
          "QUEUE and ADD",
          "A tapped QUEUE, ADD or x5 stays on until it is",
          "tapped again.",
          "Stay on",
          "One action",
          "Haptics",
          "A short vibration as a touch control acts."})
        CHECK(find_part(parts, text, settings::no_control) != nullptr);
    // At its end: Left-handed layout and Control size.
    CHECK(settings::dialog_key(dialog, DialogKey::end) == DialogAction::redraw);
    parts = settings::dialog_layout(dialog);
    for (const std::string_view text :
         {"Left-handed layout",
          "The minimap and the thumb controls on the right,",
          "the orders on the left.",
          "Control size",
          "The size of the touch controls; the game's own",
          "screens keep theirs.",
          "Standard",
          "Large",
          "Larger"})
        CHECK(find_part(parts, text, settings::no_control) != nullptr);
    CHECK(
        click_caption_at(dialog, settings::first_row_control + 5, "Larger") == DialogAction::changed
    );
    CHECK(dialog.chosen.touch_control_size == settings::ControlSize::larger);
    dialog.focused = settings::first_row_control + 5;
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(dialog.chosen.touch_control_size == settings::ControlSize::large);
    dialog.chosen.touch_control_size = settings::ControlSize::standard;
    dialog.focused = settings::no_control;
    CHECK(settings::dialog_key(dialog, DialogKey::home) == DialogAction::redraw);

    // The strips: a click on a caption picks it, and the hint follows.
    const auto click_caption = [&](int32_t control, std::string_view caption) {
        const auto shown = settings::dialog_layout(dialog);
        const auto* part = find_part(shown, caption, settings::no_control);
        CHECK(part != nullptr && part->control == control);
        return part != nullptr ? click(dialog, centre(part->rect)) : DialogAction::none;
    };
    CHECK(click_caption(settings::first_row_control, "Box") == DialogAction::changed);
    CHECK(dialog.chosen.touch_drag == settings::TouchDrag::box);
    CHECK(
        find_part(
            settings::dialog_layout(dialog), "A drag draws a selection box; two fingers scroll", -1
        ) != nullptr
    );
    CHECK(click_caption(settings::first_row_control, "Scroll") == DialogAction::changed);
    CHECK(dialog.chosen.touch_drag == settings::TouchDrag::scroll);
    CHECK(
        find_part(
            settings::dialog_layout(dialog), "A drag scrolls the map; hold, then drag, for a", -1
        ) != nullptr
    );
    CHECK(click_caption(settings::first_row_control + 2, "One action") == DialogAction::changed);
    CHECK(dialog.chosen.touch_latches == settings::TouchLatches::one_action);
    CHECK(
        find_part(
            settings::dialog_layout(dialog),
            "A tapped QUEUE, ADD or x5 turns off after the next",
            -1
        ) != nullptr
    );

    // The hold delay: ten stops of 50 ms from 250 to 700, each its own value.
    CHECK(geometry::slider_of(Setting::touch_hold_delay).stops == 10);
    settings::EngineSettings state{};
    CHECK(geometry::stop_of(state, Setting::touch_hold_delay) == 2);
    for (int32_t stop = 0; stop < 10; ++stop) {
        geometry::set_stop(state, Setting::touch_hold_delay, stop);
        CHECK(
            state.touch_hold_ms ==
            settings::lowest_touch_hold_ms + 50U * static_cast<uint32_t>(stop)
        );
        CHECK(geometry::stop_of(state, Setting::touch_hold_delay) == stop);
        CHECK(
            geometry::value_text(Setting::touch_hold_delay, state) ==
            std::to_string(state.touch_hold_ms) + " ms"
        );
    }
    geometry::set_stop(state, Setting::touch_hold_delay, 40);
    CHECK(state.touch_hold_ms == settings::highest_touch_hold_ms);
    // The arrows step it by a stop; a press at its track's ends sets the ends.
    dialog.focused = settings::first_row_control + 1;
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.touch_hold_ms == 400);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(dialog.chosen.touch_hold_ms == 300);
    const auto track = geometry::open_rows(dialog).rows.rows[1].control_area;
    static_cast<void>(click(dialog, {track.x, track.y + track.height / 2}));
    CHECK(dialog.chosen.touch_hold_ms == settings::lowest_touch_hold_ms);
    static_cast<void>(click(dialog, {track.x + track.width - 1, track.y + track.height / 2}));
    CHECK(dialog.chosen.touch_hold_ms == settings::highest_touch_hold_ms);
    CHECK(find_part(settings::dialog_layout(dialog), "700 ms", settings::no_control) != nullptr);

    // The switches.
    const auto haptics = geometry::open_rows(dialog).rows.rows[3].control_area;
    CHECK(click(dialog, {haptics.x + 2, haptics.y + 4}) == DialogAction::changed);
    CHECK(!dialog.chosen.touch_haptics);
    dialog.focused = settings::first_row_control + 4;
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::changed);
    CHECK(dialog.chosen.touch_left_handed);

    // Restore defaults puts every Touch row back; Cancel what it opened with.
    dialog.chosen.touch_control_size = settings::ControlSize::large;
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.chosen.touch_control_size == settings::ControlSize::standard);
    CHECK(dialog.chosen.touch_drag == settings::TouchDrag::automatic);
    CHECK(dialog.chosen.touch_hold_ms == settings::default_touch_hold_ms);
    CHECK(dialog.chosen.touch_latches == settings::TouchLatches::stay_on);
    CHECK(dialog.chosen.touch_haptics && !dialog.chosen.touch_left_handed);
    CHECK(settings::dialog_key(dialog, DialogKey::escape) == DialogAction::cancelled);
    CHECK(dialog.chosen == dialog.opened);
}

/// Graphics' offset at which Native pixel density shows whole.
constexpr int32_t native_density_in_view = 190;

void menu_scaling_and_native_density_show_and_change() {
    CHECK(geometry::kind_of(Setting::menu_scaling) == geometry::RowKind::levels);
    CHECK(geometry::strip_of(Setting::menu_scaling).levels == 3);
    CHECK(geometry::kind_of(Setting::native_density) == geometry::RowKind::toggle);
    CHECK(geometry::strip_caption(Setting::menu_scaling, 0) == "Sharp");
    CHECK(geometry::strip_caption(Setting::menu_scaling, 1) == "Whole steps");
    CHECK(geometry::strip_caption(Setting::menu_scaling, 2) == "Unfiltered");

    // Menu scaling is User Interface's first row. Native pixel density stays
    // on Graphics, scrolled into view.
    settings::Dialog dialog = opened(Page::user_interface);
    auto parts = settings::dialog_layout(dialog);
    for (const std::string_view text :
         {"Menu scaling",
          "The menus fill the window, every pixel as wide as",
          "the next.",
          "Sharp",
          "Whole steps",
          "Unfiltered"})
        CHECK(find_part(parts, text, settings::no_control) != nullptr);
    settings::Dialog density_page = graphics_page();
    density_page.scroll[static_cast<std::size_t>(Page::graphics)] = native_density_in_view;
    for (const std::string_view text :
         {"Native pixel density",
          "The display's own pixel density (Retina on a Mac).",
          "Applies from the next start."})
        CHECK(
            find_part(settings::dialog_layout(density_page), text, settings::no_control) != nullptr
        );

    // A click on a caption picks it, and the hint follows.
    const auto click_caption = [&](std::string_view caption) {
        const auto shown = settings::dialog_layout(dialog);
        const auto* part = find_part(shown, caption, settings::no_control);
        CHECK(part != nullptr && part->control == settings::first_row_control);
        return part != nullptr ? click(dialog, centre(part->rect)) : DialogAction::none;
    };
    CHECK(click_caption("Whole steps") == DialogAction::changed);
    CHECK(dialog.chosen.menu_scaling == settings::MenuScaling::whole_steps);
    CHECK(
        find_part(
            settings::dialog_layout(dialog), "The largest whole-number scale that fits: smaller", -1
        ) != nullptr
    );
    CHECK(click_caption("Unfiltered") == DialogAction::changed);
    CHECK(dialog.chosen.menu_scaling == settings::MenuScaling::unfiltered);
    CHECK(
        find_part(
            settings::dialog_layout(dialog),
            "The menus fill the window, unfiltered, as the game",
            -1
        ) != nullptr
    );
    // The switch, by a press and by Space.
    const auto density = geometry::open_rows(density_page).rows.rows[6].control_area;
    CHECK(
        click(density_page, {density.x + density.width - 3, density.y + 4}) == DialogAction::changed
    );
    CHECK(density_page.chosen.native_density);
    density_page.focused = settings::first_row_control + 6;
    CHECK(settings::dialog_key(density_page, DialogKey::space) == DialogAction::changed);
    CHECK(!density_page.chosen.native_density);
    CHECK(settings::dialog_key(density_page, DialogKey::space) == DialogAction::changed);
    CHECK(density_page.chosen.native_density);

    // Restore defaults gives Sharp and Off.
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.chosen.menu_scaling == settings::MenuScaling::sharp);
    CHECK(click(density_page, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(!density_page.chosen.native_density);

    // Locked by --native-density or by a platform whose windows are always
    // at native density: the switch keeps its place and its value, with the
    // lock beside it, and neither a press nor Restore defaults moves it.
    for (const Lock lock : {Lock::command_line, Lock::always_on}) {
        settings::Locks locks{};
        locks.native_density = lock;
        settings::EngineSettings current{};
        current.native_density = lock == Lock::always_on;
        settings::Dialog locked = graphics_page(current, locks);
        locked.scroll[static_cast<std::size_t>(Page::graphics)] = native_density_in_view;
        const auto row = geometry::open_rows(locked).rows.rows[6];
        CHECK(row.lock == lock);
        CHECK(row.control_area.width == geometry::switch_width);
        CHECK(row.lock_area.width > 0);
        const auto shown = settings::dialog_layout(locked);
        CHECK(
            find_part(
                shown,
                lock == Lock::always_on ? "Always on here" : "Set on the command line",
                settings::no_control
            ) != nullptr
        );
        CHECK(
            click(locked, {row.control_area.x + 3, row.control_area.y + 4}) == DialogAction::none
        );
        CHECK(click(locked, centre(geometry::restore_button)) == DialogAction::changed);
        CHECK(locked.chosen.native_density == current.native_density);
        // Menu scaling stays free, on User Interface.
        settings::Dialog scaling = opened(Page::user_interface, locks);
        CHECK(geometry::open_rows(scaling).rows.rows[0].lock == Lock::none);
    }
}

/// Smooth motion's hint while it is On.
constexpr std::string_view smooth_motion_on_hint =
    "Units, shots and spinning parts move smoothly between the game's ticks.";
/// Smooth motion's hint while it is Off.
constexpr std::string_view smooth_motion_off_hint =
    "Units and shots move 30 times a second, as the game drew them; fast spins may flicker.";

void smooth_motion_shows_and_changes() {
    CHECK(geometry::kind_of(Setting::smooth_motion) == geometry::RowKind::toggle);
    CHECK(settings::EngineSettings{}.smooth_motion);
    // The switch reads and sets the setting through the switches' table.
    settings::EngineSettings state{};
    CHECK(geometry::switch_on(state, Setting::smooth_motion));
    geometry::set_switch(state, Setting::smooth_motion, false);
    CHECK(!state.smooth_motion && !geometry::switch_on(state, Setting::smooth_motion));

    // Scrolled to it, under Vertical sync: its label and On's hint.
    settings::Dialog dialog = graphics_page();
    dialog.scroll[static_cast<std::size_t>(Page::graphics)] = smooth_motion_in_view;
    const auto row = geometry::open_rows(dialog).rows.rows[5];
    CHECK(row.setting == Setting::smooth_motion);
    CHECK(row.control == settings::first_row_control + 5);
    CHECK(inside(row.control_area, geometry::view));
    auto parts = settings::dialog_layout(dialog);
    CHECK(find_part(parts, "Smooth motion", settings::no_control) != nullptr);
    CHECK(shows_wrapped(parts, smooth_motion_on_hint, dialog));

    // A press on Off turns it off, and the hint says what Off draws.
    CHECK(click(dialog, {row.control_area.x + 3, row.control_area.y + 4}) == DialogAction::changed);
    CHECK(!dialog.chosen.smooth_motion);
    parts = settings::dialog_layout(dialog);
    CHECK(shows_wrapped(parts, smooth_motion_off_hint, dialog));
    CHECK(!shows_wrapped(parts, smooth_motion_on_hint, dialog));
    // Space turns it on and off again.
    dialog.focused = settings::first_row_control + 5;
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::changed);
    CHECK(dialog.chosen.smooth_motion);
    CHECK(shows_wrapped(settings::dialog_layout(dialog), smooth_motion_on_hint, dialog));
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::changed);
    CHECK(!dialog.chosen.smooth_motion);

    // Restore defaults gives On; Cancel what it opened with.
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.chosen.smooth_motion);
    CHECK(settings::dialog_key(dialog, DialogKey::escape) == DialogAction::cancelled);
    CHECK(dialog.chosen == dialog.opened);
}

/// Leaving Full turns the four effects off, an effect turned on below Full
/// stays on, and reaching Full turns all four on.
void hardware_acceleration_moves_the_effects() {
    const std::array<uint32_t settings::EngineSettings::*, 4> effects{
        &settings::EngineSettings::weapon_lighting,
        &settings::EngineSettings::nanolathe_lighting,
        &settings::EngineSettings::wreck_glow,
        &settings::EngineSettings::heat_distortion,
    };
    constexpr int32_t acceleration_row = settings::first_row_control + 3;
    constexpr int32_t wreck_glow_row = settings::first_row_control + 15;
    auto dialog = graphics_page();
    dialog.chosen.hardware_acceleration = settings::HardwareAcceleration::full;
    settings::follow_acceleration_with_effects(dialog.chosen, settings::HardwareAcceleration::off);
    dialog.chosen.wreck_glow = 150;
    dialog.focused = acceleration_row;
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(dialog.chosen.hardware_acceleration == settings::HardwareAcceleration::basic);
    for (const auto field : effects)
        CHECK(dialog.chosen.*field == 0);
    CHECK(dialog.chosen.water_ripples == 100);

    dialog.scroll[static_cast<std::size_t>(Page::graphics)] = 1200;
    dialog.focused = wreck_glow_row;
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.wreck_glow == 10);
    dialog.scroll[static_cast<std::size_t>(Page::graphics)] = 0;
    dialog.focused = acceleration_row;
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(dialog.chosen.hardware_acceleration == settings::HardwareAcceleration::off);
    CHECK(dialog.chosen.wreck_glow == 10);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.hardware_acceleration == settings::HardwareAcceleration::full);
    for (const auto field : effects)
        CHECK(dialog.chosen.*field == 100);
}

/// Effect sliders have ten-percent stops, restore defaults and cancel edits.
void visual_effects_show_and_change() {
    const std::array<uint32_t settings::EngineSettings::*, 8> effects{
        &settings::EngineSettings::weapon_lighting,
        &settings::EngineSettings::nanolathe_lighting,
        &settings::EngineSettings::wreck_glow,
        &settings::EngineSettings::heat_distortion,
        &settings::EngineSettings::water_ripples,
        &settings::EngineSettings::coastal_waves,
        &settings::EngineSettings::underwater_shimmer,
        &settings::EngineSettings::naval_reflections,
    };
    for (std::size_t index = 0; index < effects.size(); ++index) {
        auto dialog = graphics_page();
        // Full, as the player's own file starts: the four effects at 100%.
        dialog.chosen.hardware_acceleration = settings::HardwareAcceleration::full;
        dialog.defaults.hardware_acceleration = settings::HardwareAcceleration::full;
        settings::follow_acceleration_with_effects(
            dialog.chosen, settings::HardwareAcceleration::off
        );
        settings::follow_acceleration_with_effects(
            dialog.defaults, settings::HardwareAcceleration::off
        );
        dialog.scroll[static_cast<std::size_t>(Page::graphics)] = 1200;
        dialog.focused = settings::first_row_control + 13 + static_cast<int32_t>(index);
        CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
        CHECK(dialog.chosen.*effects[index] == 110);
        for (std::size_t other = 0; other < effects.size(); ++other)
            CHECK(dialog.chosen.*effects[other] == (other == index ? 110U : 100U));
        CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
        CHECK(dialog.chosen.*effects[index] == 100);
        dialog.focused = settings::first_row_control + 13 + static_cast<int32_t>(index);
        CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
        CHECK(dialog.chosen.*effects[index] == 90);
        CHECK(settings::dialog_key(dialog, DialogKey::escape) == DialogAction::cancelled);
        CHECK(dialog.chosen == dialog.opened);
    }
    auto dialog = graphics_page();
    CHECK(geometry::shown_locks(dialog).water_effects == settings::Lock::needs_full_acceleration);
    dialog.chosen.hardware_acceleration = settings::HardwareAcceleration::basic;
    CHECK(geometry::shown_locks(dialog).water_effects == settings::Lock::needs_full_acceleration);
    dialog.chosen.hardware_acceleration = settings::HardwareAcceleration::full;
    CHECK(geometry::shown_locks(dialog).water_effects == settings::Lock::none);
    CHECK(geometry::shown_locks(dialog).unlocked_camera == settings::Lock::none);
    dialog.chosen.allow_full_3d = false;
    CHECK(geometry::shown_locks(dialog).unlocked_camera == settings::Lock::needs_full_3d);
    dialog.focused = settings::first_row_control + 12;
    (void)settings::dialog_key(dialog, DialogKey::space);
    CHECK(!dialog.chosen.unlocked_camera);
}

void explosion_flash_shows_and_changes() {
    CHECK(geometry::kind_of(Setting::explosion_flash) == geometry::RowKind::levels);
    CHECK(geometry::strip_of(Setting::explosion_flash).levels == 3);
    CHECK(geometry::strip_caption(Setting::explosion_flash, 0) == "Off");
    CHECK(geometry::strip_caption(Setting::explosion_flash, 1) == "Reduced");
    CHECK(geometry::strip_caption(Setting::explosion_flash, 2) == "Full");
    CHECK(geometry::strip_caption(Setting::explosion_flash, 3).empty());
    CHECK(settings::EngineSettings{}.explosion_flash == settings::ExplosionFlash::full);

    // Scrolled to it: the label, the three captions and Full's hint.
    settings::Dialog dialog = graphics_page();
    dialog.scroll[static_cast<std::size_t>(Page::graphics)] = explosion_flash_in_view;
    auto parts = settings::dialog_layout(dialog);
    for (const std::string_view text :
         {"Explosion flash",
          "Off",
          "Reduced",
          "Full",
          "Explosions light up the ground as the game drew",
          "it, or less where a mod asks."})
        CHECK(find_part(parts, text, settings::no_control) != nullptr);

    // A click on one of the strip's captions picks it, and the hint follows.
    const auto click_caption = [&](std::string_view caption) {
        const auto shown = settings::dialog_layout(dialog);
        const settings::LayoutPart* found = nullptr;
        for (const auto& part : shown)
            if (part.text == caption && part.control == settings::first_row_control + 7)
                found = &part;
        CHECK(found != nullptr);
        return found != nullptr ? click(dialog, centre(found->rect)) : DialogAction::none;
    };
    CHECK(click_caption("Reduced") == DialogAction::changed);
    CHECK(dialog.chosen.explosion_flash == settings::ExplosionFlash::reduced);
    parts = settings::dialog_layout(dialog);
    CHECK(
        find_part(
            parts, "Explosions light up the ground at half strength,", settings::no_control
        ) != nullptr
    );
    CHECK(find_part(parts, "or less where a mod asks.", settings::no_control) != nullptr);
    CHECK(click_caption("Off") == DialogAction::changed);
    CHECK(dialog.chosen.explosion_flash == settings::ExplosionFlash::off);
    parts = settings::dialog_layout(dialog);
    CHECK(
        find_part(
            parts, "Explosions do not light up the ground, whatever a", settings::no_control
        ) != nullptr
    );
    CHECK(find_part(parts, "mod asks for.", settings::no_control) != nullptr);
    // The keys step it a level at a time.
    dialog.focused = settings::first_row_control + 7;
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.explosion_flash == settings::ExplosionFlash::reduced);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.explosion_flash == settings::ExplosionFlash::full);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(dialog.chosen.explosion_flash == settings::ExplosionFlash::reduced);

    // Restore defaults gives Full; Cancel what it opened with.
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.chosen.explosion_flash == settings::ExplosionFlash::full);
    CHECK(settings::dialog_key(dialog, DialogKey::escape) == DialogAction::cancelled);
    CHECK(dialog.chosen == dialog.opened);
}

/// Fog of War's strip: Smooth by default, Classic and Smooth each with its
/// hint, a click or a key choosing either, and Restore defaults giving
/// Smooth.
void fog_of_war_shows_and_changes() {
    CHECK(geometry::kind_of(Setting::fog_of_war) == geometry::RowKind::levels);
    CHECK(geometry::strip_of(Setting::fog_of_war).levels == 2);
    CHECK(geometry::offered_levels(geometry::strip_of(Setting::fog_of_war)) == 2);
    CHECK(geometry::strip_caption(Setting::fog_of_war, 0) == "Classic");
    CHECK(geometry::strip_caption(Setting::fog_of_war, 1) == "Smooth");
    CHECK(geometry::strip_caption(Setting::fog_of_war, 2).empty());
    CHECK(settings::EngineSettings{}.fog_of_war == settings::FogOfWar::smooth);

    // Whether a part of the layout begins with a text: a hint's first line.
    const auto begins = [](const std::vector<settings::LayoutPart>& parts, std::string_view text) {
        return std::any_of(parts.begin(), parts.end(), [&](const settings::LayoutPart& part) {
            return part.control == settings::no_control && part.text.starts_with(text);
        });
    };
    // Scrolled to it: the label, the two captions and Smooth's hint.
    settings::Dialog dialog = graphics_page();
    dialog.scroll[static_cast<std::size_t>(Page::graphics)] = fog_of_war_in_view;
    auto parts = settings::dialog_layout(dialog);
    for (const std::string_view text : {"Fog of War", "Classic", "Smooth"})
        CHECK(find_part(parts, text, settings::no_control) != nullptr);
    CHECK(begins(parts, "Soft edges where the graphics card"));

    // A click on Classic picks it, and the hint follows.
    const auto click_caption = [&](std::string_view caption) {
        const auto shown = settings::dialog_layout(dialog);
        const settings::LayoutPart* found = nullptr;
        for (const auto& part : shown)
            if (part.text == caption && part.control == settings::first_row_control + 8)
                found = &part;
        CHECK(found != nullptr);
        return found != nullptr ? click(dialog, centre(found->rect)) : DialogAction::none;
    };
    CHECK(click_caption("Classic") == DialogAction::changed);
    CHECK(dialog.chosen.fog_of_war == settings::FogOfWar::classic);
    parts = settings::dialog_layout(dialog);
    CHECK(begins(parts, "Hard edges that step from cell to cell"));
    CHECK(!begins(parts, "Soft edges"));
    // The keys step it: Right to Smooth, Left back to Classic.
    dialog.focused = settings::first_row_control + 8;
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.fog_of_war == settings::FogOfWar::smooth);
    CHECK(settings::dialog_key(dialog, DialogKey::right) != DialogAction::changed);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(dialog.chosen.fog_of_war == settings::FogOfWar::classic);

    // Restore defaults gives Smooth; Cancel what it opened with.
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.chosen.fog_of_war == settings::FogOfWar::smooth);
    CHECK(settings::dialog_key(dialog, DialogKey::escape) == DialogAction::cancelled);
    CHECK(dialog.chosen == dialog.opened);
}

/// Window edge's strip and Edge stickiness's slider: Capture by default,
/// with the slider at 1.0 locked Needs Sticky, so neither a click, a key
/// nor the focus reaches it; Sticky frees it, its keys step a tenth, and
/// 0.0 says it never holds; Free and Capture lock it again and keep its
/// value; Restore defaults gives Capture and 1.0.
void window_edge_and_edge_stickiness_show_and_change() {
    CHECK(geometry::kind_of(Setting::window_edge) == geometry::RowKind::levels);
    CHECK(geometry::kind_of(Setting::window_edge_stickiness) == geometry::RowKind::slider);
    CHECK(geometry::strip_caption(Setting::window_edge, 0) == "Capture");
    CHECK(geometry::strip_caption(Setting::window_edge, 1) == "Sticky");
    CHECK(geometry::strip_caption(Setting::window_edge, 2) == "Free");
    CHECK(settings::EngineSettings{}.window_edge == settings::WindowEdge::capture);
    CHECK(settings::EngineSettings{}.edge_stickiness == settings::default_edge_stickiness);
    constexpr int32_t strip = settings::first_row_control + 6;
    constexpr int32_t slider = settings::first_row_control + 7;

    settings::Dialog dialog = opened(Page::controls);
    dialog.scroll[static_cast<std::size_t>(Page::controls)] = std::numeric_limits<int32_t>::max();
    auto parts = settings::dialog_layout(dialog);
    for (const std::string_view text :
         {"Window edge",
          "Capture",
          "Sticky",
          "Free",
          "The pointer stays in the window while you play.",
          "Edge stickiness",
          "Needs Sticky",
          "1.0",
          "How firmly Sticky holds the pointer at the edge."})
        CHECK(find_part(parts, text, settings::no_control) != nullptr);
    // The slider's row, copied out of the rows laid out for it.
    const auto stickiness_row = [&]() -> std::optional<geometry::Row> {
        const auto open = geometry::open_rows(dialog);
        const geometry::Row* row = row_of(open, Setting::window_edge_stickiness);
        if (row == nullptr)
            return std::nullopt;
        return *row;
    };
    const auto locked = stickiness_row();
    CHECK(locked && locked->lock == Lock::needs_sticky);

    // Locked: the keys change nothing, a click on the track changes
    // nothing, and the focus passes it by.
    CHECK(key_on(dialog, slider, DialogKey::right) != DialogAction::changed);
    CHECK(key_on(dialog, slider, DialogKey::left) != DialogAction::changed);
    CHECK(dialog.chosen.edge_stickiness == settings::default_edge_stickiness);
    if (locked) {
        const renderer::SourceRect track = locked->control_area;
        CHECK(
            click(dialog, {track.x + track.width - 2, track.y + track.height / 2}) !=
            DialogAction::changed
        );
        CHECK(click(dialog, {track.x + 1, track.y + track.height / 2}) != DialogAction::changed);
    }
    CHECK(dialog.chosen.edge_stickiness == settings::default_edge_stickiness);
    dialog.focused = strip;
    std::ignore = settings::dialog_key(dialog, DialogKey::down);
    CHECK(dialog.focused != slider);
    dialog.focused = strip;
    std::ignore = settings::dialog_key(dialog, DialogKey::tab);
    CHECK(dialog.focused != slider);

    // A click on a caption of the strip, scrolled into view.
    const auto click_caption = [&](std::string_view caption) {
        dialog.scroll[static_cast<std::size_t>(Page::controls)] =
            std::numeric_limits<int32_t>::max();
        const auto shown = settings::dialog_layout(dialog);
        const settings::LayoutPart* found = nullptr;
        for (const auto& part : shown)
            if (part.text == caption && part.control == strip)
                found = &part;
        CHECK(found != nullptr);
        return found != nullptr ? click(dialog, centre(found->rect)) : DialogAction::none;
    };
    // Sticky frees the slider; its keys step a tenth.
    CHECK(click_caption("Sticky") == DialogAction::changed);
    CHECK(dialog.chosen.window_edge == settings::WindowEdge::sticky);
    const auto free_row = stickiness_row();
    CHECK(free_row && free_row->lock == Lock::none);
    parts = settings::dialog_layout(dialog);
    CHECK(
        find_part(
            parts, "The pointer rests on the edge so the view scrolls.", settings::no_control
        ) != nullptr
    );
    CHECK(find_part(parts, "Needs Sticky", settings::no_control) == nullptr);
    dialog.focused = strip;
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(dialog.focused == slider);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.edge_stickiness == 11);
    CHECK(find_part(settings::dialog_layout(dialog), "1.1", settings::no_control) != nullptr);
    // Up to 5.0, where Right stops, and down to 0.0, where Left stops.
    for (int32_t step = 0; step < 39; ++step)
        CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.edge_stickiness == settings::highest_edge_stickiness);
    CHECK(settings::dialog_key(dialog, DialogKey::right) != DialogAction::changed);
    CHECK(dialog.chosen.edge_stickiness == settings::highest_edge_stickiness);
    CHECK(find_part(settings::dialog_layout(dialog), "5.0", settings::no_control) != nullptr);
    for (int32_t step = 0; step < 50; ++step)
        CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(dialog.chosen.edge_stickiness == settings::lowest_edge_stickiness);
    CHECK(settings::dialog_key(dialog, DialogKey::left) != DialogAction::changed);
    parts = settings::dialog_layout(dialog);
    CHECK(find_part(parts, "0.0", settings::no_control) != nullptr);
    CHECK(find_part(parts, "At 0.0 the edge never holds.", settings::no_control) != nullptr);
    for (int32_t step = 0; step < 25; ++step)
        CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.edge_stickiness == 25);
    CHECK(find_part(settings::dialog_layout(dialog), "2.5", settings::no_control) != nullptr);

    // Free and Capture lock it again, and it keeps 2.5 for Sticky.
    CHECK(click_caption("Free") == DialogAction::changed);
    CHECK(dialog.chosen.window_edge == settings::WindowEdge::free);
    const auto relocked = stickiness_row();
    CHECK(relocked && relocked->lock == Lock::needs_sticky);
    CHECK(key_on(dialog, slider, DialogKey::left) != DialogAction::changed);
    CHECK(dialog.chosen.edge_stickiness == 25);
    CHECK(click_caption("Capture") == DialogAction::changed);
    CHECK(dialog.chosen.window_edge == settings::WindowEdge::capture);
    CHECK(dialog.chosen.edge_stickiness == 25);
    CHECK(find_part(settings::dialog_layout(dialog), "2.5", settings::no_control) != nullptr);
    CHECK(click_caption("Sticky") == DialogAction::changed);
    CHECK(dialog.chosen.edge_stickiness == 25);
    const auto again = stickiness_row();
    CHECK(again && again->lock == Lock::none);

    // Restore defaults gives Capture and 1.0; Cancel what it opened with.
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.chosen.window_edge == settings::WindowEdge::capture);
    CHECK(dialog.chosen.edge_stickiness == settings::default_edge_stickiness);
    CHECK(settings::dialog_key(dialog, DialogKey::escape) == DialogAction::cancelled);
    CHECK(dialog.chosen == dialog.opened);
}

void zoomed_out_units_show_and_change() {
    CHECK(geometry::kind_of(Setting::zoomed_out_units) == geometry::RowKind::levels);
    const auto strip = geometry::strip_of(Setting::zoomed_out_units);
    CHECK(strip.levels == 3 && geometry::offered_levels(strip) == 2);
    CHECK(geometry::offered_levels(geometry::strip_of(Setting::explosion_flash)) == 3);
    CHECK(geometry::strip_caption(Setting::zoomed_out_units, 0) == "Rendered");
    CHECK(geometry::strip_caption(Setting::zoomed_out_units, 1) == "Dots");
    CHECK(geometry::strip_caption(Setting::zoomed_out_units, 2) == "Icons");
    CHECK(geometry::kind_of(Setting::zoomed_out_after) == geometry::RowKind::choice);
    CHECK(settings::EngineSettings{}.zoomed_out_units == settings::ZoomedOutUnits::rendered);
    CHECK(settings::EngineSettings{}.zoomed_out_after == settings::ZoomedOutAfter::one_sixth);

    // Scrolled to the strip: its three captions with Rendered's hint, and
    // After zoom locked, showing 1/6.
    settings::Dialog dialog = graphics_page();
    dialog.scroll[static_cast<std::size_t>(Page::graphics)] = zoomed_out_units_in_view;
    auto parts = settings::dialog_layout(dialog);
    for (const std::string_view text :
         {"Zoomed out units",
          "Rendered",
          "Dots",
          "Icons",
          "Units are drawn as models at every zoom. Far out",
          "on a large map, needs a fast CPU.",
          "After zoom",
          "Needs Dots",
          "1/6"})
        CHECK(find_part(parts, text, settings::no_control) != nullptr);
    CHECK(geometry::open_rows(dialog).rows.rows[10].lock == Lock::needs_dots);

    // A click on a caption of the strip: Icons, faded, changes nothing.
    const auto click_caption = [&](std::string_view caption) {
        const auto shown = settings::dialog_layout(dialog);
        const settings::LayoutPart* found = nullptr;
        for (const auto& part : shown)
            if (part.text == caption && part.control == settings::first_row_control + 9)
                found = &part;
        CHECK(found != nullptr);
        return found != nullptr ? click(dialog, centre(found->rect)) : DialogAction::none;
    };
    CHECK(click_caption("Icons") != DialogAction::changed);
    CHECK(dialog.chosen.zoomed_out_units == settings::ZoomedOutUnits::rendered);
    CHECK(click_caption("Dots") == DialogAction::changed);
    CHECK(dialog.chosen.zoomed_out_units == settings::ZoomedOutUnits::dots);
    parts = settings::dialog_layout(dialog);
    CHECK(
        find_part(
            parts, "Past After zoom, each unit is a dot of its owner's", settings::no_control
        ) != nullptr
    );
    CHECK(find_part(parts, "colour, framed while selected.", settings::no_control) != nullptr);
    // With Dots, After zoom is free, and its hint names its choice.
    CHECK(find_part(parts, "Needs Dots", settings::no_control) == nullptr);
    CHECK(geometry::open_rows(dialog).rows.rows[10].lock == Lock::none);
    CHECK(
        find_part(
            parts, "Dots farther out than 1/6 of normal size. Closer", settings::no_control
        ) != nullptr
    );
    CHECK(find_part(parts, "in, units are drawn as models.", settings::no_control) != nullptr);
    // The keys: Right from Dots reaches no Icons; Left gives Rendered back.
    dialog.focused = settings::first_row_control + 9;
    CHECK(settings::dialog_key(dialog, DialogKey::right) != DialogAction::changed);
    CHECK(dialog.chosen.zoomed_out_units == settings::ZoomedOutUnits::dots);
    // Down to After zoom: Right steps a choice farther out, Left back.
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_row_control + 10);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.zoomed_out_after == settings::ZoomedOutAfter::one_eighth);
    CHECK(
        find_part(
            settings::dialog_layout(dialog),
            "Dots farther out than 1/8 of normal size. Closer",
            settings::no_control
        ) != nullptr
    );
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(dialog.chosen.zoomed_out_after == settings::ZoomedOutAfter::one_quarter);
    CHECK(settings::dialog_key(dialog, DialogKey::up) == DialogAction::redraw);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(dialog.chosen.zoomed_out_units == settings::ZoomedOutUnits::rendered);
    CHECK(geometry::open_rows(dialog).rows.rows[10].lock == Lock::needs_dots);

    // Restore defaults gives Rendered and 1/6; Cancel what it opened with.
    dialog.chosen.zoomed_out_units = settings::ZoomedOutUnits::dots;
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.chosen.zoomed_out_units == settings::ZoomedOutUnits::rendered);
    CHECK(dialog.chosen.zoomed_out_after == settings::ZoomedOutAfter::one_sixth);
    CHECK(settings::dialog_key(dialog, DialogKey::escape) == DialogAction::cancelled);
    CHECK(dialog.chosen == dialog.opened);
}

void window_frame_shows_and_changes() {
    CHECK(geometry::kind_of(Setting::window_frame) == geometry::RowKind::levels);
    CHECK(geometry::strip_of(Setting::window_frame).levels == 2);
    CHECK(geometry::strip_caption(Setting::window_frame, 0) == "Hidden in play");
    CHECK(geometry::strip_caption(Setting::window_frame, 1) == "Always shown");
    CHECK(settings::EngineSettings{}.window_frame == settings::WindowFrame::hidden_in_play);

    // User Interface's second row: the label, both captions and Hidden in
    // play's hint.
    settings::Dialog dialog = opened(Page::user_interface);
    auto parts = settings::dialog_layout(dialog);
    for (const std::string_view text :
         {"Window frame",
          "Hidden in play",
          "Always shown",
          "A window hides its title bar and borders while a",
          "game is played; menus show them."})
        CHECK(find_part(parts, text, settings::no_control) != nullptr);

    // A click on Always shown picks it, and the hint follows.
    const auto* always = find_part(parts, "Always shown", settings::no_control);
    CHECK(always != nullptr && always->control == settings::first_row_control + 1);
    if (always != nullptr)
        CHECK(click(dialog, centre(always->rect)) == DialogAction::changed);
    CHECK(dialog.chosen.window_frame == settings::WindowFrame::always_shown);
    parts = settings::dialog_layout(dialog);
    CHECK(
        find_part(
            parts, "A window shows its title bar and borders on every", settings::no_control
        ) != nullptr
    );
    CHECK(find_part(parts, "screen, a game's included.", settings::no_control) != nullptr);
    // The keys step it a way at a time.
    dialog.focused = settings::first_row_control + 1;
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(dialog.chosen.window_frame == settings::WindowFrame::hidden_in_play);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.window_frame == settings::WindowFrame::always_shown);

    // Restore defaults gives Hidden in play; Cancel what it opened with.
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(dialog.chosen.window_frame == settings::WindowFrame::hidden_in_play);
    CHECK(settings::dialog_key(dialog, DialogKey::escape) == DialogAction::cancelled);
    CHECK(dialog.chosen == dialog.opened);
}

void touch_changes_only_its_entry_and_the_sections_under_it() {
    // A section drawn with Touch listed differs from it drawn without only
    // in the list from Touch's entry down: the open section, the header,
    // the footer and the entries above Touch's are drawn alike.
    const auto fonts = block_fonts();
    const int32_t touch_top = geometry::list_item(Page::touch, true).y;
    for (const Page page : kPages) {
        Canvas plain = blank(settings::dialog_width, settings::dialog_height);
        Canvas touch = blank(settings::dialog_width, settings::dialog_height);
        settings::draw_dialog(
            plain.surface, {0, 0, 1}, opened_with_touch(page, false), fonts, kNoIcon
        );
        settings::draw_dialog(touch.surface, {0, 0, 1}, opened_with_touch(page), fonts, kNoIcon);
        std::size_t differing_outside = 0;
        std::size_t differing_below = 0;
        for (int32_t y = 0; y < settings::dialog_height; ++y)
            for (int32_t x = 0; x < settings::dialog_width; ++x) {
                if (plain.at(x, y) == touch.at(x, y))
                    continue;
                const bool in_list = x > geometry::edge && x < geometry::list_rule_column &&
                                     y >= touch_top && y < geometry::footer_rule_row;
                ++(in_list ? differing_below : differing_outside);
            }
        CHECK(differing_outside == 0);
        CHECK(differing_below > 0);
    }
}

/// A dialog on Updates with the offer the test is about to press.
settings::Dialog updates_dialog(settings::UpdatesOffer offer) {
    settings::Dialog dialog = opened(Page::updates);
    dialog.updates = std::move(offer);
    dialog.updates.mode = static_cast<int32_t>(dialog.chosen.update_check);
    return dialog;
}

void updates_lists_its_rows_and_the_compact_list_of_thirteen_scrolls() {
    // Ten sections keep the gap. Downloads, then Updates.
    CHECK(geometry::list_item(Page::downloads).y == 141);
    CHECK(geometry::list_item(Page::updates).y == 162);
    const geometry::EngineNav plain = geometry::engine_nav(
        settings::dialog_pages(settings::DialogKind::engine), kit::SizeClass::compact
    );
    CHECK(plain.stride == geometry::list_item_height + geometry::list_item_gap && !plain.scrolls);

    // Two optional sections drop the gap and still fit: System ends on the line.
    settings::Dialog two = opened_with_controller(Page::system, true, true, false);
    CHECK(two.nav_scroll == 0);
    const auto system_two = geometry::list_item(Page::system, true, true, false);
    CHECK(system_two.y + system_two.height == geometry::list_divider().y);

    // All three optional sections scroll. Opening on System keeps that entry
    // in view, Developer stays under the line, and nothing else crosses it.
    settings::Dialog system = opened_with_controller(Page::system, true, true, true);
    CHECK(system.nav_scroll > 0);
    const auto system_entry = geometry::dialog_list_item(system, Page::system);
    CHECK(system_entry.y >= geometry::list_first_top);
    CHECK(system_entry.y + system_entry.height <= geometry::list_divider().y);
    CHECK(geometry::dialog_list_item(system, Page::developer).y == 262);
    for (const auto& part : settings::dialog_layout(system)) {
        if (part.control < settings::first_page_control ||
            part.control >= settings::restore_control)
            continue;
        if (part.control == settings::page_control(Page::developer))
            continue;
        CHECK(part.rect.y >= geometry::list_first_top);
        CHECK(part.rect.y + part.rect.height <= geometry::list_divider().y);
    }
    settings::Dialog keys = opened_with_controller(Page::controls, true, true, true);
    const std::vector<int32_t> walked = entries_walked(keys);
    CHECK(walked.size() == kAllPages.size());
    for (std::size_t index = 0; index < walked.size() && index < kAllPages.size(); ++index)
        CHECK(walked[index] == settings::page_control(kAllPages[index]));

    // The status, then the check. A release offers UPDATE…; otherwise CHECK NOW.
    settings::UpdatesOffer ready;
    ready.version_line = "Open Annihilation 0.8.6";
    ready.status = "Up to date · Stable · checked today at 09:00";
    ready.can_check = true;
    settings::Dialog up_to_date = updates_dialog(ready);
    const auto up_parts = settings::dialog_layout(up_to_date);
    CHECK(find_part(up_parts, "Open Annihilation 0.8.6", settings::no_control) != nullptr);
    CHECK(find_part(up_parts, "Check for updates", settings::no_control) != nullptr);
    CHECK(find_part(up_parts, "CHECK NOW", settings::no_control) != nullptr);
    CHECK(find_part(up_parts, "UPDATE…", settings::no_control) == nullptr);
    const auto check_now = find_part(settings::dialog_layout(up_to_date), "CHECK NOW", 0);
    CHECK(check_now != nullptr);
    if (check_now != nullptr)
        CHECK(click(up_to_date, centre(check_now->rect)) == DialogAction::check_updates);

    ready.offered = true;
    ready.status = "0.8.7 is out · Stable · checked today at 09:00";
    settings::Dialog offered = updates_dialog(ready);
    const auto update = find_part(settings::dialog_layout(offered), "UPDATE…", 0);
    CHECK(update != nullptr);
    if (update != nullptr)
        CHECK(click(offered, centre(update->rect)) == DialogAction::open_release);

    ready.offered = false;
    ready.can_check = false;
    ready.status = "Checking…";
    settings::Dialog checking = updates_dialog(ready);
    const auto checking_button = find_part(settings::dialog_layout(checking), "CHECK NOW", 0);
    CHECK(checking_button != nullptr);
    if (checking_button != nullptr)
        CHECK(click(checking, centre(checking_button->rect)) == DialogAction::none);

    ready.can_check = true;
    ready.status = "Could not check";
    settings::Dialog failed = updates_dialog(ready);
    const auto failed_button = find_part(settings::dialog_layout(failed), "CHECK NOW", 0);
    CHECK(failed_button != nullptr);
    if (failed_button != nullptr)
        CHECK(click(failed, centre(failed_button->rect)) == DialogAction::check_updates);

    ready.status = "Not checked yet";
    settings::Dialog idle = updates_dialog(ready);
    CHECK(find_part(settings::dialog_layout(idle), "CHECK NOW", 0) != nullptr);

    settings::Dialog never = opened(Page::updates);
    never.chosen.update_check = settings::UpdateCheck::never;
    never.updates.mode = static_cast<int32_t>(settings::UpdateCheck::never);
    never.updates.version_line = "Open Annihilation 0.8.6";
    never.updates.status = "Up to date · Stable · checked today at 09:00";
    never.updates.can_check = false;
    never.updates.offered = true;
    const auto never_parts = settings::dialog_layout(never);
    CHECK(find_part(never_parts, "CHECK NOW", settings::no_control) == nullptr);
    CHECK(find_part(never_parts, "UPDATE…", settings::no_control) == nullptr);
    CHECK(find_part(never_parts, "OA never checks", settings::no_control) != nullptr);

    // The way changes at once on the dialog and is kept at OK. Cancel puts
    // the way it opened with back, and the button follows.
    settings::Dialog choosing = updates_dialog(ready);
    choosing.focused = settings::first_row_control + 1;
    CHECK(settings::dialog_key(choosing, DialogKey::right) == DialogAction::changed);
    CHECK(choosing.chosen.update_check == settings::UpdateCheck::when_asked);
    CHECK(click(choosing, centre(geometry::ok_button)) == DialogAction::accepted);
    CHECK(choosing.chosen.update_check == settings::UpdateCheck::when_asked);

    settings::Dialog cancelled = updates_dialog(ready);
    cancelled.focused = settings::first_row_control + 1;
    CHECK(settings::dialog_key(cancelled, DialogKey::right) == DialogAction::changed);
    CHECK(settings::dialog_key(cancelled, DialogKey::right) == DialogAction::changed);
    CHECK(cancelled.chosen.update_check == settings::UpdateCheck::never);
    CHECK(cancelled.updates.can_check == false);
    CHECK(settings::dialog_key(cancelled, DialogKey::escape) == DialogAction::cancelled);
    CHECK(cancelled.chosen.update_check == settings::UpdateCheck::automatically);
    CHECK(cancelled.updates.mode == static_cast<int32_t>(settings::UpdateCheck::automatically));
    CHECK(cancelled.updates.can_check);

    // A click on the open list chooses where the item is drawn, at each
    // class. At Compact the list opens upward, above the footer, and the
    // click still lands on Only when I ask.
    const std::array<kit::SizeClass, 3> classes{
        kit::SizeClass::compact, kit::SizeClass::regular, kit::SizeClass::large
    };
    for (const kit::SizeClass size_class : classes) {
        settings::Dialog menu = updates_dialog(ready);
        menu.size_class = size_class;
        const auto field = find_part(
            settings::dialog_layout(menu), "Automatically", settings::first_row_control + 1
        );
        CHECK(field != nullptr);
        CHECK(click(menu, centre(field->rect)) == DialogAction::redraw);
        CHECK(menu.open_list == settings::first_row_control + 1);
        const auto asked =
            find_part(settings::dialog_layout(menu), "Only when I ask", settings::no_control);
        CHECK(asked != nullptr);
        const geometry::Sizes& sized = geometry::sizes_of(menu);
        CHECK(asked->rect.y >= geometry::body_top);
        CHECK(asked->rect.y + asked->rect.height <= sized.footer_rule_row);
        CHECK(click(menu, centre(asked->rect)) == DialogAction::changed);
        CHECK(menu.chosen.update_check == settings::UpdateCheck::when_asked);
        CHECK(menu.open_list == settings::no_control);
    }
}

void controller_is_listed_only_once_a_gamepad_sent_input() {
    // Without a gamepad the engine's settings list as they always have;
    // with one, Controller comes after User Interface and Touch, before Game
    // files and Developer. A mod's options and Language & Text alone never
    // list it.
    const auto engine = settings::dialog_pages(settings::DialogKind::engine);
    CHECK(std::equal(engine.begin(), engine.end(), kPages.begin(), kPages.end()));
    const auto pad = settings::dialog_pages(settings::DialogKind::engine, false, false, true);
    CHECK(std::equal(pad.begin(), pad.end(), kControllerPages.begin(), kControllerPages.end()));
    const auto both = settings::dialog_pages(settings::DialogKind::engine, true, false, true);
    CHECK(
        std::equal(
            both.begin(), both.end(), kTouchControllerPages.begin(), kTouchControllerPages.end()
        )
    );
    const auto all = settings::dialog_pages(settings::DialogKind::engine, true, true, true);
    CHECK(std::equal(all.begin(), all.end(), kAllPages.begin(), kAllPages.end()));
    CHECK(all.size() == settings::most_listed_pages);
    const auto files = settings::dialog_pages(settings::DialogKind::engine, false, true, true);
    CHECK(
        files.size() == 12 && files[4] == Page::user_interface && files[5] == Page::controller &&
        files[6] == Page::game_files && files[7] == Page::downloads && files[8] == Page::updates &&
        files[9] == Page::game_content && files[10] == Page::system && files[11] == Page::developer
    );
    const auto mods = settings::dialog_pages(settings::DialogKind::mod_options, true, true, true);
    CHECK(std::equal(mods.begin(), mods.end(), kModPages.begin(), kModPages.end()));
    const auto language =
        settings::dialog_pages(settings::DialogKind::language_text, true, true, true);
    CHECK(language.size() == 1 && language[0] == Page::language);

    // Its entry takes the place after User Interface, or after Touch; Game
    // files follows it; the line and Developer stay at the list's foot.
    CHECK(geometry::list_item(Page::controller, false, false, true).y == 141);
    CHECK(geometry::list_item(Page::touch, true, false, true).y == 136);
    CHECK(geometry::list_item(Page::controller, true, false, true).y == 156);
    CHECK(geometry::list_item(Page::game_files, true, true, true).y == 176);
    CHECK(geometry::list_item(Page::game_files, false, true, true).y == 156);
    CHECK(geometry::list_item(Page::developer, true, true, true).y == 262);
    CHECK(
        geometry::list_item(Page::game_files, true, true, true).y + geometry::list_item_height <
        geometry::list_divider().y
    );
    // Twelve flowing entries pass the divider, so the list scrolls. System's
    // content starts on the line; Developer stays under it.
    CHECK(geometry::list_item(Page::system, true, true, true).y == geometry::list_divider().y);
    const geometry::EngineNav fullest = geometry::engine_nav(
        settings::dialog_pages(settings::DialogKind::engine, true, true, true),
        kit::SizeClass::compact
    );
    CHECK(fullest.scrolls && fullest.stride == geometry::list_item_height && fullest.limit == 20);

    // Each section's entry keeps its number with or without Controller
    // listed; a dialog without it has no entry numbered as Controller.
    for (const Page page : kPages) {
        const auto plain = list_entries(opened_with_controller(page, false));
        const auto with_pad = list_entries(opened_with_controller(page));
        CHECK(plain.size() == kPages.size());
        CHECK(with_pad.size() == kControllerPages.size());
        for (std::size_t index = 0; index < plain.size() && index < kPages.size(); ++index) {
            CHECK(plain[index].first == settings::page_control(kPages[index]));
            CHECK(plain[index].first != settings::page_control(Page::controller));
            CHECK(same_entry(plain[index].second, list_entries(opened(page))[index].second));
        }
        for (std::size_t index = 0; index < with_pad.size() && index < kControllerPages.size();
             ++index)
            CHECK(with_pad[index].first == settings::page_control(kControllerPages[index]));
        const auto every = list_entries(opened_with_controller(page, true, true, true));
        const settings::Dialog shown = opened_with_controller(page, true, true, true);
        // System's entry is scrolled off while the open section is above it,
        // and Controls is scrolled off while System is open.
        CHECK(every.size() + 1 == kAllPages.size());
        for (const auto& [control, rect] : every) {
            const auto entry_page = static_cast<Page>(control);
            CHECK(rect.y == geometry::dialog_list_item(shown, entry_page).y);
            if (entry_page != Page::developer)
                CHECK(rect.y + rect.height <= geometry::list_divider().y);
        }
    }
    const auto parts = settings::dialog_layout(opened_with_controller(Page::graphics, false));
    CHECK(find_part(parts, "Controller", settings::no_control) == nullptr);
    const auto pad_parts = settings::dialog_layout(opened_with_controller(Page::graphics));
    const auto* entry = find_part(pad_parts, "Controller", settings::no_control);
    CHECK(entry != nullptr && entry->control == settings::page_control(Page::controller));

    // Controller opens only where it is listed.
    CHECK(opened_with_controller(Page::controller, false).page == Page::controls);
    CHECK(opened_with_controller(Page::controller).page == Page::controller);
    CHECK(opened_with_controller(Page::controller).controller);
    CHECK(!opened(Page::graphics).controller && !opened(Page::graphics).steam_input);

    // A click on its entry shows it; the focus walks the entries in the
    // list's order, Controller among them.
    settings::Dialog dialog = opened_with_controller(Page::common_tweaks, true, true);
    CHECK(
        click(dialog, centre(geometry::list_item(Page::controller, true, false, true))) ==
        DialogAction::redraw
    );
    CHECK(dialog.page == Page::controller);
    CHECK(
        click(dialog, centre(geometry::list_item(Page::developer, true, false, true))) ==
        DialogAction::redraw
    );
    CHECK(dialog.page == Page::developer);
    settings::Dialog keys = opened_with_controller(Page::common_tweaks, true, true);
    const std::vector<int32_t> walked = entries_walked(keys);
    CHECK(walked.size() == kTouchControllerPages.size());
    for (std::size_t index = 0; index < walked.size() && index < kTouchControllerPages.size();
         ++index)
        CHECK(walked[index] == settings::page_control(kTouchControllerPages[index]));

    // A gamepad's first input lists Controller at once; going off, a dialog
    // showing Controller shows its first section and the focus leaves it.
    settings::Dialog later = opened_with_controller(Page::graphics, false);
    CHECK(settings::set_controller_section(later, false, false) == DialogAction::none);
    CHECK(settings::set_controller_section(later, true, false) == DialogAction::redraw);
    CHECK(later.controller && list_entries(later).size() == kControllerPages.size());
    CHECK(settings::set_controller_section(later, true, false) == DialogAction::none);
    CHECK(
        click(later, centre(geometry::list_item(Page::controller, false, false, true))) ==
        DialogAction::redraw
    );
    CHECK(later.page == Page::controller);
    later.focused = settings::first_row_control + 2;
    CHECK(settings::set_controller_section(later, false, false) == DialogAction::redraw);
    CHECK(later.page == Page::controls && later.focused == settings::no_control);
    CHECK(list_entries(later).size() == kPages.size());
    // Off while another section shows, that section stays and keeps its focus.
    settings::Dialog elsewhere = opened_with_controller(Page::graphics);
    elsewhere.focused = settings::first_row_control;
    CHECK(settings::set_controller_section(elsewhere, false, false) == DialogAction::redraw);
    CHECK(elsewhere.page == Page::graphics && elsewhere.focused == settings::first_row_control);
    // A mod's options never list it, whatever the host says.
    settings::Dialog options = opened_mod_options(Page::mod_keys);
    static_cast<void>(settings::set_controller_section(options, true, true));
    const auto option_pages =
        settings::dialog_pages(options.kind, options.touch, options.game_files, options.controller);
    CHECK(std::equal(option_pages.begin(), option_pages.end(), kModPages.begin(), kModPages.end()));
}

void controller_shows_its_rows_and_their_values() {
    namespace pad = oa::ui::pad_controls;
    const auto rows = settings::page_settings(Page::controller);
    constexpr std::array<Setting, 15> kControllerRows{
        Setting::pad_scheme,
        Setting::pad_right_trackpad,
        Setting::pad_pointer_speed,
        Setting::pad_acceleration,
        Setting::pad_glide,
        Setting::pad_right_stick,
        Setting::pad_magnetism,
        Setting::pad_gyro,
        Setting::pad_gyro_speed,
        Setting::pad_haptics,
        Setting::pad_prompts,
        Setting::pad_left_handed,
        Setting::touch_control_size,
        Setting::touch_hold_delay,
        Setting::touch_latches,
    };
    CHECK(std::equal(rows.begin(), rows.end(), kControllerRows.begin(), kControllerRows.end()));
    // Control size stands in Touch too: one setting, two rows.
    const auto touch_rows = settings::page_settings(Page::touch);
    CHECK(
        std::find(touch_rows.begin(), touch_rows.end(), Setting::touch_control_size) !=
        touch_rows.end()
    );
    for (const Setting strip :
         {Setting::touch_control_size,
          Setting::pad_scheme,
          Setting::pad_right_trackpad,
          Setting::pad_acceleration,
          Setting::pad_right_stick,
          Setting::pad_haptics})
        CHECK(geometry::kind_of(strip) == geometry::RowKind::levels);
    for (const Setting slider : {Setting::pad_pointer_speed, Setting::pad_gyro_speed})
        CHECK(geometry::kind_of(slider) == geometry::RowKind::slider);
    for (const Setting choice : {Setting::pad_gyro, Setting::pad_prompts})
        CHECK(geometry::kind_of(choice) == geometry::RowKind::choice);
    for (const Setting toggle :
         {Setting::pad_glide, Setting::pad_magnetism, Setting::pad_left_handed})
        CHECK(geometry::kind_of(toggle) == geometry::RowKind::toggle);
    CHECK(geometry::kind_of(Setting::pad_steam_input_notice) == geometry::RowKind::text);
    CHECK(geometry::kind_of(Setting::pad_steam_input_notice) != geometry::RowKind::toggle);

    // Every part inside the dialog and apart, at every offset, with and
    // without the notice; no game lock reaches a Controller row.
    const renderer::SourceRect face{
        geometry::edge,
        geometry::edge,
        settings::dialog_width - 2 * geometry::edge,
        settings::dialog_height - 2 * geometry::edge,
    };
    for (const auto& locks : lock_states())
        for (const auto& row : geometry::place_rows(Page::controller, locks).rows)
            CHECK(row.lock == Lock::none);
    for (const bool steam_input : {false, true}) {
        settings::Dialog dialog = opened_with_controller(Page::controller);
        static_cast<void>(settings::set_controller_section(dialog, true, steam_input));
        const int32_t limit = geometry::open_rows(dialog).limit;
        CHECK(limit > 0);
        for (int32_t scroll = 0; scroll <= limit; ++scroll) {
            dialog.scroll[static_cast<std::size_t>(Page::controller)] = scroll;
            const auto parts = settings::dialog_layout(dialog);
            for (std::size_t a = 0; a < parts.size(); ++a) {
                CHECK(inside(parts[a].rect, face));
                for (std::size_t b = a + 1; b < parts.size(); ++b)
                    CHECK(!overlap(parts[a].rect, parts[b].rect));
            }
        }
    }

    // At its top: the heading, labels, captions, hints and values.
    settings::Dialog dialog = opened_with_controller(Page::controller);
    auto parts = settings::dialog_layout(dialog);
    for (const std::string_view text :
         {"CONTROLLER",
          "Scheme",
          "Trackpads",
          "Sticks",
          "The right trackpad points and the left one moves",
          "the map; a pad without trackpads plays Sticks.",
          "Right trackpad",
          "Relative",
          "Absolute",
          "The pointer moves as the thumb slides.",
          "Pointer speed",
          "100%"})
        CHECK(find_part(parts, text, settings::no_control) != nullptr);
    // Each row's label shows once it is scrolled to, and the last at the end.
    const auto shows_label = [&dialog](Setting setting) {
        settings::Dialog seen = dialog;
        const auto open = geometry::open_rows(seen);
        std::size_t index = 0;
        while (index < open.rows.rows.size() && open.rows.rows[index].setting != setting)
            ++index;
        seen.scroll[static_cast<std::size_t>(Page::controller)] =
            geometry::scroll_showing(open, index);
        const auto shown = settings::dialog_layout(seen);
        return find_part(shown, geometry::label_of(setting), settings::no_control) != nullptr;
    };
    for (const Setting setting : kControllerRows)
        CHECK(shows_label(setting));
    CHECK(settings::dialog_key(dialog, DialogKey::end) == DialogAction::redraw);
    parts = settings::dialog_layout(dialog);
    for (const std::string_view text :
         {"QUEUE and ADD",
          "Stay on",
          "One action",
          "A tapped QUEUE, ADD or x5 stays on until it is"})
        CHECK(find_part(parts, text, settings::no_control) != nullptr);
    CHECK(settings::dialog_key(dialog, DialogKey::home) == DialogAction::redraw);

    // The strips: a click on a caption picks it, and the hint follows.
    CHECK(click_caption_at(dialog, settings::first_row_control, "Sticks") == DialogAction::changed);
    CHECK(dialog.chosen.pad_scheme == pad::Scheme::sticks);
    CHECK(
        find_part(settings::dialog_layout(dialog), "stick the map.", settings::no_control) !=
        nullptr
    );
    CHECK(
        click_caption_at(dialog, settings::first_row_control + 1, "Absolute") ==
        DialogAction::changed
    );
    CHECK(dialog.chosen.pad_right_trackpad == pad::RightTrackpad::absolute);
    CHECK(
        find_part(
            settings::dialog_layout(dialog),
            "Each point of the pad is a point of the view.",
            settings::no_control
        ) != nullptr
    );
    // The keys step a strip: Left and Right.
    dialog.focused = settings::first_row_control + 3;
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.pad_acceleration == pad::Acceleration::high);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::redraw);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(dialog.chosen.pad_acceleration == pad::Acceleration::off);
    // Right stick and Haptics, by the keys.
    dialog.focused = settings::first_row_control + 5;
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.pad_right_stick == pad::RightStick::pointer);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.pad_right_stick == pad::RightStick::nothing);
    CHECK(
        find_part(
            settings::dialog_layout(dialog), "The right stick does nothing.", settings::no_control
        ) != nullptr
    );
    dialog.focused = settings::first_row_control + 9;
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.pad_haptics == pad::Haptics::strong);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(settings::dialog_key(dialog, DialogKey::left) == DialogAction::changed);
    CHECK(dialog.chosen.pad_haptics == pad::Haptics::off);

    // The switches.
    dialog.focused = settings::first_row_control + 4;
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::changed);
    CHECK(dialog.chosen.pad_glide);
    dialog.focused = settings::first_row_control + 6;
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::changed);
    CHECK(!dialog.chosen.pad_magnetism);
    dialog.focused = settings::first_row_control + 11;
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.pad_left_handed);

    // The sliders: Pointer speed 26 stops of 10 % from 50 to 300, Gyro speed
    // 36 from 50 to 400, each its own value.
    CHECK(geometry::slider_of(Setting::pad_pointer_speed).stops == 26);
    CHECK(geometry::slider_of(Setting::pad_gyro_speed).stops == 36);
    settings::EngineSettings state{};
    CHECK(geometry::stop_of(state, Setting::pad_pointer_speed) == 5);
    CHECK(geometry::stop_of(state, Setting::pad_gyro_speed) == 5);
    for (int32_t stop = 0; stop < 36; ++stop) {
        geometry::set_stop(state, Setting::pad_gyro_speed, stop);
        CHECK(state.pad_gyro_speed == pad::lowest_gyro_speed + 10U * static_cast<uint32_t>(stop));
        CHECK(geometry::stop_of(state, Setting::pad_gyro_speed) == stop);
        CHECK(
            geometry::value_text(Setting::pad_gyro_speed, state) ==
            std::to_string(state.pad_gyro_speed) + "%"
        );
        if (stop >= 26)
            continue;
        geometry::set_stop(state, Setting::pad_pointer_speed, stop);
        CHECK(
            state.pad_pointer_speed == pad::lowest_pointer_speed + 10U * static_cast<uint32_t>(stop)
        );
        CHECK(geometry::stop_of(state, Setting::pad_pointer_speed) == stop);
    }
    geometry::set_stop(state, Setting::pad_pointer_speed, 99);
    CHECK(state.pad_pointer_speed == pad::highest_pointer_speed);
    dialog.focused = settings::first_row_control + 2;
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.pad_pointer_speed == 110);
    const auto track = geometry::open_rows(dialog).rows.rows[2].control_area;
    static_cast<void>(click(dialog, {track.x + track.width - 1, track.y + track.height / 2}));
    CHECK(dialog.chosen.pad_pointer_speed == pad::highest_pointer_speed);
    CHECK(find_part(settings::dialog_layout(dialog), "300%", settings::no_control) != nullptr);

    // The drop-downs: Space opens Gyro pointer's list, Down and Enter choose.
    dialog.focused = settings::first_row_control + 7;
    CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::redraw);
    CHECK(dialog.open_list == settings::first_row_control + 7);
    parts = settings::dialog_layout(dialog);
    for (const std::string_view text :
         {"Off", "While the right pad is touched", "While the right stick is touched", "Always"})
        CHECK(find_part(parts, text, settings::no_control) != nullptr);
    CHECK(settings::dialog_key(dialog, DialogKey::down) == DialogAction::redraw);
    CHECK(settings::dialog_key(dialog, DialogKey::enter) == DialogAction::changed);
    CHECK(dialog.chosen.pad_gyro == pad::Gyro::right_pad_touched);
    CHECK(dialog.open_list == settings::no_control);
    CHECK(
        find_part(
            settings::dialog_layout(dialog), "While the right pad is touched", settings::no_control
        ) != nullptr
    );
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.pad_gyro == pad::Gyro::right_stick_touched);
    // Button prompts: each choice by its list item.
    dialog.focused = settings::first_row_control + 10;
    for (std::size_t index = 0; index < 6; ++index) {
        CHECK(settings::dialog_key(dialog, DialogKey::space) == DialogAction::redraw);
        for (std::size_t step = 0; step < 6; ++step)
            static_cast<void>(settings::dialog_key(dialog, DialogKey::up));
        for (std::size_t step = 0; step < index; ++step)
            static_cast<void>(settings::dialog_key(dialog, DialogKey::down));
        static_cast<void>(settings::dialog_key(dialog, DialogKey::enter));
        constexpr std::array<pad::Prompts, 6> kPrompts{
            pad::Prompts::automatic,
            pad::Prompts::steam_deck,
            pad::Prompts::xbox,
            pad::Prompts::playstation,
            pad::Prompts::nintendo,
            pad::Prompts::off,
        };
        CHECK(dialog.chosen.pad_prompts == kPrompts[index]);
    }
    constexpr std::array<std::string_view, 6> kPromptTexts{
        "Automatic", "Steam Deck", "Xbox", "PlayStation", "Nintendo", "Off"
    };
    for (std::size_t index = 0; index < kPromptTexts.size(); ++index)
        CHECK(geometry::choice_text(dialog, Setting::pad_prompts, index) == kPromptTexts[index]);

    // Control size here is Touch's: a change shows in both sections.
    dialog.focused = settings::first_row_control + 12;
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.touch_control_size == settings::ControlSize::large);
    // The hold delay and QUEUE and ADD here are Touch's own settings.
    dialog.focused = settings::first_row_control + 13;
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.touch_hold_ms == 400);
    dialog.focused = settings::first_row_control + 14;
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.chosen.touch_latches == settings::TouchLatches::one_action);

    // Restore defaults puts every Controller row back; Cancel what it opened with.
    CHECK(click(dialog, centre(geometry::restore_button)) == DialogAction::changed);
    const settings::EngineSettings defaults{};
    CHECK(dialog.chosen.pad_scheme == defaults.pad_scheme);
    CHECK(dialog.chosen.pad_right_trackpad == defaults.pad_right_trackpad);
    CHECK(dialog.chosen.pad_pointer_speed == defaults.pad_pointer_speed);
    CHECK(dialog.chosen.pad_acceleration == defaults.pad_acceleration);
    CHECK(dialog.chosen.pad_glide == defaults.pad_glide);
    CHECK(dialog.chosen.pad_right_stick == defaults.pad_right_stick);
    CHECK(dialog.chosen.pad_magnetism == defaults.pad_magnetism);
    CHECK(dialog.chosen.pad_gyro == defaults.pad_gyro);
    CHECK(dialog.chosen.pad_gyro_speed == defaults.pad_gyro_speed);
    CHECK(dialog.chosen.pad_haptics == defaults.pad_haptics);
    CHECK(dialog.chosen.pad_prompts == defaults.pad_prompts);
    CHECK(dialog.chosen.pad_left_handed == defaults.pad_left_handed);
    CHECK(dialog.chosen.touch_control_size == defaults.touch_control_size);
    CHECK(dialog.chosen.touch_hold_ms == defaults.touch_hold_ms);
    CHECK(dialog.chosen.touch_latches == defaults.touch_latches);
    CHECK(settings::dialog_key(dialog, DialogKey::escape) == DialogAction::cancelled);
    CHECK(dialog.chosen == dialog.opened);

    // On a Steam Deck, Restore defaults gives back the Deck's frame rate and
    // Control size Larger, whatever the player chose.
    settings::EngineSettings deck{};
    deck.max_frame_rate = 90;
    deck.touch_control_size = settings::ControlSize::larger;
    settings::Dialog on_deck = opened_with_controller(Page::controller, true, false, false, deck);
    on_deck.chosen.max_frame_rate = 120;
    on_deck.chosen.touch_control_size = settings::ControlSize::standard;
    CHECK(click(on_deck, centre(geometry::restore_button)) == DialogAction::changed);
    CHECK(on_deck.chosen.max_frame_rate == 90);
    CHECK(on_deck.chosen.touch_control_size == settings::ControlSize::larger);
}

void the_steam_input_notice_shows_while_it_applies() {
    settings::Dialog dialog = opened_with_controller(Page::controller);
    const auto plain = geometry::open_rows(dialog);
    CHECK(plain.rows.rows.front().setting == Setting::pad_scheme);
    CHECK(
        find_part(settings::dialog_layout(dialog), "Steam Input", settings::no_control) == nullptr
    );
    // The notice comes first, the rows under it; it takes no press or focus.
    dialog.focused = settings::first_row_control + 1; // Right trackpad
    CHECK(settings::set_controller_section(dialog, true, true) == DialogAction::redraw);
    CHECK(dialog.steam_input);
    CHECK(dialog.focused == settings::first_row_control + 2);
    const auto open = geometry::open_rows(dialog);
    CHECK(open.rows.rows.size() == plain.rows.rows.size() + 1);
    const auto& notice = open.rows.rows.front();
    CHECK(notice.setting == Setting::pad_steam_input_notice);
    CHECK(notice.control_area.width == 0);
    CHECK(notice.hint_lines == geometry::most_notice_lines);
    CHECK(open.rows.rows[2].setting == Setting::pad_right_trackpad);
    // Its text, between words over its lines, says exactly what to do.
    const auto parts = settings::dialog_layout(dialog);
    CHECK(find_part(parts, "Steam Input", settings::no_control) != nullptr);
    std::string joined;
    for (std::size_t line = 0; line < notice.hint_lines; ++line) {
        const auto hint = geometry::row_hint(dialog, Setting::pad_steam_input_notice, line);
        CHECK(hint.notice);
        CHECK(!hint.text.empty());
        CHECK(find_part(parts, hint.text, settings::no_control) != nullptr);
        joined += (joined.empty() ? "" : " ") + hint.text;
    }
    CHECK(
        joined ==
        "Steam Input is on: the trackpads and back grips reach the game as Steam's mouse and "
        "keys. Turn Steam Input off for Open Annihilation in Steam's controller settings to use "
        "them here."
    );
    CHECK(joined == geometry::steam_input_notice_text);
    // A click on it does nothing; Tab from the footer's last entry comes to
    // the first row under it.
    CHECK(click(dialog, centre(notice.label)) == DialogAction::none);
    settings::Dialog keys = dialog;
    keys.focused = settings::no_control;
    CHECK(settings::dialog_key(keys, DialogKey::tab) == DialogAction::redraw);
    CHECK(keys.focused == settings::first_row_control + 1);
    // The notice drawn: its lines in the lock's amber.
    const auto fonts = block_fonts();
    Canvas canvas = blank(settings::dialog_width, settings::dialog_height);
    settings::draw_dialog(canvas.surface, {0, 0, 1}, dialog, fonts, kNoIcon);
    Canvas without = blank(settings::dialog_width, settings::dialog_height);
    settings::Dialog quiet = dialog;
    static_cast<void>(settings::set_controller_section(quiet, true, false));
    settings::draw_dialog(without.surface, {0, 0, 1}, quiet, fonts, kNoIcon);
    bool differs = false;
    for (int32_t y = notice.hints[0].y; y < notice.hints[0].y + notice.hints[0].height; ++y)
        for (int32_t x = notice.hints[0].x; x < notice.hints[0].x + 40; ++x)
            differs = differs || canvas.at(x, y) != without.at(x, y);
    CHECK(differs);
    // Steam Input going off takes the notice away, the focus staying on its row.
    CHECK(settings::set_controller_section(dialog, true, false) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_row_control + 1);
    CHECK(geometry::open_rows(dialog).rows.rows.front().setting == Setting::pad_scheme);
    // While another section shows, the notice coming leaves the focus be.
    settings::Dialog graphics = opened_with_controller(Page::graphics);
    graphics.focused = settings::first_row_control;
    CHECK(settings::set_controller_section(graphics, true, true) == DialogAction::redraw);
    CHECK(graphics.focused == settings::first_row_control);
    // An open list on a row closes as the rows move.
    settings::Dialog listing = opened_with_controller(Page::controller);
    listing.focused = settings::first_row_control + 7;
    CHECK(settings::dialog_key(listing, DialogKey::space) == DialogAction::redraw);
    CHECK(listing.open_list != settings::no_control);
    static_cast<void>(settings::set_controller_section(listing, true, true));
    CHECK(listing.open_list == settings::no_control);
    CHECK(listing.focused == settings::first_row_control + 8);
}

void a_steam_deck_names_its_screen_rate_under_maximum_frame_rate() {
    // Off a Deck, Maximum frame rate keeps its one hint line and Graphics its
    // rows' places exactly.
    settings::Dialog desktop = opened(Page::graphics);
    const auto plain = geometry::open_rows(desktop);
    const auto as_ever = geometry::place_rows(Page::graphics, {});
    CHECK(plain.rows.rows.size() == as_ever.rows.size());
    for (std::size_t index = 0; index < plain.rows.rows.size() && index < as_ever.rows.size();
         ++index) {
        CHECK(plain.rows.rows[index].top == as_ever.rows[index].top);
        CHECK(plain.rows.rows[index].height == as_ever.rows[index].height);
    }
    CHECK(plain.rows.rows[0].hint_lines == 1);
    auto parts = settings::dialog_layout(desktop);
    CHECK(find_part(parts, "Lower it to save power.", settings::no_control) != nullptr);
    for (const auto& part : parts)
        CHECK(part.text.find("Steam Deck") == std::string::npos);
    // On a Deck, a second line names the screen's rate it starts at.
    for (const uint32_t rate : {60U, 90U}) {
        settings::Dialog deck = opened(Page::graphics);
        deck.steam_deck_panel_hz = rate;
        const auto rows = geometry::open_rows(deck);
        CHECK(rows.rows.rows[0].hint_lines == 2);
        CHECK(rows.rows.rows[0].height == plain.rows.rows[0].height + geometry::hint_line_height);
        parts = settings::dialog_layout(deck);
        CHECK(find_part(parts, "Lower it to save power.", settings::no_control) != nullptr);
        const std::string line =
            "Steam Deck: starts at the screen's " + std::to_string(rate) + " fps.";
        CHECK(find_part(parts, line, settings::no_control) != nullptr);
        CHECK(geometry::row_hint(deck, Setting::max_frame_rate, 1).text == line);
        CHECK(geometry::row_hint(deck, Setting::max_frame_rate, 2).text.empty());
    }
    CHECK(geometry::hint_line_count(Setting::max_frame_rate, {}) == 1);
    CHECK(geometry::hint_line_count(Setting::max_frame_rate, {false, 90}) == 2);
    CHECK(geometry::hint_line_count(Setting::vertical_sync, {false, 90}) == 1);
}

void controller_changes_only_its_entry_and_the_sections_under_it() {
    // A section drawn with Controller listed differs from it drawn without
    // only in the list from Controller's entry down.
    const auto fonts = block_fonts();
    const int32_t controller_top = geometry::list_item(Page::controller, false, false, true).y;
    for (const Page page : kPages) {
        Canvas plain = blank(settings::dialog_width, settings::dialog_height);
        Canvas with_pad = blank(settings::dialog_width, settings::dialog_height);
        Canvas as_ever = blank(settings::dialog_width, settings::dialog_height);
        settings::draw_dialog(
            plain.surface, {0, 0, 1}, opened_with_controller(page, false), fonts, kNoIcon
        );
        settings::draw_dialog(
            with_pad.surface, {0, 0, 1}, opened_with_controller(page), fonts, kNoIcon
        );
        settings::draw_dialog(as_ever.surface, {0, 0, 1}, opened(page), fonts, kNoIcon);
        std::size_t differing_outside = 0;
        std::size_t differing_below = 0;
        std::size_t differing_from_ever = 0;
        for (int32_t y = 0; y < settings::dialog_height; ++y)
            for (int32_t x = 0; x < settings::dialog_width; ++x) {
                if (plain.at(x, y) != as_ever.at(x, y))
                    ++differing_from_ever;
                if (plain.at(x, y) == with_pad.at(x, y))
                    continue;
                const bool in_list = x > geometry::edge && x < geometry::list_rule_column &&
                                     y >= controller_top && y < geometry::footer_rule_row;
                ++(in_list ? differing_below : differing_outside);
            }
        CHECK(differing_from_ever == 0);
        CHECK(differing_outside == 0);
        CHECK(differing_below > 0);
    }
}

void a_finger_takes_the_nearest_control() {
    [[maybe_unused]] const InstalledSimplifiedChinese installed;
    // The touch controls' reach in the dialog's pixels: 22 points, as many
    // canvas pixels a point as the screen has, over the dialog's scale.
    const auto reach_at = [](double pixels_per_point, double dialog_scale) {
        return static_cast<int32_t>(std::lround(22.0 * pixels_per_point / dialog_scale));
    };
    CHECK(reach_at(1.0, 1.0) == 22);
    CHECK(reach_at(3.0, 2.0) == 33);
    for (const auto& [pixels_per_point, dialog_scale] :
         {std::pair{1.0, 1.0}, std::pair{3.0, 2.0}, std::pair{2.0, 1.75}}) {
        const int32_t reach = reach_at(pixels_per_point, dialog_scale);
        // 15 and 30 points in the dialog's pixels.
        const auto points = [&](double count) {
            return static_cast<int32_t>(std::lround(count * pixels_per_point / dialog_scale));
        };
        settings::Dialog dialog = opened_with_touch(Page::touch);
        const auto haptics = geometry::open_rows(dialog).rows.rows[3].control_area;
        const int32_t middle = haptics.y + haptics.height / 2;
        CHECK(dialog.chosen.touch_haptics);
        // A mouse press 15 points left of the switch takes nothing.
        const Point near_left{haptics.x - points(15), middle};
        CHECK(
            settings::dialog_pointer_down(dialog, near_left.x, near_left.y) == DialogAction::none
        );
        CHECK(dialog.pressed == settings::no_control);
        CHECK(settings::dialog_pointer_up(dialog, near_left.x, near_left.y) == DialogAction::none);
        CHECK(dialog.chosen.touch_haptics);
        // A finger 30 points off takes nothing either.
        const Point far_left{haptics.x - points(30), middle};
        CHECK(
            settings::dialog_finger_down(dialog, far_left.x, far_left.y, reach) ==
            DialogAction::none
        );
        CHECK(dialog.pressed == settings::no_control);
        CHECK(dialog.finger_shift_x == 0 && dialog.finger_shift_y == 0);
        CHECK(settings::dialog_pointer_up(dialog, far_left.x, far_left.y) == DialogAction::none);
        CHECK(dialog.chosen.touch_haptics);
        // A finger 15 points off takes the switch at its nearest pixel, its
        // Off half, and the release where it landed turns it Off.
        CHECK(
            settings::dialog_finger_down(dialog, near_left.x, near_left.y, reach) ==
            DialogAction::redraw
        );
        CHECK(dialog.pressed == settings::first_row_control + 3);
        CHECK(dialog.finger_shift_x == points(15) && dialog.finger_shift_y == 0);
        CHECK(
            settings::dialog_pointer_move(dialog, near_left.x, near_left.y) == DialogAction::none
        );
        CHECK(
            settings::dialog_pointer_up(dialog, near_left.x, near_left.y) == DialogAction::changed
        );
        CHECK(!dialog.chosen.touch_haptics);
        CHECK(dialog.finger_shift_x == 0 && dialog.finger_shift_y == 0);
    }
    {
        // Under the switch's On half, 15 points down at a pixel a point, it
        // turns it On again: the scroll bar right of the switch and OK under
        // it lie farther.
        settings::Dialog dialog = opened_with_touch(Page::touch);
        dialog.chosen.touch_haptics = false;
        const auto haptics = geometry::open_rows(dialog).rows.rows[3].control_area;
        const Point under{haptics.x + 28, haptics.y + haptics.height - 1 + 15};
        static_cast<void>(settings::dialog_finger_down(dialog, under.x, under.y, 22));
        CHECK(dialog.pressed == settings::first_row_control + 3);
        CHECK(dialog.finger_shift_x == 0 && dialog.finger_shift_y == -15);
        CHECK(settings::dialog_pointer_up(dialog, under.x, under.y) == DialogAction::changed);
        CHECK(dialog.chosen.touch_haptics);
        // Farther down, OK is nearer than the switch, and a finger there
        // takes OK.
        const Point lower{under.x, under.y + 8};
        static_cast<void>(settings::dialog_finger_down(dialog, lower.x, lower.y, 22));
        CHECK(dialog.pressed == settings::ok_control);
        CHECK(settings::dialog_pointer_up(dialog, lower.x, lower.y) == DialogAction::accepted);
    }

    // A finger on a control presses it where it lands.
    settings::Dialog dialog = opened_with_touch(Page::touch);
    const auto ok = centre(geometry::ok_button);
    static_cast<void>(settings::dialog_finger_down(dialog, ok.x, ok.y, 22));
    CHECK(dialog.pressed == settings::ok_control);
    CHECK(dialog.finger_shift_x == 0 && dialog.finger_shift_y == 0);
    CHECK(settings::dialog_pointer_up(dialog, ok.x, ok.y) == DialogAction::accepted);

    // A finger under a slider's track drags its knob along the track.
    dialog = opened_with_touch(Page::touch);
    const auto track = geometry::open_rows(dialog).rows.rows[1].control_area;
    const int32_t below = track.y + track.height - 1 + 8;
    CHECK(
        settings::dialog_finger_down(dialog, track.x, below, 22) == DialogAction::changed &&
        dialog.chosen.touch_hold_ms == settings::lowest_touch_hold_ms && dialog.dragging
    );
    CHECK(
        settings::dialog_pointer_move(dialog, track.x + track.width - 1, below) ==
        DialogAction::changed
    );
    CHECK(dialog.chosen.touch_hold_ms == settings::highest_touch_hold_ms);
    CHECK(
        settings::dialog_pointer_up(dialog, track.x + track.width - 1, below) ==
        DialogAction::redraw
    );
    CHECK(!dialog.dragging && dialog.pressed == settings::no_control);

    // A finger beside the footer's buttons takes the nearest: under OK.
    dialog = opened_with_touch(Page::touch);
    const Point under_ok{ok.x, geometry::ok_button.y + geometry::ok_button.height - 1 + 5};
    static_cast<void>(settings::dialog_finger_down(dialog, under_ok.x, under_ok.y, 22));
    CHECK(dialog.pressed == settings::ok_control);
    CHECK(settings::dialog_pointer_up(dialog, under_ok.x, under_ok.y) == DialogAction::accepted);

    // While a drop-down list is open, a finger beside it takes its nearest
    // item, and the release where it landed chooses that item.
    settings::Dialog languages = language_dialog();
    const auto field = geometry::open_rows(languages).rows.rows[0].control_area;
    static_cast<void>(click(languages, centre(field)));
    CHECK(languages.open_list == settings::first_row_control);
    const auto list =
        geometry::choice_list(field, geometry::choice_count(languages, Setting::language));
    const auto second = geometry::choice_item(list, 1);
    const Point beside_list{list.x + list.width + 6, second.y + second.height / 2};
    static_cast<void>(settings::dialog_finger_down(languages, beside_list.x, beside_list.y, 22));
    CHECK(languages.list_pressed == 1);
    CHECK(
        settings::dialog_pointer_up(languages, beside_list.x, beside_list.y) ==
        DialogAction::changed
    );
    CHECK(languages.open_list == settings::no_control);
    CHECK(geometry::choice_index(languages, Setting::language) == 1);
}

/// The most bytes a name the automation endpoint lists may have.
constexpr std::size_t kEndpointNameBytes = 120;

/// Tells whether a name has the automation endpoint's form for Open
/// Annihilation's own screens: oa, then words of a to z, 0 to 9 and hyphens,
/// each after a dot.
///
/// @param name the name
/// @return true when it matches ^oa(\.[a-z0-9-]+)+$
bool endpoint_form(std::string_view name) {
    constexpr std::string_view prefix = "oa.";
    if (name.substr(0, prefix.size()) != prefix || name.size() == prefix.size())
        return false;
    bool word = false;
    for (const char character : name.substr(prefix.size())) {
        const bool letter = character >= 'a' && character <= 'z';
        const bool digit = character >= '0' && character <= '9';
        if (letter || digit || character == '-') {
            word = true;
            continue;
        }
        if (character != '.' || !word)
            return false;
        word = false;
    }
    return word;
}

/// Returns a name in lower case, ASCII letters alone changed.
///
/// @param name the name
/// @return the name, its capitals small
std::string folded(std::string_view name) {
    std::string lower(name);
    for (char& character : lower)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    return lower;
}

/// Checks the names the automation endpoint lists a dialog's controls and
/// their parts by, and adds them to those seen: the kit finds no fault in
/// the controls' names, and every name the endpoint lists, oa. before each
/// that kit::automation_parts gives, has its form, is at most
/// kEndpointNameBytes long and is like no other ignoring case.
///
/// @param dialog the dialog
/// @param what what the dialog shows, for a failure's message
/// @param[in,out] seen every name listed so far
void check_endpoint_names(
    const settings::Dialog& dialog, const std::string& what, std::set<std::string>& seen
) {
    const oa::ui::kit::DisplayList list = geometry::dialog_list(dialog, nullptr);
    const std::string problem = oa::ui::kit::name_problem(list);
    if (!problem.empty())
        std::cerr << what << ": " << problem << '\n';
    CHECK(problem.empty());
    oa::ui::kit::Interaction interaction;
    interaction.hovered = dialog.hovered;
    interaction.pressed = dialog.pressed;
    interaction.focused = dialog.focused;
    interaction.focus_shown = dialog.focused != settings::no_control;
    std::set<std::string> listed;
    for (const oa::ui::kit::AutomationEntry& entry :
         oa::ui::kit::automation_parts(list, interaction)) {
        const std::string name = "oa." + entry.name;
        const bool sound = endpoint_form(name) && name.size() <= kEndpointNameBytes &&
                           listed.insert(folded(name)).second;
        if (!sound)
            std::cerr << what << ": the endpoint name " << name
                      << " is malformed, too long or listed twice\n";
        CHECK(sound);
        seen.insert(name);
    }
}

/// Checks a dialog's endpoint names on every section it lists, at each
/// section's top and at its scroll end.
///
/// @param dialog the dialog
/// @param what what the dialog is, for a failure's message
/// @param[in,out] seen every name listed so far
void check_every_section_endpoint_names(
    settings::Dialog dialog, const std::string& what, std::set<std::string>& seen
) {
    for (const Page page :
         settings::dialog_pages(dialog.kind, dialog.touch, dialog.game_files, dialog.controller)) {
        dialog.page = page;
        const auto at = static_cast<std::size_t>(page);
        dialog.scroll[at] = 0;
        check_endpoint_names(dialog, what + ", section " + std::to_string(at), seen);
        dialog.scroll[at] = 100000;
        check_endpoint_names(dialog, what + ", section " + std::to_string(at) + " scrolled", seen);
        dialog.scroll[at] = 0;
    }
}

/// Checks the names the automation endpoint lists every control and part
/// by, laid out at one size class: the names do not depend on the class.
///
/// @param size_class the class the dialogs are laid out at
void endpoint_names_at(oa::ui::kit::SizeClass size_class) {
    std::set<std::string> seen;
    // The engine's settings with Touch, Controller and Game files listed,
    // Your files' folder known and Developer's every area and hack open.
    settings::Dialog engine = opened_with_controller(Page::controls, true, true, true);
    engine.size_class = size_class;
    engine.user_folder = std::string(kUserFolder);
    engine.chosen.developer_mode = true;
    for (uint8_t& open : engine.developer.areas_open)
        open = 1;
    for (uint8_t& open : engine.developer.hacks_open)
        open = 1;
    check_every_section_endpoint_names(engine, "engine", seen);
    // A drop-down open on Controls: Maximum zoom out, the second row.
    settings::Dialog dropped = engine;
    dropped.page = Page::controls;
    for (const auto key : {DialogKey::tab, DialogKey::tab, DialogKey::space})
        static_cast<void>(settings::dialog_key(dropped, key));
    CHECK(dropped.open_list != settings::no_control);
    check_endpoint_names(dropped, "Controls, a drop-down open", seen);
    // Interface size's drop-down open, User Interface's fourth row.
    settings::Dialog sizes = engine;
    sizes.page = Page::user_interface;
    for (int32_t press = 0; press < 4; ++press)
        static_cast<void>(settings::dialog_key(sizes, DialogKey::tab));
    CHECK(sizes.focused == settings::first_row_control + 3);
    static_cast<void>(settings::dialog_key(sizes, DialogKey::space));
    CHECK(sizes.open_list == settings::first_row_control + 3);
    check_endpoint_names(sizes, "User Interface, Interface size open", seen);
    // A mod's options.
    settings::Dialog options;
    settings::open_mod_options_dialog(options, {}, {}, {}, "v0.2.0", Page::mod_keys);
    options.size_class = size_class;
    check_every_section_endpoint_names(options, "mod options", seen);

    for (const std::string_view name : {
             "oa.settings.nav.controls",
             "oa.settings.nav.language",
             "oa.settings.nav.user-interface",
             "oa.settings.wheel-zoom",
             "oa.settings.wheel-zoom.off",
             "oa.settings.wheel-zoom.on",
             "oa.settings.max-zoom-out",
             "oa.settings.max-zoom-out.whole-map",
             "oa.settings.menu-scaling.whole-steps",
             "oa.settings.interface-size",
             "oa.settings.interface-size.automatic",
             "oa.settings.interface-size.100",
             "oa.settings.interface-size.200",
             "oa.settings.interface-size.300",
             "oa.settings.interface-size.400",
             "oa.settings.user-folder.saves",
             "oa.settings.user-folder.screenshots",
             "oa.settings.user-folder.mods",
             "oa.settings.game-files-summary.manage",
             "oa.settings.developer-mode.off",
             "oa.settings.frame-stats.on",
             "oa.settings.active-only.off",
             "oa.settings.restore-profile-values",
             "oa.settings.scroll-bar",
             "oa.settings.nav.mod-keys",
             "oa.settings.ok",
             "oa.settings.cancel",
             "oa.settings.restore-defaults",
         }) {
        const bool listed = seen.count(std::string(name)) == 1;
        if (!listed)
            std::cerr << "the endpoint lists no " << name << '\n';
        CHECK(listed);
    }
    // The open drop-down's items are named by the words the preferences
    // keep them as, never by their places.
    for (const std::string& name : seen)
        CHECK(name.find(".item-") == std::string::npos);
}

/// Returns the control of a dialog's display list that has a name.
///
/// @param dialog the dialog
/// @param name the control's name
/// @return the control; nothing when none has it
std::optional<oa::ui::kit::Control>
named_control(const settings::Dialog& dialog, std::string_view name) {
    const oa::ui::kit::DisplayList list = geometry::dialog_list(dialog, nullptr);
    const oa::ui::kit::Control* control =
        oa::ui::kit::control_of(list, oa::ui::kit::control_named(list, name));
    if (control == nullptr)
        return std::nullopt;
    return *control;
}

void language_links_to_the_library_where_the_host_says() {
    // Without the host's word, Language lists its own rows alone.
    auto plain = language_dialog();
    CHECK(!named_control(plain, "settings.more-in-library"));
    CHECK(
        geometry::open_rows(plain).rows.rows.size() ==
        settings::page_settings(Page::language).size()
    );

    // With it, MORE IN THE LIBRARY… stands right under the language's row,
    // and a click asks the host for the Library on Languages.
    auto linked = language_dialog();
    linked.library_link = true;
    const auto rows = geometry::open_rows(linked).rows.rows;
    CHECK(rows.size() == settings::page_settings(Page::language).size() + 1);
    CHECK(rows.size() > 1 && rows[1].setting == Setting::more_in_library);
    const std::optional<oa::ui::kit::Control> link =
        named_control(linked, "settings.more-in-library");
    CHECK(link && link->text == "MORE IN THE LIBRARY…");
    if (link) {
        linked.library_tab = settings::LibraryTab::mods;
        CHECK(
            click(
                linked, {link->rect.x + link->rect.width / 2, link->rect.y + link->rect.height / 2}
            ) == DialogAction::open_library
        );
        CHECK(linked.library_tab == settings::LibraryTab::languages);
    }
    const auto parts = settings::dialog_layout(linked);
    CHECK(find_part(parts, "More languages are in the Library.", settings::no_control) != nullptr);

    // Language alone never lists it, whatever the host says.
    settings::Dialog alone;
    settings::open_language_text_dialog(alone, {}, {}, {}, "v0.6");
    alone.library_link = true;
    CHECK(!named_control(alone, "settings.more-in-library"));
}

void the_endpoint_names_every_control_and_part_once() {
    for (const auto size_class :
         {oa::ui::kit::SizeClass::compact,
          oa::ui::kit::SizeClass::regular,
          oa::ui::kit::SizeClass::large})
        endpoint_names_at(size_class);
}

} // namespace

/// Returns the columns of a row of a canvas that show a colour.
std::vector<int32_t> columns_of(const Canvas& canvas, int32_t y, renderer::Rgb color) {
    std::vector<int32_t> columns;
    for (int32_t x = 0; x < static_cast<int32_t>(canvas.surface.width); ++x)
        if (canvas.at(x, y) == color)
            columns.push_back(x);
    return columns;
}

/// Returns the controls Tab moves through from no focus, once round, at
/// most 200.
///
/// @param dialog the dialog, its focus not shown
/// @return each control the focus stops on, in order
std::vector<int32_t> tab_round(settings::Dialog dialog) {
    std::vector<int32_t> order;
    for (int32_t press = 0; press < 200; ++press) {
        if (settings::dialog_key(dialog, DialogKey::tab) != DialogAction::redraw)
            break;
        if (!order.empty() && dialog.focused == order.front())
            break;
        order.push_back(dialog.focused);
    }
    return order;
}

void the_arrows_move_by_geometry() {
    // Controls: Down walks the seven of its rows that take the focus (Edge
    // stickiness, locked while Window edge shows Capture, takes none), and
    // from the last, which it brings into view, reaches the footer under
    // it. A row lies across the
    // section, so Cancel and OK both lie in line with it; Cancel's middle
    // is nearer the row's. Up comes back to the last row.
    settings::Dialog controls = opened(Page::controls);
    for (int32_t row = 0; row < 7; ++row) {
        CHECK(settings::dialog_key(controls, DialogKey::down) == DialogAction::redraw);
        CHECK(controls.focused == settings::first_row_control + row);
    }
    CHECK(inside(geometry::open_rows(controls).rows.rows[6].control_area, geometry::view));
    CHECK(settings::dialog_key(controls, DialogKey::down) == DialogAction::redraw);
    CHECK(controls.focused == settings::cancel_control);
    CHECK(settings::dialog_key(controls, DialogKey::up) == DialogAction::redraw);
    CHECK(controls.focused == settings::first_row_control + 6);
    // Down from the footer finds nothing: the focus stays.
    controls.focused = settings::ok_control;
    CHECK(settings::dialog_key(controls, DialogKey::down) == DialogAction::none);
    CHECK(controls.focused == settings::ok_control);
    // Right from a section's entry reaches the row at its height: at the
    // section's top, Controls' entry, Use the wheel to zoom.
    controls = opened(Page::controls);
    controls.focused = settings::page_control(Page::controls);
    CHECK(settings::dialog_key(controls, DialogKey::right) == DialogAction::redraw);
    CHECK(controls.focused == settings::first_row_control);
    // Down from Developer's entry, at the foot of the list, reaches Restore
    // defaults under it.
    controls.focused = settings::page_control(Page::developer);
    CHECK(settings::dialog_key(controls, DialogKey::down) == DialogAction::redraw);
    CHECK(controls.focused == settings::restore_control);
    // Left from Restore defaults finds nothing; Right walks the footer.
    CHECK(settings::dialog_key(controls, DialogKey::left) == DialogAction::none);
    CHECK(settings::dialog_key(controls, DialogKey::right) == DialogAction::redraw);
    CHECK(controls.focused == settings::cancel_control);

    // Game files: MANAGE… takes no steps, so Left goes to the entry at its
    // height, Engine Limits', and Right comes back.
    settings::Dialog files = opened_with_game_files(Page::game_files);
    CHECK(settings::dialog_key(files, DialogKey::tab) == DialogAction::redraw);
    CHECK(files.focused == settings::first_row_control);
    CHECK(settings::dialog_key(files, DialogKey::left) == DialogAction::redraw);
    CHECK(files.focused == settings::page_control(Page::common_tweaks));
    CHECK(settings::dialog_key(files, DialogKey::right) == DialogAction::redraw);
    CHECK(files.focused == settings::first_row_control);
    CHECK(files.chosen == files.opened);

    // Graphics: Screen size is the last row the view shows whole. Down from
    // it reaches Hardware acceleration, which the view cuts, and scrolls the
    // least that shows it whole; Down again reaches Vertical sync under the
    // view.
    settings::Dialog graphics = graphics_page();
    for (int32_t row = 0; row < 3; ++row)
        CHECK(settings::dialog_key(graphics, DialogKey::down) == DialogAction::redraw);
    CHECK(graphics.focused == settings::first_row_control + 2);
    CHECK(graphics_scroll(graphics) == 0);
    CHECK(settings::dialog_key(graphics, DialogKey::down) == DialogAction::redraw);
    CHECK(graphics.focused == settings::first_row_control + 3);
    CHECK(graphics_scroll(graphics) == 25);
    CHECK(settings::dialog_key(graphics, DialogKey::down) == DialogAction::redraw);
    CHECK(graphics.focused == settings::first_row_control + 4);
    CHECK(graphics_scroll(graphics) == 72);
    CHECK(inside(geometry::open_rows(graphics).rows.rows[4].control_area, geometry::view));
    // A slider takes Left and Right as steps, and keeps the focus.
    graphics.focused = settings::first_row_control;
    CHECK(settings::dialog_key(graphics, DialogKey::left) == DialogAction::changed);
    CHECK(graphics.focused == settings::first_row_control);

    // From no focus, Up shows it on the last control in Tab's order and the
    // other arrows on the first.
    for (const auto& [key, expected] :
         {std::pair{DialogKey::up, settings::close_x_control},
          std::pair{DialogKey::down, settings::first_row_control},
          std::pair{DialogKey::left, settings::first_row_control},
          std::pair{DialogKey::right, settings::first_row_control}}) {
        settings::Dialog fresh = opened(Page::controls);
        CHECK(settings::dialog_key(fresh, key) == DialogAction::redraw);
        CHECK(fresh.focused == expected);
    }

    // Tab keeps the declared order on every section: the rows that take a
    // change, the footer, then the sections' entries.
    const int32_t row = settings::first_row_control;
    const std::vector<int32_t> rest{
        settings::restore_control,
        settings::cancel_control,
        settings::ok_control,
        settings::page_control(Page::controls),
        settings::page_control(Page::common_tweaks),
        settings::page_control(Page::language),
        settings::page_control(Page::graphics),
        settings::page_control(Page::user_interface),
        settings::page_control(Page::downloads),
        settings::page_control(Page::updates),
        settings::page_control(Page::game_content),
        settings::page_control(Page::system),
        settings::page_control(Page::developer),
        settings::close_x_control,
    };
    const auto then_rest = [&rest](std::vector<int32_t> order) {
        order.insert(order.end(), rest.begin(), rest.end());
        return order;
    };
    CHECK(
        tab_round(opened(Page::controls)) ==
        then_rest({row, row + 1, row + 2, row + 3, row + 4, row + 5, row + 6})
    );
    CHECK(tab_round(opened(Page::common_tweaks)) == then_rest({row, row + 1, row + 2}));
    // Language: Text size waits for the modern fonts.
    CHECK(
        tab_round(opened(Page::language)) ==
        then_rest({row, row + 1, row + 3, row + 4, row + 5, row + 6})
    );
    // Graphics: After zoom waits for Dots.
    CHECK(
        tab_round(opened(Page::graphics)) == then_rest(
                                                 {row,
                                                  row + 1,
                                                  row + 2,
                                                  row + 3,
                                                  row + 4,
                                                  row + 5,
                                                  row + 6,
                                                  row + 7,
                                                  row + 8,
                                                  row + 9,
                                                  row + 11,
                                                  row + 12,
                                                  row + 13,
                                                  row + 14,
                                                  row + 15,
                                                  row + 16}
                                             )
    );
    // User Interface: every row takes a change, Interface size among them.
    CHECK(
        tab_round(opened(Page::user_interface)) ==
        then_rest({row, row + 1, row + 2, row + 3, row + 4})
    );
    // Developer, Developer Mode off: its three rows, every area of its list,
    // then Show Active Only.
    std::vector<int32_t> developer{row, row + 1, row + 2};
    for (std::size_t area = 0; area < settings::developer_areas().size(); ++area)
        developer.push_back(settings::first_hack_list_control + static_cast<int32_t>(area));
    developer.push_back(settings::active_only_control);
    CHECK(tab_round(opened(Page::developer)) == then_rest(developer));
}

void the_oa_button_shows_the_icon_or_the_mark() {
    const settings::DialogFonts fonts{};
    const IconPicture icon = solid_icon();
    const auto button = [&](int32_t side,
                            settings::ButtonLook look,
                            const renderer::RgbaPicture& picture,
                            int32_t scale = 1) {
        Canvas canvas =
            blank(static_cast<uint32_t>(side * scale), static_cast<uint32_t>(side * scale));
        settings::draw_oa_button(canvas.surface, {0, 0, scale}, side, look, fonts, picture);
        return canvas;
    };
    constexpr renderer::Rgb kHover{0x20, 0x24, 0x1c};

    // At rest the icon fills the button but for 3 pixels all round.
    for (const int32_t side : {settings::menu_button_side, settings::ingame_button_side}) {
        const Canvas idle = button(side, settings::ButtonLook::idle, icon.picture());
        const int32_t middle = side / 2;
        const auto shown = columns_of(idle, middle, kIconColor);
        CHECK(shown.size() == static_cast<std::size_t>(side - 6));
        CHECK(!shown.empty() && shown.front() == 3 && shown.back() == side - 4);
        CHECK(idle.at(2, middle) == kPanel);
        CHECK(idle.at(side - 3, middle) == kPanel);
        CHECK(idle.at(3, 3) == kPanel); // the icon's clear corner
        CHECK(idle.at(8, 3) == kIconColor);
        CHECK(columns_of(idle, middle, kAccent).empty());

        // Under the pointer the face lights and a green ring lies inside
        // the bevel; the icon stays.
        const Canvas hovered = button(side, settings::ButtonLook::hovered, icon.picture());
        CHECK(hovered.at(1, middle) == kAccent && hovered.at(side - 2, middle) == kAccent);
        CHECK(hovered.at(2, middle) == kHover);
        CHECK(columns_of(hovered, middle, kIconColor) == shown);

        // Held, the bevel sinks and the icon moves a pixel right and down.
        const Canvas held = button(side, settings::ButtonLook::pressed, icon.picture());
        const auto moved = columns_of(held, middle, kIconColor);
        CHECK(moved.size() == shown.size() && !moved.empty() && moved.front() == 4);
        CHECK(held.at(3, middle) == kBand);
        CHECK(held.at(4, 4) == kBand && held.at(9, 4) == kIconColor);
    }

    // Twice as large, the icon is drawn at the surface's own resolution:
    // the same 3-pixel inset, in surface pixels.
    const Canvas large =
        button(settings::menu_button_side, settings::ButtonLook::idle, icon.picture(), 2);
    const int32_t icon_run = (settings::menu_button_side - 6) * 2;
    const auto large_row = columns_of(large, settings::menu_button_side, kIconColor);
    CHECK(large_row.size() == static_cast<std::size_t>(icon_run));
    CHECK(!large_row.empty() && large_row.front() == 6 && large_row.back() == 6 + icon_run - 1);

    // Without the icon, a green OA mark in a green outlined square, 20/32
    // of the button's side.
    const int32_t square_side = settings::menu_button_side * 20 / 32;
    const int32_t square_at = (settings::menu_button_side - square_side) / 2;
    const Canvas marked = button(settings::menu_button_side, settings::ButtonLook::idle, kNoIcon);
    CHECK(marked.at(square_at, square_at) == kAccent);
    CHECK(marked.at(square_at + square_side - 1, square_at + square_side - 1) == kAccent);
    CHECK(marked.at(square_at - 1, square_at) == kPanel);
    CHECK(columns_of(marked, 16, kIconColor).empty());
    const auto short_pixels = std::vector<uint8_t>(icon.pixels.begin(), icon.pixels.end() - 1);
    const Canvas broken = button(
        settings::menu_button_side,
        settings::ButtonLook::idle,
        renderer::RgbaPicture{IconPicture::kIconSide, IconPicture::kIconSide, short_pixels}
    );
    CHECK(broken.surface.rgb == marked.surface.rgb);
}

void the_oa_mark_alone_shows_the_icon_or_the_button_mark() {
    const IconPicture icon = solid_icon();
    const auto mark = [](int32_t side, const renderer::RgbaPicture& picture, int32_t scale = 1) {
        Canvas canvas =
            blank(static_cast<uint32_t>(side * scale), static_cast<uint32_t>(side * scale));
        settings::draw_oa_mark(canvas.surface, {0, 0, scale}, side, picture);
        return canvas;
    };
    // The icon fills the square, and nothing else is drawn: no face, no bevel.
    const Canvas iconic = mark(26, icon.picture());
    CHECK(columns_of(iconic, 13, kIconColor).size() == 26);
    CHECK(iconic.at(0, 0) == (renderer::Rgb{0, 0, 0})); // the icon's clear corner
    CHECK(iconic.at(25, 25) == kIconColor);
    // Three times as large, at the surface's own resolution.
    const Canvas sharp = mark(26, icon.picture(), 3);
    CHECK(columns_of(sharp, 40, kIconColor).size() == 78);
    // Without the icon, the OA button's mark at rest: the green outlined
    // square 20/32 of the side, the rest left as it was.
    const int32_t square_side = settings::menu_button_side * 20 / 32;
    const int32_t square_at = (settings::menu_button_side - square_side) / 2;
    const Canvas marked = mark(settings::menu_button_side, kNoIcon);
    CHECK(marked.at(square_at, square_at) == kAccent);
    CHECK(marked.at(square_at + square_side - 1, square_at + square_side - 1) == kAccent);
    CHECK(marked.at(square_at - 1, square_at) == (renderer::Rgb{0, 0, 0}));
    CHECK(marked.at(0, 0) == (renderer::Rgb{0, 0, 0}));
    Canvas button = blank(settings::menu_button_side, settings::menu_button_side);
    settings::draw_oa_button(
        button.surface,
        {0, 0, 1},
        settings::menu_button_side,
        settings::ButtonLook::idle,
        settings::DialogFonts{},
        kNoIcon
    );
    for (int32_t y = 0; y < settings::menu_button_side; ++y)
        for (int32_t x = 0; x < settings::menu_button_side; ++x)
            if (marked.at(x, y) == kAccent)
                CHECK(button.at(x, y) == kAccent);
    // Nothing for no side.
    Canvas none = blank(4, 4);
    settings::draw_oa_mark(none.surface, {0, 0, 1}, 0, kNoIcon);
    CHECK(none.at(1, 1) == (renderer::Rgb{0, 0, 0}));
}

// Regular and Large: the dialog at the larger size classes. Compact's
// expectations above are 0.7.3's and stay as they are; these hold the larger
// layouts to the same rules, with rows, text and controls at Compact's sizes.

/// The size classes larger than Compact.
constexpr std::array<oa::ui::kit::SizeClass, 2> kLargerClasses{
    oa::ui::kit::SizeClass::regular,
    oa::ui::kit::SizeClass::large,
};

/// Returns a class's name, for what a failed check prints.
///
/// @param size_class the class
/// @return "Regular" or "Large"; "Compact" for Compact
const char* class_name(oa::ui::kit::SizeClass size_class) {
    switch (size_class) {
    case oa::ui::kit::SizeClass::regular:
        return "Regular";
    case oa::ui::kit::SizeClass::large:
        return "Large";
    case oa::ui::kit::SizeClass::compact:
        break;
    }
    return "Compact";
}

/// Returns a dialog of the engine's settings laid out at a size class, with
/// Touch, Controller and Game files listed and Game files' rows filled as a
/// host fills them.
///
/// @param size_class the class
/// @param page the section shown
/// @param locks what cannot be changed
/// @return the dialog
settings::Dialog
opened_at(oa::ui::kit::SizeClass size_class, Page page, const settings::Locks& locks = {}) {
    settings::Dialog dialog;
    settings::open_dialog(
        dialog,
        settings::EngineSettings{},
        settings::EngineSettings{},
        locks,
        "v0.2.0",
        page,
        {},
        settings::highest_unit_limit,
        {},
        nullptr,
        true,
        true,
        true
    );
    dialog.game_files_summary = "3.1c · Core Contingency · Battle Tactics · music";
    dialog.game_files_sizes = "1.1 GB · 37 GB free on this tablet";
    dialog.game_files_location = "In the file manager: Open Annihilation › Total Annihilation";
    dialog.game_files_device = "tablet";
    dialog.size_class = size_class;
    return dialog;
}

/// Checks that every part a dialog lists lies inside its face at its class,
/// that no two overlap, and that each section's entry lies in the section
/// list with its name fitting it.
///
/// @param dialog the dialog
void check_parts_inside_and_apart(const settings::Dialog& dialog) {
    const geometry::Sizes& sized = geometry::sizes_of(dialog);
    const renderer::SourceRect face{
        geometry::edge,
        geometry::edge,
        sized.dialog_width - 2 * geometry::edge,
        sized.dialog_height - 2 * geometry::edge,
    };
    const renderer::SourceRect nav{
        geometry::edge,
        geometry::body_top,
        sized.list_width,
        sized.footer_rule_row - geometry::body_top,
    };
    const auto parts = settings::dialog_layout(dialog);
    CHECK(!parts.empty());
    for (std::size_t a = 0; a < parts.size(); ++a) {
        CHECK(parts[a].rect.width > 0 && parts[a].rect.height > 0);
        CHECK(inside(parts[a].rect, face));
        for (std::size_t b = a + 1; b < parts.size(); ++b)
            if (overlap(parts[a].rect, parts[b].rect)) {
                std::cerr << class_name(dialog.size_class) << " overlap: '" << parts[a].text
                          << "' and '" << parts[b].text << "'\n";
                CHECK(!overlap(parts[a].rect, parts[b].rect));
            }
        // A section's entry: in the list, its name within its words' room.
        if (parts[a].control >= settings::first_page_control &&
            parts[a].control < settings::restore_control) {
            CHECK(inside(parts[a].rect, nav));
            CHECK(
                one_a_character(parts[a].text) * settings::estimated_character_width <=
                parts[a].rect.width
            );
        }
    }
}

/// Checks that a section's rows lie in the content column at the dialog's
/// class, each control's focus outline clear of its label and hints, and
/// that the section's end comes above the footer.
///
/// @param dialog the dialog
void check_rows_in_their_column(const settings::Dialog& dialog) {
    const geometry::Sizes& sized = geometry::sizes_of(dialog);
    const auto in_columns = [&sized](const renderer::SourceRect& rect) {
        return rect.x >= sized.content_left &&
               rect.x + rect.width <= sized.content_left + sized.content_width;
    };
    const auto open = geometry::open_rows(dialog);
    for (const auto& row : open.rows.rows) {
        CHECK(in_columns(row.label));
        if (row.control_area.width == 0)
            continue;
        CHECK(in_columns(row.control_area));
        const renderer::SourceRect focus{
            row.control_area.x - 2,
            row.control_area.y - 2,
            row.control_area.width + 4,
            row.control_area.height + 4,
        };
        CHECK(!overlap(focus, row.label));
        for (std::size_t line = 0; line < row.hint_lines; ++line)
            CHECK(!overlap(focus, row.hints[line]));
    }
    for (const auto& row : open.list.rows) {
        CHECK(row.label.x >= sized.content_left);
        CHECK(row.label.x + row.label.width <= sized.content_right);
    }
    if (!geometry::developer_page(dialog))
        CHECK(open.rows.bottom + open.scroll - open.limit < sized.footer_rule_row);
}

void the_larger_classes_lay_out_every_section_inside_the_dialog() {
    for (const auto size_class : kLargerClasses) {
        for (const Page page : kAllPages) {
            for (const auto& locks : lock_states()) {
                const settings::Dialog dialog = opened_at(size_class, page, locks);
                check_parts_inside_and_apart(dialog);
                check_rows_in_their_column(dialog);
            }
            for (const auto& state : level_states()) {
                settings::Dialog dialog = opened_at(size_class, page);
                dialog.chosen = state;
                check_parts_inside_and_apart(dialog);
                check_rows_in_their_column(dialog);
            }
            // Each section at its end too.
            settings::Dialog ended = opened_at(size_class, page);
            ended.scroll[static_cast<std::size_t>(page)] = 100000;
            check_parts_inside_and_apart(ended);
        }
        // Controller with the Steam Input notice over its rows.
        settings::Dialog notice = opened_at(size_class, Page::controller);
        static_cast<void>(settings::set_controller_section(notice, true, true));
        check_parts_inside_and_apart(notice);
        check_rows_in_their_column(notice);
        // Developer with every area and hack open, at offsets through its list.
        settings::Dialog developer = everything_open();
        developer.size_class = size_class;
        const auto open = geometry::open_rows(developer);
        const geometry::Sizes& sized = geometry::sizes_of(size_class);
        CHECK(open.area.view.y == sized.developer_view.y);
        CHECK(open.area.view.height == sized.developer_view.height);
        for (int32_t scroll = 0; scroll <= open.limit + 300; scroll += 300) {
            developer.scroll[kDeveloperScroll] = std::min(scroll, open.limit);
            check_parts_inside_and_apart(developer);
            check_rows_in_their_column(developer);
        }
    }
}

void the_larger_classes_keep_hints_to_their_lines() {
    for (const auto size_class : kLargerClasses) {
        for (const Page page : kAllPages) {
            for (const bool steam_input : {false, true}) {
                settings::Dialog dialog = opened_at(size_class, page);
                static_cast<void>(settings::set_controller_section(dialog, true, steam_input));
                for (const auto& row : geometry::open_rows(dialog).rows.rows)
                    CHECK(
                        row.hint_lines <= (row.setting == Setting::pad_steam_input_notice
                                               ? geometry::most_notice_lines
                                               : geometry::most_hint_lines)
                    );
            }
        }
        // The host's lines break at the class's width: the Steam Input
        // notice and where the files are take no more lines than at Compact,
        // and keep every word.
        const auto lines_of = [](const settings::Dialog& dialog, Setting setting) {
            std::vector<std::string> lines;
            for (std::size_t line = 0; line < geometry::most_row_lines; ++line) {
                const auto hint = geometry::row_hint(dialog, setting, line);
                if (!hint.text.empty())
                    lines.push_back(hint.text);
            }
            return lines;
        };
        const auto joined = [](const std::vector<std::string>& lines) {
            std::string text;
            for (const auto& line : lines)
                text += (text.empty() ? "" : " ") + line;
            return text;
        };
        settings::Dialog compact = opened_at(oa::ui::kit::SizeClass::compact, Page::controller);
        static_cast<void>(settings::set_controller_section(compact, true, true));
        settings::Dialog larger = opened_at(size_class, Page::controller);
        static_cast<void>(settings::set_controller_section(larger, true, true));
        const auto compact_notice = lines_of(compact, Setting::pad_steam_input_notice);
        const auto larger_notice = lines_of(larger, Setting::pad_steam_input_notice);
        CHECK(larger_notice.size() < compact_notice.size());
        CHECK(joined(larger_notice) == geometry::steam_input_notice_text);
        for (const auto& line : larger_notice)
            CHECK(
                one_a_character(line) <=
                static_cast<int32_t>(geometry::sizes_of(size_class).hint_line_characters)
            );
        const auto compact_place = lines_of(compact, Setting::game_files_location);
        const auto larger_place = lines_of(larger, Setting::game_files_location);
        CHECK(larger_place.size() <= compact_place.size());
        CHECK(joined(larger_place) == larger.game_files_location);
    }
}

/// Counts the rows a section shows whole at its top: from its line to the
/// next row's, inside the view.
///
/// @param dialog the dialog, at its section's top
/// @return the rows
int32_t whole_rows_at_the_top(const settings::Dialog& dialog) {
    const auto open = geometry::open_rows(dialog);
    const auto& seen = open.area.view;
    int32_t whole = 0;
    for (const auto& row : open.rows.rows)
        if (row.top >= seen.y && row.top + row.height <= seen.y + seen.height)
            ++whole;
    return whole;
}

void the_larger_classes_show_more_graphics_rows() {
    const settings::Dialog compact = graphics_page();
    const auto compact_rows = geometry::open_rows(compact);
    int32_t shown = whole_rows_at_the_top(compact);
    int32_t limit = compact_rows.limit;
    for (const auto size_class : kLargerClasses) {
        settings::Dialog dialog = graphics_page();
        dialog.size_class = size_class;
        const auto open = geometry::open_rows(dialog);
        // The same twelve rows, each at its Compact height: more of them show.
        CHECK(open.rows.rows.size() == compact_rows.rows.rows.size());
        for (std::size_t index = 0; index < open.rows.rows.size(); ++index) {
            // A wider class wraps a description onto fewer lines, so a row
            // sits no lower and no taller than it does at Compact.
            CHECK(open.rows.rows[index].top <= compact_rows.rows.rows[index].top);
            CHECK(open.rows.rows[index].height <= compact_rows.rows.rows[index].height);
            CHECK(open.rows.rows[index].label.height == compact_rows.rows.rows[index].label.height);
            CHECK(
                open.rows.rows[index].control_area.height ==
                compact_rows.rows.rows[index].control_area.height
            );
        }
        const int32_t whole = whole_rows_at_the_top(dialog);
        if (whole <= shown)
            std::cerr << class_name(size_class) << " shows " << whole << " of Graphics' rows\n";
        CHECK(whole > shown);
        // It scrolls less, to an end of its own, which End reaches.
        CHECK(open.limit < limit);
        if (open.limit == 0) {
            CHECK(settings::dialog_key(dialog, DialogKey::end) == DialogAction::none);
            CHECK(graphics_scroll(dialog) == 0);
        } else {
            CHECK(settings::dialog_key(dialog, DialogKey::end) == DialogAction::redraw);
            CHECK(graphics_scroll(dialog) == open.limit);
        }
        shown = whole;
        limit = open.limit;
    }
}

void the_larger_classes_keep_the_footer_buttons() {
    constexpr std::array<int32_t, 3> controls{
        settings::restore_control, settings::cancel_control, settings::ok_control
    };
    for (const auto size_class : kLargerClasses) {
        const geometry::Sizes& sized = geometry::sizes_of(size_class);
        settings::Dialog dialog = opened_at(size_class, Page::controls);
        const auto parts = settings::dialog_layout(dialog);
        std::array<renderer::SourceRect, 3> rects{};
        for (std::size_t index = 0; index < controls.size(); ++index) {
            const auto rect = geometry::footer_button(controls[index], size_class);
            const auto compact = geometry::footer_button(controls[index]);
            rects[index] = rect;
            // Compact's size, in the footer's band.
            CHECK(rect.width == compact.width && rect.height == compact.height);
            CHECK(rect.y > sized.footer_rule_row);
            CHECK(rect.y + rect.height < sized.dialog_height - geometry::edge);
            const auto* part = find_part(parts, {}, controls[index]);
            CHECK(part != nullptr && same_entry(part->rect, rect));
        }
        // Restore defaults at the left, then Cancel and OK at the right, in
        // their order.
        CHECK(rects[0].x == sized.padding);
        CHECK(rects[0].x + rects[0].width < rects[1].x);
        CHECK(rects[1].x + rects[1].width < rects[2].x);
        CHECK(rects[2].x + rects[2].width == sized.content_right);
        // Each is pressed where it lies: Cancel closes the dialog.
        settings::Dialog cancelled = opened_at(size_class, Page::controls);
        CHECK(click(cancelled, centre(rects[1])) == DialogAction::cancelled);
        settings::Dialog accepted = opened_at(size_class, Page::controls);
        CHECK(click(accepted, centre(rects[2])) == DialogAction::accepted);
    }
}

void the_larger_classes_press_each_control_where_it_is_drawn() {
    for (const auto size_class : kLargerClasses) {
        const geometry::Sizes& sized = geometry::sizes_of(size_class);
        for (const Page page : kAllPages) {
            settings::Dialog dialog = opened_at(size_class, page);
            for (const auto& part : settings::dialog_layout(dialog)) {
                if (part.control == settings::no_control)
                    continue;
                const auto list = geometry::dialog_list(dialog, nullptr);
                const oa::ui::kit::Control* control = oa::ui::kit::control_of(list, part.control);
                if (control == nullptr || !control->enabled)
                    continue;
                const Point point = centre(part.rect);
                static_cast<void>(settings::dialog_pointer_move(dialog, point.x, point.y));
                CHECK(dialog.hovered == part.control);
            }
            // Each entry shows its section.
            for (const Page other : kAllPages) {
                settings::Dialog shown = opened_at(size_class, page);
                static_cast<void>(click(shown, centre(geometry::dialog_list_item(shown, other))));
                CHECK(shown.page == other);
            }
        }
        // The wheel scrolls Graphics from anywhere on the dialog, beyond
        // Compact's width too.
        settings::Dialog dialog = graphics_page();
        dialog.size_class = size_class;
        const auto before = geometry::open_rows(dialog);
        CHECK(settings::dialog_contains(dialog, sized.dialog_width - 2, sized.dialog_height - 2));
        CHECK(!settings::dialog_contains(dialog, sized.dialog_width, 0));
        const DialogAction turned =
            settings::dialog_wheel(dialog, sized.content_right - 1, sized.view.y + 1, -1.0F);
        if (before.limit == 0) {
            CHECK(turned == DialogAction::none);
            CHECK(graphics_scroll(dialog) == 0);
        } else {
            CHECK(turned == DialogAction::redraw);
            CHECK(graphics_scroll(dialog) == std::min(geometry::wheel_step, before.limit));
        }
    }
}

/// The nav's texts, top to bottom: entries and owner names. The footer's
/// captions sit in the same columns and are not part of the list.
std::vector<std::string> nav_texts(const settings::Dialog& dialog) {
    const geometry::Sizes& sized = geometry::sizes_of(dialog);
    std::vector<settings::LayoutPart> nav;
    for (const auto& part : settings::dialog_layout(dialog)) {
        if (part.text.empty() || part.rect.y < geometry::list_first_top ||
            part.rect.y >= sized.footer_rule_row || part.rect.x >= sized.content_left)
            continue;
        nav.push_back(part);
    }
    std::sort(
        nav.begin(),
        nav.end(),
        [](const settings::LayoutPart& left, const settings::LayoutPart& right) {
            return left.rect.y < right.rect.y;
        }
    );
    std::vector<std::string> texts;
    for (const auto& part : nav)
        texts.push_back(part.text);
    return texts;
}

/// Registered pages: two owners in order under labelled dividers, sixteen
/// pages scrolling at Compact with Tab reaching every entry, each page's
/// rows inside the dialog at every size class, Tab in row order, OK writing
/// a changed toggle and choice, Cancel writing nothing, and Restore defaults
/// putting the page's keys back. A mod's options list none of them.
void registered_pages_list_under_their_owners_and_write_on_ok() {
    namespace kit = oa::ui::kit;
    static const kit::PreferenceRow kToggle =
        kit::preference_toggle("toggle", "Toggle", "check.toggle", "1");
    static const std::array<std::string_view, 2> kModeIds{"quiet", "loud"};
    static const std::array<std::string_view, 2> kModeCaptions{"Quiet", "Loud"};
    static const kit::PreferenceRow kMode =
        kit::preference_choice("mode", "Mode", "check.mode", "quiet", kModeIds, kModeCaptions);
    static const std::array<kit::PreferenceRow, 2> kRows{kToggle, kMode};
    static const kit::PreferenceRow kOne = kit::preference_toggle("one", "One", "alpha.one", "1");

    const auto page_of = [](std::string id,
                            std::string title,
                            std::string owner,
                            const kit::PreferenceRow* rows,
                            std::size_t count) {
        settings::RegisteredPage page;
        page.id = std::move(id);
        page.title = std::move(title);
        page.owner = std::move(owner);
        page.rows = rows;
        page.row_count = count;
        return page;
    };

    settings::Dialog listed = opened(Page::controls);
    const std::array<settings::RegisteredPage, 3> owners{
        page_of("alpha.one", "One", "Alpha", &kOne, 1),
        page_of("alpha.two", "Two", "Alpha", &kOne, 1),
        page_of("beta.three", "Three", "Beta", &kOne, 1),
    };
    settings::set_registered_pages(listed, owners, {});
    const auto texts = nav_texts(listed);
    const auto developer = std::find(texts.begin(), texts.end(), "Developer");
    CHECK(developer != texts.end());
    const std::vector<std::string> tail(developer + 1, texts.end());
    // The owners and their pages stay in order under Developer. Updates takes
    // one more row above Developer, so One, which used to fit, is cut until
    // the list scrolls. Alpha's label still does.
    CHECK((tail == std::vector<std::string>{"Alpha"}));
    listed.nav_scroll = 100000;
    const auto scrolled = nav_texts(listed);
    const auto scrolled_developer = std::find(scrolled.begin(), scrolled.end(), "Developer");
    CHECK(scrolled_developer != scrolled.end());
    const std::vector<std::string> scrolled_tail(scrolled_developer + 1, scrolled.end());
    CHECK((scrolled_tail == std::vector<std::string>{"Alpha", "One", "Two", "Beta", "Three"}));

    settings::Dialog options = opened_mod_options(Page::mod_keys);
    settings::set_registered_pages(options, owners, {});
    CHECK(options.registered_pages.empty());
    for (const auto& text : nav_texts(options))
        CHECK(text != "Alpha" && text != "Beta");

    std::array<settings::RegisteredPage, 16> many{};
    for (std::size_t index = 0; index < many.size(); ++index) {
        many[index] =
            page_of("n" + std::to_string(index), "Page " + std::to_string(index), "Pack", &kOne, 1);
    }
    settings::Dialog crowded = opened(Page::controls);
    settings::set_registered_pages(crowded, many, {});
    std::set<int32_t> reached;
    for (int32_t press = 0; press < 200; ++press) {
        if (settings::dialog_key(crowded, DialogKey::tab) != DialogAction::redraw)
            break;
        if (crowded.focused < settings::restore_control)
            reached.insert(crowded.focused);
    }
    for (const Page page : settings::dialog_pages(settings::DialogKind::engine))
        CHECK(reached.count(settings::page_control(page)) == 1);
    for (std::size_t index = 0; index < many.size(); ++index)
        CHECK(reached.count(settings::registered_nav_control(index)) == 1);
    const int32_t last = settings::registered_nav_control(many.size() - 1);
    for (int32_t press = 0; press < 200 && crowded.focused != last; ++press)
        CHECK(settings::dialog_key(crowded, DialogKey::tab) == DialogAction::redraw);
    CHECK(crowded.focused == last);
    CHECK(crowded.nav_scroll > 0);
    CHECK(find_part(settings::dialog_layout(crowded), {}, crowded.focused) != nullptr);
    crowded.nav_scroll = 0;
    CHECK(
        settings::dialog_wheel(crowded, geometry::edge + 4, geometry::body_top + 4, -1.0F) ==
        DialogAction::redraw
    );
    CHECK(crowded.nav_scroll > 0);

    const settings::RegisteredPage sample =
        page_of("check.page", "Check", "Check", kRows.data(), kRows.size());
    for (const auto size_class :
         {kit::SizeClass::compact, kit::SizeClass::regular, kit::SizeClass::large}) {
        settings::Dialog dialog = opened(Page::controls);
        dialog.size_class = size_class;
        settings::set_registered_pages(dialog, std::span(&sample, 1), {});
        dialog.page = Page::registered;
        check_parts_inside_and_apart(dialog);
        const auto parts = settings::dialog_layout(dialog);
        CHECK(find_part(parts, "Toggle", settings::no_control) != nullptr);
        CHECK(find_part(parts, "Mode", settings::no_control) != nullptr);
        CHECK(find_part(parts, "Check", settings::no_control) != nullptr);
    }

    settings::Dialog dialog = opened(Page::controls);
    settings::set_registered_pages(dialog, std::span(&sample, 1), {});
    dialog.page = Page::registered;
    dialog.focused = settings::no_control;
    CHECK(settings::dialog_key(dialog, DialogKey::tab) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_row_control);
    CHECK(settings::dialog_key(dialog, DialogKey::tab) == DialogAction::redraw);
    CHECK(dialog.focused == settings::first_row_control + 1);
    CHECK(settings::dialog_key(dialog, DialogKey::right) == DialogAction::changed);
    CHECK(dialog.registered_values.values.at("check.mode") == "loud");

    const auto toggled = settings::dialog_layout(dialog);
    const settings::LayoutPart* off = nullptr;
    for (const auto& part : toggled) {
        if (part.control != settings::first_row_control)
            continue;
        if (off == nullptr || part.rect.x < off->rect.x)
            off = &part;
    }
    CHECK(off != nullptr);
    CHECK(click(dialog, centre(off->rect)) == DialogAction::changed);
    CHECK(dialog.registered_values.values.at("check.toggle") == "0");
    const auto accepting = settings::dialog_layout(dialog);
    const auto* ok = find_part(accepting, {}, settings::ok_control);
    CHECK(ok != nullptr);
    CHECK(click(dialog, centre(ok->rect)) == DialogAction::accepted);
    const auto writes = settings::registered_writes(dialog);
    bool wrote_toggle = false;
    bool wrote_mode = false;
    for (const auto& write : writes) {
        if (write.key == "check.toggle" && write.value == "0")
            wrote_toggle = true;
        if (write.key == "check.mode" && write.value == "loud")
            wrote_mode = true;
    }
    CHECK(wrote_toggle && wrote_mode);

    settings::Dialog cancelled = opened(Page::controls);
    settings::set_registered_pages(cancelled, std::span(&sample, 1), {});
    cancelled.page = Page::registered;
    const auto cancel_toggled = settings::dialog_layout(cancelled);
    const settings::LayoutPart* cancel_off = nullptr;
    for (const auto& part : cancel_toggled) {
        if (part.control != settings::first_row_control)
            continue;
        if (cancel_off == nullptr || part.rect.x < cancel_off->rect.x)
            cancel_off = &part;
    }
    CHECK(cancel_off != nullptr);
    CHECK(click(cancelled, centre(cancel_off->rect)) == DialogAction::changed);
    const auto cancelling = settings::dialog_layout(cancelled);
    const auto* cancel = find_part(cancelling, {}, settings::cancel_control);
    CHECK(cancel != nullptr);
    CHECK(click(cancelled, centre(cancel->rect)) == DialogAction::cancelled);
    CHECK(settings::registered_writes(cancelled).empty());
    CHECK(cancelled.registered_values.values.count("check.toggle") == 0);

    settings::Dialog restored = opened(Page::controls);
    settings::set_registered_pages(restored, std::span(&sample, 1), {});
    restored.page = Page::registered;
    const auto restore_toggled = settings::dialog_layout(restored);
    const settings::LayoutPart* restore_off = nullptr;
    for (const auto& part : restore_toggled) {
        if (part.control != settings::first_row_control)
            continue;
        if (restore_off == nullptr || part.rect.x < restore_off->rect.x)
            restore_off = &part;
    }
    CHECK(restore_off != nullptr);
    CHECK(click(restored, centre(restore_off->rect)) == DialogAction::changed);
    restored.focused = settings::restore_control;
    CHECK(settings::dialog_key(restored, DialogKey::space) == DialogAction::changed);
    CHECK(restored.registered_values.values.at("check.toggle") == "1");
    CHECK(restored.registered_values.values.at("check.mode") == "quiet");

    const auto list = geometry::dialog_list(dialog, nullptr);
    bool named_toggle = false;
    bool named_nav = false;
    for (const auto& control : list.controls) {
        if (control.name == "settings.check.page.toggle")
            named_toggle = true;
        if (control.name == "settings.nav.check.page")
            named_nav = true;
    }
    CHECK(named_toggle && named_nav);
}

/// A Launch Services stand-in. A change that does not store leaves the opener as it was.
struct FakeLaunchServices {
    std::map<std::string, oa::platform::file_types::Handler> handlers{};
    bool store_writes = true;
};

oa::platform::file_types::Handler
fake_read(void* context, const std::string_view content_type, std::string& failure) {
    auto* fake = static_cast<FakeLaunchServices*>(context);
    (void)failure;
    const auto found = fake->handlers.find(std::string(content_type));
    if (found == fake->handlers.end())
        return {};
    return found->second;
}

bool fake_write(
    void* context,
    const std::string_view content_type,
    const oa::platform::file_types::Handler& handler,
    std::string& failure
) {
    auto* fake = static_cast<FakeLaunchServices*>(context);
    (void)failure;
    if (fake->store_writes)
        fake->handlers[std::string(content_type)] = handler;
    return true;
}

/// The hint under File Type Registration, joined as one sentence.
std::string registration_hint(const settings::Dialog& dialog) {
    std::string text;
    for (std::size_t line = 0;; ++line) {
        const std::string part =
            geometry::row_hint(dialog, Setting::file_type_registration, line).text;
        if (part.empty())
            break;
        if (!text.empty())
            text += ' ';
        text += part;
    }
    return text;
}

/// A fake that keeps the opener shows the hint. One that takes the change shows none.
void file_type_hint_when_the_opener_stays() {
    namespace file_types = oa::platform::file_types;
    file_types::Handler application;
    application.bundle_id = "net.coreprime.open-annihilation";
    file_types::Handler archive;
    archive.bundle_id = "com.example.archive";
    FakeLaunchServices fake;
    fake.handlers["public.zip-archive"] = archive;
    fake.handlers["public.plain-text"] = archive;
    for (const file_types::ExportedContentType& type : file_types::exported_content_types)
        fake.handlers[std::string(type.content_type)] = application;
    fake.store_writes = false;
    file_types::HandlerCalls calls;
    calls.context = &fake;
    calls.default_handler = &fake_read;
    calls.set_handler = &fake_write;
    const file_types::OpenerChange off[] = {{file_types::mod_extension, false}};
    const file_types::Registration kept = file_types::apply_opener_changes(calls, application, off);
    CHECK(kept.error.find("oamod: the change did not take") != std::string::npos);
    file_types::TypeSet asked;
    asked.mod = false;
    const file_types::OpenerReport still =
        file_types::reported_openers(calls, application.bundle_id, asked);
    CHECK(still.open.mod);
    const std::vector<std::string> stayed = file_types::extensions_still_opened(asked, still.open);
    CHECK(stayed.size() == 1 && stayed[0] == ".oamod");
    settings::Dialog dialog = opened(Page::system);
    dialog.file_type_switches = true;
    const std::string_view dotted = stayed[0];
    dialog.file_type_kept_hint =
        settings::kept_opener_hint(std::span<const std::string_view>(&dotted, 1));
    CHECK(
        registration_hint(dialog) ==
        "macOS kept Open Annihilation as the opener of .oamod. To choose another, select a "
        ".oamod file in Finder, then File › Get Info › Open with › Change All."
    );

    fake.store_writes = true;
    const file_types::Registration taken =
        file_types::apply_opener_changes(calls, application, off);
    CHECK(taken.error.empty());
    const file_types::OpenerReport changed =
        file_types::reported_openers(calls, application.bundle_id, asked);
    CHECK(!changed.open.mod);
    CHECK(file_types::extensions_still_opened(asked, changed.open).empty());
    dialog.file_type_kept_hint.clear();
    CHECK(
        geometry::row_hint(dialog, Setting::file_type_registration, 0).text ==
        "Open these files with the game from your file manager."
    );
    CHECK(geometry::row_hint(dialog, Setting::file_type_registration, 1).text.empty());
    CHECK(geometry::row_hint(dialog, Setting::file_type_registration, 2).text.empty());
}

/// The title bar's close mark is the rightmost control in the header at
/// every size class. The version stops before it. Pressing it, with a
/// change not kept, does what Cancel does, and Cancel stays.
void the_header_close_mark_closes_as_cancel_does() {
    for (const oa::ui::kit::SizeClass size_class :
         {oa::ui::kit::SizeClass::compact,
          oa::ui::kit::SizeClass::regular,
          oa::ui::kit::SizeClass::large}) {
        settings::Dialog dialog = opened(Page::controls);
        dialog.size_class = size_class;
        const oa::ui::kit::DisplayList list = geometry::dialog_list(dialog, nullptr);
        const oa::ui::kit::Control* close = nullptr;
        int32_t header_right = 0;
        for (const oa::ui::kit::Control& control : list.controls) {
            if (control.rect.y > geometry::header_rule_row)
                continue;
            if (control.rect.x + control.rect.width >= header_right) {
                header_right = control.rect.x + control.rect.width;
                close = &control;
            }
        }
        CHECK(close != nullptr && close->name == "settings.close-x");
        CHECK(close->text == "Close");
        if (close == nullptr)
            continue;
        const geometry::SourceRect expected =
            geometry::header_close_button(geometry::sizes_of(dialog));
        CHECK(close->rect.x == expected.x && close->rect.width == expected.width);
        bool mark = false;
        for (const oa::ui::kit::Item& item : list.items) {
            if (item.role != oa::ui::kit::Role::button || item.control != close->id)
                continue;
            const auto* look = std::get_if<oa::ui::kit::ButtonLook>(&item.look);
            mark = look != nullptr && look->mark == oa::ui::kit::ButtonMark::close;
        }
        CHECK(mark);
        const oa::ui::kit::HeaderLook* header = nullptr;
        for (const oa::ui::kit::Item& item : list.items)
            if (item.role == oa::ui::kit::Role::header)
                header = std::get_if<oa::ui::kit::HeaderLook>(&item.look);
        CHECK(header != nullptr);
        if (header != nullptr) {
            const int32_t version_right = header->width - geometry::edge -
                                          oa::ui::kit::compact_metrics.padding -
                                          header->right_inset;
            CHECK(version_right <= close->rect.x);
        }
        dialog.chosen.wheel_zoom = !dialog.opened.wheel_zoom;
        const Point at{
            close->rect.x + close->rect.width / 2, close->rect.y + close->rect.height / 2
        };
        CHECK(click(dialog, at) == DialogAction::cancelled);
        CHECK(dialog.chosen.wheel_zoom == dialog.opened.wheel_zoom);
        const auto parts = settings::dialog_layout(opened(Page::controls), nullptr);
        CHECK(find_part(parts, "CANCEL", settings::cancel_control) != nullptr);
    }
}

/// Counts a text's characters.
///
/// @param text the text, in UTF-8
/// @return its characters
int32_t characters_in(std::string_view text) {
    int32_t count = 0;
    for (const char byte : text)
        if ((static_cast<unsigned char>(byte) & 0xC0U) != 0x80U)
            ++count;
    return count;
}

// Game Content's sentence breaks where the hint's font measures it: a
// language whose letters are wider than the estimate keeps every line in
// the section, and a no-break space joins the words beside it, measured
// with them, as Documents ›\u00a0Open Annihilation.
void game_content_sentence_breaks_where_measured() {
    const settings::Dialog dialog = opened(Page::game_content);
    oa::data::languages::InterfaceText catalogue;
    CHECK(catalogue.add(
        "[OA plays from its own copy in Documents \u203a\u00a0Open Annihilation. The "
        "installation is not used or changed.]\n{\nde=OA spielt aus seiner eigenen Kopie unter "
        "Dokumente \u203a\u00a0Open Annihilation. Die Installation wird weder benutzt noch "
        "ver\u00e4ndert.;\n}\n"
    ));
    oa::data::languages::set_interface_language(
        &catalogue, *oa::data::languages::find_by_tag("de")
    );
    // Wider letters than the estimate: 9 points each, a space 4.
    const geometry::TextWidth wide = [](std::string_view text) {
        int32_t width = 0;
        for (std::size_t at = 0; at < text.size();) {
            const std::size_t bytes =
                std::max<std::size_t>(oa::ui::kit::character_bytes(text.substr(at)), 1);
            width += text[at] == ' ' ? 4 : 9;
            at += bytes;
        }
        return width;
    };
    oa::ui::kit::PlacedRows placed;
    std::ignore = geometry::place_section(dialog, Page::game_content, {}, nullptr, wide, &placed);
    oa::data::languages::set_interface_language(nullptr, oa::data::languages::english());
    CHECK(!placed.rows.empty());
    if (placed.rows.empty())
        return;
    const std::vector<std::string>& lines = placed.rows.front().view.hints;
    const int32_t room = geometry::sizes_of(dialog).content_width;
    std::string joined;
    for (const std::string& line : lines) {
        if (wide(line) > room)
            std::cerr << "too wide (" << wide(line) << " of " << room << "): " << line << '\n';
        CHECK(wide(line) <= room);
        CHECK(
            line.find("\u00a0") == std::string::npos ||
            line.find("\u203a\u00a0Open") != std::string::npos
        );
        joined += joined.empty() ? line : " " + line;
    }
    CHECK(characters_in(joined) > 100);
    CHECK(joined.find("Dokumente \u203a\u00a0Open Annihilation.") != std::string::npos);

    // In the sizes a language's wide section names fit, the page's rows and
    // its sentence lie in the narrower section, as every page's do.
    geometry::Sizes fitted = geometry::sizes_of(dialog);
    fitted.content_left += 40;
    fitted.content_width -= 40;
    oa::ui::kit::PlacedRows narrow;
    std::ignore =
        geometry::place_section(dialog, Page::game_content, {}, nullptr, wide, &narrow, &fitted);
    CHECK(!narrow.rows.empty());
    for (const oa::ui::kit::PlacedRow& row : narrow.rows) {
        for (const auto& line : row.hints)
            CHECK(line.width <= 0 || line.x >= fitted.content_left);
        for (const std::string& line : row.view.hints)
            CHECK(wide(line) <= fitted.content_width);
    }
}

void game_content_lists_each_bundle_and_three_actions() {
    settings::Dialog dialog = opened(Page::game_content);
    settings::GameContentRow base;
    base.id = "totala";
    base.name = "Total Annihilation";
    base.size = "188 MB";
    base.check = "All files match";
    settings::GameContentRow expansion;
    expansion.id = "core-contingency";
    expansion.name = "Core Contingency";
    expansion.size = "42 MB";
    expansion.check = "1 file differs";
    expansion.warn = true;
    dialog.game_content.push_back(std::move(base));
    dialog.game_content.push_back(std::move(expansion));
    CHECK(geometry::open_rows(dialog).rows.rows.size() == 6);
    dialog.scroll[static_cast<std::size_t>(Page::game_content)] = 100000;
    const auto parts = settings::dialog_layout(dialog);
    CHECK(find_part(parts, "Total Annihilation", settings::no_control) != nullptr);
    CHECK(find_part(parts, "Core Contingency", settings::no_control) != nullptr);
    CHECK(find_part(parts, "188 MB · All files match", settings::no_control) != nullptr);
    CHECK(find_part(parts, "42 MB · 1 file differs", settings::no_control) != nullptr);
    const auto press = [&dialog](std::string_view caption) {
        const auto laid = settings::dialog_layout(dialog);
        const settings::LayoutPart* part = find_part(laid, caption, settings::no_control);
        CHECK(part != nullptr);
        if (part == nullptr)
            return DialogAction::none;
        return click(dialog, centre(part->rect));
    };
    CHECK(press("Check again") == DialogAction::check_content);
    CHECK(press("Copy again from…") == DialogAction::copy_content_again);
    CHECK(press("Open folder") == DialogAction::open_content_folder);

    dialog.game_content[0].copying = true;
    dialog.game_content[0].bar_done = 400;
    dialog.game_content[0].bar_whole = 1000;
    dialog.game_content[0].check = "2 of 5 · 40%";
    const auto copying = settings::dialog_layout(dialog);
    CHECK(find_part(copying, "Cancel", settings::no_control) != nullptr);
    CHECK(find_part(copying, "2 of 5 · 40%", settings::no_control) != nullptr);
    const oa::ui::kit::DisplayList drawn = geometry::dialog_list(dialog, nullptr);
    bool bar = false;
    for (const oa::ui::kit::Item& item : drawn.items)
        if (item.role == oa::ui::kit::Role::progress)
            bar = true;
    CHECK(bar);
    CHECK(press("Cancel") == DialogAction::cancel_content_copy);
}

int main(int argc, char** argv) {
    if (oa::test::game_data_requested(argc, argv))
        fonts_load_and_every_text_fits_its_place();
    else {
        opening_shows_the_settings_in_effect();
        the_oa_button_shows_the_icon_or_the_mark();
        the_oa_mark_alone_shows_the_icon_or_the_button_mark();
        every_part_lies_inside_the_dialog_and_apart();
        each_section_shows_its_rows();
        the_mod_limits_rows_show_the_counts_and_take_no_input();
        game_content_lists_each_bundle_and_three_actions();
        game_content_sentence_breaks_where_measured();
        a_click_on_an_entry_shows_its_section();
        every_control_is_pressed_where_it_is_drawn();
        switches_take_a_click_on_either_half_and_keys();
        language_and_text_shows_a_language_five_switches_and_a_size();
        language_drop_down_names_each_language_in_itself();
        language_drop_down_opens_marks_and_chooses();
        language_drop_down_locks_by_the_command_line();
        available_languages_list_download_and_show_their_state();
        drop_down_lists_scroll_and_open_over_their_field();
        path_tails_keep_the_last_components_that_fit();
        text_size_runs_from_half_to_three_times_in_tenths();
        text_size_waits_for_the_modern_fonts();
        a_slider_lock_drops_when_its_label_does_not_fit();
        a_language_locks_the_modern_fonts_and_unicode_chat();
        every_stop_maps_to_its_value_and_back();
        sliders_follow_the_pointer_and_the_arrows();
        screen_size_offers_the_displays_sizes();
        screen_size_shows_custom_for_a_window_sized_by_hand();
        the_level_strip_picks_a_level();
        enter_keeps_and_escape_cancels();
        the_footer_buttons_restore_cancel_and_keep();
        the_header_close_mark_closes_as_cancel_does();
        the_focus_moves_round_every_control();
        the_arrows_move_by_geometry();
        locks_show_their_text_and_hold_their_settings();
        the_view_and_the_scroll_bar_keep_their_places();
        sections_that_fit_do_not_scroll();
        a_long_section_scrolls_by_its_overflow();
        control_numbers_put_the_rows_after_every_fixed_control();
        the_layout_lists_the_parts_wholly_in_the_view();
        the_wheel_scrolls_by_notches_and_carries_fractions();
        the_scroll_keys_scroll_whatever_has_the_focus();
        the_focus_scrolls_its_row_into_view();
        the_scroll_bar_follows_a_drag();
        a_cut_row_answers_only_where_it_shows();
        offsets_are_kept_for_each_section_until_the_dialog_opens_again();
        the_hover_follows_the_rows_under_a_still_pointer();
        locked_switch_rows_keep_their_value_in_sight();
        the_graphics_page_scrolls_its_rows();
        interface_size_offers_150_with_the_modern_fonts();
        menu_scaling_and_native_density_show_and_change();
        smooth_motion_shows_and_changes();
        visual_effects_show_and_change();
        hardware_acceleration_moves_the_effects();
        explosion_flash_shows_and_changes();
        fog_of_war_shows_and_changes();
        zoomed_out_units_show_and_change();
        window_edge_and_edge_stickiness_show_and_change();
        window_frame_shows_and_changes();
        every_switch_reads_and_sets_through_one_table();
        the_new_rows_lock_in_their_own_forms();
        hardware_acceleration_shows_its_status();
        the_dialog_draws_its_faces_and_accents(settings::DialogFonts{});
        the_mod_options_dialog_lists_its_own_sections();
        the_mod_options_change_only_the_mod_options();
        a_mod_set_snap_radius_is_locked();
        the_dialog_draws_the_scroll_bar_and_clips_the_rows();
        the_graphics_page_draws_its_locked_rows();
        the_dialog_draws_on_the_smooth_form_on_whole_pixels();
        developer_mode_lists_every_hack_by_area_closed_at_first();
        the_kit_gallery_row_moves_only_developers_list();
        areas_and_hacks_open_and_close();
        hacks_show_their_titles_alphabetically();
        developer_mode_off_shows_the_profile_and_takes_no_change();
        hacks_turn_on_and_off_and_their_overrides_follow_the_profile();
        every_kind_of_parameter_has_its_control();
        restore_profile_values_and_show_active_only();
        restore_defaults_turns_developer_mode_off_and_keeps_the_overrides();
        the_list_scrolls_and_shows_the_focused_row();
        summaries_break_into_lines_the_fonts_hold();
        the_open_list_keeps_its_parts_apart();
        the_developer_section_draws_its_parts();
        touch_is_listed_only_with_touch_controls();
        touch_shows_its_rows_and_their_values();
        touch_changes_only_its_entry_and_the_sections_under_it();
        updates_lists_its_rows_and_the_compact_list_of_thirteen_scrolls();
        controller_is_listed_only_once_a_gamepad_sent_input();
        controller_shows_its_rows_and_their_values();
        the_steam_input_notice_shows_while_it_applies();
        a_steam_deck_names_its_screen_rate_under_maximum_frame_rate();
        controller_changes_only_its_entry_and_the_sections_under_it();
        a_finger_takes_the_nearest_control();
        game_files_is_listed_only_where_the_host_says();
        game_files_shows_what_is_installed_the_backups_and_the_folder();
        manage_asks_the_host_and_the_backups_switch_changes_at_once();
        language_text_lists_one_section_and_draws_without_the_game_fonts();
        dark_letters_on_the_accent_are_drawn_bare();
        your_files_shows_the_folder_and_opens_its_folders();
        file_type_hint_when_the_opener_stays();
        hint_lines_part_while_the_modern_fonts_draw_the_words();
        hint_lines_stay_whole_at_the_end_of_a_section();
        the_notice_wraps_its_text_and_places_its_buttons();
        the_notice_answers_its_buttons_and_keys();
        the_notice_draws_in_the_dialogs_colours(settings::DialogFonts{});
        language_links_to_the_library_where_the_host_says();
        the_endpoint_names_every_control_and_part_once();
        the_larger_classes_lay_out_every_section_inside_the_dialog();
        the_larger_classes_keep_hints_to_their_lines();
        the_larger_classes_show_more_graphics_rows();
        the_larger_classes_keep_the_footer_buttons();
        the_larger_classes_press_each_control_where_it_is_drawn();
        registered_pages_list_under_their_owners_and_write_on_ok();
    }
    if (failures != 0)
        return 1;
    std::cout << "settings dialog: ok\n";
    return 0;
}

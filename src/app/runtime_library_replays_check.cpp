// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-library-replays: the Library's Replays section over made-up replays
// in a scratch player's folder. The replays are written from one recorded
// game, library-replays-source.oademo in the run's working folder (the test
// records it first with --record-game): copies under other dates, kinds, maps
// and players, a .tad of the same recording, an imported one, a damaged file
// and one whose map is not installed. The check walks the section:
//
// 1. Replays is the second section, under Active, with an empty Recordings
//    folder and with replays; the empty section says so.
// 2. WATCH plays ticks of the recorded game in the running game; leaving its
//    match opens the Library again on Replays, the same row selected and the
//    list scrolled as it was. A replay whose map is not installed is refused
//    in the card before anything loads; a recording whose unit table is not
//    the installed one is refused after its match began loading, and the game
//    comes back to the main menu with the Library on the same row and the
//    refusal in its card.
// 3. KEEP is in `kept.json`, by recording, and a Library whose section state
//    is made afresh reads it back, as the next start does: with the index
//    deleted, truncated or filled with noise as well. A ten-player replay's
//    card scrolls to its last fact.
// 4. Each filter, the kind menu, the search and the sort give the rows they
//    should, a damaged file listed as damaged.
// 5. TURN ON RECORDING in the empty section writes the setting.
// 6. EXPORT writes one replay as a .oademo, then three as .tad files, into
//    Exported replays under the player's folder. A .tad chosen after the
//    measurement is written without a compression. A long recording is still
//    measuring on the frame after the dialog opens, frames are presented
//    while it measures, and Cancel during the measurement and during the
//    write each leave the folder empty. With --snapshot, the dialog's states
//    (a .oademo, a .tad with its warnings, exporting, done, a failed write
//    and nothing selected) at 640x480, 960x540, 1280x720 and 1920x1080.
// 7. After that walk, the retention question appears once for the replays
//    older than 30 days. Keep them all sets Forever and removes nothing.
//    A second consideration asks nothing. Let them go then moves exactly the
//    replays that are due, and leaves a kept replay, one whose bytes changed
//    after Keep, an imported one, a downloaded one, one marked exported, and
//    both copies of one kept recording. Exporting a replay older than the
//    span then marks it, and the next consideration leaves that replay and
//    moves one of the same age that was not exported. A cut exported.json is
//    set aside; while that set-aside file is there, an export writes nothing
//    and every old replay stays. Deleting a replay while exported.json is
//    damaged does not write a new file, and the next start leaves the
//    exported replays. The move runs off the main thread; the check waits
//    until it has finished.
// 8. Delete replay asks a question over the Library, which names the replay
//    or how many are marked and says where they go; Cancel, Escape and Enter
//    on its marked Cancel keep them, and its Delete moves the marked replays
//    to the trash and takes their rows out of the list and the index.
//    Deleting one copy of a kept recording leaves the other kept. A kept
//    file whose bytes changed reads Kept. A delete the trash refuses leaves
//    the file and says so on the card. A kept.json cut in half is set
//    aside, its whole marks are kept, and pressing Keep on another replay
//    and considering again leaves the earlier kept replays, the ones whose
//    marks were cut off too.
// 9. A campaign replay is titled by its mission name, with trailing spaces
//    trimmed. The stored name stays as the file has it, and the map's file
//    name stays searchable. On a build that can start a process, the pass's
//    own unread replays say Reading…, and closing the Library leaves no
//    index process. A key the automation endpoint holds marks a second row.
//    Watch from starts at the tick the earlier Watch left, which is at least
//    300, and the jump is the replay's own. A replay watched to its end
//    offers Watch. A watcher has no result. The pass's own card shows a
//    result and, scrolled, its timeline strip.
//
// With --snapshot, the section at 640x480, 960x540, 1280x720 and 1920x1080, empty and
// with its long list, while the folder is first read, with a damaged replay,
// the ten-player card scrolled to its foot, the refusals, the Reading…
// chip and the indexed replay's result and timeline strip, beside the
// snapshot. Everything the check writes, including Exported replays and the
// Mods/ridge folder the content note installs, is removed again as it ends,
// so a second run starts as the first did.

#include "library_host.hpp"
#include "oa_layer.hpp"
#include "oa_layer_check.hpp"

#include "oa/app/automation_host.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/app/package_install/inbox.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/data/replays/import.hpp"
#include "oa/data/replays/replays.hpp"

#include "oa/formats/oademo/oademo.hpp"
#include "oa/formats/oademo/timeline_index.hpp"
#include "oa/formats/tad.hpp"
#include "oa/platform/files.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/replays.hpp"
#include "oa/ui/library/screen.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef PROCESS_QUERY_LIMITED_INFORMATION
#define PROCESS_QUERY_LIMITED_INFORMATION PROCESS_QUERY_INFORMATION
#endif
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#ifndef OA_PROCESS_SPAWNING
#error "OA_PROCESS_SPAWNING (0 or 1) says whether the game may start other programs"
#endif

namespace oa::app {

namespace {

namespace fs = std::filesystem;
namespace library = oa::ui::library;
namespace oademo = oa::formats::oademo;
namespace replays = oa::data::replays;
namespace tad = oa::formats::tad;

/// The recorded game the test leaves in the working folder.
constexpr std::string_view kSourceName = "library-replays-source.oademo";
/// Seconds in a day.
constexpr int64_t kDaySeconds = 24 * 60 * 60;
/// The made-up replays that fill This month, one a day, so the list scrolls.
constexpr int kFillers = 20;
/// The days ago the recorded game is dated, in the middle of the list.
constexpr int kWatchedDaysAgo = 10;
/// The most frames a wait runs before the check gives up.
constexpr int kMostFrames = 600;
/// The most frames the long recording's measurement or write may take.
constexpr int kLongFrames = 4000;
/// Incompressible packets in the long recording, so measuring it takes frames.
constexpr int kLongPackets = 200;
/// Bytes in each of those packets, under a chunk's limit.
constexpr int kLongPacketBytes = 20000;
/// The ticks WATCH must play before the match is left. Far enough in that a
/// Watch from which started at the beginning cannot pass for it.
constexpr uint32_t kWatchedTicks = 300;
/// The map no installation holds, which WATCH refuses.
constexpr std::string_view kMissingMap = "Nowhere Isle";
/// The made-up planet and sector of an extension's kind of game.
constexpr std::string_view kPlanet = "Corvane";
constexpr std::string_view kSector = "Low Reach";

/// Stops the check.
///
/// @param ok the step held
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("library replays check: " + std::string(what));
}

/// Returns the path of one step's snapshot: <stem>-<step>.ppm beside --snapshot.
///
/// @param snapshot the --snapshot path
/// @param step the step's name
/// @return the step's snapshot path
fs::path step_snapshot(const fs::path& snapshot, std::string_view step) {
    fs::path file = snapshot.parent_path() / snapshot.stem();
    file += '-' + std::string(step) + ".ppm";
    return file;
}

/// Reads a whole file.
///
/// @param file the file
/// @return its bytes; empty when it cannot be read
std::vector<uint8_t> read_bytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Writes a whole file, making its folder.
///
/// @param file the file
/// @param bytes what it holds
void write_bytes(const fs::path& file, std::span<const uint8_t> bytes) {
    std::error_code made;
    fs::create_directories(file.parent_path(), made);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    require(static_cast<bool>(out), "cannot write " + platform::path_to_utf8(file));
}

/// Writes a whole text file, making its folder.
///
/// @param file the file
/// @param text what it holds
void write_text(const fs::path& file, std::string_view text) {
    write_bytes(file, {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
}

/// Writes an instant as a replay's description does.
///
/// @param seconds seconds since 1970 (UTC)
/// @return `YYYY-MM-DDTHH:MM:SSZ`
std::string utc_text(int64_t seconds) {
    const auto instant = static_cast<std::time_t>(seconds);
    std::tm parts{};
#if defined(_WIN32)
    gmtime_s(&parts, &instant);
#else
    gmtime_r(&instant, &parts);
#endif
    char text[32];
    const std::size_t length = std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &parts);
    return std::string(text, length);
}

/// One made-up replay.
struct MadeReplay {
    std::string path{};                 ///< below Recordings
    int64_t days_ago{};                 ///< how long before now it was recorded, in days
    std::string kind{};                 ///< game.kind
    std::string map{};                  ///< the map's name its description gives
    std::vector<std::string> players{}; ///< the players' names; empty keeps the recording's
    uint64_t length_ms{};               ///< 0 keeps the recording's
    bool after_crash{};                 ///< finished after a crash
    std::string kind_object{};          ///< the object under game named by the kind; empty for none
    std::string mod{};                  ///< the mod's name; empty for none
    /// Gives the replay a recording of its own, so that its identity differs
    /// from every other's; 0 keeps the recording's.
    uint32_t variant{};
    std::vector<std::string> expansions{}; ///< content.expansions
    std::vector<std::string> mutators{};   ///< content.mutators
    /// The player, counted from 0, who recorded the game. -1 leaves none.
    int recorded_at{-1};
};

/// Returns the recording with its first player renamed, so that it has an
/// identity (its SHA-256) of its own.
///
/// @param recording the recording
/// @param variant the number added to the name
/// @return the recording as a TA Demo file
std::vector<uint8_t> recording_variant(std::span<const uint8_t> recording, uint32_t variant) {
    const oa::formats::tad::ParseResult parsed = oa::formats::tad::parse(recording);
    require(parsed.ok(), "the recorded game does not parse");
    oa::formats::tad::Demo demo = *parsed.demo;
    require(!demo.players.empty(), "the recorded game has no player");
    demo.players[0].name += "#" + std::to_string(variant);
    const oa::formats::tad::WriteResult written = oa::formats::tad::write(demo);
    require(written.ok(), "the recording with another player could not be written");
    return written.bytes;
}

/// Returns the recording with three quarters of its unit table's records left
/// out: it loads, and then differs from the installed unit definitions.
///
/// @param recording the recording
/// @return the recording as a TA Demo file
std::vector<uint8_t> recording_with_other_units(std::span<const uint8_t> recording) {
    const oa::formats::tad::ParseResult parsed = oa::formats::tad::parse(recording);
    require(parsed.ok(), "the recorded game does not parse");
    oa::formats::tad::Demo demo = *parsed.demo;
    const auto records = oa::formats::tad::unit_check_records(demo);
    require(records.size() >= 4, "the recorded game holds too few unit records to change");
    const std::size_t each = demo.unit_checks.size() / records.size();
    demo.unit_checks = demo.unit_checks.first(records.size() / 4 * each);
    const oa::formats::tad::WriteResult written = oa::formats::tad::write(demo);
    require(written.ok(), "the recording with another unit table could not be written");
    return written.bytes;
}

/// Writes a replay from the recorded game with another description.
///
/// @param recordings the Recordings folder
/// @param source the recorded game
/// @param made what differs
/// @param now the clock now, in seconds since 1970
void write_made(
    const fs::path& recordings, const oademo::Replay& source, const MadeReplay& made, int64_t now
) {
    oademo::Replay replay = source;
    if (made.variant != 0) {
        replay.recording = recording_variant(source.recording, made.variant);
        oademo::set_recording_identity(replay);
    }
    replay.meta.recorded.at = utc_text(now - made.days_ago * kDaySeconds - 3600);
    replay.meta.recorded.after_crash = made.after_crash;
    replay.meta.game.kind = made.kind;
    replay.meta.game.kind_object.reset();
    if (!made.kind_object.empty())
        replay.meta.game.kind_object = made.kind_object;
    if (!made.map.empty())
        replay.meta.map.name = made.map;
    if (made.length_ms != 0)
        replay.meta.game.length_ms = made.length_ms;
    if (!made.players.empty()) {
        replay.meta.players.clear();
        for (std::size_t place = 0; place < made.players.size(); ++place) {
            oademo::PlayerInfo player;
            player.number = static_cast<int>(place + 1);
            player.name = made.players[place];
            player.side = place % 2 == 0 ? "arm" : "core";
            player.colour = static_cast<int>(place % 10);
            player.kind = "human";
            replay.meta.players.push_back(std::move(player));
        }
        if (made.recorded_at >= 0 &&
            static_cast<std::size_t>(made.recorded_at) < replay.meta.players.size()) {
            replay.meta.players[static_cast<std::size_t>(made.recorded_at)].recorded_here = true;
            replay.meta.players[static_cast<std::size_t>(made.recorded_at)].kind = "watcher";
        }
    }
    if (!made.mod.empty()) {
        oademo::ModRef mod;
        mod.id = "ridge";
        mod.name = made.mod;
        mod.version = "2.8";
        mod.revision = 1;
        replay.meta.content.mod = std::move(mod);
    }
    if (!made.expansions.empty())
        replay.meta.content.expansions = made.expansions;
    if (!made.mutators.empty())
        replay.meta.content.mutators = made.mutators;
    const oademo::WriteResult written = oademo::write(replay);
    require(written.ok(), "a made-up replay could not be written: " + written.error);
    std::vector<fs::path> parts;
    fs::path file = recordings;
    std::size_t at = 0;
    while (at <= made.path.size()) {
        const std::size_t slash = made.path.find('/', at);
        const std::size_t end = slash == std::string::npos ? made.path.size() : slash;
        file /= platform::path_from_utf8(made.path.substr(at, end - at));
        if (slash == std::string::npos)
            break;
        at = slash + 1;
    }
    write_bytes(file, written.bytes);
}

/// Returns the paths of the visible replays, in the order shown.
///
/// @param model the Library's model
/// @return the paths
std::vector<std::string> shown_paths(const library::Library& model) {
    std::vector<std::string> paths;
    for (const std::size_t index : model.replays.visible)
        paths.push_back(model.inputs.replays.rows[index].replay.path);
    return paths;
}

/// Tells whether a list holds a path.
///
/// @param paths the list
/// @param path the path
/// @return true when it does
bool holds(const std::vector<std::string>& paths, std::string_view path) {
    return std::find(paths.begin(), paths.end(), path) != paths.end();
}

#if !defined(__ANDROID__) && OA_PROCESS_SPAWNING
/// Tells whether a process has gone.
///
/// @param pid the process; 0 counts as gone
/// @return true when it is not running
bool process_gone(int pid) {
    if (pid <= 0)
        return true;
#if defined(_WIN32)
    const HANDLE handle =
        OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr)
        return true;
    DWORD code = 0;
    const BOOL read = GetExitCodeProcess(handle, &code);
    CloseHandle(handle);
    return read == FALSE || code != STILL_ACTIVE;
#else
    if (kill(pid, 0) == 0)
        return false;
    return errno == ESRCH;
#endif
}
#endif

} // namespace

std::vector<std::string> replay_texts_cut(
    const oa::ui::kit::DisplayList& list,
    const library::Library& model,
    bool card,
    library::MenuKind menu,
    int32_t window_width,
    bool lone_warning_cut
) {
    namespace kit = oa::ui::kit;
    std::vector<std::string> problems;
    const auto cut_short = [](std::string_view text) {
        return text.ends_with("\xE2\x80\xA6") || text.ends_with("...");
    };
    // What the layout draws, by kind: nav captions, field and menu words,
    // buttons' captions, and every text.
    std::vector<std::string> nav;
    std::vector<std::string> fields;
    std::vector<std::string> menu_items;
    std::vector<std::string> buttons;
    std::vector<std::string> chips;
    std::vector<std::string> texts;
    for (const kit::Item& item : list.items) {
        switch (item.role) {
        case kit::Role::nav:
            if (const auto* look = std::get_if<kit::NavLook>(&item.look))
                for (const kit::NavEntry& entry : look->entries)
                    nav.push_back(entry.caption);
            break;
        case kit::Role::choice:
            if (const auto* look = std::get_if<kit::ChoiceLook>(&item.look))
                fields.push_back(look->text);
            break;
        case kit::Role::choice_menu:
            if (const auto* look = std::get_if<kit::ChoiceMenuLook>(&item.look))
                for (const std::string& shown : look->shown)
                    menu_items.push_back(shown);
            break;
        case kit::Role::button:
            if (const auto* look = std::get_if<kit::ButtonLook>(&item.look))
                buttons.push_back(look->caption);
            break;
        case kit::Role::chip:
            if (const auto* look = std::get_if<kit::ChipLook>(&item.look))
                chips.push_back(look->text);
            break;
        case kit::Role::text:
            texts.push_back(item.text);
            break;
        default:
            break;
        }
    }
    const auto holds = [](const std::vector<std::string>& drawn, std::string_view word) {
        return std::find(drawn.begin(), drawn.end(), word) != drawn.end();
    };
    const auto must =
        [&](const std::vector<std::string>& drawn, std::string_view what, const std::string& word) {
            if (!word.empty() && !holds(drawn, word))
                problems.push_back(std::string(what) + " \"" + word + "\" is not drawn whole");
        };
    // Compact's card for one replay has no section list. The list still has
    // the switch and the four filters, and a cut word on either screen fails.
    if (model.replays.source == library::ReplaySource::core_prime) {
        if (!nav.empty()) {
            for (const library::CorePrimeFilter filter :
                 {library::CorePrimeFilter::latest,
                  library::CorePrimeFilter::longest,
                  library::CorePrimeFilter::downloads,
                  library::CorePrimeFilter::planet})
                must(nav, "the filter", library::core_filter_title(model, filter));
            must(
                buttons,
                "the switch",
                std::string(library::shown(model.text, library::words::core_on_computer))
            );
            must(
                buttons,
                "the switch",
                std::string(library::shown(model.text, library::words::core_on_prime))
            );
            if (model.replays.core_filter == library::CorePrimeFilter::planet) {
                const std::string caption =
                    model.replays.planet.empty()
                        ? std::string(library::shown(model.text, library::words::core_by_planet))
                        : model.replays.planet;
                must(fields, "the planet field", caption);
            }
        }
        if (card && library::selected_replay(model) != nullptr) {
            const library::ReplayRow* selected = library::selected_replay(model);
            const std::string_view download = selected->saved_here ? library::words::core_downloaded
                                                                   : library::words::fact_download;
            must(buttons, "Download", std::string(library::shown(model.text, download)));
            must(
                buttons,
                "Watch",
                std::string(library::shown(model.text, library::words::action_watch))
            );
        }
        // A sentence may wrap onto several lines. One that already ends in an
        // ellipsis is whole when the drawn line is that ending. A cut line is not.
        const auto sentence = [&](std::string_view word) {
            return std::string(library::shown(model.text, word));
        };
        const std::string allowed[] = {
            sentence(library::words::core_reading),
            sentence(library::words::core_getting_replay),
            sentence(library::words::action_open_replay_file),
        };
        const auto cut_line = [&](std::string_view drawn) {
            if (!cut_short(drawn))
                return false;
            for (const std::string& whole : allowed)
                if (drawn == whole || whole.ends_with(drawn))
                    return false;
            return true;
        };
        // A row may shorten a kind or a map. The empty section's own lines may not.
        if (model.replays.visible.empty()) {
            for (const std::string& drawn : texts)
                if (cut_line(drawn))
                    problems.push_back("the list's text \"" + drawn + "\" is cut");
        }
        for (const std::string& drawn : buttons)
            if (cut_line(drawn))
                problems.push_back("the button \"" + drawn + "\" is cut");
    } else if (!nav.empty()) {
        for (const library::ReplayFilter filter :
             {library::ReplayFilter::this_month,
              library::ReplayFilter::older,
              library::ReplayFilter::kept,
              library::ReplayFilter::imported})
            must(nav, "the filter", library::replay_filter_title(model, filter));
        must(
            fields, "the kind menu's field", library::replay_kind_title(model, model.replays.kind)
        );
        must(
            fields, "the sort menu's field", library::replay_sort_title(model, model.replays.sort)
        );
    }
    if (menu == library::MenuKind::replay_planet)
        for (const std::string& planet : model.inputs.replays.planets)
            must(menu_items, "the planet menu", planet);
    if (menu == library::MenuKind::replay_kind)
        for (const library::ReplayKindItem& item : library::replay_kind_items(model))
            must(menu_items, "the kind menu's item", item.title);
    if (menu == library::MenuKind::replay_sort)
        for (const library::ReplaySort sort :
             {library::ReplaySort::newest,
              library::ReplaySort::longest,
              library::ReplaySort::map,
              library::ReplaySort::players})
            must(menu_items, "the sort menu's item", library::replay_sort_title(model, sort));
    // A need's chip is on the row. A short one is drawn whole. A name the
    // catalogue allows at its longest may be shortened, and the chip still
    // starts with the word that says it is needed.
    if (!nav.empty() || !chips.empty()) {
        const std::string pattern(library::shown(model.text, library::words::replay_needs_name));
        const std::size_t brace = pattern.find('{');
        const std::string prefix = brace == std::string::npos ? pattern : pattern.substr(0, brace);
        const std::vector<library::ReplayRow>& rows = library::shown_replay_rows(model);
        for (const std::size_t index : model.replays.visible) {
            if (index >= rows.size())
                continue;
            const library::ReplayRow& shown_row = rows[index];
            if (model.replays.selected != shown_row.replay.path)
                continue;
            const std::string chip = library::replay_need_chip(model, shown_row);
            if (chip.empty())
                continue;
            if (chip.size() <= 48) {
                must(chips, "the chip", chip);
                continue;
            }
            bool present = false;
            for (const std::string& drawn : chips) {
                if (drawn == chip || (!prefix.empty() && drawn.starts_with(prefix)))
                    present = true;
            }
            if (!present)
                problems.push_back("the chip \"" + chip + "\" is not on the row");
        }
    }
    // At Regular the chip may take the players' line, and the map's name stays
    // readable: some drawn text still starts with its first eight characters.
    if (window_width == 960) {
        const auto prefix_of = [](std::string_view text) {
            std::string prefix;
            std::size_t scalars = 0;
            std::size_t index = 0;
            while (index < text.size() && scalars < 8) {
                const auto lead = static_cast<unsigned char>(text[index]);
                std::size_t bytes = 1;
                if ((lead & 0xE0U) == 0xC0U)
                    bytes = 2;
                else if ((lead & 0xF0U) == 0xE0U)
                    bytes = 3;
                else if ((lead & 0xF8U) == 0xF0U)
                    bytes = 4;
                if (index + bytes > text.size())
                    break;
                prefix.append(text.substr(index, bytes));
                index += bytes;
                ++scalars;
            }
            return prefix;
        };
        const auto body_of = [](std::string_view drawn) {
            if (drawn.ends_with("\xE2\x80\xA6"))
                return drawn.substr(0, drawn.size() - 3);
            if (drawn.ends_with("..."))
                return drawn.substr(0, drawn.size() - 3);
            return drawn;
        };
        if (const library::ReplayRow* selected = library::selected_replay(model)) {
            const std::string title = library::replay_title(*selected);
            const std::string head = prefix_of(title);
            if (!head.empty()) {
                bool readable = false;
                for (const std::string& drawn : texts) {
                    if (body_of(drawn).starts_with(head))
                        readable = true;
                }
                if (!readable)
                    problems.push_back("the map name \"" + title + "\" is not readable");
            }
        }
    }
    // The dialog's warnings, descriptions and format captions wrap. A list
    // row and the Where line may still cut themselves, and are not named here.
    if (model.export_dialog.open && !model.export_dialog.browsing) {
        const auto cut_line = [&](std::string_view text) {
            if (!cut_short(text))
                return;
            problems.push_back("the export dialog's text \"" + std::string(text) + "\" is cut");
        };
        const auto inside = [](const kit::Rect& inner, const kit::Rect& outer) {
            return inner.x >= outer.x && inner.y >= outer.y &&
                   inner.x + inner.width <= outer.x + outer.width &&
                   inner.y + inner.height <= outer.y + outer.height;
        };
        // The dialog's panel is the last fill. Only the items drawn with it
        // belong to the dialog: a list row under the shade can share its
        // rectangle and is allowed to cut itself.
        std::size_t panel_at = list.items.size();
        kit::Rect panel{};
        for (std::size_t at = 0; at < list.items.size(); ++at) {
            if (list.items[at].role != kit::Role::fill)
                continue;
            panel_at = at;
            panel = list.items[at].rect;
        }
        const bool have_panel = panel_at < list.items.size();
        const kit::Control* lone_warning = nullptr;
        std::size_t warnings_shown = 0;
        for (const kit::Control& control : list.controls) {
            if (control.name.starts_with("library.export.warning.")) {
                cut_line(control.text);
                lone_warning = &control;
                ++warnings_shown;
            }
            if (control.name == "library.export.format.oademo" ||
                control.name == "library.export.format.tad")
                cut_line(control.text);
        }
        // A warning alone on its page and taller than it is cut to the lines
        // that fit, the last ending with the ellipsis. Its label, read above,
        // still holds the whole sentence.
        std::size_t cut_ending = list.items.size();
        if (lone_warning_cut && warnings_shown == 1) {
            for (std::size_t at = panel_at; at < list.items.size(); ++at)
                if (list.items[at].role == kit::Role::text &&
                    inside(list.items[at].rect, lone_warning->rect))
                    cut_ending = at;
        }
        for (std::size_t at = panel_at; at < list.items.size(); ++at) {
            const kit::Item& item = list.items[at];
            if (item.role == kit::Role::text) {
                for (const kit::Control& control : list.controls) {
                    if (!control.name.starts_with("library.export.warning."))
                        continue;
                    if (inside(item.rect, control.rect) && at != cut_ending)
                        cut_line(item.text);
                }
                if (have_panel && item.colour == kit::colour::hint && inside(item.rect, panel))
                    cut_line(item.text);
            }
            if (item.role != kit::Role::radio)
                continue;
            const kit::Control* format = nullptr;
            for (const kit::Control& one : list.controls)
                if (one.id == item.control)
                    format = &one;
            if (format == nullptr || (format->name != "library.export.format.oademo" &&
                                      format->name != "library.export.format.tad"))
                continue;
            if (const auto* look = std::get_if<kit::RadioLook>(&item.look))
                cut_line(look->caption);
        }
    }
    if (!card || library::selected_replay(model) == nullptr)
        return problems;
    for (const library::ReplayButton& button : library::replay_buttons(model)) {
        if (button.text.empty() || holds(buttons, button.text))
            continue;
        // A primary caption wider than the bar is drawn as the shorter Watch words.
        if (button.action == library::Action::watch_replay) {
            const std::string shorter = library::replay_watch_short(model);
            if (!shorter.empty() && holds(buttons, shorter))
                continue;
        }
        problems.push_back("the button \"" + button.text + "\" is not drawn whole");
    }
    const library::ReplayRow& row = *library::selected_replay(model);
    for (const library::ReplayFact& fact : library::replay_facts(model, row))
        must(texts, "the fact's label", fact.label);
    // Nothing of the card's own ends in an ellipsis: a long value wraps.
    const kit::Control* label = nullptr;
    for (const kit::Control& control : list.controls)
        if (control.name == "library.status" && control.clip.width > 0)
            label = &control;
    if (label != nullptr)
        for (const kit::Item& item : list.items)
            if (item.role == kit::Role::text && item.clip.x == label->clip.x &&
                item.clip.y == label->clip.y && item.clip.width == label->clip.width &&
                item.clip.height == label->clip.height && cut_short(item.text))
                problems.push_back("the card's text \"" + item.text + "\" is cut");
    return problems;
}

void Runtime::check_library_replays() {
    require(sdl_.window != nullptr, "the check needs the SDL presenter");
    require(!user_folder_.empty(), "the run has no folder of the player's own");
    const fs::path recordings = user_folder_ / std::string(recordings_folder_name);
    const std::vector<uint8_t> source_bytes =
        read_bytes(fs::current_path() / std::string(kSourceName));
    require(!source_bytes.empty(), std::string(kSourceName) + " is not in the working folder");
    const oademo::ReadResult source_read = oademo::read(source_bytes);
    require(source_read.ok(), std::string(kSourceName) + " does not read: " + source_read.error);
    const oademo::Replay& source = *source_read.replay;

    bool running = true;
    LibraryHost& host = library_host();

    // Everything the check writes goes as it ends, on every path.
    const fs::path exported = user_folder_ / "Exported replays";

    struct CleanUp {
        Runtime& runtime;
        fs::path recordings;
        fs::path exported;
        fs::path ridge;

        ~CleanUp() {
            std::error_code removed;
            runtime.replay_trash_refusal_.clear();
            if (!restore_write.empty()) {
                fs::permissions(
                    restore_write, fs::perms::owner_write, fs::perm_options::add, removed
                );
            }
            fs::remove_all(recordings, removed);
            fs::remove_all(exported, removed);
            fs::remove_all(ridge, removed);
            for (const fs::path& left : runtime.replay_trash_left_)
                fs::remove(left, removed);
            runtime.replay_trash_left_.clear();
            runtime.preference_values_.erase(std::string(replays_keep_preference));
            runtime.preference_values_.erase(std::string(replays_retention_asked_preference));
            runtime.preference_values_[std::string(replays_record_preference)] = "on";
            runtime.preferences_dirty_ = true;
            runtime.flush_preferences();
        }

        /// A folder the check made unwritable, put back before the tree is removed.
        fs::path restore_write{};
    } clean_up{*this, recordings, exported, user_folder_ / "Mods" / "ridge"};

    {
        std::error_code removed;
        fs::remove_all(recordings, removed);
        fs::remove_all(exported, removed);
        fs::remove_all(clean_up.ridge, removed);
    }
    preference_values_[std::string(replays_record_preference)] = "on";
    preference_values_.erase(std::string(replays_keep_preference));
    preference_values_.erase(std::string(replays_retention_asked_preference));
    // The check and the pass it starts ask for no update and no catalogue.
    preference_values_["open-annihilation.update-check"] = "never";
    preference_values_["open-annihilation.content-updates"] = "never";
    preferences_dirty_ = true;
    flush_preferences();

    const auto window_of = [this](int width, int height) {
        require(
            SDL_SetWindowSize(sdl_.window, width, height) && SDL_SyncWindow(sdl_.window),
            "the window did not take " + std::to_string(width) + 'x' + std::to_string(height)
        );
        apply_output_mode();
    };
    const auto frames = [&](int count) {
        for (int frame = 0; frame < count; ++frame) {
            run_frame(running);
            require(running, "a frame ended the run");
        }
    };
    // Let them go moves on a worker. Wait until that move has been taken
    // into the index, and fail rather than wait forever. The wait is timed,
    // not counted in frames: a frame here takes about a millisecond, and the
    // system's trash can take longer than hundreds of them on a busy machine.
    const auto drain_moves = [&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (replay_trash_sweep_ != nullptr && std::chrono::steady_clock::now() < deadline)
            frames(1);
        require(replay_trash_sweep_ == nullptr, "old replays were still being moved");
    };
    const auto named = [this](std::string_view name) -> std::optional<AutomationControl> {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        for (const AutomationControl& control : controls)
            if (control.name == name && control.visible && control.window_width > 0 &&
                control.window_height > 0)
                return control;
        return std::nullopt;
    };
    const auto click_named = [&](std::string_view name, SDL_Keymod held = SDL_KMOD_NONE) {
        const std::optional<AutomationControl> control = named(name);
        require(control.has_value(), std::string(name) + " is not shown");
        require(control->enabled, std::string(name) + " is not enabled");
        const oa::ui::display_layout::Point pixel{
            control->window_x + control->window_width / 2,
            control->window_y + control->window_height / 2
        };
        SDL_SetModState(held);
        for (const SDL_EventType type :
             {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event event = window_pointer_event(
                sdl_.window,
                sdl_.renderer,
                type,
                pixel,
                type == SDL_EVENT_MOUSE_MOTION ? 0 : SDL_BUTTON_LEFT
            );
            bool going = true;
            dispatch_event(event, going);
            require(going, "a click ended the run");
        }
        SDL_SetModState(SDL_KMOD_NONE);
        run_frame(running);
        require(running, "a frame ended the run");
    };
    // A key pressed and let go, then a frame.
    const auto press_key = [&](SDL_Keycode code) {
        for (const bool down : {true, false}) {
            SDL_Event event{};
            event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.windowID = SDL_GetWindowID(sdl_.window);
            event.key.key = code;
            event.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
            event.key.down = down;
            bool going = true;
            dispatch_event(event, going);
            require(going, "a key ended the run");
        }
        run_frame(running);
        require(running, "a frame ended the run");
    };
    // Delete replay asks its question over the Library: the card gains no
    // button, and the question's text and buttons are what a screen reader
    // reads.
    const auto delete_asks = [&](std::string_view title) {
        click_named("oa.library.action.delete");
        const std::optional<AutomationControl> body = named("oa.prompt.body");
        require(
            body.has_value() && body->text.find(title) != std::string::npos &&
                body->text.find("trash") != std::string::npos,
            "Delete replay did not ask \"" + std::string(title) + "\" with where the replays go"
        );
        const std::optional<AutomationControl> cancel = named("oa.prompt.cancel");
        const std::optional<AutomationControl> confirm = named("oa.prompt.delete");
        require(
            cancel.has_value() && cancel->text == "Cancel" && cancel->enabled &&
                confirm.has_value() && confirm->text.rfind("Delete replay", 0) == 0 &&
                confirm->enabled,
            "the delete question does not offer Cancel and Delete"
        );
    };
    // Pictures of the list that predate the index pass: every row is already
    // read, with nothing to show, and the footer is the count.
    bool seed_rows = false;
    bool hold_unread = false;
    const auto settle_rows = [&] {
        host.mark_replays_read(*this);
        if (oa_layer().find(library_screen_name) == nullptr)
            return;
        if (host.model.tab != library::Tab::replays)
            return;
        if (host.model.inputs.replays.rows.empty() || host.model.inputs.replays.reading)
            return;
        for (const library::ReplayRow& row : host.model.inputs.replays.rows) {
            require(!row.indexing, "a listed replay says Reading on a picture of the list");
            require(
                library::replay_reading(host.model, row).find("Reading") == std::string::npos,
                "a row says Reading"
            );
        }
        if (host.model.replays.visible.empty())
            return;
        const std::string status = library::replay_status(host.model);
        require(status.find("Reading") == std::string::npos, "the footer says " + status);
        require(
            status.find("replay") != std::string::npos, "the footer is not the count: " + status
        );
    };
    const auto snapshot = [&](const std::string& step, bool rows = false) {
        if (rows)
            settle_rows();
        if (options_.snapshot.empty())
            return;
        oa_layer().read_whole_window(true);
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        oa_layer().read_whole_window(false);
        write_ppm(step_snapshot(options_.snapshot, step), presented);
    };
    const auto top_is = [this](std::string_view name) {
        const LayerScreen* top = oa_layer().top();
        return top != nullptr && top->name() == name;
    };
    // Opens the Library on Replays and waits for its folder to be read.
    const auto open_replays = [&](std::string_view reading_step = {}) {
        open_library(library::Tab::replays);
        // From the moment the section opens, it says that it reads the folder;
        // it never says "No replays yet." until a read has ended with none.
        require(
            host.model.inputs.replays.reading || !host.replays_reading(),
            "the section does not say that it reads the folder while it does"
        );
        if (host.model.inputs.replays.reading && host.model.inputs.replays.rows.empty()) {
            const library::ReplayEmpty reading = library::replay_empty(host.model);
            require(
                reading.reading && reading.title.find("No replays yet.") == std::string::npos &&
                    !reading.turn_on && library::replay_status(host.model) == reading.title,
                "the section shows \"" + reading.title + "\" while it reads an empty folder"
            );
            if (!reading_step.empty())
                snapshot(std::string(reading_step));
        }
        for (int frame = 0; frame < kMostFrames && host.replays_reading(); ++frame) {
            run_frame(running);
            require(running, "a frame ended the run");
        }
        frames(2);
        require(top_is(library_screen_name), "the Library did not open");
        require(host.model.tab == library::Tab::replays, "the Library did not open on Replays");
        require(!host.replays_reading(), "the Recordings folder was not read");
        if (seed_rows && !hold_unread)
            settle_rows();
    };
    const auto close = [&] {
        close_library();
        frames(2);
        require(oa_layer().top() == nullptr, "the Library did not close");
    };
    // The section list's entries, top to bottom: Replays is second, under Active.
    const auto replays_second = [&](std::string_view when) {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        std::vector<AutomationControl> tabs;
        for (const AutomationControl& control : controls)
            if (control.name.rfind("oa.library.tab.", 0) == 0)
                tabs.push_back(control);
        std::sort(
            tabs.begin(), tabs.end(), [](const AutomationControl& a, const AutomationControl& b) {
                return a.window_y < b.window_y;
            }
        );
        require(
            tabs.size() == library::tab_total && tabs[0].name == "oa.library.tab.active" &&
                tabs[1].name == "oa.library.tab.replays" && tabs[2].name == "oa.library.tab.mods",
            "Replays is not the second section " + std::string(when)
        );
        std::cout << "library replays: Replays is the second section " << when << ": "
                  << tabs[1].text << '\n';
    };

    load(Screen::main_menu);
    require(screen_ == Screen::main_menu, "the main menu did not open");
    window_of(640, 480);
    frames(4);

    // 1 and 5. The empty section: Replays second, its words, then with the
    // setting off TURN ON RECORDING, which writes the setting.
    // Compact, Regular, Large, and a larger window at twice the scale.
    const std::array<std::array<int, 2>, 4> sizes{
        {{640, 480}, {960, 540}, {1280, 720}, {1920, 1080}}
    };
    for (const auto& [width, height] : sizes) {
        window_of(width, height);
        frames(2);
        const std::string empty_size = std::to_string(width) + 'x' + std::to_string(height);
        open_replays("reading-" + empty_size);
        snapshot("empty-" + empty_size);
        close();
    }
    window_of(1280, 720);
    frames(2);
    open_replays();
    replays_second("with an empty Recordings folder");
    {
        const std::optional<AutomationControl> words = named("oa.library.status");
        require(
            words && words->text.find("No replays yet.") != std::string::npos &&
                words->text.find("kept here for 30 days") != std::string::npos,
            "the empty section does not say there are no replays yet"
        );
        require(
            !named("oa.library.action.turn-on-recording"),
            "TURN ON RECORDING shows while recording is on"
        );
    }
    close();
    preference_values_[std::string(replays_record_preference)] = "off";
    preferences_dirty_ = true;
    flush_preferences();
    open_replays();
    {
        const std::optional<AutomationControl> words = named("oa.library.status");
        require(
            words && words->text.find("Recording is off.") != std::string::npos,
            "the empty section does not say recording is off"
        );
    }
    click_named("oa.library.action.turn-on-recording");
    require(
        replays_record_setting(preference_values_), "TURN ON RECORDING did not turn the setting on"
    );
    {
        const auto stored = preference_values_.find(std::string(replays_record_preference));
        require(
            stored != preference_values_.end() && stored->second == "on",
            "TURN ON RECORDING did not write open-annihilation.replays.record on"
        );
    }
    require(
        !named("oa.library.action.turn-on-recording"),
        "TURN ON RECORDING stayed after it was pressed"
    );
    std::cout << "library replays: turn on recording wrote open-annihilation.replays.record on\n";
    close();

    // The made-up replays.
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    const std::vector<replays::Kind> registered = replays::registered_kinds();
    std::vector<MadeReplay> made;
    made.push_back(
        {"default/watched.oademo", kWatchedDaysAgo, "multiplayer", {}, {}, 0, false, {}, {}}
    );
    for (int filler = 1; filler <= kFillers; ++filler) {
        const std::string number = std::to_string(filler);
        made.push_back(
            {"default/filler-" + number + ".oademo",
             filler,
             "multiplayer",
             "Filler Field " + number,
             {"Bex " + number, "Morrow " + number},
             static_cast<uint64_t>(filler) * 60'000U,
             false,
             {},
             {}}
        );
    }
    made.push_back(
        {"ridge/skirmish.oademo",
         3,
         "skirmish",
         "Twin Rivers",
         {"Kestrel", "Computer"},
         99U * 3600U * 1000U,
         false,
         {},
         "Ridge"}
    );
    made.push_back(
        {"default/campaign.oademo",
         45,
         "campaign",
         "EXP1CC06",
         {"Commander"},
         0,
         false,
         "{\"mission_name\":\"6: Alien Bugs\",\"campaign\":\"Core Campaign - Core Contingency \"}",
         {}}
    );
    made.push_back(
        {"default/crashed.oademo",
         1,
         "multiplayer",
         "Aardvark Flats",
         {"Varn", "Sato"},
         0,
         true,
         {},
         {}}
    );
    made.push_back(
        {"Imported/imported.oademo", 5, "multiplayer", "Anteer Strait", {}, 0, false, {}, {}}
    );
    made.push_back(
        {"default/missing-map.oademo",
         0,
         "multiplayer",
         std::string(kMissingMap),
         {},
         0,
         false,
         {},
         {}}
    );
    made.push_back(
        {"default/ten-players.oademo",
         4,
         "multiplayer",
         "Comet Catcher",
         {"Varn",
          "Tamsin",
          "Ilse",
          "Juniper",
          "Okafor",
          "Sato",
          "Lindqvist",
          "Halvard",
          "Mirelle",
          "Brannock"},
         67U * 60U * 1000U,
         false,
         {},
         "Ridge",
         0,
         {"core-contingency", "battle-tactics"},
         {"Sturdy Commanders", "Double Metal", "Fast Build"}}
    );
    const bool extension_kind = !registered.empty();
    if (extension_kind)
        made.push_back(
            {"default/battle.oademo",
             2,
             registered.front().id,
             "Show Down",
             {"Varn", "Tamsin", "Ilse", "Juniper", "Okafor", "Sato", "Lindqvist", "Halvard"},
             0,
             false,
             "{\"planet\":\"" + std::string(kPlanet) + "\",\"sector\":\"" + std::string(kSector) +
                 "\"}",
             {}}
        );
    // Each replay but the one that is played has a recording of its own, so
    // that its identity is its own and keeping one keeps no other.
    uint32_t next_variant = 1;
    for (MadeReplay& replay : made)
        if (replay.path != "default/watched.oademo" && replay.path != "default/missing-map.oademo")
            replay.variant = next_variant++;
    for (const MadeReplay& replay : made) {
        if (replay.path != "default/missing-map.oademo") {
            write_made(recordings, source, replay, now);
            continue;
        }
        // The map named in the recording itself, which playback looks for.
        const oa::formats::tad::ParseResult parsed = oa::formats::tad::parse(source.recording);
        require(parsed.ok(), "the recorded game does not parse");
        oa::formats::tad::Demo demo = *parsed.demo;
        demo.map_name = std::string(kMissingMap);
        const oa::formats::tad::WriteResult rewritten = oa::formats::tad::write(demo);
        require(rewritten.ok(), "the recording with another map could not be written");
        oademo::Replay elsewhere = source;
        elsewhere.recording = rewritten.bytes;
        oademo::set_recording_identity(elsewhere);
        write_made(recordings, elsewhere, replay, now);
    }
    // A .tad of the same game, one whose unit table is not the installed one,
    // and a damaged file.
    write_bytes(recordings / "default" / "recorder.tad", recording_variant(source.recording, 900));
    write_bytes(
        recordings / "default" / "other-units.tad", recording_with_other_units(source.recording)
    );
    const std::string junk = "this is not a replay";
    write_bytes(
        recordings / "default" / "damaged.oademo",
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(junk.data()), junk.size())
    );
    // Every made-up replay is already read, with nothing to show, before the
    // first picture of the list. The pass's own replays are written later.
    {
        replays::Index seeded;
        const auto add = [&](std::string_view path) {
            replays::Replay row = replays::read_file(recordings, path);
            if (!row.timeline.has_value())
                row.timeline = std::string();
            row.read_stamp.reset();
            seeded.replays.push_back(std::move(row));
        };
        for (const MadeReplay& replay : made)
            add(replay.path);
        add("default/recorder.tad");
        add("default/other-units.tad");
        add("default/damaged.oademo");
        require(
            replays::save_index(recordings, seeded), "the made-up replays' index could not be saved"
        );
    }
    seed_rows = true;
    // All but the campaign, and the two .tad files and the damaged file.
    const std::size_t month = made.size() - 1 + 3;

    // 1 again: with replays.
    open_replays();
    replays_second("with replays");
    require(
        host.model.inputs.replays.rows.size() == made.size() + 3,
        "the section does not list every replay below Recordings"
    );
    require(
        library::replay_filter_count(host.model, library::ReplayFilter::this_month) == month,
        "This month does not count " + std::to_string(month) + " replays"
    );
    {
        const library::ReplayRow* campaign =
            library::find_replay(host.model, "default/campaign.oademo");
        require(campaign != nullptr, "the campaign replay is not listed");
        require(
            library::replay_title(*campaign) == "6: Alien Bugs",
            "a campaign replay is not titled by its mission name"
        );
        std::cout << "library replays: a campaign replay is titled by its mission name\n";
    }
    {
        const library::ReplayRow* damaged =
            library::find_replay(host.model, "default/damaged.oademo");
        require(
            damaged != nullptr && replays::damaged(damaged->replay),
            "the damaged file is not listed as damaged"
        );
        library::select_replay(host.model, "default/damaged.oademo");
        ++host.model_revision;
        frames(2);
        const std::optional<AutomationControl> watch = named("oa.library.action.watch");
        require(watch && !watch->enabled, "WATCH is enabled on a damaged replay");
        std::cout << "library replays: a damaged file is listed as damaged: "
                  << damaged->replay.damage << '\n';
    }

    // 4. The filters, the kind menu, the search and the sort.
    const auto filter_rows = [&](std::string_view filter) {
        click_named("oa.library.filter." + std::string(filter));
        return shown_paths(host.model);
    };
    {
        const std::vector<std::string> older = filter_rows("older");
        require(
            older == std::vector<std::string>{"default/campaign.oademo"},
            "Older does not show the campaign alone"
        );
        const std::vector<std::string> imported = filter_rows("imported");
        require(
            imported == std::vector<std::string>{"Imported/imported.oademo"},
            "Imported does not show the imported replay alone"
        );
        const std::vector<std::string> kept = filter_rows("kept");
        require(kept.empty(), "Kept shows a replay nobody kept");
        const std::vector<std::string> this_month = filter_rows("this-month");
        require(this_month.size() == month, "This month does not show this month's replays");
        require(!holds(this_month, "default/campaign.oademo"), "This month shows an older replay");
        const std::vector<library::ReplayDay> days = library::replay_days(host.model);
        require(
            !days.empty() && days.front().heading == "Today",
            "the newest replays are not under Today"
        );
        std::cout << "library replays: filters: this month " << this_month.size() << ", older "
                  << older.size() << ", kept " << kept.size() << ", imported " << imported.size()
                  << "; " << days.size() << " days\n";
    }
    const auto choose = [&](std::string_view menu, std::string_view item) {
        click_named("oa.library.replays." + std::string(menu));
        click_named("oa.library.replays." + std::string(menu) + "." + std::string(item));
        return shown_paths(host.model);
    };
    {
        require(
            choose("kind", "skirmish") == std::vector<std::string>{"ridge/skirmish.oademo"},
            "Skirmish does not show the skirmish alone"
        );
        require(choose("kind", "campaign").empty(), "Campaign shows a replay this month");
        click_named("oa.library.filter.older");
        require(
            shown_paths(host.model) == std::vector<std::string>{"default/campaign.oademo"},
            "Campaign under Older does not show the campaign"
        );
        click_named("oa.library.filter.this-month");
        const std::vector<std::string> multiplayer = choose("kind", "multiplayer");
        require(
            !holds(multiplayer, "ridge/skirmish.oademo") &&
                holds(multiplayer, "default/recorder.tad"),
            "Multiplayer does not show the network games"
        );
        const std::vector<std::string> all = choose("kind", "all");
        require(all.size() == month, "All kinds does not show every replay of this month");
        std::cout << "library replays: kinds: multiplayer " << multiplayer.size() << ", all "
                  << all.size() << '\n';
    }
    if (extension_kind) {
        library::set_replay_kind(host.model, registered.front().id);
        ++host.model_revision;
        frames(1);
        require(
            shown_paths(host.model) == std::vector<std::string>{"default/battle.oademo"},
            "an extension's kind does not show its battle alone"
        );
        const library::ReplayRow* battle =
            library::find_replay(host.model, "default/battle.oademo");
        require(
            battle != nullptr && battle->replay.planet == kPlanet &&
                battle->replay.sector == kSector,
            "an extension's battle does not name its planet and sector"
        );
        library::set_replay_kind(host.model, {});
        ++host.model_revision;
        frames(1);
    }
    const auto search = [&](std::string_view words) {
        library::set_query(host.model, words);
        host.state.search = {host.model.query, host.model.query.size()};
        ++host.model_revision;
        frames(1);
        return shown_paths(host.model);
    };
    require(
        search("kestrel") == std::vector<std::string>{"ridge/skirmish.oademo"},
        "searching a player does not find the skirmish"
    );
    require(
        search("aardvark") == std::vector<std::string>{"default/crashed.oademo"},
        "searching a map does not find its replay"
    );
    require(
        search("ridge") ==
            std::vector<std::string>{"ridge/skirmish.oademo", "default/ten-players.oademo"},
        "searching a mod does not find its replays"
    );
    // A damaged file has no map: it is found by the file name its row shows.
    require(
        search("damaged") == std::vector<std::string>{"default/damaged.oademo"},
        "searching the name a damaged file's row shows does not find it"
    );
    library::set_replay_filter(host.model, library::ReplayFilter::older);
    ++host.model_revision;
    frames(1);
    require(
        search("EXP1CC06") == std::vector<std::string>{"default/campaign.oademo"},
        "searching a campaign's map file does not find it"
    );
    library::set_query(host.model, {});
    host.state.search = {};
    library::set_replay_filter(host.model, library::ReplayFilter::this_month);
    ++host.model_revision;
    frames(1);
    std::cout << "library replays: a campaign's map file is found by EXP1CC06\n";
    if (extension_kind)
        require(
            search("corvane") == std::vector<std::string>{"default/battle.oademo"},
            "searching a planet does not find its battle"
        );
    require(search("nothing like this").empty(), "a search that matches nothing shows rows");
    search({});
    {
        require(
            choose("sort", "longest").front() == "ridge/skirmish.oademo",
            "Longest first does not lead with the longest game"
        );
        require(
            choose("sort", "map").front() == "default/crashed.oademo",
            "By map does not lead with Aardvark Flats"
        );
        require(
            choose("sort", "players").front() == "default/ten-players.oademo",
            "Most players first does not lead with the ten-player replay"
        );
        const std::vector<std::string> newest = choose("sort", "newest");
        require(
            library::replay_days(host.model).size() > 1 && newest.size() == month,
            "Newest first does not group the replays by day"
        );
        std::cout << "library replays: search and sort give the rows they should\n";
    }

    // 3. KEEP: one replay, then two more marked with Ctrl, in the index and
    // read back by a section made afresh.
    const auto row_of = [&](std::string_view path) {
        const std::optional<std::size_t> place = library::replay_place(host.model, path);
        require(place.has_value(), std::string(path) + " is not listed");
        host.state.reveal_row = static_cast<int32_t>(*place);
        ++host.model_revision;
        frames(1);
        return "oa.library.row.replay." + std::to_string(*place + 1);
    };
    // A Ctrl key the automation endpoint holds marks a second row.
    click_named(row_of("default/filler-1.oademo"));
    {
        const AutomationHost automation = automation_host(*this);
        automation.hold_key(automation.context, static_cast<uint32_t>(SDL_SCANCODE_LCTRL), true);
        click_named(row_of("default/filler-2.oademo"));
        require(
            host.model.replays.marked.size() == 2,
            "a Ctrl the automation endpoint holds did not mark a second replay"
        );
        automation.hold_key(automation.context, static_cast<uint32_t>(SDL_SCANCODE_LCTRL), false);
        std::cout << "library replays: a held Ctrl marked a second replay\n";
    }
    click_named(row_of("ridge/skirmish.oademo"));
    click_named("oa.library.action.keep");
    // The mark is in its own file at once, by the recording, whatever the
    // folder's read is doing: a Keep pressed while the folder is read is kept.
    {
        const replays::KeptRead stored = replays::read_kept(recordings);
        require(
            !stored.damaged && stored.kept.marks.size() == 1,
            "kept.json does not hold the replay kept at once"
        );
    }
    click_named(row_of("default/filler-1.oademo"));
    click_named(row_of("default/filler-2.oademo"), SDL_KMOD_CTRL);
    require(host.model.replays.marked.size() == 2, "Ctrl did not mark a second replay");
    click_named("oa.library.action.keep");
    require(filter_rows("kept").size() == 3, "Kept does not show the three replays kept");
    require(
        replays::read_kept(recordings).kept.marks.size() == 3,
        "kept.json does not hold the three replays kept"
    );
    // The index is a cache: nothing about a kept replay is in it, and the
    // section made afresh reads the marks back, with the index as it is, deleted,
    // truncated or filled with noise.
    fs::path index_aside = replays::index_path(recordings);
    index_aside += ".damaged";
    const auto reopen_kept = [&](std::string_view index_is) {
        close();
        host.replays.reset();
        open_replays();
        require(
            library::replay_filter_count(host.model, library::ReplayFilter::kept) == 3,
            "Keep did not survive the section being made afresh with the index " +
                std::string(index_is)
        );
    };
    reopen_kept("whole");
    std::cout << "library replays: three replays kept, read back from kept.json\n";
    close();
    {
        std::error_code removed;
        fs::remove(replays::index_path(recordings), removed);
    }
    reopen_kept("deleted");
    require(
        fs::exists(replays::index_path(recordings)) && !replays::read_index(recordings).damaged,
        "the index was not made again after it was deleted"
    );
    close();
    write_text(replays::index_path(recordings), "{\"replays_index\":1,\"replays\":[{\"path\":");
    reopen_kept("truncated");
    require(
        fs::exists(index_aside) && !replays::read_index(recordings).damaged,
        "a truncated index was overwritten, not set aside"
    );
    close();
    write_text(replays::index_path(recordings), std::string(512, '\x01'));
    reopen_kept("full of noise");
    require(fs::exists(index_aside), "an index of noise was overwritten, not set aside");
    std::cout << "library replays: Kept survives the index deleted, truncated or damaged\n";
    click_named("oa.library.filter.this-month");

    // The ten-player replay's card: its parts scroll, so that every fact can be
    // brought into view, Kept until the last. At Compact it is the page.
    const auto ten_players = [&](const std::string& size, bool compact) {
        const std::string ten_row = row_of("default/ten-players.oademo");
        click_named(ten_row);
        require(
            host.model.replays.selected == std::optional<std::string>("default/ten-players.oademo"),
            "the ten-player replay was not selected at " + size
        );
        host.state.details_scroll = 0;
        ++host.model_revision;
        frames(2);
        snapshot("ten-players-" + size, true);
        host.state.details_scroll = 1 << 20;
        ++host.model_revision;
        frames(2);
        require(
            named("oa.library.fact.kept-until").has_value(),
            "the ten-player card cannot be scrolled to its last fact at " + size
        );
        snapshot("ten-players-foot-" + size, true);
        std::cout << "library replays: the ten-player card shows Kept until at " << size << '\n';
        host.state.details_scroll = 0;
        if (compact)
            click_named("oa.library.back");
        else
            ++host.model_revision;
        frames(1);
    };
    // A recording whose unit table is not the installed one is refused after its
    // match began loading: the game is on the main menu again within a bounded
    // number of frames, the Library is on that row at that scroll, and the card
    // gives the refusal without the file's name.
    const auto refused_after_loading = [&](const std::string& size, bool compact) {
        const std::string other_row = row_of("default/other-units.tad");
        click_named(other_row);
        const int32_t scroll_at = host.state.list_scroll;
        click_named("oa.library.action.watch");
        for (int frame = 0;
             frame < kMostFrames && !(screen_ == Screen::main_menu && top_is(library_screen_name));
             ++frame) {
            run_frame(running);
            require(running, "a frame ended the run");
        }
        require(
            screen_ == Screen::main_menu && match_ == nullptr && !host.replays_watching(),
            "a refused WATCH left the game on its loading screen at " + size
        );
        require(top_is(library_screen_name), "a refused WATCH did not leave the Library open");
        require(
            host.model.tab == library::Tab::replays &&
                host.model.replays.selected ==
                    std::optional<std::string>("default/other-units.tad") &&
                host.state.list_scroll == scroll_at,
            "the Library did not stay on the replay it refused, at its scroll, at " + size
        );
        const std::optional<AutomationControl> note = named("oa.library.note");
        require(
            note && note->text.find("unit definitions") != std::string::npos &&
                note->text.find("other-units.tad") == std::string::npos,
            "the card does not give the unit table's refusal alone after loading began, at " + size
        );
        std::cout << "library replays: refused after loading began, back in the card at " << size
                  << ": " << note->text << '\n';
        snapshot("refused-after-loading-" + size, true);
        if (compact)
            click_named("oa.library.back");
    };

    // The long list, at the three sizes, and Compact's page of a replay;
    // the Imported filter at 1280x720.
    for (const auto& [width, height] : sizes) {
        close();
        window_of(width, height);
        frames(2);
        open_replays();
        const std::string size = std::to_string(width) + 'x' + std::to_string(height);
        const std::string row = row_of("default/watched.oademo");
        if (width == 640) {
            library::select_replay(host.model, "default/watched.oademo");
            ++host.model_revision;
            frames(1);
            snapshot("long-" + size, true);
            // The section list with a two-digit count beside This month.
            snapshot("section-list-two-digits-" + size, true);
            click_named(row);
            require(
                named("oa.library.back").has_value(),
                "a row's press at Compact did not open its page"
            );
            snapshot("page-" + size, true);
            click_named("oa.library.back");
            ten_players(size, true);
            continue;
        }
        click_named(row);
        snapshot("long-" + size, true);
        ten_players(size, false);
        if (width == 1280) {
            click_named(row_of("default/damaged.oademo"));
            snapshot("damaged-" + size, true);
            click_named("oa.library.filter.imported");
            snapshot("imported-" + size, true);
            click_named("oa.library.filter.this-month");
            click_named(row_of("default/watched.oademo"));
        }
    }

    // 2. A replay whose map is not installed is refused in the card.
    click_named(row_of("default/missing-map.oademo"));
    click_named("oa.library.action.watch");
    require(top_is(library_screen_name), "WATCH left the Library for a replay it cannot play");
    {
        const std::optional<AutomationControl> note = named("oa.library.note");
        require(
            note && note->text.find(kMissingMap) != std::string::npos,
            "the card does not say why the replay cannot be watched"
        );
        std::cout << "library replays: refused in the card: " << note->text << '\n';
        snapshot("refused-1920x1080", true);
    }

    // 2. A recording whose unit table differs is refused after loading began.
    window_of(960, 540);
    frames(2);
    refused_after_loading("960x540", false);
    window_of(1280, 720);
    frames(2);
    refused_after_loading("1280x720", false);

    // 2. WATCH, some ticks, and back to the same row at the same scroll.
    click_named(row_of("default/watched.oademo"));
    const int32_t scroll_before = host.state.list_scroll;
    click_named("oa.library.action.watch");
    for (int frame = 0; frame < kMostFrames && screen_ != Screen::match; ++frame) {
        run_frame(running);
        require(running, "a frame ended the run");
    }
    require(
        screen_ == Screen::match && match_ != nullptr, "WATCH did not start the replay's match"
    );
    require(oa_layer().find(library_screen_name) == nullptr, "the Library stayed over the replay");
    const uint32_t first_tick = match_->state().game.tick;
    // The check's clock is fixed: ticks are granted, as automation grants them.
    for (int frame = 0; frame < kMostFrames && match_ != nullptr &&
                        match_->state().game.tick < first_tick + kWatchedTicks;
         ++frame) {
        static_cast<void>(run_granted_ticks(1));
        run_frame(running);
        require(running, "a frame ended the run");
    }
    require(
        match_ != nullptr && match_->state().game.tick >= first_tick + kWatchedTicks,
        "the replay did not play " + std::to_string(kWatchedTicks) + " ticks"
    );
    std::cout << "library replays: WATCH played ticks " << first_tick << " to "
              << match_->state().game.tick << '\n';
    leave_match();
    load(Screen::main_menu);
    for (int frame = 0; frame < kMostFrames && !top_is(library_screen_name); ++frame) {
        run_frame(running);
        require(running, "a frame ended the run");
    }
    frames(2);
    require(top_is(library_screen_name), "leaving the replay did not open the Library again");
    require(host.model.tab == library::Tab::replays, "the Library came back on another tab");
    require(
        host.model.replays.selected == std::optional<std::string>("default/watched.oademo"),
        "the Library came back on another replay"
    );
    require(host.state.list_scroll == scroll_before, "the list came back scrolled elsewhere");
    std::cout << "library replays: leaving came back on the same replay, scrolled "
              << host.state.list_scroll << '\n';
    const library::ReplayRow* watched = library::find_replay(host.model, "default/watched.oademo");
    require(
        watched != nullptr && watched->replay.watched_tick.has_value() &&
            *watched->replay.watched_tick >= kWatchedTicks,
        "leaving the replay did not remember where it was"
    );
    const uint32_t remembered = *watched->replay.watched_tick;
    click_named("oa.library.action.watch-from");
    for (int frame = 0; frame < kMostFrames && screen_ != Screen::match; ++frame) {
        run_frame(running);
        require(running, "a frame ended the run");
    }
    require(screen_ == Screen::match && match_ != nullptr, "Watch from did not start the replay");
    // The jump is the replay's own. Granting ticks would let a start at 0 pass.
    bool saw_skipping = false;
    bool arrived = false;
    uint32_t at_tick = match_->state().game.tick;
    for (int frame = 0; frame < kMostFrames && match_ != nullptr; ++frame) {
        run_frame(running);
        require(running, "a frame ended the run");
        at_tick = match_->state().game.tick;
        ReplayPlaybackView view{};
        const bool have = call_hook_or_report<&Extension::replay_state>(
            extension_, hook_error_report(), *this, &view
        );
        if (have && view.skipping)
            saw_skipping = true;
        if (have && !view.skipping && (saw_skipping || at_tick == remembered) &&
            at_tick == remembered && at_tick >= kWatchedTicks) {
            arrived = true;
            break;
        }
    }
    require(arrived, "Watch from did not reach the remembered tick without granting ticks");
    std::cout << "library replays: Watch from starts at demo tick " << at_tick << '\n';
    for (int frame = 0; frame < 4000 && match_ != nullptr && !host.watched_replay_finished();
         ++frame) {
        static_cast<void>(run_granted_ticks(8));
        run_frame(running);
        require(running, "a frame ended the run");
    }
    require(
        match_ != nullptr && host.watched_replay_finished(),
        "the replay watched on from its tick did not finish"
    );
    leave_match();
    load(Screen::main_menu);
    for (int frame = 0; frame < kMostFrames && !top_is(library_screen_name); ++frame) {
        run_frame(running);
        require(running, "a frame ended the run");
    }
    frames(2);
    require(top_is(library_screen_name), "leaving Watch from did not open the Library again");
    {
        const library::ReplayRow* ended =
            library::find_replay(host.model, "default/watched.oademo");
        require(
            ended != nullptr && !ended->replay.watched_tick.has_value(),
            "a replay watched to its end kept a watched tick"
        );
        require(
            !named("oa.library.action.watch-from"), "a replay watched to its end offers Watch from"
        );
        const std::optional<AutomationControl> watch = named("oa.library.action.watch");
        require(
            watch && watch->text == "Watch", "a replay watched to its end does not offer Watch"
        );
        std::cout << "library replays: a replay watched to its end offers Watch\n";
    }
    close();
    window_of(640, 480);
    frames(2);
    // The same refusal at Compact, where the card is the replay's page.
    open_replays();
    refused_after_loading("640x480", true);
    close();

    // 6. EXPORT. The pictures are staged, so a .oademo is compressed once for
    // the real write and not once per picture. The files go to Exported replays,
    // which is removed with everything else this check writes.
    {
        std::error_code created;
        fs::create_directories(exported, created);
        require(!created, "the export folder could not be made");
        const auto files_with = [&](std::string_view extension) {
            std::vector<fs::path> found;
            std::error_code failed;
            for (const fs::directory_entry& entry : fs::directory_iterator(exported, failed)) {
                if (failed)
                    break;
                std::error_code skipped;
                if (!entry.is_regular_file(skipped) || skipped)
                    continue;
                const std::string name = platform::path_to_utf8(entry.path().filename());
                if (name.size() >= extension.size() &&
                    name.compare(name.size() - extension.size(), extension.size(), extension) == 0)
                    found.push_back(entry.path());
            }
            return found;
        };
        const auto empty_export_folder = [&] {
            std::error_code removed;
            fs::remove_all(exported, removed);
            std::error_code again;
            fs::create_directories(exported, again);
            require(!again, "the export folder could not be made again");
        };
        const auto retarget = [&] {
            host.model.export_dialog.folder = platform::path_to_utf8(exported);
            host.model.export_dialog.folder_is_downloads = false;
            host.model.export_dialog.folder_label = "Exported replays";
            ++host.model_revision;
            frames(1);
        };
        const auto wait_until_written = [&] {
            for (int frame = 0;
                 frame < kMostFrames && host.model.export_dialog.open &&
                 !host.model.export_dialog.done && host.model.export_dialog.problem.empty();
                 ++frame)
                frames(1);
            const std::string why = host.model.export_dialog.problem.empty()
                                        ? std::string("the export did not finish")
                                        : host.model.export_dialog.problem;
            require(host.model.export_dialog.done, why);
        };
        // Export takes nothing until every replay has been measured.
        const auto wait_until_ready = [&] {
            for (int frame = 0; frame < kMostFrames; ++frame) {
                const library::ExportDialog& dialog = host.model.export_dialog;
                bool ready = dialog.open && dialog.problem.empty() && !dialog.files.empty();
                for (const library::ExportFile& file : dialog.files)
                    ready = ready && file.ready && !file.unreadable;
                if (ready)
                    return;
                frames(1);
            }
            require(false, "the export was not measured");
        };
        const auto stage = [&](const std::string& size, std::string_view state) {
            library::ExportOffer offer;
            offer.folder = platform::path_to_utf8(exported);
            offer.folder_is_downloads = true;
            if (state != "empty") {
                library::ExportFile file;
                file.path = "default/watched.oademo";
                file.title = "Watched";
                file.oademo_bytes = 12000;
                file.tad_bytes = 8000;
                file.tad_without_bytes = 7000;
                file.ready = true;
                if (state == "tad") {
                    file.names_cut = true;
                    file.status_cut = true;
                    file.wide_records = true;
                    file.mutators = true;
                    file.mutator_ids.push_back("ridge");
                }
                offer.files.push_back(std::move(file));
            }
            library::open_export(host.model, std::move(offer));
            if (state == "tad")
                library::set_export_format(host.model, library::ExportFormat::tad);
            if (state == "exporting") {
                library::set_export_progress(host.model, 0, 1);
                library::set_export_byte_progress(host.model, 310U * 1024U, 500U * 1024U, true);
            }
            if (state == "problem")
                library::fail_export(host.model, "There is no room in Downloads.");
            if (state == "done") {
                library::set_export_progress(host.model, 1, 1);
                library::finish_export(host.model);
            }
            ++host.model_revision;
            frames(1);
            if (state == "oademo" || state == "tad") {
                const std::optional<AutomationControl> where = named("oa.library.export.where");
                require(where && where->text == "Downloads", "Where does not say Downloads");
                require(
                    named("oa.library.export.format.oademo").has_value() &&
                        named("oa.library.export.format.tad").has_value(),
                    "the format choices are not shown"
                );
            }
            if (state == "oademo")
                require(
                    !named("oa.library.export.oa-sectors"),
                    "What only Open Annihilation reads shows for a .oademo"
                );
            if (state == "tad")
                require(
                    named("oa.library.export.oa-sectors").has_value(),
                    "What only Open Annihilation reads is not shown for a .tad"
                );
            if (state == "empty") {
                const std::optional<AutomationControl> note = named("oa.library.export.note");
                require(
                    note && note->text.find("Nothing is selected") != std::string::npos,
                    "an export with nothing selected does not say so"
                );
            }
            if (state == "done")
                require(
                    named("oa.library.export.show-folder").has_value(),
                    "Show in folder is not shown"
                );
            if (state == "problem") {
                require(
                    !library::export_can_run(host.model.export_dialog),
                    "Export stays on after a failed write"
                );
                require(
                    named("oa.library.export.format.oademo").has_value(),
                    "a failed export leaves the form"
                );
                const std::optional<AutomationControl> note = named("oa.library.export.note");
                require(
                    note && note->text.find("no room") != std::string::npos,
                    "a failed export does not say there is no room"
                );
            }
            snapshot("export-" + std::string(state) + "-" + size, true);
        };

        if (!options_.snapshot.empty()) {
            const std::array<std::array<int, 2>, 4> picture_sizes{
                {{640, 480}, {960, 540}, {1280, 720}, {1920, 1080}}
            };
            for (const auto& [width, height] : picture_sizes) {
                window_of(width, height);
                frames(2);
                if (!top_is(library_screen_name))
                    open_replays();
                host.state.replay_page = false;
                const std::string size = std::to_string(width) + 'x' + std::to_string(height);
                for (const std::string_view state :
                     {"oademo", "tad", "exporting", "done", "problem", "empty"})
                    stage(size, state);
            }
            library::close_export(host.model);
            ++host.model_revision;
            frames(1);
        }

        window_of(1280, 720);
        frames(2);
        if (!top_is(library_screen_name))
            open_replays();
        host.state.replay_page = false;
        library::set_replay_filter(host.model, library::ReplayFilter::this_month);
        library::close_export(host.model);
        ++host.model_revision;
        frames(1);
        empty_export_folder();
        click_named(row_of("default/watched.oademo"));
        click_named("oa.library.action.export");
        require(host.model.export_dialog.open, "Export did not open");
        require(host.model.export_dialog.files.size() == 1, "Export did not offer the one replay");
        retarget();
        wait_until_ready();
        click_named("oa.library.export.export");
        wait_until_written();
        require(
            named("oa.library.export.show-folder").has_value(),
            "Show in folder is not shown after the export"
        );
        {
            const std::vector<fs::path> written = files_with(".oademo");
            require(written.size() == 1, "one replay did not export as one .oademo");
            const std::vector<uint8_t> bytes = read_bytes(written.front());
            const oademo::ReadResult read = oademo::read(bytes);
            require(read.ok(), "the exported .oademo does not read: " + read.error);
            require(files_with(".tad").empty(), "a .oademo export also wrote a .tad");
        }
        std::cout << "library replays: exported one replay as a .oademo\n";
        click_named("oa.library.export.close");

        // A .tad chosen after the measurement is written, not compressed again.
        // The recording is the skirmish, so its export keeps one status per player.
        empty_export_folder();
        click_named(row_of("ridge/skirmish.oademo"));
        click_named("oa.library.action.export");
        require(host.model.export_dialog.open, "the skirmish export did not open");
        retarget();
        wait_until_ready();
        click_named("oa.library.export.format.tad");
        require(
            host.model.export_dialog.format == library::ExportFormat::tad,
            "the skirmish .tad choice did not take"
        );
        click_named("oa.library.export.export");
        {
            bool saw_write = false;
            for (int frame = 0; frame < kMostFrames; ++frame) {
                const library::ExportDialog& dialog = host.model.export_dialog;
                if (!dialog.problem.empty())
                    require(false, dialog.problem);
                if (dialog.done)
                    break;
                if (dialog.exporting) {
                    saw_write = true;
                    require(
                        !dialog.compressing,
                        "a .tad chosen after measuring was compressed while it wrote"
                    );
                }
                frames(1);
            }
            require(saw_write, "the skirmish .tad export was not seen while it wrote");
            require(host.model.export_dialog.done, "the skirmish .tad export did not finish");
            const std::vector<fs::path> written = files_with(".tad");
            require(written.size() == 1, "the skirmish did not export as one .tad");
            const oa::formats::tad::ParseResult parsed =
                oa::formats::tad::parse(read_bytes(written.front()));
            require(parsed.ok() && parsed.demo.has_value(), "the skirmish .tad does not parse");
            require(!parsed.demo->players.empty(), "the skirmish .tad names no player");
            require(
                parsed.demo->statuses.size() == parsed.demo->players.size(),
                "the skirmish .tad does not keep one status per player"
            );
        }
        std::cout << "library replays: a .tad chosen after measuring was written without a "
                     "compression\n";
        click_named("oa.library.export.close");
        library::close_export(host.model);
        ++host.model_revision;

        // A content note names the mod as the player saw it. An installed Ridge
        // supplies its id, version and digest. A mod that is not installed does not.
        {
            const std::string digest(64, 'a');
            const fs::path ridge_folder = user_folder_ / "Mods" / "ridge";
            write_text(
                ridge_folder / "oamod.yaml",
                "oamod: 1\n"
                "id: ridge\n"
                "name: Ridge\n"
                "version: \"28\"\n"
                "requires: {base: ta-3.1c}\n"
            );
            write_text(
                ridge_folder / ".oa-origin.yaml",
                "oa-origin: 1\norigin: file\nsha256: " + digest + "\ninstalled: 2026-11-07\n"
            );
            const std::vector<uint8_t> recorded =
                read_bytes(recordings / "default" / "recorder.tad");
            require(!recorded.empty(), "the recorder .tad could not be read");
            write_bytes(recordings / "default" / "ridge-note.tad", recorded);
            write_text(
                recordings / "default" / "ridge-note.tad.oa-content",
                "base-game: totala\nmod: Ridge\n"
            );
            write_bytes(recordings / "default" / "absent-mod.tad", recorded);
            write_text(
                recordings / "default" / "absent-mod.tad.oa-content",
                "base-game: totala\nmod: No Such Mod\n"
            );
            const auto exported_note = [&](const std::string& path) {
                empty_export_folder();
                host.open_replay_export(*this, path);
                require(host.model.export_dialog.open, path + " did not open Export");
                retarget();
                wait_until_ready();
                require(
                    host.model.export_dialog.format == library::ExportFormat::oademo,
                    path + " did not offer a .oademo"
                );
                click_named("oa.library.export.export");
                wait_until_written();
                const std::vector<fs::path> written = files_with(".oademo");
                require(written.size() == 1, path + " did not export as one .oademo");
                const oademo::ReadResult read = oademo::read(read_bytes(written.front()));
                require(read.ok() && read.replay.has_value(), path + " export does not read");
                oademo::Replay replay = *read.replay;
                click_named("oa.library.export.close");
                library::close_export(host.model);
                ++host.model_revision;
                return replay;
            };
            const oademo::Replay ridge = exported_note("default/ridge-note.tad");
            require(ridge.meta.content.mod.has_value(), "an installed mod was left off the replay");
            require(ridge.meta.content.mod->id == "ridge", "the note's mod was not the package id");
            require(ridge.meta.content.mod->name == "Ridge", "the note's mod lost its name");
            require(ridge.meta.content.mod->version == "28", "the note's mod lost its version");
            bool ridge_need = false;
            for (const oademo::Need& need : ridge.manifest.needs) {
                if (need.kind != "oamod")
                    continue;
                ridge_need = true;
                require(need.id == "ridge", "the oamod need is not the package id");
                require(
                    need.version.has_value() && *need.version == "28",
                    "the oamod need has no version"
                );
                require(
                    need.sha256.has_value() && *need.sha256 == digest,
                    "the oamod need has no digest"
                );
            }
            require(ridge_need, "an installed mod wrote no oamod need");
            const oademo::Replay absent = exported_note("default/absent-mod.tad");
            require(
                absent.meta.content.mod.has_value() &&
                    absent.meta.content.mod->name == "No Such Mod",
                "a missing mod was not kept as a name"
            );
            require(absent.meta.content.mod->id.empty(), "a missing mod was given an id");
            for (const oademo::Need& need : absent.manifest.needs)
                require(need.kind != "oamod", "a missing mod wrote an oamod need");
            std::cout << "library replays: a content note names an installed mod by its package\n";
        }

        empty_export_folder();
        click_named(row_of("default/filler-1.oademo"));
        click_named(row_of("default/filler-2.oademo"), SDL_KMOD_CTRL);
        click_named(row_of("default/watched.oademo"), SDL_KMOD_CTRL);
        require(host.model.replays.marked.size() == 3, "three replays were not marked for export");
        click_named("oa.library.action.export");
        require(
            host.model.export_dialog.files.size() == 3, "Export did not offer the three replays"
        );
        click_named("oa.library.export.format.tad");
        require(
            host.model.export_dialog.format == library::ExportFormat::tad,
            "the .tad choice did not take"
        );
        retarget();
        wait_until_ready();
        click_named("oa.library.export.export");
        wait_until_written();
        {
            const std::vector<fs::path> written = files_with(".tad");
            require(written.size() == 3, "three replays did not export as three .tad files");
            for (const fs::path& file : written) {
                const std::vector<uint8_t> bytes = read_bytes(file);
                require(!bytes.empty(), "an exported .tad is empty");
                const oa::formats::tad::ParseResult parsed = oa::formats::tad::parse(bytes);
                require(parsed.ok() && parsed.demo.has_value(), "an exported .tad does not parse");
                require(
                    parsed.demo->version == oa::formats::tad::supported_version,
                    "an exported .tad is not version 5"
                );
                require(
                    parsed.demo->statuses.size() == parsed.demo->players.size(),
                    "an exported .tad drops a player's status"
                );
                for (const oa::formats::tad::Sector& sector : parsed.demo->sectors)
                    require(
                        sector.type !=
                            static_cast<uint32_t>(oa::formats::tad::SectorType::player_address),
                        "an exported .tad holds an address"
                    );
                for (const oa::formats::tad::Packet& packet : parsed.demo->packets) {
                    require(!packet.payload.empty(), "an exported .tad has an empty packet");
                    require(
                        packet.payload.front() == oa::formats::tad::payload_marker,
                        "an exported .tad has a packet that is not plain"
                    );
                }
            }
        }
        std::cout << "library replays: exported three replays as .tad files\n";
        click_named("oa.library.export.close");
        library::close_export(host.model);
        ++host.model_revision;

        // A recording long enough that measuring it takes several frames. Each
        // packet is its own incompressible payload, so the compression cannot
        // finish inside the frame the dialog opens on. Cancel during the
        // measurement, and again during the write, each leaves the folder empty.
        {
            const oa::formats::tad::ParseResult parsed = oa::formats::tad::parse(source.recording);
            require(
                parsed.ok() && parsed.demo.has_value(), "the long recording's source does not parse"
            );
            oa::formats::tad::Demo demo = *parsed.demo;
            std::vector<std::vector<uint8_t>> payloads;
            payloads.reserve(static_cast<std::size_t>(kLongPackets));
            for (int extra = 0; extra < kLongPackets; ++extra) {
                std::vector<uint8_t> bulk(static_cast<std::size_t>(kLongPacketBytes));
                bulk[0] = oa::formats::tad::payload_marker;
                uint32_t state = 0xA5A5A5A5U + static_cast<uint32_t>(extra) * 0x9E3779B9U;
                for (std::size_t at = 1; at < bulk.size(); ++at) {
                    state = state * 1664525U + 1013904223U;
                    bulk[at] = static_cast<uint8_t>(state >> 24);
                }
                payloads.push_back(std::move(bulk));
                oa::formats::tad::Packet packet;
                packet.delay_ms = 1;
                packet.sender = demo.players.empty() ? uint8_t{0} : demo.players.front().number;
                packet.payload = payloads.back();
                demo.packets.push_back(packet);
            }
            const oa::formats::tad::WriteResult long_file = oa::formats::tad::write(demo);
            require(long_file.ok(), "the long recording could not be written");
            write_bytes(recordings / "default" / "long-export.tad", long_file.bytes);
        }
        const auto still_measuring = [&] {
            const library::ExportDialog& dialog = host.model.export_dialog;
            return dialog.open && !dialog.files.empty() && !dialog.files.front().ready &&
                   !dialog.files.front().unreadable;
        };
        const auto folder_empty = [&](std::string_view when) {
            require(files_with(".oademo").empty(), std::string(when) + " wrote a .oademo");
            require(files_with(".tad").empty(), std::string(when) + " wrote a .tad");
            require(files_with(".part").empty(), std::string(when) + " left a partial file");
        };
        const auto present_one = [&] {
            renderer::Surface presented;
            capture_frame_ = &presented;
            run_frame(running);
            capture_frame_ = nullptr;
            require(running, "a frame ended the run");
            require(
                presented.width > 0 && presented.height > 0,
                "a frame was not presented while the long recording was measured"
            );
        };
        empty_export_folder();
        host.open_replay_export(*this, "default/long-export.tad");
        require(host.model.export_dialog.open, "a long recording's export did not open at once");
        frames(1);
        require(still_measuring(), "the long recording was measured within a frame");
        for (int frame = 0; frame < 2; ++frame) {
            require(still_measuring(), "the long recording finished before several frames");
            present_one();
        }
        require(still_measuring(), "the long recording finished while frames were presented");
        click_named("oa.library.export.cancel");
        for (int frame = 0; frame < 60; ++frame)
            frames(1);
        folder_empty("cancel during a long measurement");
        library::close_export(host.model);
        ++host.model_revision;

        empty_export_folder();
        host.open_replay_export(*this, "default/long-export.tad");
        require(host.model.export_dialog.open, "the long recording's second export did not open");
        retarget();
        for (int frame = 0; frame < kLongFrames && still_measuring(); ++frame)
            frames(1);
        require(!still_measuring(), "the long recording was not measured");
        require(
            !host.model.export_dialog.files.empty() && host.model.export_dialog.files.front().ready,
            "the long recording could not be read"
        );
        click_named("oa.library.export.format.tad");
        click_named("oa.library.export.export");
        bool saw_write = false;
        for (int frame = 0; frame < kLongFrames && host.model.export_dialog.exporting; ++frame) {
            require(
                !host.model.export_dialog.compressing,
                "a long .tad was compressed while it was written"
            );
            const bool writing =
                host.model.export_dialog.progress_bytes_done > 0 || !files_with(".part").empty();
            if (writing) {
                saw_write = true;
                break;
            }
            frames(1);
        }
        require(saw_write, "the long .tad export did not start writing");
        click_named("oa.library.export.cancel");
        for (int frame = 0; frame < 60; ++frame)
            frames(1);
        folder_empty("cancel during a long write");
        std::cout << "library replays: cancel during a long measurement and a long write left "
                     "the folder empty\n";
        library::close_export(host.model);
        ++host.model_revision;
    }
    if (top_is(library_screen_name))
        close();

    // With --snapshot, the section list on Mods and on Keys at 640x480, as
    // every tab draws it with Replays second.
    if (!options_.snapshot.empty()) {
        for (const auto& [tab, word] :
             {std::pair{library::Tab::mods, "mods"}, std::pair{library::Tab::keys, "keys"}}) {
            open_library(tab);
            frames(2);
            replays_second(std::string("on the ") + word + " tab");
            snapshot(std::string("section-list-") + word + "-640x480", true);
            close();
        }
    }

    // Old replays, after the walk, so the empty section still says 30 days.
    // The shelf's marks were read before these files existed: drop them so the
    // next open reads the mark written here.
    host.replays.reset();
    const auto aged = [&](std::string path, int64_t days_ago, uint32_t variant) {
        write_made(
            recordings,
            source,
            {std::move(path),
             days_ago,
             "multiplayer",
             "Aged Field",
             {"Bex", "Morrow"},
             0,
             false,
             {},
             {},
             variant},
            now
        );
    };
    aged("default/aged.oademo", 40, 800);
    aged("default/kept-old.oademo", 400, 801);
    aged("default/kept-changed.oademo", 400, 805);
    aged("Imported/ancient.oademo", 400, 802);
    aged("Core Prime/ancient.oademo", 400, 803);
    aged("default/exported-old.oademo", 400, 804);
    aged("default/copy-a.oademo", 400, 810);
    aged("locked/stuck.oademo", 1, 811);
    {
        const std::vector<uint8_t> copy = read_bytes(recordings / "default" / "copy-a.oademo");
        write_bytes(recordings / "ridge" / "copy-b.oademo", copy);
    }
    const auto still_there = [&](const fs::path& file) {
        std::error_code failure;
        return fs::is_regular_file(file, failure);
    };
    const fs::path campaign_file = recordings / "default" / "campaign.oademo";
    const fs::path aged_file = recordings / "default" / "aged.oademo";
    const fs::path kept_file = recordings / "default" / "kept-old.oademo";
    const fs::path changed_file = recordings / "default" / "kept-changed.oademo";
    const fs::path imported_file = recordings / "Imported" / "ancient.oademo";
    const fs::path downloaded_file = recordings / "Core Prime" / "ancient.oademo";
    const fs::path exported_file = recordings / "default" / "exported-old.oademo";
    const fs::path copy_a = recordings / "default" / "copy-a.oademo";
    const fs::path copy_b = recordings / "ridge" / "copy-b.oademo";
    const fs::path stuck_file = recordings / "locked" / "stuck.oademo";
    const fs::path filler_one = recordings / "default" / "filler-1.oademo";
    const fs::path filler_two = recordings / "default" / "filler-2.oademo";
    {
        replays::KeptMarks marks = replays::read_kept(recordings).kept;
        replays::set_kept(marks, replays::read_file(recordings, "default/kept-old.oademo"), true);
        const replays::Replay unchanged =
            replays::read_file(recordings, "default/kept-changed.oademo");
        replays::set_kept(marks, unchanged, true);
        replays::set_kept(marks, replays::read_file(recordings, "default/copy-a.oademo"), true);
        require(replays::save_kept(recordings, marks), "the kept old replays could not be saved");
        replays::KeptMarks exported_marks = replays::read_exported(recordings).exported;
        replays::set_exported(
            exported_marks, replays::read_file(recordings, "default/exported-old.oademo"), true
        );
        require(
            replays::save_exported(recordings, exported_marks),
            "the exported replay could not be saved"
        );
        // The mark names the file as it was. Sixteen bytes are added after
        // that. A file that still has the same identity is cut in half, so
        // the mark matches by its path. The file time is put back: a file
        // that no longer reads is dated by that, and it is still old.
        std::vector<uint8_t> changed = read_bytes(changed_file);
        const char extra[] = "0123456789abcdef";
        changed.insert(changed.end(), extra, extra + 16);
        write_bytes(changed_file, changed);
        if (replays::read_file(recordings, "default/kept-changed.oademo").sha256 ==
            unchanged.sha256) {
            changed.resize(std::max<std::size_t>(changed.size() / 2, std::size_t{1}));
            write_bytes(changed_file, changed);
        }
        std::error_code dated;
        const fs::file_time_type written = fs::last_write_time(changed_file, dated);
        require(!dated, "the changed replay's date could not be read");
        fs::last_write_time(
            changed_file, written - std::chrono::seconds(400 * kDaySeconds + 3600), dated
        );
        require(!dated, "the changed replay could not be dated");
        const replays::Replay reread =
            replays::read_file(recordings, "default/kept-changed.oademo");
        require(
            reread.sha256 != unchanged.sha256, "the changed replay still has the mark's identity"
        );
        require(
            reread.recorded < now - 40 * kDaySeconds, "the changed replay is not old enough to go"
        );
    }
    const auto spared = [&] {
        return still_there(kept_file) && still_there(changed_file) && still_there(imported_file) &&
               still_there(downloaded_file) && still_there(exported_file) && still_there(copy_a) &&
               still_there(copy_b) && still_there(stuck_file);
    };
    window_of(1280, 720);
    frames(2);
    consider_replay_retention(true);
    frames(2);
    {
        const std::optional<AutomationControl> body = named("oa.prompt.body");
        require(named("oa.prompt.keep-them-all").has_value(), "the retention question did not ask");
        require(
            named("oa.prompt.let-them-go").has_value(),
            "the retention question offers no letting go"
        );
        require(
            body && body->text.find("2 replays") != std::string::npos,
            "the retention question does not name the two old replays"
        );
        require(
            still_there(campaign_file) && still_there(aged_file) && spared(),
            "a replay was removed before the question was answered"
        );
        snapshot("retention-question", true);
    }
    click_named("oa.prompt.keep-them-all");
    frames(2);
    require(!named("oa.prompt.keep-them-all"), "Keep them all left the question open");
    {
        const auto keep = preference_values_.find(std::string(replays_keep_preference));
        const auto answer =
            preference_values_.find(std::string(replays_retention_asked_preference));
        require(
            keep != preference_values_.end() && keep->second == replays::keep_forever,
            "Keep them all did not set Forever"
        );
        require(
            answer != preference_values_.end() && answer->second == replays_retention_keep,
            "Keep them all did not remember the answer"
        );
        require(
            still_there(campaign_file) && still_there(aged_file) && spared(),
            "Keep them all removed a replay"
        );
    }
    consider_replay_retention(true);
    frames(2);
    require(!named("oa.prompt.keep-them-all"), "the retention question asked again");
    std::cout << "library replays: the retention question asked once, and Keep them all set "
                 "Forever\n";

    // The answer is forgotten so the same folder can be asked again. Let them
    // go moves the two replays that are due, and nothing that is spared.
    preference_values_.erase(std::string(replays_keep_preference));
    preference_values_.erase(std::string(replays_retention_asked_preference));
    preferences_dirty_ = true;
    flush_preferences();
    consider_replay_retention(true);
    frames(2);
    require(named("oa.prompt.let-them-go").has_value(), "Let them go was not offered");
    require(
        still_there(campaign_file) && still_there(aged_file) && spared(),
        "a replay was removed before Let them go"
    );
    click_named("oa.prompt.let-them-go");
    require(!named("oa.prompt.let-them-go"), "Let them go left the question open");
    drain_moves();
    require(
        !still_there(campaign_file) && !still_there(aged_file),
        "Let them go left a replay that was due"
    );
    require(spared(), "Let them go removed a replay it spares");
    std::cout << "library replays: Let them go moved the replays that were due\n";

    // Exporting a replay older than the span writes the mark. The next
    // consideration, which already has its answer, leaves that replay and
    // moves one of the same age that was not exported.
    aged("default/export-aged.oademo", 40, 820);
    aged("default/due-again.oademo", 40, 821);
    const fs::path export_aged = recordings / "default" / "export-aged.oademo";
    const fs::path due_again = recordings / "default" / "due-again.oademo";
    require(
        still_there(export_aged) && still_there(due_again),
        "the replays for the export mark were not written"
    );
    window_of(1280, 720);
    frames(2);
    open_replays();
    click_named("oa.library.filter.older");
    {
        std::error_code removed;
        fs::remove_all(exported, removed);
        std::error_code created;
        fs::create_directories(exported, created);
        require(!created, "the export folder could not be made for the old replay");
    }
    click_named(row_of("default/export-aged.oademo"));
    click_named("oa.library.action.export");
    require(host.model.export_dialog.open, "Export did not open for the old replay");
    host.model.export_dialog.folder = platform::path_to_utf8(exported);
    host.model.export_dialog.folder_is_downloads = false;
    host.model.export_dialog.folder_label = "Exported replays";
    ++host.model_revision;
    frames(1);
    for (int frame = 0; frame < kMostFrames; ++frame) {
        const library::ExportDialog& dialog = host.model.export_dialog;
        bool ready = dialog.open && dialog.problem.empty() && !dialog.files.empty();
        for (const library::ExportFile& file : dialog.files)
            ready = ready && file.ready && !file.unreadable;
        if (ready)
            break;
        frames(1);
    }
    require(
        !host.model.export_dialog.files.empty() && host.model.export_dialog.files.front().ready &&
            host.model.export_dialog.problem.empty(),
        "the old replay was not measured"
    );
    click_named("oa.library.export.export");
    for (int frame = 0; frame < kMostFrames && host.model.export_dialog.open &&
                        !host.model.export_dialog.done && host.model.export_dialog.problem.empty();
         ++frame)
        frames(1);
    require(
        host.model.export_dialog.done,
        host.model.export_dialog.problem.empty()
            ? std::string("the old replay's export did not finish")
            : host.model.export_dialog.problem
    );
    click_named("oa.library.export.close");
    frames(1);
    click_named(row_of("default/export-aged.oademo"));
    {
        const library::ReplayRow* row =
            library::find_replay(host.model, "default/export-aged.oademo");
        require(
            row != nullptr && row->replay.exported, "the exported replay is not marked on its row"
        );
        // The Kept line sits below the card's visible part at 1280x720, so
        // the check reads the fact from the model. named() only returns a
        // control the card is showing.
        bool says_kept = false;
        if (row != nullptr) {
            for (const library::ReplayFact& fact : library::replay_facts(host.model, *row))
                if (fact.key == "kept" && fact.value == "Exported replays are kept")
                    says_kept = true;
        }
        require(says_kept, "the exported replay's card does not say it is kept");
        const replays::Replay written =
            replays::read_file(recordings, "default/export-aged.oademo");
        const replays::ExportedRead stored = replays::read_exported(recordings);
        require(!stored.damaged, "exported.json could not be read after the export");
        require(
            replays::is_kept(stored.exported, written), "exporting the old replay did not mark it"
        );
        const replays::Replay unmarked = replays::read_file(recordings, "default/due-again.oademo");
        require(
            !replays::is_kept(stored.exported, unmarked), "exporting one replay marked another"
        );
    }
    close();
    consider_replay_retention(true);
    frames(2);
    require(!named("oa.prompt.let-them-go"), "the consideration after an export asked again");
    drain_moves();
    require(still_there(export_aged), "the next consideration removed a replay that was exported");
    require(!still_there(due_again), "the next consideration left a replay that was due");
    require(spared(), "the next consideration removed a replay it spares");
    std::cout << "library replays: exporting a replay older than the span kept it\n";

    // Exports one replay the section is already showing, into Exported replays.
    const auto export_one = [&](std::string_view relative) {
        click_named(row_of(relative));
        click_named("oa.library.action.export");
        require(host.model.export_dialog.open, std::string(relative) + " did not open Export");
        host.model.export_dialog.folder = platform::path_to_utf8(exported);
        host.model.export_dialog.folder_is_downloads = false;
        host.model.export_dialog.folder_label = "Exported replays";
        ++host.model_revision;
        frames(1);
        for (int measure = 0; measure < kMostFrames; ++measure) {
            const library::ExportDialog& dialog = host.model.export_dialog;
            bool ready = dialog.open && dialog.problem.empty() && !dialog.files.empty();
            for (const library::ExportFile& held_file : dialog.files)
                ready = ready && held_file.ready && !held_file.unreadable;
            if (ready)
                break;
            frames(1);
        }
        require(
            !host.model.export_dialog.files.empty() &&
                host.model.export_dialog.files.front().ready &&
                host.model.export_dialog.problem.empty(),
            std::string(relative) + " was not measured"
        );
        click_named("oa.library.export.export");
        for (int measure = 0;
             measure < kMostFrames && host.model.export_dialog.open &&
             !host.model.export_dialog.done && host.model.export_dialog.problem.empty();
             ++measure)
            frames(1);
        require(
            host.model.export_dialog.done,
            host.model.export_dialog.problem.empty()
                ? std::string(relative) + "'s export did not finish"
                : host.model.export_dialog.problem
        );
        click_named("oa.library.export.close");
        frames(1);
    };
    const auto clear_export_folder = [&] {
        std::error_code cleared;
        fs::remove_all(exported, cleared);
        std::error_code made_folder;
        fs::create_directories(exported, made_folder);
        require(!made_folder, "the export folder could not be made");
    };

    // The marks written above. Put back after the damage steps, so the
    // considerations that follow spare the same replays they did before.
    const std::vector<uint8_t> stored_export_marks = read_bytes(replays::exported_path(recordings));
    require(stored_export_marks.size() > 1, "exported.json is not there to cut");

    // A cut exported.json is set aside. The next start has that set-aside
    // file and no exported.json. An export then writes nothing, and every
    // old replay stays.
    {
        const fs::path export_marks = replays::exported_path(recordings);
        const std::size_t export_cut = stored_export_marks.size() / 2;
        const std::vector<uint8_t> cut_marks(
            stored_export_marks.data(), stored_export_marks.data() + export_cut
        );
        write_bytes(export_marks, std::span<const uint8_t>(cut_marks.data(), cut_marks.size()));
        consider_replay_retention(true);
        frames(2);
        drain_moves();
        require(still_there(export_aged) && spared(), "a cut exported.json let a replay go");
        fs::path export_aside = export_marks;
        export_aside += ".damaged";
        require(!fs::exists(export_marks), "a cut exported.json was not set aside");
        require(
            read_bytes(export_aside) == cut_marks, "exported.json.damaged does not hold the cut"
        );

        // A later start reads the folder again: no exported.json, and the
        // set-aside file is still there.
        host.replays.reset();
        aged("default/export-held.oademo", 40, 822);
        const fs::path export_held = recordings / "default" / "export-held.oademo";
        window_of(1280, 720);
        frames(2);
        open_replays();
        click_named("oa.library.filter.older");
        clear_export_folder();
        export_one("default/export-held.oademo");
        close();
        consider_replay_retention(true);
        frames(2);
        require(
            !named("oa.prompt.let-them-go"),
            "the consideration after a set-aside export asked again"
        );
        drain_moves();
        require(
            !fs::exists(export_marks),
            "an export wrote exported.json while its set-aside file was there"
        );
        require(
            read_bytes(export_aside) == cut_marks,
            "the export replaced the set-aside exported marks"
        );
        require(
            still_there(export_aged) && still_there(export_held) && spared(),
            "an old replay went while exported.json was set aside"
        );
        std::error_code removed_aside;
        fs::remove(export_aside, removed_aside);
        require(!removed_aside, "the set-aside exported marks could not be removed");
        std::error_code removed_held;
        fs::remove(export_held, removed_held);
        require(!removed_held, "the replay exported during the hold could not be removed");
        std::cout << "library replays: a set-aside exported.json left every old replay, and the "
                     "export wrote nothing\n";
    }

    // Delete finds a damaged exported.json, sets it aside, and must not
    // write a new file. The next start still leaves the exported replays.
    {
        host.replays.reset();
        aged("default/hold-delete.oademo", 1, 823);
        const fs::path hold_delete = recordings / "default" / "hold-delete.oademo";
        window_of(1280, 720);
        frames(2);
        open_replays();
        click_named("oa.library.filter.older");
        clear_export_folder();
        export_one("default/export-aged.oademo");
        const fs::path export_marks = replays::exported_path(recordings);
        const std::vector<uint8_t> whole_marks = read_bytes(export_marks);
        require(whole_marks.size() > 1, "exported.json is not there to damage before Delete");
        const std::size_t export_cut = whole_marks.size() / 2;
        const std::vector<uint8_t> cut_marks(whole_marks.data(), whole_marks.data() + export_cut);
        click_named("oa.library.filter.this-month");
        click_named(row_of("default/hold-delete.oademo"));
        delete_asks("Delete this replay?");
        require(still_there(hold_delete), "Delete removed a replay before it was confirmed");
        // The shelf still holds the marks it read while the file was whole.
        // Drop it, then cut the file, so Delete is what finds the damage.
        host.replays.reset();
        write_bytes(export_marks, std::span<const uint8_t>(cut_marks.data(), cut_marks.size()));
        click_named("oa.prompt.delete");
        require(!still_there(hold_delete), "the confirmed delete left the replay");
        fs::path export_aside = export_marks;
        export_aside += ".damaged";
        require(!fs::exists(export_marks), "Delete wrote exported.json over the damaged file");
        require(
            read_bytes(export_aside) == cut_marks, "Delete replaced the set-aside exported marks"
        );
        close();
        host.replays.reset();
        consider_replay_retention(true);
        frames(2);
        drain_moves();
        require(
            still_there(export_aged) && spared(),
            "the next start removed an exported replay after Delete damaged its marks"
        );
        require(
            !fs::exists(export_marks),
            "the next start wrote exported.json while its set-aside file was there"
        );
        require(
            read_bytes(export_aside) == cut_marks,
            "the next start replaced the set-aside exported marks"
        );
        std::error_code removed_aside;
        fs::remove(export_aside, removed_aside);
        require(!removed_aside, "the set-aside exported marks could not be removed after Delete");
        write_bytes(replays::exported_path(recordings), stored_export_marks);
        host.replays.reset();
        std::cout << "library replays: Delete left a damaged exported.json set aside, and the "
                     "exported replays stayed\n";
    }

    // Delete's question, for two marked replays: Cancel, Escape and Enter on
    // the marked Cancel keep them; its Delete moves them.
    open_replays();
    click_named("oa.library.filter.this-month");
    click_named("oa.library.replays.kind");
    click_named("oa.library.replays.kind.all");
    click_named(row_of("default/filler-1.oademo"));
    click_named(row_of("default/filler-2.oademo"), SDL_KMOD_CTRL);
    require(host.model.replays.marked.size() == 2, "Ctrl did not mark a second replay to delete");
    const auto both_kept = [&](std::string_view after) {
        require(
            !named("oa.prompt.body").has_value(),
            std::string(after) + " did not close the delete question"
        );
        require(
            still_there(filler_one) && still_there(filler_two) &&
                host.model.replays.marked.size() == 2,
            std::string(after) + " deleted a replay"
        );
    };
    delete_asks("Delete these 2 replays?");
    require(
        named("oa.prompt.delete")->text == "Delete replays",
        "the delete question's button does not say Delete replays"
    );
    {
        const std::optional<AutomationControl> cancel = named("oa.prompt.cancel");
        require(cancel && cancel->focused, "the delete question does not mark Cancel at first");
    }
    snapshot("delete-question", true);
    click_named("oa.prompt.cancel");
    both_kept("Cancel");
    delete_asks("Delete these 2 replays?");
    press_key(SDLK_ESCAPE);
    both_kept("Escape");
    delete_asks("Delete these 2 replays?");
    press_key(SDLK_RETURN);
    both_kept("Enter on the marked Cancel");
    std::cout << "library replays: Cancel, Escape and Enter on Cancel kept the replays\n";
    delete_asks("Delete these 2 replays?");
    click_named("oa.prompt.delete");
    require(
        !still_there(filler_one) && !still_there(filler_two), "the confirmed delete left a file"
    );
    require(
        library::find_replay(host.model, "default/filler-1.oademo") == nullptr &&
            library::find_replay(host.model, "default/filler-2.oademo") == nullptr,
        "a deleted replay stayed in the list"
    );
    {
        const replays::IndexRead stored = replays::read_index(recordings);
        require(
            !stored.damaged && replays::find(stored.index, "default/filler-1.oademo") == nullptr &&
                replays::find(stored.index, "default/filler-2.oademo") == nullptr,
            "a deleted replay stayed in the index"
        );
    }
    require(spared(), "deleting one replay removed another");
    std::cout << "library replays: Delete moved the marked replays and took their rows\n";

    // Two copies of one kept recording. Deleting one leaves the other kept.
    // A kept file whose bytes changed still reads Kept, and is in this filter.
    click_named("oa.library.filter.kept");
    click_named(row_of("default/kept-changed.oademo"));
    {
        const library::ReplayRow* changed_row =
            library::find_replay(host.model, "default/kept-changed.oademo");
        require(
            changed_row != nullptr && changed_row->replay.kept, "the changed replay is not kept"
        );
        const std::optional<AutomationControl> fact = named("oa.library.fact.kept");
        require(
            fact && fact->text.find("Kept") != std::string::npos &&
                fact->text.find("Until you delete it") != std::string::npos,
            "the changed replay does not read Kept until it is deleted"
        );
    }
    click_named(row_of("default/copy-a.oademo"));
    delete_asks("Delete this replay?");
    click_named("oa.prompt.delete");
    require(!still_there(copy_a), "deleting one copy left its file");
    require(still_there(copy_b), "deleting one copy removed the other");
    {
        const library::ReplayRow* other = library::find_replay(host.model, "ridge/copy-b.oademo");
        require(other != nullptr && other->replay.kept, "the other copy is no longer kept");
    }
    close();
    consider_replay_retention(true);
    drain_moves();
    require(
        still_there(copy_b) && still_there(kept_file) && still_there(changed_file),
        "the next consideration removed a kept replay"
    );
    require(!named("oa.prompt.let-them-go"), "the next consideration asked again");
    std::cout << "library replays: deleting one copy left the other kept\n";

    // A folder the trash cannot rename out of. Where the system allows the
    // move anyway, the check refuses it itself, so the card still says so.
    clean_up.restore_write = stuck_file.parent_path();
    {
        std::error_code failure;
        fs::permissions(
            clean_up.restore_write,
            fs::perms::owner_read | fs::perms::owner_exec | fs::perms::group_read |
                fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec,
            fs::perm_options::replace,
            failure
        );
        require(!failure, "the replay's folder could not be made read-only");
        const fs::path probe = clean_up.restore_write / "stuck-probe.oademo";
        fs::rename(stuck_file, probe, failure);
        if (!failure) {
            std::error_code back;
            fs::rename(probe, stuck_file, back);
            require(!back, "the probed replay could not be put back");
            replay_trash_refusal_ = "the trash did not take the file";
        }
    }
    open_replays();
    click_named("oa.library.filter.this-month");
    click_named(row_of("locked/stuck.oademo"));
    delete_asks("Delete this replay?");
    click_named("oa.prompt.delete");
    require(still_there(stuck_file), "a refused delete removed the file");
    require(
        host.model.replays.delete_failure.find("could not be moved to the trash") !=
            std::string::npos,
        "a refused delete said nothing"
    );
    std::cout << "library replays: a delete the trash refused left the file and said so\n";
    close();
    replay_trash_refusal_.clear();

    // Last: a damaged kept.json loses no kept replay. It is cut in half, so
    // the marks in the half cut off are gone: the bytes left name neither
    // those replays' recordings nor their paths. That consideration removes
    // nothing. Keeping another replay and considering again leaves every
    // replay recorded before the file was set aside, the ones whose marks
    // were cut off among them.
    {
        const fs::path marks = replays::kept_path(recordings);
        const std::vector<uint8_t> bytes = read_bytes(marks);
        require(bytes.size() > 1, "kept.json is not there to damage");
        const std::string whole(bytes.begin(), bytes.end());
        const std::array<std::string_view, 3> old_kept_paths{
            "default/kept-old.oademo", "default/kept-changed.oademo", "ridge/copy-b.oademo"
        };
        std::vector<replays::Replay> old_kept;
        for (const std::string_view relative : old_kept_paths)
            old_kept.push_back(replays::read_file(recordings, relative));
        // How many old kept replays the first `size` bytes no longer name.
        const auto lost_by = [&](std::size_t size) {
            const std::string_view kept_part(whole.data(), size);
            std::size_t unnamed = 0;
            for (const replays::Replay& kept_row : old_kept)
                if (!replays::spared_by_kept_bytes(kept_part, kept_row.sha256, kept_row.path))
                    ++unnamed;
            return unnamed;
        };
        // The marks are sorted by recording, so the half cut off may hold
        // only the replays kept this month. Then the cut moves back to the
        // start of the mark that first names the last old kept replay.
        std::size_t cut = whole.size() / 2;
        if (lost_by(cut) == 0) {
            std::size_t last_named = 0;
            for (const replays::Replay& kept_row : old_kept) {
                std::size_t named_at = whole.find(kept_row.path);
                if (!kept_row.sha256.empty())
                    named_at = std::min(named_at, whole.find(kept_row.sha256));
                if (named_at != std::string::npos)
                    last_named = std::max(last_named, named_at);
            }
            const std::size_t opens = whole.rfind('{', last_named);
            if (opens != std::string::npos)
                cut = opens + 1;
        }
        const std::size_t lost = lost_by(cut);
        require(lost > 0, "cutting kept.json left every kept replay named");
        write_bytes(marks, std::span<const uint8_t>(bytes.data(), cut));
        consider_replay_retention(true);
        require(
            still_there(kept_file) && still_there(changed_file) && still_there(copy_b),
            "a damaged kept file let a kept replay go"
        );
        std::error_code failure;
        fs::path aside = marks;
        aside += ".damaged";
        require(fs::is_regular_file(aside, failure), "the damaged kept file was not set aside");
        const replays::KeptRead restored = replays::read_kept(recordings);
        require(
            !restored.damaged && !restored.kept.marks.empty(),
            "the kept marks were not carried into a new file"
        );
        open_replays();
        click_named("oa.library.filter.this-month");
        click_named(row_of("default/watched.oademo"));
        click_named("oa.library.action.keep");
        close();
        consider_replay_retention(true);
        drain_moves();
        require(
            still_there(kept_file) && still_there(changed_file) && still_there(copy_b),
            "a kept replay whose mark was cut off went while the damaged kept file was set aside"
        );
        require(!named("oa.prompt.let-them-go"), "a damaged kept file made the question ask again");
        std::cout << "library replays: kept.json cut to " << cut << " of " << whole.size()
                  << " bytes lost every mark of " << lost
                  << " old kept replays; it was set aside and every kept replay stayed\n";
    }

#if !defined(__ANDROID__) && OA_PROCESS_SPAWNING
    {
        // The pass's own replays, written after every earlier picture. The
        // made-up list stays marked read. These stay unread, so the Reading…
        // chip, the footer's count and the card belong to them.
        write_bytes(recordings / "default" / "indexed.oademo", source_bytes);
        for (int copy = 1; copy <= 3; ++copy) {
            const std::string name = "waiting-" + std::to_string(copy) + ".oademo";
            write_bytes(recordings / "default" / name, source_bytes);
        }
        const std::vector<std::string> names{
            "Ada", "Bea", "Cid", "Dot", "Eve", "Fay", "Gus", "Hal", "Ivy", "Joss"
        };
        MadeReplay watchers;
        watchers.path = "default/watchers.oademo";
        watchers.days_ago = 0;
        watchers.kind = "skirmish";
        watchers.map = "Watcher Field";
        watchers.players = names;
        watchers.recorded_at = 1;
        watchers.variant = 910;
        write_made(recordings, source, watchers, now);
        hold_unread = true;

        const auto show_pass_rows = [&] {
            library::set_replay_filter(host.model, library::ReplayFilter::this_month);
            library::set_replay_kind(host.model, {});
            library::set_query(host.model, {});
            host.state.search = {};
            ++host.model_revision;
            frames(1);
        };
        const auto ours = [](std::string_view path) {
            return path == "default/indexed.oademo" || path == "default/watchers.oademo" ||
                   path.rfind("default/waiting-", 0) == 0;
        };
        const auto indexing_row = [&] {
            std::string path;
            for (const library::ReplayRow& row : host.model.inputs.replays.rows) {
                if (!ours(row.replay.path) || !row.indexing)
                    continue;
                if (library::replay_reading(host.model, row).find("Reading") == std::string::npos)
                    continue;
                path = row.replay.path;
                break;
            }
            require(!path.empty(), "no unread replay says Reading");
            return path;
        };

        window_of(1280, 720);
        frames(2);
        open_replays();
        show_pass_rows();
        for (int frame = 0; frame < kMostFrames && !host.index_pass_running(); ++frame)
            frames(1);
        require(host.index_pass_running(), "the index pass did not start");
        const int pid = host.index_pass_pid();
        require(pid > 0, "the index pass has no process");
        close();
        require(process_gone(pid), "closing the Library left the index process");
        require(!host.index_pass_running(), "closing the Library left the index pass");
        std::cout << "library replays: closing the Library left no index process\n";

        const auto chip_at = [&](int width, int height) {
            window_of(width, height);
            frames(2);
            if (oa_layer().find(library_screen_name) == nullptr)
                open_replays();
            show_pass_rows();
            const std::string path = indexing_row();
            row_of(path);
            const std::string status = library::replay_status(host.model);
            require(
                status.find("Reading") != std::string::npos, "the footer is not the pass: " + status
            );
            const std::string size = std::to_string(width) + 'x' + std::to_string(height);
            snapshot("reading-chip-" + size);
            std::cout << "library replays: Reading… on " << path << " at " << size << ", footer \""
                      << status << "\"\n";
        };
        chip_at(640, 480);
        chip_at(1280, 720);

        if (oa_layer().find(library_screen_name) == nullptr)
            open_replays();
        show_pass_rows();
        oademo::TimelineIndex timeline;
        timeline.length_ticks = 900;
        timeline.ended = oademo::IndexEnd::finished;
        timeline.ended_tick = 900;
        timeline.verdict.finished = true;
        timeline.result.by = oademo::ResultBy::commanders;
        timeline.result.winners = {1};
        timeline.period_ticks = 300;
        for (std::size_t place = 0; place < names.size(); ++place) {
            oademo::IndexPlayer player;
            player.number = static_cast<int>(place + 1);
            player.name = names[place];
            player.watcher = place == 1;
            if (place == 0) {
                player.final.kills = 1;
                player.final.losses = 1;
            }
            timeline.players.push_back(std::move(player));
        }
        oademo::IndexMarker marker;
        marker.tick = 120;
        marker.kind = oademo::MarkerKind::commander_death;
        marker.player = 1;
        marker.other = 2;
        timeline.markers.push_back(marker);
        const std::string text = oademo::timeline_index_text(timeline);
        require(
            oademo::parse_timeline_index(text).has_value(), "the watcher's timeline does not read"
        );
        host.store_replay_timeline(*this, "default/watchers.oademo", text);
        frames(2);
        {
            const library::ReplayRow* row =
                library::find_replay(host.model, "default/watchers.oademo");
            require(
                row != nullptr && row->standings.size() == names.size() &&
                    row->recorded == library::RecordedResult::none && row->length_ticks == 900 &&
                    !row->marks.empty(),
                "the watcher's card has no timeline"
            );
            require(
                row->standings[0].result == library::RecordedResult::won &&
                    row->standings[0].known && row->standings[0].kills == 1 &&
                    row->standings[0].losses == 1,
                "the winner's card does not say 1 kill and 1 loss"
            );
            require(
                row->standings[1].result == library::RecordedResult::none &&
                    !row->standings[1].known,
                "a watcher has a result"
            );
            require(
                row->standings[2].result == library::RecordedResult::lost &&
                    row->standings[2].known,
                "another player has no result"
            );
            std::cout << "library replays: a watcher has no result\n";
        }

        const auto card_at = [&](int width, int height) {
            window_of(width, height);
            frames(2);
            if (oa_layer().find(library_screen_name) == nullptr)
                open_replays();
            show_pass_rows();
            const bool compact = width <= 640;
            if (compact) {
                if (!named("oa.library.back"))
                    click_named(row_of("default/watchers.oademo"));
                require(named("oa.library.back").has_value(), "the watcher's page did not open");
            } else {
                click_named(row_of("default/watchers.oademo"));
            }
            const std::string size = std::to_string(width) + 'x' + std::to_string(height);
            host.state.details_scroll = 0;
            ++host.model_revision;
            frames(2);
            const std::optional<AutomationControl> top = named("oa.library.status");
            const int top_y = top ? top->window_y : -1;
            snapshot("timeline-strip-" + size);
            host.state.details_scroll = 80;
            ++host.model_revision;
            frames(2);
            const std::optional<AutomationControl> moved = named("oa.library.status");
            const int moved_y = moved ? moved->window_y : -1;
            require(moved_y != top_y, "the card did not scroll to its results at " + size);
            snapshot("index-results-" + size);
            if (compact)
                click_named("oa.library.back");
        };
        card_at(640, 480);
        card_at(1280, 720);

        if (oa_layer().find(library_screen_name) == nullptr)
            open_replays();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
        std::string said;
        while (std::chrono::steady_clock::now() < deadline && said.empty()) {
            frames(1);
            const library::ReplayRow* row =
                library::find_replay(host.model, "default/indexed.oademo");
            if (row == nullptr || !row->replay.timeline.has_value() ||
                row->replay.timeline->empty())
                continue;
            bool known = row->recorded != library::RecordedResult::none;
            for (const library::PlayerStanding& standing : row->standings)
                if (standing.known)
                    known = true;
            if (!known)
                continue;
            if (row->recorded == library::RecordedResult::won)
                said = "Won";
            else if (row->recorded == library::RecordedResult::lost)
                said = "Lost";
            else
                said = "no side";
            for (const library::PlayerStanding& standing : row->standings) {
                if (!standing.known)
                    continue;
                said += ", " + std::to_string(standing.kills) + " kills, " +
                        std::to_string(standing.losses) + " losses";
                break;
            }
        }
        require(!said.empty(), "indexed.oademo was not read");
        std::cout << "library replays: indexed.oademo shows " << said << '\n';
        close();
    }
#endif
}

void Runtime::check_replay_import() {
    const auto stop = [](bool ok, std::string_view what) {
        if (!ok)
            throw std::runtime_error("replay import check: " + std::string(what));
    };
    stop(sdl_.window != nullptr, "the check needs the SDL presenter");
    stop(!user_folder_.empty(), "the run has no folder of the player's own");
    const fs::path recordings = user_folder_ / std::string(recordings_folder_name);
    const fs::path sources = user_folder_ / "opened-replays";

    struct CleanUp {
        fs::path recordings;
        fs::path sources;

        ~CleanUp() {
            std::error_code removed;
            fs::remove_all(recordings, removed);
            fs::remove_all(sources, removed);
        }
    } clean_up{recordings, sources};

    {
        std::error_code removed;
        fs::remove_all(recordings, removed);
        fs::remove_all(sources, removed);
    }

    const std::vector<uint8_t> recorder{'o', 'a', '-', 't', 'e', 's', 't'};
    const std::vector<uint8_t> status_bytes{0x04};
    const std::vector<uint8_t> payload{tad::payload_marker, 0xff};
    const auto recording_of = [&](std::string map) {
        tad::Demo demo;
        demo.version = tad::supported_version;
        demo.max_units = 250;
        demo.map_name = std::move(map);
        demo.sectors = {
            {static_cast<uint32_t>(tad::SectorType::recorder_version), recorder},
        };
        demo.players = {{2, 0, 1, "Pilot"}, {5, 1, 2, "Screamer"}};
        demo.statuses = {{1, status_bytes}, {2, status_bytes}};
        demo.packets = {
            {0, 40, 40, 1, payload},
            {0, 60, 100, 2, payload},
            {0, 1400, 1500, 1, payload},
        };
        const tad::WriteResult written = tad::write(demo);
        stop(written.ok(), "a recording could not be written");
        return written.bytes;
    };
    const auto show_down = [&](std::string map = "Show Down") {
        oademo::Replay replay;
        replay.recording = recording_of(map);
        oademo::set_recording_identity(replay);
        replay.meta.recording.tad_version = 5;
        replay.meta.recording.packets = 3;
        replay.meta.recorded.how = "multiplayer";
        replay.meta.recorded.at = "2026-11-04T14:32:00Z";
        replay.meta.recorded.local = "2026-11-04 14:32";
        replay.meta.recorded.by = "Open Annihilation 0.9.0";
        replay.meta.game.kind = "multiplayer";
        replay.meta.game.length_ms = 1500;
        replay.meta.game.ended = "finished";
        replay.meta.map.name = map;
        replay.meta.content.base.id = "totala";
        replay.meta.content.base.version = "3.1c";
        replay.meta.units.count = 1;
        replay.meta.units.table_sha256 = std::string(64, 'a');
        replay.meta.units.index_bits = 1;
        oademo::PlayerInfo first;
        first.number = 1;
        first.name = "Pilot";
        first.side = "arm";
        first.colour = 2;
        first.team = 1;
        first.kind = "human";
        first.recorded_here = true;
        oademo::PlayerInfo second;
        second.number = 2;
        second.name = "Screamer";
        second.side = "core";
        second.colour = 5;
        second.kind = "computer";
        replay.meta.players = {first, second};
        replay.manifest.engine.recorded_with = "0.9.0";
        replay.manifest.engine.needs = ">=0.9.0";
        oademo::Need base;
        base.kind = "base";
        base.id = "totala";
        base.version = "3.1c";
        replay.manifest.needs.push_back(base);
        const oademo::WriteResult written = oademo::write(replay, {});
        stop(written.ok(), "the .oademo could not be written: " + written.error);
        return written.bytes;
    };
    const auto u16_at = [](std::span<const uint8_t> bytes, std::size_t at) {
        return static_cast<uint16_t>(bytes[at] | (static_cast<uint16_t>(bytes[at + 1]) << 8U));
    };
    const std::vector<uint8_t> clean_tad = recording_of("Anteer");
    std::vector<uint8_t> dirty_tad = clean_tad;
    stop(dirty_tad.size() >= 12, "the recording is shorter than its header");
    const uint16_t header = u16_at(dirty_tad, 0);
    const std::size_t count_at = header;
    stop(count_at + 6 <= dirty_tad.size(), "the recording has no sector count");
    const std::size_t count_value = count_at + tad::layout::chunk_length_bytes;
    const uint32_t sectors = static_cast<uint32_t>(dirty_tad[count_value]) |
                             (static_cast<uint32_t>(dirty_tad[count_value + 1]) << 8U) |
                             (static_cast<uint32_t>(dirty_tad[count_value + 2]) << 16U) |
                             (static_cast<uint32_t>(dirty_tad[count_value + 3]) << 24U);
    dirty_tad[count_value] = static_cast<uint8_t>(sectors + 1U);
    dirty_tad[count_value + 1] = static_cast<uint8_t>((sectors + 1U) >> 8U);
    dirty_tad[count_value + 2] = static_cast<uint8_t>((sectors + 1U) >> 16U);
    dirty_tad[count_value + 3] = static_cast<uint8_t>((sectors + 1U) >> 24U);
    const std::array<uint8_t, 6> sector_six{0x06, 0x00, 0x06, 0x00, 0x00, 0x00};
    dirty_tad.insert(
        dirty_tad.begin() + static_cast<std::ptrdiff_t>(count_at + 6),
        sector_six.begin(),
        sector_six.end()
    );
    std::vector<uint8_t> versioned = clean_tad;
    stop(versioned.size() > 11, "the recording has no version");
    versioned[10] = 3;
    versioned[11] = 0;
    write_bytes(sources / "show-down.oademo", show_down());
    write_bytes(sources / "anteer.tad", dirty_tad);
    write_bytes(sources / "broken.tad", {reinterpret_cast<const uint8_t*>("hello"), 5});
    write_bytes(sources / "version.tad", versioned);

    bool running = true;
    stop(
        SDL_SetWindowSize(sdl_.window, 1280, 720) && SDL_SyncWindow(sdl_.window),
        "the window did not take 1280x720"
    );
    apply_output_mode();
    const auto frame = [&] {
        run_frame(running);
        stop(running, "a frame ended the run");
    };
    const auto control_named = [this](std::string_view name) -> std::optional<AutomationControl> {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        for (const AutomationControl& control : controls)
            if (control.name == name && control.visible && control.window_width > 0 &&
                control.window_height > 0)
                return control;
        return std::nullopt;
    };
    const auto click_named = [&](std::string_view name) {
        const std::optional<AutomationControl> control = control_named(name);
        stop(control.has_value(), std::string(name) + " is not shown");
        const oa::ui::display_layout::Point pixel{
            control->window_x + control->window_width / 2,
            control->window_y + control->window_height / 2
        };
        for (const SDL_EventType type :
             {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event event = window_pointer_event(
                sdl_.window,
                sdl_.renderer,
                type,
                pixel,
                type == SDL_EVENT_MOUSE_MOTION ? 0 : SDL_BUTTON_LEFT
            );
            bool going = true;
            dispatch_event(event, going);
            stop(going, "a click ended the run");
        }
        frame();
    };
    const auto body = [&]() -> std::string {
        const std::optional<AutomationControl> words = control_named("oa.replay-import.body");
        return words ? words->text : std::string{};
    };
    const auto close_notice = [&] {
        for (int wait = 0; wait < 10 && control_named("oa.replay-import.ok"); ++wait)
            click_named("oa.replay-import.ok");
        stop(!control_named("oa.replay-import.body"), "the notice stayed open");
    };
    const auto selected = [this]() -> std::string {
        const LibraryHost& host = library_host();
        return host.model.replays.selected ? *host.model.replays.selected : std::string{};
    };
    const auto row_of = [this](std::string_view path) -> const library::ReplayRow* {
        const LibraryHost& host = library_host();
        for (const library::ReplayRow& row : host.model.inputs.replays.rows)
            if (row.replay.path == path)
                return &row;
        return nullptr;
    };
    const auto open_and_select = [&](const fs::path& file, std::string_view path) {
        package_install::post_replay_file(file);
        for (int wait = 0; wait < 300 && selected() != path; ++wait)
            frame();
        stop(selected() == path, std::string(path) + " was not selected");
    };
    constexpr int most_frames = 300;

    open_and_select(sources / "show-down.oademo", "Imported/show-down.oademo");
    for (int wait = 0;
         wait < most_frames && body().find("Added Show Down to Replays") == std::string::npos;
         ++wait)
        frame();
    stop(
        body().find("Added Show Down to Replays") != std::string::npos,
        "the .oademo was not announced"
    );
    const library::ReplayRow* show = row_of("Imported/show-down.oademo");
    stop(show != nullptr, "the .oademo is not listed");
    std::cout << "replay import: copied " << show->replay.path << ' ' << show->replay.sha256 << ' '
              << show->replay.map << '\n';
    std::cout << "replay import: selected " << show->replay.path << '\n';
    close_notice();

    open_and_select(sources / "anteer.tad", "Imported/anteer.tad");
    for (int wait = 0;
         wait < most_frames && body().find("Added Anteer to Replays") == std::string::npos;
         ++wait)
        frame();
    stop(body().find("Added Anteer to Replays") != std::string::npos, "the .tad was not announced");
    const std::vector<uint8_t> kept_tad = read_bytes(recordings / "Imported" / "anteer.tad");
    const tad::ParseResult kept_parsed = tad::parse(kept_tad);
    stop(kept_parsed.ok(), "the imported .tad does not parse");
    int sector_six_count = 0;
    if (kept_parsed.ok()) {
        for (const tad::Sector& sector : kept_parsed.demo->sectors)
            if (sector.type == static_cast<uint32_t>(tad::SectorType::player_address))
                ++sector_six_count;
    }
    stop(sector_six_count == 0, "the imported .tad holds sector 6");
    stop(kept_tad == clean_tad, "the imported .tad changed more than sector 6");
    const library::ReplayRow* anteer = row_of("Imported/anteer.tad");
    stop(anteer != nullptr, "the .tad is not listed");
    std::cout << "replay import: copied " << anteer->replay.path << ' ' << anteer->replay.sha256
              << ' ' << anteer->replay.map << '\n';
    std::cout << "replay import: selected " << anteer->replay.path << '\n';
    std::cout << "replay import: tad-sector-6 " << sector_six_count << '\n';
    close_notice();

    const auto count_imported = [&] {
        int count = 0;
        std::error_code error;
        const fs::directory_iterator end;
        for (fs::directory_iterator it(recordings / "Imported", error); !error && it != end;
             it.increment(error))
            if (it->is_regular_file())
                ++count;
        return count;
    };
    const int imported_before = count_imported();
    open_and_select(sources / "show-down.oademo", "Imported/show-down.oademo");
    for (int wait = 0; wait < 30; ++wait)
        frame();
    stop(count_imported() == imported_before, "opening the .oademo again copied it");
    stop(body().find("Added") == std::string::npos, "opening the .oademo again announced it");
    std::cout << "replay import: kept Imported/show-down.oademo\n";
    open_and_select(sources / "anteer.tad", "Imported/anteer.tad");
    for (int wait = 0; wait < 30; ++wait)
        frame();
    stop(count_imported() == imported_before, "opening the .tad again copied it");
    std::cout << "replay import: kept Imported/anteer.tad\n";

    const auto refuse = [&](const fs::path& file, std::string_view sentence) {
        package_install::post_replay_file(file);
        for (int wait = 0; wait < most_frames && body().find(sentence) == std::string::npos; ++wait)
            frame();
        stop(body().find(sentence) != std::string::npos, std::string(sentence) + " was not shown");
        std::cout << "replay import: refused " << sentence << '\n';
        close_notice();
    };
    refuse(sources / "broken.tad", "This file is not a TA Demo recording");
    refuse(
        sources / "version.tad",
        "This recording is in version 3 of the format, which Open Annihilation does not play"
    );

    // Two files given together: each notice stays until it is closed, and the
    // last file is the one selected.
    write_bytes(sources / "coast.oademo", show_down("Coast"));
    write_bytes(sources / "plain.tad", recording_of("Plain"));
    package_install::post_replay_file(sources / "coast.oademo");
    package_install::post_replay_file(sources / "plain.tad");
    for (int wait = 0;
         wait < most_frames && body().find("Added Coast to Replays") == std::string::npos;
         ++wait)
        frame();
    stop(
        body().find("Added Coast to Replays") != std::string::npos,
        "the first of two replays was not announced"
    );
    std::cout << "replay import: both Added Coast to Replays\n";
    for (int wait = 0; wait < 10 && body().find("Added Coast to Replays") != std::string::npos;
         ++wait)
        click_named("oa.replay-import.ok");
    for (int wait = 0;
         wait < most_frames && (body().find("Added Plain to Replays") == std::string::npos ||
                                selected() != "Imported/plain.tad");
         ++wait)
        frame();
    stop(
        body().find("Added Plain to Replays") != std::string::npos,
        "the second of two replays was not announced"
    );
    stop(selected() == "Imported/plain.tad", "the last of two replays was not selected");
    std::cout << "replay import: both Added Plain to Replays\n";
    std::cout << "replay import: both-selected Imported/plain.tad\n";
    close_notice();
}

} // namespace oa::app

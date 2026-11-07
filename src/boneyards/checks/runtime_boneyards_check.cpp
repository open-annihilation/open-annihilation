// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Boneyards login checks, and the galactic map and the Main Frame
// consoles driven through SDL input on the session the login check reached:
// a planet hovered and pressed, a console opened with its button and
// released with Escape. They drive the running game through the engine's
// check host. Test scaffolding; the Boneyards behaviour it drives is
// implemented elsewhere.
#include "boneyards_check.hpp"
#include "boneyards_options.hpp"
#include "console_state.hpp"
#include "galaxy_graphics.hpp"

#include "oa/app/app.hpp"
#include "oa/app/asset_files.hpp"
#include "oa/app/check_host.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/rover_calls.hpp"
#include "oa/boneyards/rover_replay.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/screens/contents.hpp"
#include "oa/boneyards/screens/navigator.hpp"
#include "oa/boneyards/screens/profile.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/platform/system.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace oa::app {

// The Boneyards overlay's status widget, its sound hook and its settings
// (console_screens.cpp).
oa::boneyards::console::StatusWidget* boneyards_console_status_widget();
void boneyards_console_observe_sounds(void (*observer)(void* user, const char* name), void* user);
bool boneyards_console_write_setting(const char* key, const char* value);

namespace boneyards_check {

bool heard_since(const std::vector<std::string>& sounds, std::size_t from, const char* name) {
    return std::find(sounds.begin() + static_cast<std::ptrdiff_t>(from), sounds.end(), name) !=
           sounds.end();
}

void print_probe(const char* when, const oa::boneyards::console::MainFrameProbe& p) {
    std::cout << "boneyards login check: " << when << ": level " << p.connection_level
              << ", console " << p.console_screen << (p.screen_shown ? " shown" : "")
              << ", layer sliding " << p.layer_sliding << " direction " << p.layer_direction
              << " covering " << p.layer_covering << " countdown " << p.layer_countdown << '/'
              << p.slide_steps << ", " << p.planet_count << " planets"
              << (p.galaxy_loading ? " loading" : "") << (p.map_locked ? " locked" : "")
              << (p.unlock_pending ? " unlock pending" : "") << ", buttons release "
              << p.release_button << " recon " << p.recon_button << " chat " << p.chat_button
              << " battle " << p.battle_button << ", light "
              << (p.activity_light_running ? "running" : "dark") << '\n';
}

} // namespace boneyards_check

namespace {

namespace by = oa::boneyards;
namespace by_console = oa::boneyards::console;
namespace by_screens = oa::boneyards::screens;
using namespace boneyards_check;

// The login check pumps the session once a frame, as the idle tick does.
constexpr std::chrono::milliseconds kBoneyardsFrame{33};
constexpr std::chrono::seconds kBoneyardsLoginLimit{45};
constexpr std::chrono::seconds kBoneyardsLogoutLimit{5};
// Time the galactic map's feeds get once the war map has loaded.
constexpr std::chrono::seconds kBoneyardsMapLimit{10};
// The guest a replayed login uses when no account is given.
constexpr const char* kReplayGuestName = "oa_check";

// Planets the loaded war map must show in the map panel's viewer.
constexpr std::size_t kDrawnPlanetsNeeded = 20;
// Passes the main menu gets to enter the Boneyards after "-y": its first
// update asks for the entry, so a few are plenty.
constexpr std::size_t kSetupEntryPasses = 30;
constexpr int kLoginLevel = static_cast<int>(by::SessionLevel::dossier_loaded);

// Exit codes of --check-boneyards-login.
constexpr int kBoneyardsLoggedIn = 0;
constexpr int kBoneyardsLoginFailed = 1;
constexpr int kBoneyardsNoAccount = 2;
constexpr int kBoneyardsDossierMissing = 3; // login confirmed, dossier not received
constexpr int kBoneyardsMapUnavailable = 4; // logged in, galactic map unavailable

// What the check prints of the events it forwards to the screens. Texts come
// from the client and the server, never from the account.
struct LoginCheckSink {
    by::ServiceEvents screens;
    by::SessionRecord session;
    std::size_t census_replies;
};

void print_event(const char* what, const char* text) {
    std::string line = text != nullptr ? text : "";
    std::replace(line.begin(), line.end(), '\n', ' ');
    std::cout << "boneyards login check: " << what << ": " << line << '\n';
}

const char* level_name(by::SessionLevel level) {
    switch (level) {
    case by::SessionLevel::offline:
        return "offline";
    case by::SessionLevel::client_active:
        return "client active";
    case by::SessionLevel::disconnected:
        return "disconnected";
    case by::SessionLevel::logged_in:
        return "logged in, dossier pending";
    case by::SessionLevel::dossier_loaded:
        return "dossier loaded";
    }
    return "unknown";
}

by::ServiceEvents login_check_events(LoginCheckSink* sink) {
    by::ServiceEvents events{};
    events.user = sink;
    events.login_status = [](void* user, const char* text) {
        auto* sink = static_cast<LoginCheckSink*>(user);
        print_event("status", text);
        sink->screens.login_status(sink->screens.user, text);
    };
    events.login_reset = [](void* user) {
        auto* sink = static_cast<LoginCheckSink*>(user);
        sink->screens.login_reset(sink->screens.user);
    };
    events.session_changed = [](void* user, const by::SessionRecord* session) {
        auto* sink = static_cast<LoginCheckSink*>(user);
        sink->session = *session;
        print_event("session", level_name(session->level));
        sink->screens.session_changed(sink->screens.user, session);
    };
    events.notice = [](void* user, const char* text) {
        auto* sink = static_cast<LoginCheckSink*>(user);
        print_event("notice", text);
        sink->screens.notice(sink->screens.user, text);
    };
    events.system_alert = [](void* user, const by::AlertReport* alert) {
        auto* sink = static_cast<LoginCheckSink*>(user);
        print_event("alert", alert->text);
        sink->screens.system_alert(sink->screens.user, alert);
    };
    events.allegiances = [](void* user, const char* allegiances) {
        auto* sink = static_cast<LoginCheckSink*>(user);
        print_event("allegiances", allegiances);
        sink->screens.allegiances(sink->screens.user, allegiances);
    };
    events.activity = [](void* user, const char* digits) {
        auto* sink = static_cast<LoginCheckSink*>(user);
        print_event("activity", digits);
        sink->screens.activity(sink->screens.user, digits);
    };
    events.census = [](void* user, const by::CensusReport* report) {
        auto* sink = static_cast<LoginCheckSink*>(user);
        ++sink->census_replies;
        sink->screens.census(sink->screens.user, report);
    };
    return events;
}

// The game's files, which the war-map graphics are read from.
struct InstallFiles {
    const oa::AssetStore* assets;
};

bool read_install_file(void* user, const char* path, std::vector<std::uint8_t>* out) {
    try {
        *out = static_cast<InstallFiles*>(user)->assets->read(path).bytes;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// What the Main Frame does once the dossier has arrived: the war map of the
// server whose yard master answered (Boneyards\<name>_map.gwm) loads into
// the galaxy view, then the allegiance feed clears its loading state. The
// session is pumped once a frame until the feed and a census reply for
// every planet arrived, or the time limit passed.
int check_galactic_map(
    const by::BoneyardsService& service,
    by_screens::BoneyardsScreens& screens,
    const LoginCheckSink& sink,
    const oa::AssetStore& assets
) {
    const char* area = service.area_name(service.user);
    if (area == nullptr || area[0] == '\0') {
        print_event("galactic map unavailable", "no server name (slot 72)");
        return kBoneyardsMapUnavailable;
    }
    print_event("area name", area);
    auto graphics = std::make_unique<by_console::GalaxyGraphics>();
    InstallFiles files{&assets};
    if (!by_console::galaxy_load_graphics(
            *graphics, by_console::FileReader{&files, read_install_file}
        )) {
        print_event("galactic map unavailable", "the war-map graphics are missing");
        return kBoneyardsMapUnavailable;
    }
    const std::string path = std::string("Boneyards\\") + area + "_map.gwm";
    by_screens::GalaxyView& galaxy = screens.galaxy;
    if (!by_console::galaxy_view_load(&galaxy, *graphics, service, path.c_str())) {
        print_event("galactic map unavailable", ("cannot load " + path).c_str());
        return kBoneyardsMapUnavailable;
    }
    std::cout << "boneyards login check: galactic map: " << galaxy.planet_count << " planets from "
              << path << " (sector " << galaxy.sector_id << ' ' << galaxy.sector_name << ")\n";
    const auto deadline = std::chrono::steady_clock::now() + kBoneyardsMapLimit;
    while ((galaxy.loading || sink.census_replies < galaxy.planet_count) &&
           std::chrono::steady_clock::now() < deadline) {
        service.pump(service.user);
        std::this_thread::sleep_for(kBoneyardsFrame);
    }
    std::cout << "boneyards login check: census replies: " << sink.census_replies << '\n';
    if (galaxy.loading) {
        print_event("galactic map unavailable", "no allegiance feed arrived");
        return kBoneyardsMapUnavailable;
    }
    std::string sides;
    for (std::size_t i = 0; i < galaxy.planet_count; ++i)
        sides += galaxy.planets[i].side;
    print_event("allegiances applied", sides.c_str());
    print_event("galactic map", "loading cleared");
    return kBoneyardsLoggedIn;
}

std::uint64_t frame_checksum(const renderer::Surface& frame) {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    for (const std::uint8_t byte : frame.rgb) {
        hash ^= byte;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

const char* activation_problem(by::ActivationResult result) {
    switch (result) {
    case by::ActivationResult::ok:
        return "none";
    case by::ActivationResult::module_missing:
        return "the Rover table was not accepted";
    case by::ActivationResult::attach_failed:
        return "Rover could not attach (slot 1)";
    case by::ActivationResult::activate_failed:
        return "Rover could not activate (slot 3)";
    }
    return "unknown";
}

// First entry of Rover's server list, as enumerating the areas (slot 70) hands it over.
char g_first_server[by_screens::kLoginServerCapacity];

void ROVER_CALL remember_first_server(const char* location) {
    if (g_first_server[0] == '\0' && location != nullptr)
        std::snprintf(g_first_server, sizeof g_first_server, "%s", location);
}

// The entry the login dialog would offer: the list's default, else the first.
std::string boneyards_server_entry() {
    std::string server = by::rover_session_selected_server();
    if (!server.empty())
        return server;
    g_first_server[0] = '\0';
    by::rover_call_enumerate_areas(remember_first_server);
    return g_first_server;
}

constexpr const char* kSelectSound = "GREEN_BUTTON";
constexpr const char* kGalacticButtonSound = "GALACTIC_BUTTON";
// The Profile console's ProfileList lists every Profile.def category.
constexpr int32_t kProfileRows = 7;

struct ConsoleTarget {
    int32_t screen; // console_screen id
    by_console::ConsoleButton button;
    const char* sound;
};

// In BoneyardsConsole order.
constexpr ConsoleTarget kConsoleTargets[] = {
    {by_console::console_screen::recon, by_console::ConsoleButton::recon, kPlanetButtonSound},
    {by_console::console_screen::chat, by_console::ConsoleButton::chat, kPlanetButtonSound},
    {by_console::console_screen::battle, by_console::ConsoleButton::battle, kPlanetButtonSound},
    {by_console::console_screen::news, by_console::ConsoleButton::news, kGalacticButtonSound},
    {by_console::console_screen::ladders, by_console::ConsoleButton::ladders, kGalacticButtonSound},
    {by_console::console_screen::profile, by_console::ConsoleButton::profile, kGalacticButtonSound},
    {by_console::console_screen::help, by_console::ConsoleButton::help, kGalacticButtonSound},
};
static_assert(std::size(kConsoleTargets) == std::size(kBoneyardsConsoleNames));

bool same_census(const by::CensusReport& a, const by::CensusReport& b) {
    return a.area_id == b.area_id && a.population == b.population && a.activity == b.activity &&
           a.in_progress == b.in_progress && a.forming == b.forming && a.core_wins == b.core_wins &&
           a.arm_wins == b.arm_wins;
}

void print_census(const char* what, const by::CensusReport& c) {
    std::cout << "boneyards login check: " << what << " of " << c.area_id << ": population "
              << c.population << ", activity " << c.activity << ", in progress " << c.in_progress
              << ", forming " << c.forming << ", core wins " << c.core_wins << ", arm wins "
              << c.arm_wins << '\n';
}

// The planet summary of the selected planet: its war-map name,
// its owner, the census's Battles (in progress) and Forming counts and,
// only while the planet is contested, the Arm and Core wins.
bool summary_reads_census(const by_console::MainFrameProbe& p) {
    const char* owner = by::screens::planet_affiliation_label(p.selected_side);
    const bool contested = owner != nullptr && std::strcmp(owner, "Contested") == 0;
    const by::CensusReport& c = p.selected_census;
    const auto reads = [](const char* text, int32_t value) {
        return std::to_string(value) == text;
    };
    std::cout << "boneyards login check: the summary reads \"" << p.summary_name << "\", "
              << p.summary_owner << ", Battles: " << p.summary_battles
              << ", Forming: " << p.summary_forming;
    if (p.summary_wins_shown)
        std::cout << ", wins Arm " << p.summary_arm_wins << " Core " << p.summary_core_wins;
    std::cout << '\n';
    return p.summary_shown && p.selected_name[0] != '\0' &&
           std::strcmp(p.summary_name, p.selected_name) == 0 && owner != nullptr &&
           std::strcmp(p.summary_owner, owner) == 0 && reads(p.summary_battles, c.in_progress) &&
           reads(p.summary_forming, c.forming) && p.summary_wins_shown == contested &&
           (!contested ||
            (reads(p.summary_arm_wins, c.arm_wins) && reads(p.summary_core_wins, c.core_wins)));
}

// The Recon console of the picked planet: once the census its open asked
// for has arrived, its Planet field names the planet as the war map does,
// Gravity reads a value from the planet's map OTA and Population the
// planet's census (the replay's line when it has one); with a
// panorama the scan scrolls a pixel a pass.
template <typename Run>
int check_recon(
    const by_console::MainFrameProbe& probe, const Run& run, std::size_t& pass, std::size_t passes
) {
    std::size_t census_passes = 0;
    while (probe.activity_light_running && pass < passes) {
        run();
        ++census_passes;
    }
    by::CensusReport expected{};
    const int32_t population = by::rover_replay_census(probe.selected_area_id, &expected)
                                   ? expected.population
                                   : probe.selected_census.population;
    std::cout << "boneyards login check: recon of " << probe.selected_area_id << ": planet \""
              << probe.recon_planet_text << "\", gravity \"" << probe.recon_gravity_text
              << "\", population " << probe.recon_population << " after " << census_passes
              << " passes, " << (probe.recon_panorama ? "panorama" : "map static") << '\n';
    if (!probe.recon_shown || probe.activity_light_running ||
        std::string(probe.recon_planet_text) != probe.selected_name ||
        probe.recon_gravity_text[0] == '\0' || probe.recon_population != population) {
        std::cout << "boneyards login check: the Recon console does not show planet "
                  << probe.selected_area_id << " (census population " << population << ")\n";
        return kEntryReconWrong;
    }
    if (!probe.recon_panorama)
        return kEntryPassed;
    const int32_t scroll = probe.recon_pan_scroll;
    constexpr std::size_t kScanPasses = 3;
    std::size_t scanned = 0;
    for (; scanned < kScanPasses && pass < passes; ++scanned)
        run();
    std::cout << "boneyards login check: the panorama scrolled from " << scroll << " to "
              << probe.recon_pan_scroll << " in " << scanned << " passes\n";
    if (scanned != kScanPasses || probe.recon_pan_scroll == scroll)
        return kEntryReconWrong;
    return kEntryPassed;
}

// The Chat console of the picked planet, when the replay records its
// rooms: the listing's last row picks a default frequency to enter, and once
// entered the console lists the room's members and the planet's frequencies
// in the replay, with a row for each in its frequency and player lists, and
// has announced the entry in its log. The activity light the opening
// started must be dark again: the last row of the listing that follows the
// entry stops it.
template <typename Run>
int check_chat(
    const by_console::MainFrameProbe& probe, const Run& run, std::size_t& pass, std::size_t passes
) {
    const std::size_t rooms = by::rover_replay_room_count(probe.selected_area_id);
    if (rooms == 0) {
        std::cout << "boneyards login check: the replay records no rooms for "
                  << probe.selected_area_id << '\n';
        return kEntryPassed;
    }
    const auto entered = [&] {
        return probe.chat_room_id != 0 && probe.chat_members > 0 &&
               static_cast<std::size_t>(probe.chat_frequencies) == rooms &&
               probe.chat_frequency_rows == probe.chat_frequencies &&
               probe.chat_player_rows == probe.chat_members && probe.chat_lines > 0 &&
               !probe.activity_light_running;
    };
    std::size_t entry_passes = 0;
    while (!entered() && pass < passes) {
        run();
        ++entry_passes;
    }
    std::cout << "boneyards login check: chat on " << probe.selected_area_id << ": frequency "
              << probe.chat_room_id << ", " << probe.chat_members << " members ("
              << probe.chat_player_rows << " rows), " << probe.chat_frequencies << " of " << rooms
              << " frequencies (" << probe.chat_frequency_rows << " rows), " << probe.chat_lines
              << " lines, light " << (probe.activity_light_running ? "running" : "dark")
              << " after " << entry_passes << " passes; newest: " << probe.chat_last_line << '\n';
    return entered() ? kEntryPassed : kEntryChatNotEntered;
}

// A plain line (no + or - command) typed into the chat console under a
// replay must reach Rover as one say to the whole of the entered frequency.
bool say_reached_replay(
    const by_console::MainFrameProbe& probe, const char* say, std::size_t sent_from
) {
    if (say[0] == '+' || say[0] == '-')
        return true;
    by::ReplaySent sent{};
    const bool said =
        by::rover_replay_sent(sent_from, &sent) && sent.request == by::ReplayRequest::say &&
        sent.area_id == probe.selected_area_id && sent.room_id == probe.chat_room_id &&
        sent.target_user_id == 0 && std::string(sent.text) == say;
    if (!said) {
        std::cout << "boneyards login check: \"" << say
                  << "\" did not reach the replay as a say to frequency " << probe.chat_room_id
                  << '\n';
        return false;
    }
    std::cout << "boneyards login check: the replay heard \"" << sent.text
              << "\" said to frequency " << sent.room_id << " on " << sent.area_id << '\n';
    return true;
}

// Closing the chat console under a replay must leave the frequency it had
// entered.
bool left_in_replay(uint32_t area_id, uint32_t room_id, std::size_t sent_from) {
    by::ReplaySent sent{};
    for (std::size_t i = sent_from; by::rover_replay_sent(i, &sent); ++i) {
        if (sent.request == by::ReplayRequest::leave_room && sent.area_id == area_id &&
            sent.room_id == room_id) {
            std::cout << "boneyards login check: the replay heard frequency " << room_id << " on "
                      << area_id << " left\n";
            return true;
        }
    }
    std::cout << "boneyards login check: frequency " << room_id << " on " << area_id
              << " was not left\n";
    return false;
}

bool is_contents_console(int32_t screen) {
    return screen == by_console::console_screen::help ||
           screen == by_console::console_screen::ladders ||
           screen == by_console::console_screen::news;
}

// A Help, Ladders or News console: once its contents file has arrived its
// list shows rows and its view has left the waiting page for the first
// headline's page, the activity light dark. News and Ladders pages are
// remote: the page opened must be a URL on the content server of the
// console's kind as the session names it, and under a replay the page must
// have been downloaded and each download since `fetch_from` must have
// arrived and its temporary file been deleted.
template <typename Run>
int check_contents(
    const by_console::MainFrameProbe& probe,
    const Run& run,
    std::size_t& pass,
    std::size_t passes,
    int32_t screen,
    bool replayed,
    std::size_t fetch_from
) {
    const auto loaded = [&] {
        return probe.contents_rows > 0 && !probe.contents_waiting && !probe.activity_light_running;
    };
    std::size_t load_passes = 0;
    while (!loaded() && pass < passes) {
        run();
        ++load_passes;
    }
    std::cout << "boneyards login check: contents console: " << probe.contents_rows
              << " rows, page \"" << probe.contents_page << "\" "
              << (probe.contents_waiting ? "not shown" : "shown") << " with "
              << probe.contents_images_missing << " images missing, light "
              << (probe.activity_light_running ? "running" : "dark") << " after " << load_passes
              << " passes\n";
    if (!loaded())
        return kEntryContentsNotLoaded;
    if (screen == by_console::console_screen::help)
        return kEntryPassed;
    const int32_t type = screen == by_console::console_screen::ladders ? by::kContentServerLadders
                                                                       : by::kContentServerNews;
    const by::BoneyardsService service = by::rover_session_service();
    char host[0x100] = {};
    uint16_t port = 0;
    char prefix[0x120] = {};
    if (service.content_server(service.user, type, host, sizeof host, &port))
        std::snprintf(prefix, sizeof prefix, "http://%s:%d/", host, static_cast<int>(port));
    if (prefix[0] == '\0' || std::strncmp(probe.contents_page, prefix, std::strlen(prefix)) != 0) {
        std::cout << "boneyards login check: the page is not on content server " << type << " ("
                  << (prefix[0] != '\0' ? prefix : "none named") << ")\n";
        return kEntryContentsNotLoaded;
    }
    std::size_t downloads = 0;
    bool page_fetched = false;
    by::ReplaySent sent{};
    for (std::size_t i = fetch_from; by::rover_replay_sent(i, &sent); ++i) {
        if (sent.request != by::ReplayRequest::fetch)
            continue;
        ++downloads;
        page_fetched = page_fetched || std::strcmp(sent.text, probe.contents_page) == 0;
        std::error_code error;
        if (std::filesystem::exists(sent.path, error)) {
            std::cout << "boneyards login check: " << sent.text << " left " << sent.path << '\n';
            return kEntryContentsNotLoaded;
        }
    }
    std::cout << "boneyards login check: " << downloads << " downloads arrived and were deleted\n";
    if (replayed && !page_fetched) {
        std::cout << "boneyards login check: the replay was not asked for the page\n";
        return kEntryContentsNotLoaded;
    }
    return kEntryPassed;
}

// The --snapshot file with `step` appended to its stem.
fs::path step_snapshot(const fs::path& snapshot, const char* step) {
    return snapshot.parent_path() / (snapshot.stem().string() + '-' + step + ".ppm");
}

// The file a sound name plays, as the game finds it; "" when the path does
// not fit.
std::string sound_resource(const CheckHost& host, const char* name) {
    char path[0x200] = {};
    if (!host.sound_resource(host.context, name, path, sizeof path))
        return {};
    return path;
}

} // namespace

// Pick: the pointer moves onto the planet's hotspot, which plays MORE, and
// a left press and release there plays GREEN_BUTTON, makes the planet the
// current area, enables Recon, Chat and Battle and runs the activity light
// while one census request for it is out. The light must stop once the
// census reply arrives, and a replayed reply must have set the planet's
// census fields to the replay's line. Open: a click on the console's Main
// Frame button plays its sound and GREEN_SLIDE, and the console must show
// once the green has slid in, with BIGBUTTON. Say: the line is typed into
// the chat console and sent with Enter, and a new line must reach its log;
// under a replay a plain line must have gone to Rover as a say. Escape then
// acts as Release: GREEN_SLIDE, and the map is handed back unlocked with
// Release disabled and BIGBUTTON once the green has slid out; under a
// replay the chat console's frequency must have been left. --boneyards-chat
// picks its planet the same way and goes on to check_boneyards_chat.
int check_boneyards_screens(
    const CheckHost& host,
    const Options& options,
    const std::vector<std::string>& sounds,
    std::size_t pass,
    std::size_t passes
) {
    const std::optional<int32_t> pick = boneyards_options().boneyards_chat
                                            ? boneyards_options().boneyards_chat
                                            : boneyards_options().boneyards_pick;
    if (!pick && !boneyards_options().boneyards_open)
        return kEntryPassed;
    by_console::StatusWidget* widget = boneyards_console_status_widget();
    by_console::MainFrameProbe probe{};
    const auto refresh = [&] { by_console::boneyards_status_widget_probe(widget, &probe); };
    const auto run = [&] {
        boneyards_pass(host);
        ++pass;
        refresh();
    };
    const auto mouse = [&](SDL_EventType type, int32_t x, int32_t y) {
        boneyards_pointer(host, type, x, y, 1);
    };
    const auto press_key = [&](SDL_Keycode code) { boneyards_key(host, code); };
    const auto snapshot = [&](const char* step) {
        if (!options.snapshot.empty())
            write_ppm(step_snapshot(options.snapshot, step), *host.surface(host.context));
    };
    refresh();

    if (pick) {
        const int32_t area = *pick;
        int32_t x = 0;
        int32_t y = 0;
        if (!by_console::boneyards_status_widget_planet_hotspot(widget, area, &x, &y)) {
            std::cout << "boneyards login check: planet " << area
                      << " has no hotspot in the map view\n";
            return kEntryPickMissing;
        }
        const std::size_t hover_from = sounds.size();
        mouse(SDL_EVENT_MOUSE_MOTION, x, y);
        const bool hover_sound = heard_since(sounds, hover_from, kMoreSound);
        const std::size_t press_from = sounds.size();
        const uint32_t requests = probe.census_requests;
        mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
        mouse(SDL_EVENT_MOUSE_BUTTON_UP, x, y);
        refresh();
        print_probe("planet pressed", probe);
        const auto enabled = [](int32_t state) { return state != by::button_state::disabled; };
        if (!hover_sound || !heard_since(sounds, press_from, kSelectSound) ||
            probe.selected_area_id != static_cast<uint32_t>(area) ||
            probe.selected_census.area_id != static_cast<uint32_t>(area) ||
            !enabled(probe.recon_button) || !enabled(probe.chat_button) ||
            !enabled(probe.battle_button) || !probe.activity_light_running ||
            probe.census_requests != requests + 1) {
            std::cout << "boneyards login check: planet " << area << " at " << x << ',' << y
                      << " was not selected (" << (hover_sound ? "" : "no MORE, ") << "area "
                      << probe.selected_area_id << ", " << probe.census_requests - requests
                      << " census requests)\n";
            return kEntryPickNotSelected;
        }
        std::size_t census_passes = 0;
        while (probe.activity_light_running && pass < passes) {
            run();
            ++census_passes;
        }
        print_census("census", probe.selected_census);
        if (probe.activity_light_running) {
            std::cout << "boneyards login check: no census reply for planet " << area << '\n';
            return kEntryPickNoCensus;
        }
        by::CensusReport expected{};
        if (by::rover_replay_census(static_cast<uint32_t>(area), &expected) &&
            !same_census(probe.selected_census, expected)) {
            print_census("replay census", expected);
            return kEntryPickNoCensus;
        }
        // The summary takes the census on the next draw, whatever the pass budget.
        run();
        if (!summary_reads_census(probe)) {
            std::cout << "boneyards login check: the summary does not read planet \""
                      << probe.selected_name << "\" (side " << probe.selected_side
                      << ") and its census\n";
            return kEntrySummaryNotShown;
        }
        std::cout << "boneyards login check: planet " << area << " picked at " << x << ',' << y
                  << ", its census arrived after " << census_passes << " passes\n";
        snapshot("pick");
    }
    if (boneyards_options().boneyards_chat)
        return check_boneyards_chat(host, options, sounds, pass, passes);

    if (boneyards_options().boneyards_open) {
        const auto index = static_cast<std::size_t>(*boneyards_options().boneyards_open);
        const char* name = kBoneyardsConsoleNames[index];
        const ConsoleTarget& target = kConsoleTargets[index];
        const by_console::ScreenRect button =
            probe.console_buttons[static_cast<int32_t>(target.button)];
        const std::size_t open_from = sounds.size();
        const std::size_t fetch_from = by::rover_replay_sent_count();
        const int32_t x = button.x + button.width / 2;
        const int32_t y = button.y + button.height / 2;
        mouse(SDL_EVENT_MOUSE_MOTION, x, y);
        mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
        mouse(SDL_EVENT_MOUSE_BUTTON_UP, x, y);
        refresh();
        const auto shown = [&] {
            return probe.console_screen == target.screen && probe.screen_shown &&
                   !probe.layer_sliding;
        };
        std::size_t open_passes = 0;
        bool light_ran = probe.activity_light_running;
        while (!shown() && pass < passes) {
            run();
            ++open_passes;
            light_ran = light_ran || probe.activity_light_running;
        }
        print_probe("console button pressed", probe);
        if (!shown() || !heard_since(sounds, open_from, target.sound) ||
            !heard_since(sounds, open_from, kGreenSlideSound) ||
            !heard_since(sounds, open_from, kBigButtonSound)) {
            std::cout << "boneyards login check: " << name << " at " << x << ',' << y
                      << " did not bring its console up\n";
            return kEntryOpenTimeout;
        }
        std::cout << "boneyards login check: the " << name << " console showed after "
                  << open_passes << " passes\n";
        if (target.screen == by_console::console_screen::recon) {
            const int result = check_recon(probe, run, pass, passes);
            if (result != kEntryPassed)
                return result;
        }
        if (target.screen == by_console::console_screen::chat) {
            const int result = check_chat(probe, run, pass, passes);
            if (result != kEntryPassed)
                return result;
        }
        if (is_contents_console(target.screen)) {
            const int result = check_contents(
                probe,
                run,
                pass,
                passes,
                target.screen,
                !boneyards_options().boneyards_replay.empty(),
                fetch_from
            );
            if (result != kEntryPassed)
                return result;
        }
        // The console's first record reads Profile.def, whose category
        // descriptions label the tabs.
        if (target.screen == by_console::console_screen::profile) {
            std::cout << "boneyards login check: profile tab description \""
                      << probe.profile_tab_description << "\"\n";
            if (probe.profile_tab_description[0] == '\0')
                return kEntryProfileNoDescription;
            std::size_t receive_passes = 0;
            while (probe.profile_receiving && pass < passes) {
                run();
                ++receive_passes;
                light_ran = light_ran || probe.activity_light_running;
            }
            std::cout << "boneyards login check: the player's own record, \""
                      << probe.profile_login_name << "\", arrived after " << receive_passes
                      << " passes; the activity light " << (light_ran ? "ran" : "did not run")
                      << " while it was out and is "
                      << (probe.activity_light_running ? "running" : "dark") << '\n';
            if (probe.profile_receiving || probe.profile_login_name[0] == '\0')
                return kEntryProfileNotReceived;
            if (!light_ran || probe.activity_light_running)
                return kEntryActivityLightWrong;
            std::cout << "boneyards login check: the Profile console "
                      << (probe.profile_shown ? "shows " : "hides ") << probe.profile_rows
                      << " rows, row of tab " << probe.profile_row_tab << " selected, \""
                      << probe.profile_description_line << "\" under them, status "
                      << (probe.profile_status_shown ? "up" : "down") << " (\""
                      << probe.profile_status << "\")\n";
            const std::string description = probe.profile_tab_description;
            const std::string line = probe.profile_description_line;
            if (!probe.profile_shown || probe.profile_rows != kProfileRows ||
                probe.profile_row_tab != 0 || line.empty() ||
                description.compare(0, line.size(), line) != 0 || probe.profile_status_shown)
                return kEntryProfileViewWrong;
        }
        snapshot(name);

        const bool replayed = !boneyards_options().boneyards_replay.empty();
        if (!boneyards_options().boneyards_say.empty()) {
            const char* say = boneyards_options().boneyards_say.c_str();
            const int32_t lines = probe.chat_lines;
            const std::size_t sent_from = by::rover_replay_sent_count();
            SDL_Event text{};
            text.type = SDL_EVENT_TEXT_INPUT;
            text.text.text = say;
            (void)host.dispatch(host.context, &text);
            press_key(SDLK_RETURN);
            refresh();
            std::size_t say_passes = 0;
            while (probe.chat_lines == lines && pass < passes) {
                run();
                ++say_passes;
            }
            if (probe.chat_lines == lines) {
                std::cout << "boneyards login check: \"" << say
                          << "\" brought no line to the chat log\n";
                return kEntrySayNoReply;
            }
            std::cout << "boneyards login check: \"" << say << "\" answered after " << say_passes
                      << " passes: " << probe.chat_last_line << '\n';
            if (replayed && !say_reached_replay(probe, say, sent_from))
                return kEntrySayNoReply;
            snapshot("say");
        }

        const uint32_t chat_area = probe.selected_area_id;
        const uint32_t chat_room =
            target.screen == by_console::console_screen::chat ? probe.chat_room_id : 0;
        const std::size_t leave_from = by::rover_replay_sent_count();
        const std::size_t release_from = sounds.size();
        press_key(SDLK_ESCAPE);
        refresh();
        const auto handed_back = [&] {
            return probe.console_screen == by_console::console_screen::none &&
                   !probe.layer_sliding && !probe.layer_covering && !probe.map_locked &&
                   !probe.unlock_pending && probe.release_button == by::button_state::disabled;
        };
        std::size_t release_passes = 0;
        while (!handed_back() && pass < passes) {
            run();
            ++release_passes;
        }
        print_probe("released", probe);
        if (!handed_back() || !heard_since(sounds, release_from, kGreenSlideSound) ||
            !heard_since(sounds, release_from, kBigButtonSound)) {
            std::cout << "boneyards login check: Escape did not hand the map back from " << name
                      << '\n';
            return kEntryReleaseFailed;
        }
        std::cout << "boneyards login check: Escape handed the map back after " << release_passes
                  << " passes\n";
        if (replayed && chat_room != 0 && !left_in_replay(chat_area, chat_room, leave_from))
            return kEntryReleaseFailed;
    }
    return kEntryPassed;
}

// Applies one unit limit from the Profile and reads the engine's preference
// back. The Boneyards' own UnitLimit is stored through a stand-in settings
// store. The preference file is the one the run was given.
int check_profile_unit_limit(Runtime& runtime, int32_t limit) {
    namespace screens = oa::boneyards::screens;
    static screens::ProfileConsole console;
    std::memset(static_cast<void*>(&console), 0, sizeof console);
    console.unit_limit = 0;
    console.tab_dirty[static_cast<int>(screens::ProfileTab::local_settings)] = true;
    std::snprintf(console.unit_limit_text, sizeof console.unit_limit_text, "%d", limit);
    std::snprintf(console.slide_steps_text, sizeof console.slide_steps_text, "0");

    struct Stored {
        int32_t value = 0;
    };

    static Stored stored;
    stored = {};
    console.settings.user = &stored;
    console.settings.save_number = [](void* user, const char*, int32_t value) {
        static_cast<Stored*>(user)->value = value;
        return true;
    };
    screens::profile_apply(&console);
    const auto& file = runtime_options(runtime).preferences_file;
    if (console.unit_limit != limit || stored.value != limit || !file) {
        std::cerr << "profile unit limit: APPLY did not store " << limit << "\n";
        return 1;
    }
    const auto values = oa::platform::preferences::load(*file);
    // The key the engine keeps the unit limit preference under.
    const auto found = values.find("open-annihilation.unit-limit");
    if (found == values.end() || found->second != std::to_string(limit)) {
        std::cerr << "profile unit limit: the engine preference does not hold " << limit << "\n";
        return 1;
    }
    std::cout << "profile unit limit preference: " << found->second << "\n";
    return 0;
}

// Logs into Central Command the way the Boneyards login screen does, with the
// account from OA_BY_USER / OA_BY_PASSWORD (a guest without a password; with
// --boneyards-replay and no account, the guest oa_check) and the server list
// in <game-dir>/boneyards or --boneyards-server (a list, or a host listed
// alone), pumping the session once a frame until the dossier arrives, the
// attempt ends or the time limit passes. A loaded dossier is followed by the
// galactic map; then the check logs out.
int check_boneyards_login(const CheckHost& host) {
    const auto user_value = oa::platform::environment_value("OA_BY_USER");
    const char* user = user_value ? user_value->c_str() : nullptr;
    const auto password_value = oa::platform::environment_value("OA_BY_PASSWORD");
    const char* password = password_value ? password_value->c_str() : nullptr;
    if (user == nullptr || user[0] == '\0') {
        if (boneyards_options().boneyards_replay.empty()) {
            std::cerr
                << "boneyards login check: set OA_BY_USER and OA_BY_PASSWORD to the account\n";
            return kBoneyardsNoAccount;
        }
        user = kReplayGuestName;
        password = nullptr;
    }
    by::BoneyardsService service = by::rover_session_service();
    auto screens = std::make_unique<by_screens::BoneyardsScreens>();
    by_screens::boneyards_screens_init(screens.get(), &service, by_screens::SettingsStore{});
    LoginCheckSink sink{by_screens::boneyards_screens_events(screens.get()), {}, 0};
    const by::ServiceEvents events = login_check_events(&sink);
    service.set_events(service.user, &events);

    const by::ActivationResult activation = service.activate(service.user);
    if (activation != by::ActivationResult::ok) {
        print_event("failed", activation_problem(activation));
        service.set_events(service.user, nullptr);
        return kBoneyardsLoginFailed;
    }
    const std::string server_list =
        boneyards_options().boneyards_server.empty()
            ? std::string("boneyards/serv_sav.byd in the game directory")
            : boneyards_options().boneyards_server.string();
    print_event("server list", server_list.c_str());
    const std::string server = boneyards_server_entry();
    print_event("server entry", server.empty() ? "(none listed)" : server.c_str());
    if (!boneyards_options().boneyards_replay.empty())
        print_event("replaying", boneyards_options().boneyards_replay.string().c_str());

    by_screens::boneyards_open(screens.get(), by_screens::ByScreen::login);
    by_screens::LoginDialog& dialog = screens->login;
    std::snprintf(dialog.name, sizeof dialog.name, "%s", user);
    if (password == nullptr || password[0] == '\0')
        by_screens::login_on_control(&dialog, by_screens::LoginControl::guest_radio);
    else {
        by_screens::login_on_control(&dialog, by_screens::LoginControl::member_radio);
        std::snprintf(dialog.password, sizeof dialog.password, "%s", password);
    }
    std::snprintf(dialog.server, sizeof dialog.server, "%s", server.c_str());
    by_screens::login_on_control(&dialog, by_screens::LoginControl::login_button);

    const auto login_deadline = std::chrono::steady_clock::now() + kBoneyardsLoginLimit;
    while (dialog.state == by_screens::LoginState::connecting &&
           std::chrono::steady_clock::now() < login_deadline) {
        service.pump(service.user);
        std::this_thread::sleep_for(kBoneyardsFrame);
    }
    int result = kBoneyardsLoginFailed;
    if (sink.session.level == by::SessionLevel::dossier_loaded)
        result = kBoneyardsLoggedIn;
    else if (sink.session.level == by::SessionLevel::logged_in)
        result = kBoneyardsDossierMissing;
    if (dialog.state == by_screens::LoginState::connecting && result != kBoneyardsLoggedIn)
        print_event("timed out", level_name(sink.session.level));
    if (result == kBoneyardsLoginFailed)
        print_event("failed", dialog.status);
    else
        std::cout << "boneyards login check: logged in as user id " << sink.session.user_id
                  << " in area " << sink.session.area_id << " (" << level_name(sink.session.level)
                  << ")\n";
    if (result == kBoneyardsLoggedIn)
        result = check_galactic_map(service, *screens, sink, *host.assets(host.context));

    service.close(service.user);
    const auto logout_deadline = std::chrono::steady_clock::now() + kBoneyardsLogoutLimit;
    while (sink.session.level != by::SessionLevel::offline &&
           std::chrono::steady_clock::now() < logout_deadline) {
        service.pump(service.user);
        std::this_thread::sleep_for(kBoneyardsFrame);
    }
    if (sink.session.level != by::SessionLevel::offline)
        print_event("logout", "not confirmed in time");
    service.set_events(service.user, nullptr);
    return result;
}

// Enter Boneyards goes in as the B key through the overlay; after "-y" the
// main menu's first update enters instead, which a few passes with no key
// pressed must show. The idle passes then step the Boneyards GUI one
// frontend tick apart and one frame is composed through the normal draw
// path. Without a login, a request to close the window then goes in as SDL
// reports it: the run must go on under the Boneyards box "Exit to
// Windows?", which a click on NO must close. The client module is released
// before returning; nothing logs in.
int check_boneyards_entry(const CheckHost& host, const Options& options, std::size_t ticks) {
    void* const game = host.context;
    host.hold_clock(game, host.clock(game));
    bool running = true;
    const auto send = [&](SDL_Event event) {
        if (!host.dispatch(game, &event))
            running = false;
    };
    if (boneyards_options().setup_requested) {
        std::size_t passes = 0;
        while (passes < kSetupEntryPasses && !host.frame_owned_by_package(game)) {
            boneyards_pass(host);
            ++passes;
        }
        if (host.frame_owned_by_package(game))
            std::cout << "boneyards entry check: -y entered frontend state 7 after " << passes
                      << " passes\n";
    } else {
        for (const auto type : {SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_UP}) {
            SDL_Event event{};
            event.type = type;
            event.key.key = SDLK_B;
            send(event);
        }
    }
    int result = kEntryNotEntered;
    if (host.frame_owned_by_package(game)) {
        if (boneyards_options().boneyards_login) {
            result = check_boneyards_post_login(host, options, ticks);
        } else {
            for (std::size_t pass = 0; pass < ticks; ++pass)
                boneyards_pass(host);
            std::cout << "boneyards entry check: frontend state 7 after " << ticks << " ticks\n";
            result = kEntryPassed;
            SDL_Event close{};
            close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
            send(close);
            by_console::StatusWidget* strip = boneyards_console_status_widget();
            int32_t x = 0;
            int32_t y = 0;
            if (!running || !by_console::boneyards_status_widget_message_button(
                                strip, by::message_box_name::no, &x, &y
                            )) {
                std::cerr << "boneyards entry check: closing the window did not ask "
                          << by_console::menu_message::exit_to_windows << '\n';
                result = kEntryCloseNotAsked;
            } else {
                boneyards_pointer(host, SDL_EVENT_MOUSE_MOTION, x, y, 0);
                boneyards_pointer(host, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, 1);
                boneyards_pointer(host, SDL_EVENT_MOUSE_BUTTON_UP, x, y, 1);
                boneyards_pass(host);
                if (by_console::boneyards_status_widget_message_open(strip)) {
                    std::cerr << "boneyards entry check: NO left the exit box open\n";
                    result = kEntryCloseNotAsked;
                } else {
                    std::cout << "boneyards entry check: closing the window asked "
                              << by_console::menu_message::exit_to_windows
                              << " and NO kept the Boneyards\n";
                }
            }
        }
        host.compose(game);
        if (!options.snapshot.empty())
            write_ppm(options.snapshot, *host.surface(game));
    } else {
        std::cerr << "boneyards entry check: Enter Boneyards left frontend state "
                  << static_cast<int>(host.frontend_state(game)) << '\n';
    }
    const by::BoneyardsService service = by::rover_session_service();
    service.close(service.user);
    host.release_clock(game);
    return result;
}

// The Console pulse samples the viewer drawn by the preceding frame, so each
// pass composes the frame after stepping the clock and the Boneyards GUI.
std::uint64_t boneyards_pass(const CheckHost& host) {
    host.hold_clock(host.context, host.clock(host.context) + 1);
    host.package_pass(host.context);
    const renderer::Surface& frame = *host.surface(host.context);
    if (!boneyards_options().boneyards_frames.empty()) {
        char name[0x20];
        std::snprintf(name, sizeof name, "frame-%06u.ppm", host.clock(host.context));
        write_ppm(boneyards_options().boneyards_frames / name, frame);
    }
    return frame_checksum(frame);
}

// After Enter Boneyards the passes run until the login console shows, with
// no user name saved from an earlier login, so it opens on Registered User
// with the password shown. A member types the name, Tab into the password,
// Tab, a space, Shift+Tab, the password, then Enter once the pulse has
// enabled LOGIN. A guest (no password) is selected on the dialog, which a
// player cannot do while Guest Login is grey, then the name, Tab and Enter.
// The passes then run until the dossier arrives (connection level 4), and
// that pass must close the login console, sliding the green out with
// GREEN_SLIDE. The slide must animate the frame and end with BIGBUTTON,
// the map handed back with Release, Recon, Chat and Battle disabled; the war
// map must hold its planets and leave its loading state once the allegiances
// arrive, and the next frame must show at least kDrawnPlanetsNeeded planet
// sprites at their anchors in the map panel's viewer. No chat console may
// open on the way. GREEN_SLIDE and BIGBUTTON, as heard, and MORE, which a
// planet plays under the pointer, must resolve to WAVs the install holds.
// A press on the first planet with a hotspot must then select it, and the
// next frame must paint its picture into the Main Frame and show its
// war-map name, its owner and both counts in the planet summary, with each
// side's wins only for a contested planet.
// The --boneyards-pick and --boneyards-open steps follow on that map.
int check_boneyards_post_login(const CheckHost& host, const Options& options, std::size_t passes) {
    const auto user_value = oa::platform::environment_value("OA_BY_USER");
    const char* user = user_value ? user_value->c_str() : nullptr;
    const auto password_value = oa::platform::environment_value("OA_BY_PASSWORD");
    const char* password = password_value ? password_value->c_str() : nullptr;
    if (user == nullptr || user[0] == '\0') {
        if (boneyards_options().boneyards_replay.empty()) {
            std::cerr << "boneyards login check: set OA_BY_USER and OA_BY_PASSWORD\n";
            return kEntryNoAccount;
        }
        user = kReplayGuestName;
        password = nullptr;
    }
    const bool member = password != nullptr && password[0] != '\0';
    (void)boneyards_console_write_setting("Username", "");
    by_console::StatusWidget* widget = boneyards_console_status_widget();

    struct HeardSounds {
        const CheckHost* host;
        std::vector<std::string> names;
        std::vector<std::string> resources; // the WAV each name resolved to
    } heard{&host, {}, {}};

    boneyards_console_observe_sounds(
        [](void* user, const char* name) {
            auto* sounds = static_cast<HeardSounds*>(user);
            sounds->names.emplace_back(name);
            sounds->resources.push_back(sound_resource(*sounds->host, name));
        },
        &heard
    );

    struct StopListening {
        ~StopListening() { boneyards_console_observe_sounds(nullptr, nullptr); }
    } stop_listening;

    by_console::MainFrameProbe probe{};
    std::size_t pass = 0;
    const auto run = [&] {
        const std::uint64_t checksum = boneyards_pass(host);
        ++pass;
        by_console::boneyards_status_widget_probe(widget, &probe);
        return checksum;
    };
    const auto chat_opened = [&] {
        if (probe.console_screen != by_console::console_screen::chat)
            return false;
        print_probe("chat console opened", probe);
        return true;
    };
    const auto login_shown = [&] {
        return probe.console_screen == by_console::console_screen::login && probe.screen_shown;
    };
    by_console::boneyards_status_widget_probe(widget, &probe);
    while (pass < passes && !login_shown())
        run();
    if (!login_shown()) {
        print_probe("no login console", probe);
        return kEntryNoLoginConsole;
    }
    std::cout << "boneyards login check: login console shown after " << pass << " passes\n";

    if (!member && widget != nullptr && widget->navigation != nullptr)
        by_screens::login_on_control(
            &widget->navigation->login, by_screens::LoginControl::guest_radio
        );

    const auto key = [&](SDL_EventType type, SDL_Keycode code) {
        SDL_Event event{};
        event.type = type;
        event.key.key = code;
        (void)host.dispatch(host.context, &event);
    };
    const auto press = [&](SDL_Keycode code) {
        key(SDL_EVENT_KEY_DOWN, code);
        key(SDL_EVENT_KEY_UP, code);
    };
    const auto type = [&](const char* text) {
        SDL_Event event{};
        event.type = SDL_EVENT_TEXT_INPUT;
        event.text.text = text;
        (void)host.dispatch(host.context, &event);
    };
    type(user);
    press(SDLK_TAB);
    if (member) {
        press(SDLK_TAB);
        type(" ");
        key(SDL_EVENT_KEY_DOWN, SDLK_LSHIFT);
        press(SDLK_TAB);
        key(SDL_EVENT_KEY_UP, SDLK_LSHIFT);
        type(password);
    }
    run();
    press(SDLK_RETURN);
    by_console::boneyards_status_widget_probe(widget, &probe);
    print_probe("LOGIN pressed", probe);

    bool at_level = false;
    std::size_t close_sounds = 0;
    while (pass < passes && !at_level) {
        close_sounds = heard.names.size();
        run();
        if (chat_opened())
            return kEntryChatOpened;
        at_level = probe.connection_level == kLoginLevel;
    }
    if (!at_level) {
        print_probe("no dossier", probe);
        return kEntryNoDossier;
    }
    print_probe("dossier loaded", probe);
    if (probe.console_screen != by_console::console_screen::none || !probe.layer_sliding ||
        probe.layer_direction != kSlideOut ||
        !heard_since(heard.names, close_sounds, kGreenSlideSound)) {
        std::cout << "boneyards login check: the login console did not close with GREEN_SLIDE\n";
        return kEntryLoginNotClosed;
    }

    std::uint64_t previous = frame_checksum(*host.surface(host.context));
    std::size_t slide_passes = 0;
    std::size_t changed = 0;
    while (probe.layer_sliding && pass < passes) {
        const std::uint64_t checksum = run();
        ++slide_passes;
        changed += checksum != previous ? 1U : 0U;
        previous = checksum;
        if (chat_opened())
            return kEntryChatOpened;
    }
    std::cout << "boneyards login check: the green slid out over " << slide_passes << " passes, "
              << changed << " of them changing the frame\n";
    if (changed < static_cast<std::size_t>(probe.slide_steps) / 2U)
        return kEntryStillSlide;
    print_probe("map handed back", probe);
    if (probe.layer_sliding || probe.layer_covering ||
        !heard_since(heard.names, close_sounds, kBigButtonSound) ||
        probe.release_button != by::button_state::disabled ||
        probe.recon_button != by::button_state::disabled ||
        probe.chat_button != by::button_state::disabled ||
        probe.battle_button != by::button_state::disabled) {
        std::cout << "boneyards login check: the slide did not hand the map back\n";
        return kEntryMapNotReturned;
    }

    while (probe.galaxy_loading && pass < passes) {
        run();
        if (chat_opened())
            return kEntryChatOpened;
    }
    print_probe("galactic map", probe);
    const bool planets = boneyards_options().boneyards_planets
                             ? probe.planet_count == *boneyards_options().boneyards_planets
                             : probe.planet_count != 0;
    if (!planets || probe.galaxy_loading) {
        std::cout << "boneyards login check: the galactic map did not load\n";
        return kEntryMapNotLoaded;
    }
    std::cout << "boneyards login check: on the galactic map after " << pass << " passes\n";
    run();
    std::cout << "boneyards login check: " << probe.planets_drawn << " of " << probe.planet_count
              << " planets show their sprite in the map viewer\n";
    if (probe.planets_drawn < kDrawnPlanetsNeeded)
        return kEntryMapNotDrawn;

    const oa::data::defs::Files files = asset_files(*host.assets(host.context));
    for (const char* name : {kGreenSlideSound, kBigButtonSound, kMoreSound}) {
        const auto at = std::find(heard.names.begin(), heard.names.end(), name);
        const std::string resource =
            at != heard.names.end()
                ? heard.resources[static_cast<std::size_t>(at - heard.names.begin())]
                : sound_resource(host, name);
        const bool present = files.exists(files.context, resource.c_str());
        std::cout << "boneyards login check: " << name << " plays " << resource
                  << (present ? "" : ", which the install lacks") << '\n';
        if (!present)
            return kEntrySoundMissing;
    }

    std::size_t target = 0;
    int32_t target_x = 0;
    int32_t target_y = 0;
    while (target < probe.planet_count &&
           !by_console::boneyards_status_widget_planet_point(widget, target, &target_x, &target_y))
        ++target;
    if (target == probe.planet_count) {
        std::cout << "boneyards login check: no planet on the map has a hotspot\n";
        return kEntrySummaryNotShown;
    }
    const auto pointer = [&](by::MessageCode code, std::uint32_t buttons) {
        by::Message message{};
        message.code = code;
        message.x = target_x;
        message.y = target_y;
        message.buttons = buttons;
        by_console::boneyards_status_widget_event(widget, message);
    };
    pointer(by::MessageCode::mouse_move, 0);
    pointer(by::MessageCode::left_down, by::button_left);
    pointer(by::MessageCode::left_up, 0);
    run();
    std::cout << "boneyards login check: planet " << target << " pressed at " << target_x << ','
              << target_y << ": the summary shows planet " << probe.summary_planet
              << (probe.summary_shown ? "" : " (hidden)") << " with "
              << probe.summary_picture_pixels << " picture pixels\n";
    const auto selected = static_cast<std::int32_t>(target);
    if (probe.selected_planet != selected || probe.summary_planet != selected ||
        !probe.summary_shown || probe.summary_picture_pixels == 0) {
        print_probe("planet pressed", probe);
        return kEntrySummaryNotShown;
    }
    std::cout << "boneyards login check: the summary reads \"" << probe.summary_name << "\", "
              << probe.summary_owner << ", Battles: " << probe.summary_battles
              << ", Forming: " << probe.summary_forming;
    if (probe.summary_wins_shown)
        std::cout << ", wins Arm " << probe.summary_arm_wins << " Core " << probe.summary_core_wins;
    std::cout << '\n';
    const char* owner = by::screens::planet_affiliation_label(probe.selected_side);
    const bool contested = owner != nullptr && std::strcmp(owner, "Contested") == 0;
    if (probe.selected_name[0] == '\0' ||
        std::strcmp(probe.summary_name, probe.selected_name) != 0 || owner == nullptr ||
        std::strcmp(probe.summary_owner, owner) != 0 || probe.summary_battles[0] == '\0' ||
        probe.summary_forming[0] == '\0' || probe.summary_wins_shown != contested) {
        std::cout << "boneyards login check: the summary does not read planet \""
                  << probe.selected_name << "\" (side " << probe.selected_side << ")\n";
        return kEntrySummaryNotShown;
    }
    return check_boneyards_screens(host, options, heard.names, pass, passes);
}

} // namespace oa::app

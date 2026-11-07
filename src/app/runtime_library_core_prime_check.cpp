// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-replays-core-prime. The working folder holds core-prime.facts,
// written by tools/check_native_replays_core_prime.py. The feed and the
// download keys come from the loopback server --core-prime-api names.
//
// case=play opens Replays, follows Browse Core Prime's replays, waits for
// the next page, filters by the facts' planet, downloads one replay, reads
// a release that changed, refuses a file whose digest differs, and watches
// the replay it saved.
//
// case=offline opens the same list with that server stopped.
//
// case=beside starts with Ridge 2.9 installed and chosen. The feed's replay
// needs Ridge 2.8. Watch fetches that release beside the one in use, plays
// it, and puts 2.9 back.

#include "library_host.hpp"
#include "oa_layer.hpp"
#include "oa_layer_check.hpp"

#include "oa/app/automation_host.hpp"
#include "oa/app/content/service.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/app/mod_profile_loader.hpp"
#include "oa/app/package_install/origin.hpp"
#include "oa/app/package_install/replay_release.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/replays/replays.hpp"
#include "oa/platform/files.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/replays.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace fs = std::filesystem;
namespace library = oa::ui::library;
namespace replay_data = oa::data::replays;

constexpr std::string_view kFactsName = "core-prime.facts";
constexpr int kMostFrames = 600;
constexpr uint32_t kWatchedTicks = 30;
constexpr uint64_t kListWaitMs = 45000;
constexpr uint64_t kDownloadWaitMs = 90000;
constexpr uint64_t kOfflineWaitMs = 30000;
constexpr uint64_t kRegistryWaitMs = 45000;
constexpr std::string_view kPhaseName = "core-prime.phase";
constexpr std::string_view kKeptName = "kept.txt";
constexpr std::string_view kFileDiffers = "The file does not match the replay.";
constexpr std::string_view kOfflineSentence = "Core Prime cannot be reached.";

void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("core prime check: " + std::string(what));
}

fs::path step_snapshot(const fs::path& snapshot, std::string_view step) {
    fs::path file = snapshot.parent_path() / snapshot.stem();
    file += '-' + std::string(step) + ".ppm";
    return file;
}

/// The facts file. A line is `key=value`. A blank line is skipped.
std::map<std::string, std::string> read_facts() {
    const fs::path file = fs::current_path() / std::string(kFactsName);
    std::ifstream input(file);
    require(static_cast<bool>(input), std::string(kFactsName) + " is not in the working folder");
    std::map<std::string, std::string> facts;
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        const std::size_t split = line.find('=');
        require(split != std::string::npos, "a facts line has no equals");
        facts[line.substr(0, split)] = line.substr(split + 1);
    }
    return facts;
}

std::string fact(const std::map<std::string, std::string>& facts, std::string_view key) {
    const auto found = facts.find(std::string(key));
    require(
        found != facts.end() && !found->second.empty(), std::string(key) + " is not in the facts"
    );
    return found->second;
}

/// The automation word of a planet, as the Library names a menu item.
std::string planet_word(std::string_view planet) {
    std::string word;
    for (const char character : planet) {
        const bool small = character >= 'a' && character <= 'z';
        const bool capital = character >= 'A' && character <= 'Z';
        const bool digit = character >= '0' && character <= '9';
        if (small || digit)
            word.push_back(character);
        else if (capital)
            word.push_back(static_cast<char>(character - 'A' + 'a'));
    }
    require(!word.empty(), "the planet has no automation name");
    return word;
}

fs::path saved_replay(const fs::path& user_folder, std::string_view file_name) {
    return user_folder / std::string(recordings_folder_name) /
           std::string(replay_data::core_prime_folder_name) / platform::path_from_utf8(file_name);
}

constexpr std::string_view kRidgeUnit = "[UNITINFO]\n"
                                        "{\n"
                                        "UnitName=RGNEW;\n"
                                        "MaxDamage=1000;\n"
                                        "}\n";

// The same empty model the content check's Ridge ships. The unit table keeps
// a unit whose FBI loaded with its model.
constexpr uint8_t kRidgeModel[] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
    0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/// A profile the install accepts. It changes no rule. The added unit is a file.
std::string ridge_profile(std::string_view version) {
    return "oamod: 1\n"
           "id: ridge\n"
           "name: Ridge\n"
           "version: \"" +
           std::string(version) +
           "\"\n"
           "description: A made-up mod the replays check plays.\n"
           "requires: {base: ta-3.1c, catalogue: 1}\n"
           "author: {name: unknown}\n"
           "packaging: {revision: 1, date: 2026-11-01, packager: Open Annihilation}\n";
}

void write_bytes(const fs::path& file, std::span<const uint8_t> bytes) {
    std::error_code made;
    fs::create_directories(file.parent_path(), made);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    require(static_cast<bool>(out), "cannot write " + platform::path_to_utf8(file));
}

void write_text(const fs::path& file, std::string_view text) {
    write_bytes(file, {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
}

std::string read_phase(const fs::path& user) {
    std::ifstream in(user / std::string(kPhaseName));
    std::string text;
    std::getline(in, text);
    if (!text.empty() && text.back() == '\r')
        text.pop_back();
    return text;
}

void write_phase(const fs::path& user, std::string_view phase) {
    write_text(user / std::string(kPhaseName), std::string(phase) + "\n");
}

/// The first line of a file the check wrote, without a trailing CR.
std::string read_line(const fs::path& file) {
    std::ifstream in(file);
    std::string text;
    std::getline(in, text);
    if (!text.empty() && text.back() == '\r')
        text.pop_back();
    return text;
}

/// Reports whether an installed folder's profile names `version`.
bool folder_names_version(const fs::path& folder, std::string_view version) {
    std::ifstream in(folder / "oamod.yaml");
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return text.find("version: \"" + std::string(version) + "\"") != std::string::npos;
}

/// Writes Ridge 2.9 under Mods, with the catalogue origin the facts name.
fs::path
install_ridge_29(const fs::path& user, const std::string& registry, std::string_view sha_hex) {
    const fs::path ridge = user / std::string(user_mods_folder_name) / "ridge";
    const std::vector<uint8_t> model(std::begin(kRidgeModel), std::end(kRidgeModel));
    write_text(ridge / "oamod.yaml", ridge_profile("2.9"));
    write_text(ridge / "units" / "RGNEW.fbi", kRidgeUnit);
    write_bytes(ridge / "objects3d" / "RGNEW.3DO", model);
    const std::optional<oa::base::sha256::Digest> digest = oa::base::sha256::parse_hex(sha_hex);
    require(digest.has_value(), "Ridge 2.9's digest is not hex");
    package_install::Origin record;
    record.kind = package_install::OriginKind::catalogue;
    record.registry = registry;
    record.catalogue_id = "ridge";
    record.release = 3;
    record.sha256 = *digest;
    record.installed = "2026-11-01";
    const std::string origin_record = package_install::origin_text(record);
    require(!origin_record.empty(), "Ridge 2.9's origin was not written");
    write_text(ridge / std::string(package_install::origin_file_name), origin_record);
    write_text(user / std::string(kKeptName), "2.9\n");
    return ridge;
}

} // namespace

void Runtime::check_replays_core_prime() {
    require(sdl_.window != nullptr, "the check needs the SDL presenter");
    require(!user_folder_.empty(), "the run has no folder of the player's own");
    const std::map<std::string, std::string> facts = read_facts();
    const std::string which = fact(facts, "case");
    require(
        which == "play" || which == "offline" || which == "backoff" || which == "rate" ||
            which == "challenge" || which == "wait" || which == "restart" || which == "beside",
        "the facts name no case this check runs"
    );

    bool running = true;
    LibraryHost& host = library_host();
    const auto frames = [&](int count) {
        for (int frame = 0; frame < count; ++frame) {
            run_frame(running);
            require(running, "a frame ended the run");
        }
    };
    const auto until = [&](uint64_t budget_ms, const auto& done, std::string_view what) {
        const uint64_t began = SDL_GetTicks();
        while (SDL_GetTicks() - began < budget_ms) {
            run_frame(running);
            require(running, "a frame ended the run");
            if (done())
                return;
        }
        throw std::runtime_error("core prime check: " + std::string(what));
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
    const auto click_named = [&](std::string_view name) {
        const std::optional<AutomationControl> control = named(name);
        require(control.has_value(), std::string(name) + " is not shown");
        require(control->enabled, std::string(name) + " is not enabled");
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
            require(going, "a click ended the run");
        }
        run_frame(running);
        require(running, "a frame ended the run");
    };
    const auto snapshot = [this](const std::string& step) {
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
    const auto row_of = [&](std::string_view path) {
        const std::optional<std::size_t> place = library::replay_place(host.model, path);
        require(place.has_value(), std::string(path) + " is not listed");
        host.state.reveal_row = static_cast<int32_t>(*place);
        ++host.model_revision;
        frames(1);
        return "oa.library.row.replay." + std::to_string(*place + 1);
    };
    const auto open_replays = [&] {
        load(Screen::main_menu);
        require(screen_ == Screen::main_menu, "the main menu did not open");
        require(
            SDL_SetWindowSize(sdl_.window, 1280, 720) && SDL_SyncWindow(sdl_.window),
            "the window did not take 1280x720"
        );
        apply_output_mode();
        frames(2);
        open_library(library::Tab::replays);
        for (int frame = 0; frame < kMostFrames && host.replays_reading(); ++frame) {
            run_frame(running);
            require(running, "a frame ended the run");
        }
        frames(2);
        require(!host.replays_reading(), "the Recordings folder was not read");
    };
    const auto core_rows = [&]() -> const std::vector<library::ReplayRow>& {
        return host.model.inputs.replays.core_rows;
    };
    const auto download_finished =
        [&](const std::string& id, const fs::path& file, bool want_file) {
            if (host.model.inputs.replays.core_downloading)
                return false;
            std::error_code missing;
            const bool there = fs::is_regular_file(file, missing);
            if (want_file)
                return there;
            const library::ReplayRow* row = library::find_replay(host.model, id);
            if (row == nullptr || there)
                return false;
            return library::replay_note(host.model, *row).find(kFileDiffers) != std::string::npos;
        };

    // Ridge 2.9 is installed. Watch fetches 2.8 beside it, plays, and puts
    // 2.9 back. A restart must not open the Library before that phase runs.
    if (which == "beside") {
        const auto version_is = [&](std::string_view version) {
            return mod_profile() != nullptr && mod_profile()->version == version;
        };
        const auto play_through = [&] {
            require(
                call_hook_or_raise<&Extension::start_scene>(extension_, *this),
                "the replay did not start"
            );
            until(
                kDownloadWaitMs,
                [&] { return screen_ == Screen::match && match_ != nullptr; },
                "the replay did not start"
            );
            constexpr int kReplayTickBudget = 8000;
            int replay_ticks = 0;
            while (screen_ != Screen::campaign_end && replay_ticks < kReplayTickBudget) {
                if (match_ != nullptr && !match_finished_) {
                    const GrantedTicks grant = run_granted_ticks(30);
                    replay_ticks += grant.ran > 0 ? static_cast<int>(grant.ran) : 1;
                } else {
                    ++replay_ticks;
                }
                run_frame(running);
                require(running, "a frame ended the run");
            }
            require(screen_ == Screen::campaign_end, "the replay did not reach its end screen");
            require(!soft_restart_requested(), "the end screen did not stay until Main Menu");
            frames(4);
            require(
                screen_ == Screen::campaign_end && !soft_restart_requested(),
                "the end screen did not stay until Main Menu"
            );
            require(step_end_screen_to_panel().panel, "the end screen reached no panel");
            frames(2);
            require(
                screen_ == Screen::campaign_end && !soft_restart_requested(),
                "the end screen did not stay until Main Menu"
            );
            const auto* end_menu = widget("MainMenu");
            require(end_menu != nullptr && end_menu->common.active != 0, "Main Menu is not shown");
            click_end_panel(static_cast<std::size_t>(end_menu - resources_.layout.gadgets.data()));
            require(screen_ == Screen::main_menu, "Main Menu did not leave the end screen");
            std::cout << "core prime: end screen stayed until Main Menu\n";
        };
        const auto next_start_idle = [&] {
            require(
                !call_hook_or_raise<&Extension::start_scene>(extension_, *this),
                "the next start played the replay again"
            );
            require(match_ == nullptr, "the next start opened a match");
            require(screen_ != Screen::match, "the next start opened a match");
            std::cout << "core prime: the next start played nothing\n";
        };
        if (options_.restarts > 0) {
            const std::string phase = read_phase(user_folder_);
            const fs::path mods = user_folder_ / std::string(user_mods_folder_name);
            const fs::path installed_ridge = mods / "ridge";
            const std::string watch_id = fact(facts, "watch_id");
            if (phase == "installed") {
                require(version_is("2.9"), "Ridge 2.9 is not chosen");
                const std::string registry_id = fact(facts, "registry");
                until(
                    kRegistryWaitMs,
                    [&] {
                        const std::shared_ptr<const content::Snapshot> snapshot_now =
                            content_service().snapshot();
                        if (!snapshot_now)
                            return false;
                        for (const content::RegistryView& registry_view :
                             snapshot_now->registries) {
                            if (registry_view.registry.descriptor.id != registry_id)
                                continue;
                            return registry_view.releases != nullptr && !registry_view.refreshing;
                        }
                        return false;
                    },
                    "the releases list was not held"
                );
                require(
                    read_line(user_folder_ / std::string(kKeptName)) == "2.9",
                    "the 2.9 marker is gone"
                );
                const std::vector<fs::path> listed = list_mods_in(mods);
                require(listed.size() == 1, "Mods lists something besides Ridge");
                for (const fs::directory_entry& entry : fs::directory_iterator(mods)) {
                    require(
                        platform::path_to_utf8(entry.path().filename()) !=
                            package_install::replay_releases_folder_name,
                        "Replay releases is under Mods"
                    );
                }
                open_replays();
                require(
                    named("oa.library.action.browse-core-prime").has_value(), "Browse is not shown"
                );
                click_named("oa.library.action.browse-core-prime");
                until(
                    kListWaitMs,
                    [&] {
                        return core_rows().size() >= 48 && !host.model.inputs.replays.core_loading;
                    },
                    "the first page did not fill"
                );
                click_named(row_of(watch_id));
                std::string chip;
                std::string note;
                std::string button;
                const uint64_t began = SDL_GetTicks();
                bool ready = false;
                while (SDL_GetTicks() - began < kDownloadWaitMs) {
                    run_frame(running);
                    require(running, "a frame ended the run");
                    const library::ReplayRow* row = library::find_replay(host.model, watch_id);
                    if (row == nullptr)
                        continue;
                    chip = library::replay_need_chip(host.model, *row);
                    note = library::replay_note(host.model, *row);
                    if (const std::optional<AutomationControl> watch =
                            named("oa.library.action.watch"))
                        button = watch->text;
                    else
                        button.clear();
                    if (row->replay.needs_read && chip == "Needs Ridge 2.8" &&
                        note.find("You have 2.9") != std::string::npos &&
                        button == "Get Ridge 2.8 and watch") {
                        ready = true;
                        break;
                    }
                }
                std::cout << "core prime: chip \"" << chip << "\" note \"" << note << "\" button \""
                          << button << "\"\n";
                require(ready, "the card did not offer Ridge 2.8");
                write_phase(user_folder_, "watching");
                click_named("oa.library.action.watch");
                until(
                    kDownloadWaitMs,
                    [&] { return soft_restart_requested(); },
                    "Get Ridge 2.8 and watch did not restart"
                );
                return;
            }
            if (phase == "watching") {
                require(version_is("2.8"), "Ridge 2.8 is not playing");
                const std::string layered = platform::path_to_utf8(options_.layered_mod_folder);
                require(
                    layered.find(package_install::replay_releases_folder_name) != std::string::npos,
                    "Ridge 2.8 is not in Replay releases"
                );
                std::error_code same;
                require(
                    !same && !fs::equivalent(options_.layered_mod_folder, installed_ridge, same),
                    "the replay is using the installed Ridge"
                );
                require(
                    read_line(user_folder_ / std::string(kKeptName)) == "2.9",
                    "the 2.9 marker was changed"
                );
                require(
                    folder_names_version(installed_ridge, "2.9"),
                    "the installed Ridge is no longer 2.9"
                );
                write_phase(user_folder_, "back");
                play_through();
                std::cout << "core prime: ridge 2.8 playing\n";
                until(
                    kDownloadWaitMs,
                    [&] { return soft_restart_requested(); },
                    "Ridge 2.9 was not put back"
                );
                return;
            }
            if (phase == "back") {
                next_start_idle();
                require(version_is("2.9"), "Ridge 2.9 was not chosen after the replay");
                require(
                    read_line(user_folder_ / std::string(kKeptName)) == "2.9",
                    "the 2.9 marker was changed"
                );
                std::cout << "core prime: 2.9 chosen\n";
                return;
            }
            throw std::runtime_error("core prime check: the beside phase is unknown");
        }
        const std::string descriptor = fact(facts, "descriptor");
        const std::string registry_id = fact(facts, "registry");
        const uint64_t request = content_service().check_registry_url(descriptor);
        std::optional<content::RegistryCheck> checked;
        until(
            kRegistryWaitMs,
            [&] {
                checked = content_service().registry_check(request);
                return checked.has_value();
            },
            "the registry was not checked"
        );
        require(
            checked->refusal == data::registry::Refusal::none,
            checked->detail.empty() ? "the registry was refused" : checked->detail
        );
        std::string why;
        const data::registry::Refusal added = content_service().add_checked_registry(request, &why);
        require(
            added == data::registry::Refusal::none, why.empty() ? "the registry was not added" : why
        );
        content_service().refresh(content::RefreshReason::check_now, registry_id);
        until(
            kRegistryWaitMs,
            [&] {
                const std::shared_ptr<const content::Snapshot> snapshot_now =
                    content_service().snapshot();
                if (!snapshot_now)
                    return false;
                for (const content::RegistryView& registry_view : snapshot_now->registries) {
                    if (registry_view.registry.descriptor.id != registry_id)
                        continue;
                    return registry_view.releases != nullptr && !registry_view.refreshing;
                }
                return false;
            },
            "the releases list was not fetched"
        );
        const fs::path ridge =
            install_ridge_29(user_folder_, registry_id, fact(facts, "ridge_new_sha"));
        remember_mod_directory(preference_values_, ridge);
        preferences_dirty_ = true;
        flush_preferences();
        write_phase(user_folder_, "installed");
        request_soft_restart();
        std::cout << "core prime: Ridge 2.9 installed\n";
        return;
    }

    open_replays();
    require(
        named("oa.library.replays.source.computer").has_value(), "On this computer is not shown"
    );
    require(
        named("oa.library.replays.source.core-prime").has_value(), "On Core Prime is not shown"
    );

    if (which == "offline") {
        click_named("oa.library.replays.source.core-prime");
        until(
            kOfflineWaitMs,
            [&] {
                const std::optional<AutomationControl> status = named("oa.library.status");
                return host.model.inputs.replays.core_offline && status.has_value() &&
                       status->text.find(kOfflineSentence) != std::string::npos;
            },
            "Core Prime did not read as offline"
        );
        const std::optional<AutomationControl> status = named("oa.library.status");
        require(status.has_value(), "the offline sentence is not shown");
        std::cout << "core prime: " << status->text << '\n';
        snapshot("core-offline");
        return;
    }

    if (which == "backoff") {
        click_named("oa.library.replays.source.core-prime");
        until(
            kListWaitMs,
            [&] { return host.model.inputs.replays.core_unavailable; },
            "a 503 did not read as unavailable"
        );
        const uint64_t began = SDL_GetTicks();
        while (SDL_GetTicks() - began < 10000) {
            run_frame(running);
            require(running, "a frame ended the run");
        }
        std::cout << "core prime: backed off\n";
        return;
    }

    const std::string watch_id = fact(facts, "watch_id");
    const fs::path watch_file = saved_replay(user_folder_, fact(facts, "watch_file"));
    const auto open_core = [&] {
        require(named("oa.library.action.browse-core-prime").has_value(), "Browse is not shown");
        click_named("oa.library.action.browse-core-prime");
        until(
            kListWaitMs,
            [&] { return core_rows().size() >= 48 && !host.model.inputs.replays.core_loading; },
            "the first page did not fill"
        );
    };

    if (which == "rate" || which == "challenge") {
        open_core();
        const std::string needle =
            which == "challenge" ? std::string("R13CODE") : std::string("Try again in");
        bool saw = false;
        click_named(row_of(watch_id));
        click_named("oa.library.action.download");
        until(
            kDownloadWaitMs,
            [&] {
                const library::ReplayRow* row = library::find_replay(host.model, watch_id);
                if (row != nullptr &&
                    library::replay_note(host.model, *row).find(needle) != std::string::npos)
                    saw = true;
                return download_finished(watch_id, watch_file, true);
            },
            which + " did not save the replay"
        );
        require(saw, which + " did not show on the card");
        require(fs::is_regular_file(watch_file), which + " left no file");
        std::cout << "core prime: " << which << " saved\n";
        return;
    }

    if (which == "wait") {
        const std::string other_id = fact(facts, "other_id");
        open_core();
        click_named(row_of(watch_id));
        click_named("oa.library.action.download");
        until(
            kDownloadWaitMs,
            [&] {
                const library::ReplayRow* row = library::find_replay(host.model, watch_id);
                return row != nullptr &&
                       library::replay_note(host.model, *row).find("Try again in") !=
                           std::string::npos;
            },
            "the wait did not show"
        );
        click_named(row_of(other_id));
        until(
            2000,
            [&] {
                const library::ReplayRow* row = library::find_replay(host.model, other_id);
                return row != nullptr && row->replay.needs_read;
            },
            "the other card was not asked for"
        );
        std::cout << "core prime: other card\n";
        click_named(row_of(watch_id));
        until(
            kListWaitMs,
            [&] {
                const std::optional<AutomationControl> button = named("oa.library.action.download");
                return button.has_value() && button->enabled && button->text == "Cancel";
            },
            "the waiting replay's button did not read Cancel"
        );
        click_named("oa.library.action.download");
        until(
            kListWaitMs,
            [&] {
                const std::optional<AutomationControl> button = named("oa.library.action.download");
                return !host.model.inputs.replays.core_downloading && button.has_value() &&
                       button->enabled && button->text.find("Downloaded") == std::string::npos &&
                       button->text.find("Download") != std::string::npos;
            },
            "Cancel did not stop the wait"
        );
        const uint64_t settle = SDL_GetTicks();
        while (SDL_GetTicks() - settle < 2000) {
            run_frame(running);
            require(running, "a frame ended the run");
        }
        std::cout << "core prime: cancelled\n";
        return;
    }

    if (which == "restart") {
        // The folder already holds the replay, so Browse is not offered.
        click_named("oa.library.replays.source.core-prime");
        until(
            kListWaitMs,
            [&] {
                return !host.model.inputs.replays.core_loading &&
                       library::find_replay(host.model, watch_id) != nullptr;
            },
            "the saved replay is not listed"
        );
        click_named(row_of(watch_id));
        until(
            kListWaitMs,
            [&] {
                const std::optional<AutomationControl> button = named("oa.library.action.download");
                return button.has_value() && !button->enabled &&
                       button->text.find("Downloaded") != std::string::npos;
            },
            "the saved replay did not read Downloaded"
        );
        std::cout << "core prime: still downloaded\n";
        return;
    }

    const std::string mismatch_id = fact(facts, "mismatch_id");
    const std::string bad_id = fact(facts, "bad_id");
    const std::string planet = fact(facts, "planet");
    const std::string sector = fact(facts, "sector");
    const std::string kind = fact(facts, "kind");
    const fs::path mismatch_file = saved_replay(user_folder_, fact(facts, "mismatch_file"));
    const fs::path bad_file = saved_replay(user_folder_, fact(facts, "bad_file"));
    const std::string planet_item = "oa.library.replays.planet." + planet_word(planet);
    const std::string other = fact(facts, "other_planet");
    const std::string other_item = "oa.library.replays.planet." + planet_word(other);

    require(named("oa.library.action.browse-core-prime").has_value(), "Browse is not shown");
    snapshot("computer-empty");
    click_named("oa.library.action.browse-core-prime");
    until(
        kListWaitMs,
        [&] { return core_rows().size() >= 48 && !host.model.inputs.replays.core_loading; },
        "the first page did not fill"
    );
    host.state.list_scroll = 1000000;
    ++host.model_revision;
    until(
        kListWaitMs,
        [&] { return core_rows().size() > 48 && !host.model.inputs.replays.core_loading; },
        "the next page did not arrive"
    );
    const library::ReplayRow* watched = library::find_replay(host.model, watch_id);
    require(watched != nullptr, "the replay to watch is not listed");
    require(!watched->standings.empty(), "the replay names no player");
    require(
        watched->standings.front().result == library::RecordedResult::won,
        "the first player did not win"
    );
    require(
        watched->standings.front().known && watched->standings.front().kills_only &&
            watched->standings.front().kills == 41,
        "the first player's kills are not the feed's"
    );
    require(!watched->indexing, "a Core Prime row started a pass");
    require(watched->recorded == library::RecordedResult::none, "the row itself names a result");
    std::cout << "core prime: feed result\n";
    require(watched->replay.planet == planet, "the replay's planet is " + watched->replay.planet);
    require(watched->replay.sector == sector, "the replay's sector is " + watched->replay.sector);
    require(watched->replay.kind == kind, "the replay's kind is " + watched->replay.kind);
    std::cout << "core prime: page 2\n";
    snapshot("core-list");
    frames(2);
    const std::size_t held_rows = core_rows().size();
    const int32_t held_scroll = host.state.list_scroll;
    const uint64_t held_began = SDL_GetTicks();
    while (SDL_GetTicks() - held_began < 61000) {
        run_frame(running);
        require(running, "a frame ended the run");
    }
    require(core_rows().size() == held_rows, "the minute replaced the rows");
    require(host.state.list_scroll == held_scroll, "the minute moved the scroll");
    std::cout << "core prime: pages held\n";

    click_named("oa.library.replays.filter.planet");
    until(
        kListWaitMs,
        [&] { return named("oa.library.replays.planet").has_value(); },
        "the planet field did not show"
    );
    click_named("oa.library.replays.planet");
    until(
        kListWaitMs, [&] { return named(planet_item).has_value(); }, "the planet is not in the menu"
    );
    click_named(planet_item);
    until(
        kListWaitMs,
        [&] {
            if (host.model.inputs.replays.core_loading || core_rows().empty())
                return false;
            for (const library::ReplayRow& row : core_rows()) {
                if (row.replay.planet != planet)
                    return false;
            }
            return true;
        },
        "the planet did not narrow the list"
    );
    std::cout << "core prime: planet " << planet << '\n';
    snapshot("core-planet");
    click_named("oa.library.replays.planet");
    until(
        kListWaitMs, [&] { return named(other_item).has_value(); }, "the other planet left the menu"
    );
    click_named(other_item);
    until(
        kListWaitMs,
        [&] {
            if (host.model.inputs.replays.core_loading || core_rows().empty())
                return false;
            for (const library::ReplayRow& row : core_rows()) {
                if (row.replay.planet != other)
                    return false;
            }
            return true;
        },
        "the other planet did not narrow the list"
    );
    std::cout << "core prime: planet " << other << '\n';
    click_named("oa.library.replays.filter.latest");
    until(
        kListWaitMs,
        [&] {
            return !host.model.inputs.replays.core_loading &&
                   library::find_replay(host.model, watch_id) != nullptr;
        },
        "Latest did not list the replay"
    );

    const auto fetch_one =
        [&](const std::string& id, const fs::path& file, bool want_file, std::string_view waiting) {
            click_named(row_of(id));
            until(
                kListWaitMs,
                [&] {
                    const std::optional<AutomationControl> button =
                        named("oa.library.action.download");
                    return button.has_value() && button->enabled;
                },
                std::string(id) + " offered no Download"
            );
            click_named("oa.library.action.download");
            until(kDownloadWaitMs, [&] { return download_finished(id, file, want_file); }, waiting);
        };

    fetch_one(watch_id, watch_file, true, "the replay was not saved");
    require(fs::is_regular_file(watch_file), "the saved replay is not in Recordings");
    const std::optional<AutomationControl> saved_button = named("oa.library.action.download");
    require(
        saved_button.has_value() && !saved_button->enabled &&
            saved_button->text.find("Downloaded") != std::string::npos,
        "the card did not say the replay was saved"
    );
    std::cout << "core prime: downloaded\n";
    click_named("oa.library.replays.source.computer");
    const std::string saved_path =
        std::string(replay_data::core_prime_folder_name) + "/" + fact(facts, "watch_file");
    until(
        kDownloadWaitMs,
        [&] {
            return !host.replays_reading() &&
                   library::find_replay(host.model, saved_path) != nullptr;
        },
        "the saved replay is not on this computer"
    );
    std::cout << "core prime: on this computer\n";
    click_named("oa.library.replays.source.core-prime");
    until(
        kListWaitMs,
        [&] {
            return !host.model.inputs.replays.core_loading &&
                   library::find_replay(host.model, watch_id) != nullptr;
        },
        "Core Prime did not list the replay again"
    );

    fetch_one(mismatch_id, mismatch_file, true, "the changed release was not saved");
    require(fs::is_regular_file(mismatch_file), "the changed release is not in Recordings");
    std::cout << "core prime: changed release read again\n";

    fetch_one(bad_id, bad_file, false, "a file that differs was not refused");
    require(!fs::exists(bad_file), "a file that differs was kept");
    require(
        library::find_replay(host.model, bad_id) != nullptr, "the refused replay left the list"
    );
    std::cout << "core prime: file differs\n";

    click_named(row_of(watch_id));
    click_named("oa.library.action.watch");
    until(
        kDownloadWaitMs,
        [&] { return screen_ == Screen::match && match_ != nullptr; },
        "Watch did not start the replay"
    );
    until(
        kDownloadWaitMs,
        [&] { return oa_layer().find(library_screen_name) == nullptr; },
        "the Library stayed over the replay"
    );
    const uint32_t first_tick = match_->state().game.tick;
    for (int frame = 0; frame < kMostFrames && match_ != nullptr &&
                        match_->state().game.tick < first_tick + kWatchedTicks;
         ++frame) {
        static_cast<void>(run_granted_ticks(1));
        run_frame(running);
        require(running, "a frame ended the run");
    }
    require(
        match_ != nullptr && match_->state().game.tick >= first_tick + kWatchedTicks,
        "the replay did not play its ticks"
    );
    std::cout << "core prime: watching\n";
    leave_match();
    load(Screen::main_menu);
}

} // namespace oa::app

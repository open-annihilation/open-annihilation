// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-update-download: a package served on 127.0.0.1, then the ready
// prompt and the recorded hand-off. Keep it ready, Escape's answer, schedules
// nothing and a quit then installs nothing. The key is made from a fixed
// seed. The published feed is not requested, and the helper is not started.

#include "oa/app/runtime.hpp"

#include "oa_layer.hpp"
#include "oa_layer_check.hpp"
#include "update_state.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/update/build.hpp"
#include "oa/app/update/centre.hpp"
#include "oa/app/update/choices.hpp"
#include "oa/app/update/receipt.hpp"
#include "oa/formats/png.hpp"
#include "window_placement.hpp"
#include "oa/base/sha256.hpp"
#include "oa/base/signing/ed25519.hpp"
#include "oa/data/catalogue/check.hpp"
#include "oa/netgame/http_fixture/server.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/first_run/first_run.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/kit/components_more.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace signing = oa::base::signing;
namespace sha = oa::base::sha256;
namespace catalogue = oa::data::catalogue;
namespace fixture = oa::netgame::http_fixture;
namespace dialogs = oa::ui::frontend_dialogs;
namespace kit = oa::ui::kit;
namespace layout = oa::ui::display_layout;
namespace settings = oa::ui::engine_settings;
namespace feed = oa::data::engine_feed;

namespace {

constexpr std::string_view kKeyId = "update-download-check";
constexpr std::string_view kOfferedVersion = "9.9.9";
/// The second answer waits this long, so a skirmish can start before its body.
constexpr uint32_t kHeadDelayMs = 18000;

/// Throws when a step of the check fails.
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("update-download: " + std::string(what));
}

/// Bytes with a pattern a wrong file cannot share by accident.
std::vector<uint8_t> patterned(std::size_t size, uint8_t seed) {
    std::vector<uint8_t> bytes(size);
    for (std::size_t index = 0; index < size; ++index)
        bytes[index] = static_cast<uint8_t>(seed + static_cast<uint8_t>(index * 17U));
    return bytes;
}

/// The hex SHA-256 of `bytes`.
std::string sha256_hex(const std::vector<uint8_t>& bytes) {
    const auto hex = sha::to_hex(sha::digest_of(bytes));
    return std::string(hex.begin(), hex.end());
}

/// One release of the stable feed. `mirror` ends in a slash.
std::string feed_json(
    int64_t release,
    std::string_view version,
    std::string_view package,
    const std::string& digest,
    uint64_t size,
    std::string_view mirror
) {
    std::string json = R"({"feed":1,"channel":"stable","releases":[{"release":)";
    json += std::to_string(release);
    json += R"(,"version":")";
    json += version;
    json +=
        R"(","date":"2026-10-25","from":"0.0.1","notes":{"en":["Wider bars"]},"page":"https://example.invalid/releases/)";
    json += version;
    json += R"(","packages":{")";
    json += package;
    json += R"(":{"file":"engine.zip","size":)";
    json += std::to_string(size);
    json += R"(,"sha256":")";
    json += digest;
    json += R"(","mirrors":[")";
    json += mirror;
    json += R"("]}}}]})";
    return json;
}

/// Signs `json` with a key derived from `fill`. The seed and the secret are wiped.
struct SignedFeed {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> signature;
    std::string key_text;
};

SignedFeed sign_feed(std::string_view json, uint8_t fill) {
    std::array<uint8_t, 32> seed{};
    seed.fill(fill);
    signing::SecretKey secret{};
    signing::PublicKey public_key{};
    signing::key_pair_from_seed(seed, secret, public_key);
    seed.fill(0);
    SignedFeed signed_feed;
    signed_feed.bytes.assign(json.begin(), json.end());
    const signing::Signature signature = signing::sign(secret, signed_feed.bytes);
    secret.fill(0);
    const std::string text =
        catalogue::signature_file_text(catalogue::SignatureLine{std::string(kKeyId), signature});
    signed_feed.signature.assign(text.begin(), text.end());
    signed_feed.key_text = std::string(signing::public_key_text(public_key).view());
    return signed_feed;
}

/// This program's build, using the same folder the check will.
update::ThisBuild running_build() {
    const char* const base = SDL_GetBasePath();
    if (base == nullptr)
        return update::this_build({});
    return update::this_build(path_from_utf8(base));
}

/// The update question, named by its Skip button.
bool is_update_question(const LayerScreen* screen) {
    if (screen == nullptr || screen->name() != QuestionScreen::screen_name)
        return false;
    const kit::Question& asked = static_cast<const QuestionScreen*>(screen)->question();
    return !asked.buttons.empty() && asked.buttons.front().id == "skip";
}

/// A request for the package file.
bool is_zip(const fixture::LoggedRequest& request) {
    return request.target.find("engine.zip") != std::string::npos;
}

/// How many package requests the server has logged.
int zip_count(const fixture::Server& server) {
    int count = 0;
    for (const fixture::LoggedRequest& request : server.requests())
        if (is_zip(request))
            ++count;
    return count;
}

/// True once a package request asked for a range.
bool zip_ranged(const fixture::Server& server) {
    for (const fixture::LoggedRequest& request : server.requests()) {
        if (!is_zip(request))
            continue;
        const std::string* header = request.header("Range");
        if (header != nullptr && header->find("bytes=") == 0)
            return true;
    }
    return false;
}

/// One answer of a range, named by the first byte it sends.
fixture::Reply partial_reply(const std::vector<uint8_t>& file, std::size_t first) {
    fixture::Reply reply;
    reply.status = 206;
    reply.head_delay_ms = kHeadDelayMs;
    reply.body.assign(file.begin() + static_cast<std::ptrdiff_t>(first), file.end());
    const std::string last = std::to_string(file.size() - 1);
    reply.headers.push_back(
        {"Content-Range",
         "bytes " + std::to_string(first) + "-" + last + "/" + std::to_string(file.size())}
    );
    return reply;
}

/// Writes a frame as a PNG file in the working folder.
///
/// @param file the file
/// @param frame the frame
void write_picture(const fs::path& file, const renderer::Surface& frame) {
    std::vector<uint8_t> bytes;
    const oa::formats::png::Header header{
        frame.width, frame.height, 8, oa::formats::png::ColorType::rgb, {}
    };
    require(
        oa::formats::png::write(oa::formats::png::Image{header, {}, frame.rgb}, &bytes),
        "could not encode " + path_to_utf8(file)
    );
    std::ofstream out(file, std::ios::binary);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    require(static_cast<bool>(out), "could not write " + path_to_utf8(file));
}

} // namespace

int Runtime::check_update_download() {
    require(options_.data_dir.has_value(), "the run has no --data-dir");
    // A feed and a package an earlier run of this case stored would be found
    // again at the first menu; this run starts with none.
    {
        std::error_code cleared;
        fs::remove_all(*options_.data_dir / "updates", cleared);
    }
    preference_values_.erase(std::string(settings::key::update_skipped));
    preference_values_.erase(std::string(settings::key::update_checked));
    preference_values_[std::string(settings::key::language)] = "en";
    preference_values_[std::string(oa::ui::first_run::preference_key)] =
        std::string(oa::ui::first_run::preference_done);
    if (options_.update_download_case == "background")
        preference_values_[std::string(settings::key::update_download)] = "1";
    preferences_dirty_ = true;
    flush_preferences();
    if (shown_language().tag != "en")
        set_language_choice("en");
    if (options_.update_install_folder.has_value())
        fs::create_directories(*options_.update_install_folder);

    const bool notice_only = options_.updated_from.has_value() &&
                             options_.update_download_case.empty() && !options_.update_and_quit;
    bool running = true;
    const auto deadline_from = [](std::chrono::seconds limit) {
        return std::chrono::steady_clock::now() + limit;
    };
    const auto clear_blockers = [this]() {
        dialogs::reset_dialogs();
        OaLayer& layer = oa_layer();
        for (int pass = 0; pass < 8; ++pass) {
            LayerScreen* top = layer.top();
            if (top == nullptr || is_update_question(top) || top->name() == "settings" ||
                top->name() == "ready-update" || top->name() == "updated-notice")
                return;
            layer.close(top);
        }
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
        const auto deadline = deadline_from(std::chrono::seconds{10});
        std::optional<AutomationControl> control = named(name);
        while (!control) {
            require(
                std::chrono::steady_clock::now() < deadline, std::string(name) + " is not shown"
            );
            clear_blockers();
            idle_tick();
            control = named(name);
        }
        const layout::Point pixel{
            control->window_x + control->window_width / 2,
            control->window_y + control->window_height / 2
        };
        const auto pointer = [&](SDL_EventType type, uint8_t button) {
            SDL_Event event = window_pointer_event(sdl_.window, sdl_.renderer, type, pixel, button);
            dispatch_event(event, running);
            require(running, "a pointer event ended the run");
        };
        pointer(SDL_EVENT_MOUSE_MOTION, 0);
        pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
        pointer(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
        idle_tick();
    };
    const auto press_escape = [&]() {
        SDL_Event event{};
        event.key.windowID = SDL_GetWindowID(sdl_.window);
        event.key.key = SDLK_ESCAPE;
        event.key.scancode = SDL_GetScancodeFromKey(SDLK_ESCAPE, nullptr);
        for (const bool down : {true, false}) {
            event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.down = down;
            dispatch_event(event, running);
            require(running, "Escape ended the run");
        }
        idle_tick();
    };
    const auto say = [](std::string_view line) {
        std::cout << "update-download: " << line << '\n';
        std::cout.flush();
    };

    if (options_.update_download_case == "pictures") {
        // Each update screen in each state, at Compact, Regular and Large:
        // the update centre reads made-up states, and nothing is fetched.
        // The pictures are written to the working folder as
        // update-<screen>-<state>-<width>x<height>.png. The check is asked
        // for only, so the named feed is never fetched.
        update::write_check_mode(preference_values_, update::CheckMode::when_asked);
        preferences_dirty_ = true;
        flush_preferences();

        struct PictureWindow {
            int32_t width{};                     ///< the window's width, in pixels
            int32_t height{};                    ///< its height
            window_placement::WindowSize size{}; ///< the Window size it is drawn at
        };

        constexpr std::array<PictureWindow, 3> windows{{
            {640, 480, window_placement::WindowSize::compact},
            {1280, 720, window_placement::WindowSize::regular},
            {1920, 1080, window_placement::WindowSize::large},
        }};
        if (update_ == nullptr)
            update_.reset(new UpdateState());
        const auto ticks = [&](int32_t count) {
            for (int32_t frame = 0; frame < count; ++frame)
                idle_tick();
        };
        const auto capture = [&](const std::string& name) {
            ticks(3);
            oa_layer().read_whole_window(true);
            renderer::Surface presented;
            capture_frame_ = &presented;
            render();
            capture_frame_ = nullptr;
            oa_layer().read_whole_window(false);
            require(presented.width > 0 && presented.height > 0, "no frame for " + name);
            write_picture(fs::path(name), presented);
            say("wrote " + name);
        };
        // Every screen over the main menu goes; the pill comes back with the next state.
        const auto clear_screens = [&]() {
            dialogs::reset_dialogs();
            OaLayer& layer = oa_layer();
            for (int pass = 0; pass < 12; ++pass) {
                LayerScreen* top = layer.top();
                if (top == nullptr)
                    break;
                layer.close(top);
            }
            ticks(2);
        };
        update::CentreInputs offered;
        offered.current_version = "0.8.7";
        offered.kept_version = "0.8.6";
        offered.check = update::CheckState::offered;
        offered.offer_version = "0.8.8";
        offered.offer_release = 808;
        offered.offer_size = 25690112;
        update::CentreInputs checking = offered;
        checking.check = update::CheckState::checking;
        checking.offer_version.clear();
        checking.offer_release = 0;
        update::CentreInputs downloading = offered;
        downloading.download = update::DownloadState::downloading;
        downloading.download_release = 808;
        downloading.download_version = "0.8.8";
        downloading.received = 10590617;
        downloading.total = 25690112;
        downloading.bytes_per_second = 180000;
        update::CentreInputs paused = downloading;
        paused.download = update::DownloadState::paused_by_player;
        paused.bytes_per_second = 0;
        update::CentreInputs ready = downloading;
        ready.download = update::DownloadState::ready;
        ready.received = ready.total;
        update::CentreInputs scheduled = ready;
        scheduled.scheduled = 808;
        update::CentreInputs up_to_date;
        up_to_date.current_version = "0.8.7";
        up_to_date.kept_version = "0.8.6";
        up_to_date.check = update::CheckState::up_to_date;

        // The release the offer asks about, as a verified feed would hold it.
        auto feed_held = std::make_shared<feed::Feed>();
        {
            feed::Release release;
            release.release = 808;
            const std::optional<formats::oamod::EngineVersion> version =
                formats::oamod::parse_engine_version("0.8.8");
            if (version.has_value())
                release.version = *version;
            release.version_text = "0.8.8";
            release.notes.push_back(
                {"en",
                 {"Settings, the Library and the keys on Kit 2",
                  "Brains and mutators in the game's own rooms",
                  "Every update state named, with Pause and Cancel"}}
            );
            release.page = "https://example.invalid/releases/0.8.8";
            feed::Package package;
            package.id = update::this_build({}).package;
            package.zip.file = "engine.zip";
            package.zip.size = 25690112;
            release.packages.push_back(std::move(package));
            feed_held->channel = "stable";
            feed_held->releases.push_back(std::move(release));
        }

        update::Receipt restored;
        restored.action = update::Action::install;
        restored.outcome = update::Outcome::failed_restored;
        restored.from_version = "0.8.7";
        restored.to_version = "0.8.8";
        restored.release = 808;
        restored.started_at = 1;
        restored.finished_at = 2;
        restored.shown = true;
        restored.steps.push_back({update::Step::verify, update::StepResult::done, {}});
        restored.steps.push_back(
            {update::Step::swap,
             update::StepResult::failed,
             "open-annihilation.exe: another program had it open"}
        );
        restored.steps.push_back({update::Step::undo, update::StepResult::done, {}});
        update::Receipt unfinished = restored;
        unfinished.outcome = update::Outcome::recovery_incomplete;
        unfinished.steps.back() = {
            update::Step::undo, update::StepResult::failed, "3 files could not be moved back"
        };
        const auto failed_inputs = [](const update::Receipt& receipt) {
            update::CentreInputs inputs;
            inputs.current_version = "0.8.7";
            inputs.held = receipt.release;
            inputs.receipt = receipt;
            return inputs;
        };

        for (const PictureWindow& window : windows) {
            const std::string size =
                std::to_string(window.width) + "x" + std::to_string(window.height);
            require(
                SDL_SetWindowSize(sdl_.window, window.width, window.height) &&
                    SDL_SyncWindow(sdl_.window),
                "the window did not take " + size
            );
            apply_output_mode();
            oa_layer().set_window_size(window.size);
            if (screen_ != Screen::main_menu)
                load(Screen::main_menu);
            ticks(4);
            clear_screens();
            const auto settings_picture = [&](const std::string& state) {
                clear_screens();
                open_engine_settings_from_menu();
                settings::Dialog* dialog = engine_settings_dialog();
                require(dialog != nullptr, "settings did not open on " + size);
                dialog->page = settings::Page::updates;
                apply_update_download(*dialog);
                capture("update-settings-" + state + "-" + size + ".png");
                clear_screens();
            };
            const auto menu_picture = [&](const std::string& state) {
                clear_screens();
                ticks(3);
                capture("update-menu-" + state + "-" + size + ".png");
            };
            const auto screen_picture = [&](const std::string& screen, const std::string& state) {
                const auto deadline = deadline_from(std::chrono::seconds{10});
                while (oa_layer().find(screen) == nullptr) {
                    require(
                        std::chrono::steady_clock::now() < deadline,
                        screen + " did not show on " + size
                    );
                    idle_tick();
                }
                capture("update-" + screen + "-" + state + "-" + size + ".png");
                clear_screens();
            };

            update_->pictured = checking;
            settings_picture("checking");

            update_->pictured = offered;
            update_->held = {};
            update_->held.state = update::CheckState::offered;
            update_->held.feed = feed_held;
            update_->held.offer.release = &feed_held->releases.front();
            update_->held.offer.package = &feed_held->releases.front().packages.front();
            update_->held.offer.way = feed::Way::in_place;
            update_->offered_release.reset();
            {
                const auto deadline = deadline_from(std::chrono::seconds{10});
                while (!is_update_question(oa_layer().find(QuestionScreen::screen_name))) {
                    require(
                        std::chrono::steady_clock::now() < deadline,
                        "the offer did not show on " + size
                    );
                    idle_tick();
                }
                capture("update-offer-offered-" + size + ".png");
                clear_screens();
            }
            update_->held = {};
            settings_picture("offered");

            update_->pictured = downloading;
            menu_picture("downloading");
            settings_picture("downloading");

            update_->pictured = paused;
            menu_picture("paused");
            settings_picture("paused");

            update_->pictured = ready;
            update_->ready_prompt_release.reset();
            screen_picture("ready-update", "ready");
            menu_picture("ready");
            settings_picture("ready");
            update_->ready_prompt_release = 808;

            update_->pictured = scheduled;
            menu_picture("scheduled");
            settings_picture("scheduled");

            update_->pictured = failed_inputs(restored);
            update_->receipt = restored;
            update_->receipt_loaded = true;
            take_update_action(update::Act::details);
            screen_picture("update-receipt", "failed-restored");
            settings_picture("failed-restored");

            update_->pictured = failed_inputs(unfinished);
            update_->receipt = unfinished;
            take_update_action(update::Act::details);
            screen_picture("update-receipt", "recovery-incomplete");

            update_->pictured = up_to_date;
            update_->receipt.reset();
            take_update_action(update::Act::roll_back);
            screen_picture("update-previous", "roll-back");
            settings_picture("up-to-date");
        }
        update_->pictured.reset();
        say("pictures done");
        return 0;
    }

    if (notice_only) {
        // The dummy feed on the command line must not be fetched.
        options_.update_feed.reset();
        const auto deadline = deadline_from(std::chrono::seconds{30});
        while (oa_layer().find("updated-notice") == nullptr &&
               oa_layer().waiting("updated-notice") == nullptr) {
            require(std::chrono::steady_clock::now() < deadline, "the Updated notice did not show");
            clear_blockers();
            idle_tick();
        }
        const update::ThisBuild built = running_build();
        say("updated to " + built.version_text);
        click_named("oa.updated-notice.ok");
        for (int frame = 0; frame < 8; ++frame) {
            idle_tick();
            require(
                oa_layer().find("updated-notice") == nullptr &&
                    oa_layer().waiting("updated-notice") == nullptr,
                "the Updated notice showed again"
            );
        }
        say("updated once");
        return 0;
    }

#if !defined(OA_SELF_UPDATE) || !OA_SELF_UPDATE
    say("this build does not replace itself");
    return 1;
#else
    const update::ThisBuild built = running_build();
    require(built.release >= 1, "this build has no release number");
    const bool older = options_.update_and_quit && options_.update_download_case == "no-offer";
    const bool resume = !options_.update_and_quit && options_.update_download_case != "corrupt" &&
                        options_.update_download_case != "background";
    const std::size_t step = update::download_step_bytes;
    std::vector<uint8_t> promised = patterned(resume ? step * 2 + 48 : 64, 5);
    std::vector<uint8_t> sent = promised;
    if (options_.update_download_case == "corrupt")
        sent = patterned(promised.size(), 9);
    const int64_t release = older ? built.release : built.release + 1;
    const std::string version = older ? built.version_text : std::string(kOfferedVersion);

    fixture::Server server;
    server.set_idle_close_ms(120000);
    server.set_ranges(true);
    std::string listen_error;
    require(server.start(&listen_error), "the server did not listen: " + listen_error);
    const std::string mirror = server.url("/v1/engine/");
    SignedFeed offered = sign_feed(
        feed_json(release, version, built.package, sha256_hex(promised), promised.size(), mirror),
        0x61
    );
    server.serve_bytes("/v1/engine/stable.json", offered.bytes, "application/json");
    server.serve_bytes("/v1/engine/stable.json.sig", offered.signature, "text/plain");
    if (resume) {
        fixture::Reply cut;
        cut.status = 200;
        cut.body = sent;
        cut.cut_after = step + 8;
        server.override_next("/v1/engine/engine.zip", std::move(cut), 1);
        server.override_next("/v1/engine/engine.zip", partial_reply(sent, step), 1);
        server.override_next("/v1/engine/engine.zip", partial_reply(sent, step * 2), 1);
    }
    server.serve_bytes("/v1/engine/engine.zip", sent);
    options_.update_feed = server.url("/v1/engine/stable.json");
    options_.update_keys = {std::string(kKeyId) + "=" + offered.key_text};

    const auto offer_visible = [this]() {
        return is_update_question(oa_layer().find(QuestionScreen::screen_name)) ||
               is_update_question(oa_layer().waiting(QuestionScreen::screen_name));
    };
    const auto ready_visible = [this]() {
        return oa_layer().find("ready-update") != nullptr ||
               oa_layer().waiting("ready-update") != nullptr;
    };
    const auto shot = [this]() {
        require(update_ != nullptr && update_->download != nullptr, "the download did not start");
        return update_->download->snapshot();
    };
    const fs::path package_file =
        *options_.data_dir / "updates" / std::to_string(release) / "engine.zip";

    if (older) {
        const auto deadline = deadline_from(std::chrono::seconds{60});
        while (!quit_requested_) {
            require(std::chrono::steady_clock::now() < deadline, "a run with no offer did not end");
            require(!offer_visible() && !ready_visible(), "a run with no offer showed a prompt");
            idle_tick();
        }
        return exit_status_;
    }

    if (options_.update_and_quit) {
        bool prompted = false;
        const auto deadline = deadline_from(std::chrono::seconds{60});
        while (!quit_requested_) {
            require(std::chrono::steady_clock::now() < deadline, "update-and-quit did not end");
            if (offer_visible() || ready_visible())
                prompted = true;
            if (update_ != nullptr && update_->download != nullptr &&
                update_->download->snapshot().state == update::DownloadState::failed)
                throw std::runtime_error(
                    "update-download: " + update_->download->snapshot().failure
                );
            idle_tick();
        }
        require(!prompted, "update-and-quit showed a prompt");
        say("update-and-quit no prompt");
        return exit_status_;
    }

    if (options_.update_download_case == "background") {
        bool saw_offer = false;
        const auto deadline = deadline_from(std::chrono::seconds{60});
        while (!ready_visible()) {
            require(std::chrono::steady_clock::now() < deadline, "the ready prompt did not show");
            if (offer_visible())
                saw_offer = true;
            clear_blockers();
            idle_tick();
        }
        require(!saw_offer, "Download in the background still asked");
        say("no offer prompt");
        say("ready prompt first");
        // Escape is Keep it ready: nothing is scheduled, and quitting installs nothing.
        {
            const auto deadline = deadline_from(std::chrono::seconds{10});
            while (oa_layer().find("ready-update") == nullptr) {
                require(
                    std::chrono::steady_clock::now() < deadline, "the ready prompt did not show"
                );
                clear_blockers();
                idle_tick();
            }
        }
        press_escape();
        for (int frame = 0; frame < 4; ++frame)
            idle_tick();
        require(!ready_visible(), "Escape left the ready prompt open");
        require(
            !update::read_scheduled(preference_values_).has_value(),
            "Keep it ready scheduled the install"
        );
        require(!hand_off_requested(), "a quit after Keep it ready would install");
        require(
            update_header_note().find("ready") != std::string::npos, "the package is not kept ready"
        );
        say("keep it ready schedules nothing");
        return 0;
    }

    // UPDATE, including the run whose package does not match the feed.
    {
        const auto deadline = deadline_from(std::chrono::seconds{60});
        while (!offer_visible()) {
            require(
                std::chrono::steady_clock::now() < deadline, "the update question did not show"
            );
            clear_blockers();
            idle_tick();
        }
        click_named("oa.prompt.update");
    }

    if (options_.update_download_case == "corrupt") {
        const auto deadline = deadline_from(std::chrono::seconds{30});
        while (update_ == nullptr || update_->download == nullptr ||
               update_->download->snapshot().state != update::DownloadState::failed) {
            require(std::chrono::steady_clock::now() < deadline, "a bad package did not fail");
            idle_tick();
        }
        require(!fs::exists(package_file), "a bad package was kept");
        require(!fs::exists(fs::path(package_file.string() + ".part")), "a bad part was kept");
        require(!ready_visible(), "a bad package queued the ready prompt");
        say("corrupt package deleted");
        say("queued nothing");
        return 0;
    }

    {
        const auto deadline = deadline_from(std::chrono::seconds{60});
        bool started = false;
        while (!zip_ranged(server)) {
            require(std::chrono::steady_clock::now() < deadline, "the download did not resume");
            if (!started && zip_count(server) > 0) {
                say("update started the download");
                started = true;
            }
            idle_tick();
        }
        if (!started)
            say("update started the download");
        say("resumed with Range");
    }
    // The menu pill shows the download: its Pause holds it, a state of its
    // own the pill then names, and its Resume carries it on.
    {
        click_named("oa.update-pill.pause");
        require(
            shot().state == update::DownloadState::paused_by_player,
            "the pill's Pause did not pause the download"
        );
        require(
            update_header_note().find("paused") != std::string::npos,
            "the header note does not say paused"
        );
        click_named("oa.update-pill.resume");
        require(
            shot().state != update::DownloadState::paused_by_player,
            "the pill's Resume did not carry the download on"
        );
        say("pill paused and resumed");
    }
    // The ranged answer is held for kHeadDelayMs. The match has to be on
    // before that body arrives, or the file finishes without a pause.
    start_benchmark_skirmish();
    for (int frame = 0; frame < 4; ++frame)
        idle_tick();
    require(match_ != nullptr || screen_ == Screen::match, "the skirmish did not start");
    {
        const auto deadline = deadline_from(std::chrono::seconds{40});
        while (shot().state != update::DownloadState::paused_for_match) {
            require(std::chrono::steady_clock::now() < deadline, "the download did not pause");
            if (shot().state == update::DownloadState::failed)
                throw std::runtime_error("update-download: " + shot().failure);
            idle_tick();
        }
        say("paused_for_match");
    }
    leave_match();
    // leave_match keeps the match screen. The download stays paused, and
    // Settings will not open, until the main menu is showing.
    load(Screen::main_menu);
    require(screen_ == Screen::main_menu, "the main menu did not open");
    {
        const auto deadline = deadline_from(std::chrono::seconds{30});
        while (zip_count(server) < 3) {
            require(std::chrono::steady_clock::now() < deadline, "the download did not resume");
            idle_tick();
        }
        say("resumed after the match");
    }
    dialogs::reset_dialogs();
    {
        const auto deadline = deadline_from(std::chrono::seconds{10});
        while (engine_settings_dialog() == nullptr) {
            require(std::chrono::steady_clock::now() < deadline, "settings did not open");
            if (screen_ != Screen::main_menu)
                load(Screen::main_menu);
            open_engine_settings_from_menu();
            if (engine_settings_dialog() == nullptr)
                idle_tick();
        }
    }
    engine_settings_dialog()->page = settings::Page::updates;
    {
        const auto deadline = deadline_from(std::chrono::seconds{40});
        bool queued = false;
        while (true) {
            idle_tick();
            settings::Dialog* dialog = engine_settings_dialog();
            require(dialog != nullptr, "settings closed before the package was ready");
            apply_update_download(*dialog);
            const update::DownloadSnapshot now = shot();
            if (now.state == update::DownloadState::failed)
                throw std::runtime_error("update-download: " + now.failure);
            // The worker can finish after this frame already offered, so the
            // prompt is queued on the next tick. Settings being open does
            // not hold it: the offer queues whether or not that dialog shows.
            if (!queued && (ready_visible() ||
                            (update_ != nullptr && update_->ready_prompt_release.has_value()))) {
                say("ready prompt queued");
                queued = true;
            }
            const bool file_ready = now.state == update::DownloadState::ready &&
                                    dialog->updates.download == settings::DownloadShown::ready &&
                                    dialog->header_note.find("ready") != std::string::npos;
            if (file_ready && queued)
                break;
            require(
                std::chrono::steady_clock::now() < deadline,
                file_ready ? "the ready prompt did not queue" : "the package did not become ready"
            );
        }
        say("status ready");
        say("header note ready");
    }
    {
        const auto deadline = deadline_from(std::chrono::seconds{15});
        while (engine_settings_dialog() != nullptr) {
            require(std::chrono::steady_clock::now() < deadline, "settings stayed open");
            // Escape on the ready prompt is WHEN I QUIT. Settings has to
            // close first, and a prompt already showing is left up.
            if (oa_layer().find("ready-update") != nullptr)
                break;
            press_escape();
        }
    }
    click_named("oa.ready-update.restart-now");
    say("restart now");
    return 0;
#endif
}

} // namespace oa::app

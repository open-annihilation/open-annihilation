// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-update-download: a package served on 127.0.0.1, then the ready
// prompt and the recorded hand-off. The key is made from a fixed seed.
// The published feed is not requested, and the helper is not started.

#include "oa/app/runtime.hpp"

#include "oa_layer.hpp"
#include "oa_layer_check.hpp"
#include "update_state.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/update/build.hpp"
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
#include <iostream>
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

} // namespace

int Runtime::check_update_download() {
    require(options_.data_dir.has_value(), "the run has no --data-dir");
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
            require(
                std::chrono::steady_clock::now() < deadline, "the package did not become ready"
            );
            idle_tick();
            settings::Dialog* dialog = engine_settings_dialog();
            require(dialog != nullptr, "settings closed before the package was ready");
            apply_update_download(*dialog);
            const update::DownloadSnapshot now = shot();
            if (now.state == update::DownloadState::failed)
                throw std::runtime_error("update-download: " + now.failure);
            if (!queued && (ready_visible() ||
                            (update_ != nullptr && update_->ready_prompt_release.has_value()))) {
                say("ready prompt queued");
                queued = true;
            }
            if (now.state == update::DownloadState::ready &&
                dialog->updates.download == settings::DownloadShown::ready &&
                dialog->header_note.find("ready") != std::string::npos)
                break;
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

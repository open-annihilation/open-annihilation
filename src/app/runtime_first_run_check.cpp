// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-first-run: walks the five-step setup over the main menu. It switches
// to Deutsch, downloads a language pack from the check's own registry
// (missing, then Retry), quits, and shows again because the welcome was not
// recorded. The second walk stays in
// Deutsch, copies a small fixture install into the player's folder, checks
// it, then applies both online choices. The pack cannot be signed as Core
// Prime, so the registry is the made-up one Ridge and the setup reads it
// only while this check runs. The named game folder is not copied.
//
// --check-setup-window walks language, the game and the copy in the pre-game
// window, then checks that online features continue over the main menu in
// the language chosen there. The named game folder is what the menu plays.

#include "oa_layer.hpp"
#include "oa_layer_check.hpp"

#include "oa/app/content/downloads.hpp"
#include "oa/app/content/service.hpp"
#include "oa/app/content/settings.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/base/sha256.hpp"
#include "oa/base/signing/ed25519.hpp"
#include "oa/data/catalogue/check.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/registry/descriptor.hpp"
#include "oa/data/registry/player_registries.hpp"
#include "oa/data/registry/trust.hpp"
#include "oa/formats/url.hpp"
#include "oa/formats/zip.hpp"
#include "oa/netgame/http_fixture/server.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/first_run/first_run.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oa::app {
namespace {

namespace fs = std::filesystem;
namespace sha = oa::base::sha256;
namespace signing = oa::base::signing;
namespace catalogue = oa::data::catalogue;
namespace registry = oa::data::registry;
namespace zip = oa::formats::zip;
namespace fixture = oa::netgame::http_fixture;
namespace first_run = oa::ui::first_run;
namespace settings = oa::ui::engine_settings;
namespace kit = oa::ui::kit;

/// The check's registry. The setup accepts it only while this check runs.
constexpr std::string_view kRegistry = "ridge";
/// The key that signs its catalogue.
constexpr std::string_view kKeyId = "ridge-check-key";
/// The language pack the catalogue offers.
constexpr std::string_view kTag = "en-XA";
/// How long the check waits for the catalogue, a download or the setup.
constexpr std::chrono::seconds kWait{30};
/// The setup's name on the OA layer.
constexpr std::string_view kScreen = "first-run";

/// The server the check's catalogue and pack are served from. It lives for
/// the run.
fixture::Server& fixture_server() {
    static fixture::Server server;
    return server;
}

/// The pack, served only after the first download has failed.
struct HeldPack {
    std::string path{};
    std::vector<uint8_t> bytes{};
};

/// The pack prepare built. The check serves it on Retry.
HeldPack& held_pack() {
    static HeldPack pack;
    return pack;
}

/// Throws when a step of the check fails.
///
/// @param ok the step passed
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("first-run check: " + std::string(what));
}

/// Writes a text file whole.
///
/// @param file the file
/// @param text its text
void write_text(const fs::path& file, std::string_view text) {
    std::error_code io;
    fs::create_directories(file.parent_path(), io);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    require(static_cast<bool>(out), "could not write " + path_to_utf8(file));
}

/// Returns a JSON string, with quotes and backslashes escaped.
///
/// @param text the text
/// @return the quoted text
std::string json_string(std::string_view text) {
    std::string out = "\"";
    for (const char letter : text) {
        const auto byte = static_cast<unsigned char>(letter);
        if (letter == '"' || letter == '\\')
            out.push_back('\\');
        else if (byte < 0x20)
            continue;
        out.push_back(letter);
    }
    out.push_back('"');
    return out;
}

/// Tells whether the wait has run out.
///
/// @param deadline when the wait ends
/// @return true when it has
bool deadline_passed(std::chrono::steady_clock::time_point deadline) {
    return std::chrono::steady_clock::now() >= deadline;
}

} // namespace

void Runtime::prepare_first_run_check() {
    require(
        options_.user_folder && !options_.user_folder->empty() && !user_folder().empty(),
        "the run has no folder of the player's own (--user-folder)"
    );
    require(options_.data_dir.has_value(), "the run has no --data-dir");

    const std::string yaml = "oalang: 1\n"
                             "tag: en-XA\n"
                             "name: Pseudo\n"
                             "english-name: Pseudo\n"
                             "word: Pseudo\n"
                             "version: \"1\"\n"
                             "locales: [en-XA]\n"
                             "fallbacks: []\n"
                             "text: {needs: game-fonts}\n"
                             "requires: {engine: \">= 0.0.1\"}\n";
    const std::vector<uint8_t> yaml_bytes(yaml.begin(), yaml.end());
    const zip::NewEntry entries[] = {{"language.yaml", yaml_bytes}};
    std::vector<uint8_t> archive;
    zip::ZipError zip_error{};
    require(zip::write_archive(entries, archive, zip_error), "the language pack could not be made");
    const sha::Digest digest = sha::digest_of(archive);
    const auto hex_chars = sha::to_hex(digest);
    const std::string hex(hex_chars.begin(), hex_chars.end());
    held_pack().path = "/v1/p/" + hex + ".oalang";
    held_pack().bytes = std::move(archive);

    std::string listen_error;
    require(fixture_server().start(&listen_error), "the server did not listen: " + listen_error);
    const std::string catalogue_url = fixture_server().url("/v1/catalogue.json");

    std::array<uint8_t, 32> seed{};
    seed.fill(0x39);
    signing::SecretKey secret{};
    signing::PublicKey public_key{};
    signing::key_pair_from_seed(seed, secret, public_key);
    seed.fill(0);
    const std::string key_text(signing::public_key_text(public_key).view());

    std::string json = "{\"catalogue\":1,\"registry\":";
    json += json_string(kRegistry);
    json += ",\"sequence\":1,\"generated\":\"2026-10-01T00:00:00Z\","
            "\"expires\":\"2027-01-01T00:00:00Z\",\"keys\":[{\"id\":";
    json += json_string(kKeyId);
    json += ",\"public\":";
    json += json_string(key_text);
    json += "}],\"packages\":[{\"kind\":\"oalang\",\"id\":";
    json += json_string(kTag);
    json += ",\"name\":\"Pseudo\",\"english_name\":\"Pseudo\",\"word\":\"Pseudo\","
            "\"release\":1,\"size\":";
    json += std::to_string(held_pack().bytes.size());
    json += ",\"sha256\":";
    json += json_string(hex);
    json += ",\"file\":";
    json += json_string(held_pack().path);
    json += ",\"locales\":[\"en-XA\"],\"fallbacks\":[],\"needs\":\"game-fonts\","
            "\"requires\":{\"engine\":\">= 0.0.1\"}}]}";

    const std::vector<uint8_t> catalogue_bytes(json.begin(), json.end());
    const signing::Signature signature = signing::sign(secret, catalogue_bytes);
    secret.fill(0);
    const std::string signature_text =
        catalogue::signature_file_text(catalogue::SignatureLine{std::string(kKeyId), signature});
    const std::vector<uint8_t> signature_bytes(signature_text.begin(), signature_text.end());
    fixture_server().serve_bytes("/v1/catalogue.json", catalogue_bytes, "application/json");
    fixture_server().serve_bytes("/v1/catalogue.json.sig", signature_bytes, "text/plain");

    registry::PlayerRegistries registries;
    // SDL caches this string for the process. The caller does not free it.
    if (const char* base = SDL_GetBasePath(); base != nullptr) {
        const fs::path builtins =
            path_from_utf8(base) / path_from_utf8(content::builtin_registries_folder_name);
        std::error_code io;
        if (fs::is_directory(builtins, io))
            for (const registry::Descriptor& built_in :
                 registry::read_builtin_registries(builtins, nullptr))
                registries.disabled.push_back(built_in.id);
    }
    const std::optional<oa::formats::url::Url> parsed =
        oa::formats::url::parse_http_url(catalogue_url);
    require(parsed.has_value(), "the catalogue's address did not read");
    registry::AddedRegistry added;
    added.descriptor.id = std::string(kRegistry);
    added.descriptor.name = "Ridge";
    added.descriptor.catalogue = *parsed;
    added.descriptor.keys.push_back(registry::RegistryKey{std::string(kKeyId), public_key});
    added.descriptor.mode = registry::DownloadMode::direct;
    added.added = "2026-10-01";
    added.enabled = true;
    registries.registries.push_back(std::move(added));
    write_text(
        user_folder() / std::string(registry::player_registries_file),
        registry::player_registries_text(registries)
    );
    std::cout << "first-run check: registry " << kRegistry << " at " << catalogue_url << ", "
              << registries.disabled.size() << " built-in registries turned off" << std::endl;
}

void Runtime::check_first_run() {
    require(options_.first_run, "needs --first-run");
    require(options_.preferences_file.has_value(), "needs --preferences-file");
    require(sdl_.window != nullptr && sdl_.renderer != nullptr, "the window did not open");
    require(
        SDL_SetWindowSize(sdl_.window, kCanvasWidth, kCanvasHeight) && SDL_SyncWindow(sdl_.window),
        "the window did not take 640x480"
    );
    apply_output_mode();
    int width = 0;
    int height = 0;
    SDL_GetWindowSize(sdl_.window, &width, &height);
    require(width == kCanvasWidth && height == kCanvasHeight, "the window is not 640x480");

    load(Screen::main_menu);
    require(screen_ == Screen::main_menu, "the main menu did not open");
    require(engine_settings_fonts() != nullptr, "the dialog's fonts did not load");

    const auto preference = [this](std::string_view key) -> std::string {
        const auto found = preference_values_.find(std::string(key));
        if (found == preference_values_.end())
            return {};
        return found->second;
    };
    const auto pump = [this] {
        bool running = true;
        run_frame(running);
        require(running, "a frame ended the run");
    };
    const auto setup = [this]() -> LayerScreen* { return oa_layer().find(kScreen); };
    const auto control_named = [&](std::string_view name) -> std::optional<kit::Control> {
        LayerScreen* screen = setup();
        if (screen == nullptr)
            return std::nullopt;
        const kit::DisplayList display = screen->display_list();
        for (const kit::Control& control : display.controls)
            if (control.name == name)
                return control;
        return std::nullopt;
    };
    const auto shows_words = [&](std::string_view words) {
        LayerScreen* screen = setup();
        if (screen == nullptr)
            return false;
        const kit::DisplayList display = screen->display_list();
        for (const kit::Item& item : display.items)
            if (item.text.find(words) != std::string::npos)
                return true;
        for (const kit::Control& control : display.controls)
            if (control.text.find(words) != std::string::npos)
                return true;
        return false;
    };
    const auto click = [&](std::string_view name) {
        const std::optional<kit::Control> control = control_named(name);
        require(
            control.has_value() && control->enabled, std::string(name) + " is not on the setup"
        );
        LayerScreen* screen = setup();
        require(screen != nullptr, "the setup is not showing");
        const LayerPlacement placed = screen->placement(oa_layer().view());
        // At the setup's own scale, which may be a fraction.
        const int32_t percent = std::max(placed.scale_percent, int32_t{100});
        const oa::ui::display_layout::Point at{
            placed.shown.x + (control->rect.x + control->rect.width / 2) * percent / 100,
            placed.shown.y + (control->rect.y + control->rect.height / 2) * percent / 100
        };
        send_check_pointer(SDL_EVENT_MOUSE_MOTION, at, 0);
        send_check_pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, at, SDL_BUTTON_LEFT);
        send_check_pointer(SDL_EVENT_MOUSE_BUTTON_UP, at, SDL_BUTTON_LEFT);
    };
    const auto settle = [&](std::string_view name) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        while (std::chrono::steady_clock::now() < until) {
            if (control_named(name))
                return;
            require(!deadline_passed(deadline), std::string(name) + " did not appear");
            pump();
        }
    };
    const auto show_control = [&](std::string_view name) {
        for (int back = 0; back < 8 && !control_named(name); ++back) {
            const std::optional<kit::Control> previous =
                control_named("first-run.language.previous");
            if (!previous || !previous->enabled)
                break;
            click("first-run.language.previous");
            settle(name);
        }
        for (int forward = 0; forward < 8 && !control_named(name); ++forward) {
            const std::optional<kit::Control> next_card =
                control_named("first-run.language.next-card");
            require(next_card && next_card->enabled, std::string(name) + " is not on the carousel");
            click("first-run.language.next-card");
            settle(name);
        }
        require(control_named(name).has_value(), std::string(name) + " is not on the carousel");
    };
    const auto pick_card = [&](std::string_view name) {
        show_control(name);
        click(name);
        click(name);
    };
    const auto catalogue_lists_pack = [this] {
        if (!content_)
            return false;
        const std::shared_ptr<const content::Snapshot> snapshot = content_service().snapshot();
        if (!snapshot)
            return false;
        for (const content::Entry& entry : snapshot->entries) {
            if (entry.package != nullptr && entry.registry == kRegistry &&
                entry.package->id == kTag)
                return true;
        }
        return false;
    };
    const auto pack_download = [this]() -> std::optional<content::DownloadView> {
        if (!content_)
            return std::nullopt;
        const std::vector<content::DownloadView> items = content_downloads().view();
        for (const content::DownloadView& item : items) {
            if (item.target.registry == kRegistry &&
                settings::language_notice_same_tag(item.target.key, kTag))
                return item;
        }
        return std::nullopt;
    };

    {
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        content_service().refresh(content::RefreshReason::check_now, kRegistry);
        while (setup() == nullptr || !catalogue_lists_pack()) {
            require(!deadline_passed(deadline), "the setup did not show the catalogue's language");
            pump();
        }
    }
    std::cout << "first-run check: showing, catalogue lists " << kTag << std::endl;

    // A window taller than the picture puts the footer in the letterbox. The
    // software cursor has to be drawn there, and the check puts 640 by 480 back
    // before the walk.
    {
        require(
            SDL_SetWindowSize(sdl_.window, kCanvasWidth, 960) && SDL_SyncWindow(sdl_.window),
            "the window did not take 640x960"
        );
        apply_output_mode();
        pump();
        const std::optional<kit::Control> next = control_named("first-run.language.next");
        require(next.has_value() && next->enabled, "Next is not on the setup");
        LayerScreen* screen = setup();
        require(screen != nullptr, "the setup is not showing");
        const LayerView seen = oa_layer().view();
        const LayerPlacement placed = screen->placement(seen);
        const oa::ui::display_layout::Point pixel = layer_window_pixel(
            placed, {next->rect.x + next->rect.width / 2, next->rect.y + next->rect.height / 2}
        );
        const bool outside = pixel.x < seen.picture.x || pixel.y < seen.picture.y ||
                             pixel.x >= seen.picture.x + seen.picture.width ||
                             pixel.y >= seen.picture.y + seen.picture.height;
        require(outside, "Next sits inside the picture");
        SDL_Event motion =
            window_pointer_event(sdl_.window, sdl_.renderer, SDL_EVENT_MOUSE_MOTION, pixel, 0);
        bool going = true;
        dispatch_event(motion, going);
        require(going, "moving onto Next ended the run");
        const auto capture = [this] {
            oa_layer().read_whole_window(true);
            renderer::Surface presented;
            capture_frame_ = &presented;
            render();
            capture_frame_ = nullptr;
            oa_layer().read_whole_window(false);
            return presented;
        };
        const renderer::Surface with_cursor = capture();
        frame_without_cursor_ = true;
        const renderer::Surface without_cursor = capture();
        frame_without_cursor_ = false;
        const double across = seen.picture_size.x > 0 ? static_cast<double>(seen.picture.width) /
                                                            static_cast<double>(seen.picture_size.x)
                                                      : 1.0;
        const double down = seen.picture_size.y > 0 ? static_cast<double>(seen.picture.height) /
                                                          static_cast<double>(seen.picture_size.y)
                                                    : 1.0;
        const auto tip_of = [](int32_t origin, float picture_point, double scale) {
            return origin + static_cast<int32_t>(
                                std::floor((static_cast<double>(picture_point) + 0.5) * scale)
                            );
        };
        const LayerPointer tip{
            {tip_of(seen.picture.x, pointer_x_, across), tip_of(seen.picture.y, pointer_y_, down)},
            0
        };
        require(
            layer_differences(with_cursor, without_cursor, tip) > 0,
            "the cursor is not drawn over the footer"
        );
        const int32_t reach = static_cast<int32_t>(
            std::ceil(static_cast<double>(layer_cursor_reach) * std::max(across, down))
        );
        const LayerPointer covered{tip.at, reach};
        require(
            layer_differences(with_cursor, without_cursor, covered) == 0,
            "the footer frames differ outside the cursor"
        );
        std::cout << "first-run check: the cursor shows over the footer" << std::endl;
        require(
            SDL_SetWindowSize(sdl_.window, kCanvasWidth, kCanvasHeight) &&
                SDL_SyncWindow(sdl_.window),
            "the window did not return to 640x480"
        );
        apply_output_mode();
        pump();
    }

    // The header leaves out the mark and the name in a window, shows them
    // full screen, and follows a switch while the setup is up.
    {
        const auto branded = [&]() -> std::optional<bool> {
            LayerScreen* screen = setup();
            if (screen == nullptr)
                return std::nullopt;
            const kit::DisplayList display = screen->display_list();
            for (const kit::Item& item : display.items)
                if (const auto* look = std::get_if<kit::SetupChromeLook>(&item.look))
                    return look->brand;
            return std::nullopt;
        };
        const auto wait_for = [&](bool brand, std::string_view what) {
            const auto deadline = std::chrono::steady_clock::now() + kWait;
            while (branded() != brand) {
                require(!deadline_passed(deadline), what);
                pump();
            }
        };
        const auto switch_to = [&](bool full) {
            require(
                SDL_SetWindowFullscreen(sdl_.window, full) && SDL_SyncWindow(sdl_.window),
                full ? "full screen did not take" : "the window did not come back"
            );
            pump();
        };
        require(
            (SDL_GetWindowFlags(sdl_.window) & SDL_WINDOW_FULLSCREEN) == 0,
            "the check did not start in a window"
        );
        wait_for(false, "the header names the game in a window");
        switch_to(true);
        wait_for(true, "the header does not name the game full screen");
        switch_to(false);
        wait_for(false, "the header still names the game in the window");
        require(
            SDL_SetWindowSize(sdl_.window, kCanvasWidth, kCanvasHeight) &&
                SDL_SyncWindow(sdl_.window),
            "the window did not return to 640x480"
        );
        apply_output_mode();
        pump();
        std::cout << "first-run check: the header follows the window" << std::endl;
    }

    // Left, Left and Right leave the box on the card in the middle, not on
    // the language in use.
    {
        const auto press = [&](SDL_Keycode key) {
            SDL_Event event{};
            event.type = SDL_EVENT_KEY_DOWN;
            event.key.key = key;
            event.key.down = true;
            bool going = true;
            dispatch_event(event, going);
            event.type = SDL_EVENT_KEY_UP;
            event.key.down = false;
            dispatch_event(event, going);
            require(going, "a key ended the run");
            pump();
        };
        press(SDLK_LEFT);
        press(SDLK_LEFT);
        press(SDLK_RIGHT);
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
        while (std::chrono::steady_clock::now() < until)
            pump();
        LayerScreen* screen = setup();
        require(screen != nullptr, "the setup is not showing");
        const kit::DisplayList display = screen->display_list();
        // Left and Right turn the carousel. The focus stays on the first stop,
        // so the card in the middle is the carousel's chosen card.
        int32_t chosen = -1;
        for (const kit::Item& item : display.items) {
            if (const auto* carousel = std::get_if<kit::CarouselLook>(&item.look)) {
                chosen = carousel->chosen;
                break;
            }
        }
        const kit::Control* card =
            chosen < 0 ? nullptr : kit::control_of(display, first_run::card_id + chosen);
        require(
            card != nullptr && card->name.starts_with("first-run.language.card."),
            "the carousel's chosen card is not a language card"
        );
        require(card->name != "first-run.language.card.en", "the box stayed on English");
        const LayerPlacement placed = screen->placement(oa_layer().view());
        const int32_t middle = card->rect.x + card->rect.width / 2;
        require(
            middle >= placed.points_width / 2 - 2 && middle <= placed.points_width / 2 + 2,
            "the box is not on the card in the middle"
        );
        std::cout << "first-run check: the box is on " << card->name << std::endl;
    }

    // A window of 2880 by 1620 draws the setup larger than the layer's own
    // scale while the Interface size is Auto, even on whole steps, and a
    // press lands where the larger setup draws its control.
    {
        require(
            SDL_SetWindowSize(sdl_.window, 2880, 1620) && SDL_SyncWindow(sdl_.window),
            "the window did not take 2880x1620"
        );
        apply_output_mode();
        pump();
        pump();
        LayerScreen* screen = setup();
        require(screen != nullptr, "the setup is not showing at 2880x1620");
        const LayerView seen = oa_layer().view();
        const LayerPlacement placed = screen->placement(seen);
        require(seen.interface_percent <= 0, "the check's Interface size is not Auto");
        require(placed.scale_percent > seen.scale_percent, "the setup is not drawn larger");
        const std::optional<kit::Control> next = control_named("first-run.language.next");
        const std::optional<kit::Control> arrow = control_named("first-run.language.next-card");
        require(next.has_value() && arrow.has_value(), "the setup's controls are missing");
        const int32_t tall = next->rect.height * placed.scale_percent / 100;
        require(tall >= 3 * kit::compact_metrics.button_height, "Next is not three times as tall");
        const kit::ControlId before = screen->interaction().focused;
        const oa::ui::display_layout::Point pixel = layer_window_pixel(
            placed, {arrow->rect.x + arrow->rect.width / 2, arrow->rect.y + arrow->rect.height / 2}
        );
        for (const SDL_EventType type :
             {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event event = window_pointer_event(
                sdl_.window,
                sdl_.renderer,
                type,
                pixel,
                type == SDL_EVENT_MOUSE_MOTION ? uint8_t{0} : uint8_t{SDL_BUTTON_LEFT}
            );
            bool going = true;
            dispatch_event(event, going);
            require(going, "a press ended the run");
            pump();
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
        while (std::chrono::steady_clock::now() < until)
            pump();
        LayerScreen* after = setup();
        require(after != nullptr, "the setup closed at 2880x1620");
        const kit::DisplayList display = after->display_list();
        bool moved = false;
        for (const kit::Control& control : display.controls) {
            const int32_t middle = control.rect.x + control.rect.width / 2;
            if (control.name.starts_with("first-run.language.card.") &&
                middle >= placed.points_width / 2 - 2 && middle <= placed.points_width / 2 + 2)
                moved = control.id != before;
        }
        require(moved, "a press on the next arrow at 2880x1620 did not turn the carousel");
        std::cout << "first-run check: at 2880x1620 the setup is drawn at " << placed.scale_percent
                  << "%, the layer at " << seen.scale_percent << "%" << std::endl;
        require(
            SDL_SetWindowSize(sdl_.window, kCanvasWidth, kCanvasHeight) &&
                SDL_SyncWindow(sdl_.window),
            "the window did not return to 640x480"
        );
        apply_output_mode();
        pump();
    }

    pick_card("first-run.language.card.de");
    require(
        settings::language_notice_same_tag(shown_language().tag, "de"), "Deutsch was not chosen"
    );
    std::cout << "first-run check: language de" << std::endl;

    pick_card("first-run.language.card.en-xa");
    {
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        while (true) {
            const std::optional<content::DownloadView> item = pack_download();
            if (item && (item->state == content::DownloadState::failed ||
                         item->state == content::DownloadState::cancelled))
                break;
            if (deadline_passed(deadline)) {
                std::string detail = "the missing pack did not fail";
                if (item) {
                    detail += " state " + std::to_string(static_cast<int>(item->state));
                    if (item->failure != content::DownloadFailure::none) {
                        detail += " ";
                        detail += content::failure_text(item->failure);
                    }
                }
                require(false, detail);
            }
            pump();
        }
    }
    std::cout << "first-run check: the pack was missing" << std::endl;
    fixture_server().serve_bytes(held_pack().path, held_pack().bytes, "application/octet-stream");
    {
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        while (!control_named("first-run.language.retry")) {
            require(!deadline_passed(deadline), "Retry did not show");
            pump();
        }
    }
    click("first-run.language.retry");
    {
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        while (!settings::language_notice_same_tag(shown_language().tag, kTag)) {
            if (deadline_passed(deadline)) {
                std::string detail = "Pseudo was not shown";
                const std::optional<content::DownloadView> queued = pack_download();
                if (queued) {
                    detail += " state " + std::to_string(static_cast<int>(queued->state));
                    if (queued->failure != content::DownloadFailure::none) {
                        detail += " ";
                        detail += content::failure_text(queued->failure);
                        detail += " ";
                        detail += queued->detail;
                    }
                }
                require(false, detail);
            }
            pump();
        }
    }
    std::cout << "first-run check: language " << kTag << std::endl;

    {
        const std::optional<kit::Control> leaving = control_named("first-run.language.skip");
        require(
            leaving.has_value() && leaving->text == "Quit",
            "a first start's left button does not say Quit"
        );
    }
    click("first-run.language.skip");
    require(setup() == nullptr, "Quit left the setup showing");
    require(
        preference(first_run::preference_key) != first_run::preference_done,
        "Quit recorded the welcome as done"
    );
    std::cout << "first-run check: quit, preference not done" << std::endl;

    {
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        while (setup() == nullptr) {
            require(!deadline_passed(deadline), "--first-run did not show the setup again");
            pump();
        }
    }
    pick_card("first-run.language.card.de");
    require(
        settings::language_notice_same_tag(shown_language().tag, "de"),
        "Deutsch was not chosen for the game steps"
    );
    require(
        std::string(oa::data::languages::interface_text("Locate Total Annihilation Game Files")) ==
            "Spieldateien von Total Annihilation suchen",
        "the game step's title is not German"
    );
    click("first-run.language.next");
    require(control_named("first-run.game.next").has_value(), "the game step did not show");
    require(shows_words("OA kopiert das Spiel einmal"), "the game step is not in German");
    require(shows_words("Beenden"), "the game step's left button is not Quit");
    click("first-run.game.next");
    require(control_named("first-run.copying.next").has_value(), "the copying step did not show");
    require(shows_words("Wenn OA anhält"), "the copying step is not in German");
    require(shows_words("Beenden"), "the copying step's left button is not Quit");
    {
        const auto deadline = std::chrono::steady_clock::now() + kWait;
        while (!shows_words("Alle Dateien stimmen überein")) {
            require(!deadline_passed(deadline), "the copy was not checked");
            pump();
        }
    }
    std::cout << "first-run check: the game was copied and checked" << std::endl;
    click("first-run.copying.next");
    require(control_named("first-run.online.not-now").has_value(), "the online step did not show");
    require(shows_words("Nicht jetzt"), "Not now is not in German");
    click("first-run.online.not-now");
    click("first-run.online.next");
    require(
        preference(content::content_updates_key) == "never",
        "Not now did not set content updates to never"
    );
    require(
        preference(content::install_id_key("coreprime")) == "off",
        "Not now did not turn Core Prime's install ID off"
    );
    std::cout << "first-run check: online features off" << std::endl;

    click("first-run.buttons.back");
    require(control_named("first-run.online.on").has_value(), "Back did not return to online");
    click("first-run.online.on");
    click("first-run.online.next");
    require(
        preference(content::content_updates_key) == "automatically",
        "Turn on did not set content updates to automatically"
    );
    require(
        preference_values_.find(content::install_id_key("coreprime")) == preference_values_.end(),
        "Turn on made an install ID"
    );
    std::cout << "first-run check: online features on, no install ID made" << std::endl;

    click("first-run.buttons.start");
    require(setup() == nullptr, "Start playing left the setup showing");
    require(
        preference(first_run::preference_key) == first_run::preference_done,
        "Start playing cleared the finished preference"
    );
    options_.first_run = false;
    for (int frame = 0; frame < 8; ++frame)
        pump();
    require(
        setup() == nullptr && oa_layer().waiting(kScreen) == nullptr,
        "the setup showed again after it was finished"
    );
    std::cout << "first-run check: passed\n" << std::flush;
}

void Runtime::check_setup_window() {
    const auto expect = [](bool ok, std::string_view what) {
        if (!ok)
            throw std::runtime_error("setup window check: " + std::string(what));
    };
    expect(options_.first_run, "needs --first-run");
    expect(options_.preferences_file.has_value(), "needs --preferences-file");
    expect(sdl_.window != nullptr && sdl_.renderer != nullptr, "the window did not open");

    load(Screen::main_menu);
    expect(screen_ == Screen::main_menu, "the main menu did not open");
    expect(engine_settings_fonts() != nullptr, "the dialog's fonts did not load");

    const auto pump = [this] {
        bool running = true;
        run_frame(running);
        if (!running)
            throw std::runtime_error("setup window check: a frame ended the run");
    };
    const auto online_in_german = [this] {
        LayerScreen* screen = oa_layer().find(kScreen);
        if (screen == nullptr)
            return false;
        const kit::DisplayList display = screen->display_list();
        bool button = false;
        bool words = false;
        for (const kit::Control& control : display.controls) {
            if (control.name == "first-run.online.not-now" && control.enabled)
                button = true;
            if (control.text.find("Nicht jetzt") != std::string::npos)
                words = true;
        }
        for (const kit::Item& item : display.items)
            if (item.text.find("Nicht jetzt") != std::string::npos)
                words = true;
        return button && words;
    };

    const auto deadline = std::chrono::steady_clock::now() + kWait;
    while (!online_in_german()) {
        expect(!deadline_passed(deadline), "the online step did not show in German");
        pump();
    }
    const std::string not_now{oa::data::languages::interface_text("Not now")};
    expect(not_now == "Nicht jetzt", "Not now is not German");
    std::cout << "setup window check: passed\n" << std::flush;
}

} // namespace oa::app

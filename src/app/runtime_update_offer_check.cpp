// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-update-offer: the update question against a feed this check
// serves on 127.0.0.1. The key is made from a fixed seed. The published
// feed is not requested, and the release page is not opened in a browser.

#include "oa/app/runtime.hpp"

#include "oa_layer.hpp"
#include "oa_layer_check.hpp"
#include "update_state.hpp"
#include "web_link_state.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/update/build.hpp"
#include "oa/app/update/check.hpp"
#include "oa/app/update/offer.hpp"
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
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace update = oa::app::update;
namespace signing = oa::base::signing;
namespace catalogue = oa::data::catalogue;
namespace fixture = oa::netgame::http_fixture;
namespace dialogs = oa::ui::frontend_dialogs;
namespace kit = oa::ui::kit;
namespace layout = oa::ui::display_layout;

constexpr std::string_view kSha =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr std::string_view kKeyId = "update-offer-check";
constexpr std::string_view kPage = "https://example.invalid/releases/9.9.9";
constexpr std::string_view kEnglishOne = "Wider bars";
constexpr std::string_view kEnglishTwo = "A second English line";
constexpr std::string_view kGermanOne = "Breitere Balken";
constexpr std::string_view kGermanTwo = "Eine deutsche Zeile";

/// Throws when a step of the check fails.
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("update-offer: " + std::string(what));
}

/// One release of the stable feed, with notes in English and German. The mirror is not fetched.
std::string feed_json(int64_t release, std::string_view package) {
    std::string json = R"({"feed":1,"channel":"stable","releases":[{"release":)";
    json += std::to_string(release);
    json += R"(,"version":"9.9.9","date":"2026-10-25","from":"0.0.1","notes":{"en":[")";
    json += kEnglishOne;
    json += R"(",")";
    json += kEnglishTwo;
    json += R"("],"de":[")";
    json += kGermanOne;
    json += R"(",")";
    json += kGermanTwo;
    json += R"("]},"page":")";
    json += kPage;
    json += R"(","packages":{")";
    json += package;
    json += R"(":{"file":"engine.zip","size":43011223,"sha256":")";
    json += kSha;
    json += R"(","mirrors":["http://127.0.0.1/v1/engine/"]}}}]})";
    return json;
}

/// Signs `json` with a key derived from `fill`. The seed and the secret are wiped.
struct SignedFeed {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> signature;
    std::string key_text;
};

SignedFeed sign_feed(std::string_view json, std::string_view key_id, uint8_t fill) {
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
        catalogue::signature_file_text(catalogue::SignatureLine{std::string(key_id), signature});
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

bool paragraph_has(const kit::Question& asked, std::string_view line) {
    for (const kit::Paragraph& paragraph : asked.paragraphs)
        if (paragraph.text.find(line) != std::string::npos)
            return true;
    return false;
}

} // namespace

void Runtime::check_update_offer() {
    require(options_.data_dir.has_value(), "the run has no --data-dir");
    // A feed an earlier run of this check stored would be read again at the
    // first menu; this run starts with none.
    {
        std::error_code cleared;
        std::filesystem::remove(*options_.data_dir / "updates" / "feed.json", cleared);
        std::filesystem::remove(*options_.data_dir / "updates" / "feed.json.sig", cleared);
    }
    require(web_links_ != nullptr, "the run is not recording web links");
    // Step 1 starts from German, with no skipped release and no recorded
    // check time. An earlier run of this check stores both in the same file.
    namespace settings = oa::ui::engine_settings;
    preference_values_.erase(std::string(settings::key::update_skipped));
    preference_values_.erase(std::string(settings::key::update_checked));
    preference_values_[std::string(settings::key::language)] = "de";
    preference_values_[std::string(oa::ui::first_run::preference_key)] =
        std::string(oa::ui::first_run::preference_done);
    preferences_dirty_ = true;
    flush_preferences();
    if (shown_language().tag != "de")
        set_language_choice("de");

    const update::ThisBuild built = running_build();
    const int64_t first_release = built.release + 1;
    const int64_t higher_release = built.release + 2;
    SignedFeed offered_feed = sign_feed(feed_json(first_release, built.package), kKeyId, 0x61);
    fixture::Server server;
    server.set_idle_close_ms(120000);
    std::string listen_error;
    require(server.start(&listen_error), "the server did not listen: " + listen_error);
    server.serve_bytes("/v1/engine/stable.json", offered_feed.bytes, "application/json");
    server.serve_bytes("/v1/engine/stable.json.sig", offered_feed.signature, "text/plain");
    options_.update_feed = server.url("/v1/engine/stable.json");
    options_.update_keys = {std::string(kKeyId) + "=" + offered_feed.key_text};

    bool running = true;
    const auto deadline_from = [](std::chrono::seconds limit) {
        return std::chrono::steady_clock::now() + limit;
    };
    const auto shown_update = [this]() -> QuestionScreen* {
        LayerScreen* screen = oa_layer().find(QuestionScreen::screen_name);
        if (!is_update_question(screen))
            return nullptr;
        return static_cast<QuestionScreen*>(screen);
    };
    const auto clear_blockers = [this]() {
        dialogs::reset_dialogs();
        OaLayer& layer = oa_layer();
        for (int pass = 0; pass < 8; ++pass) {
            LayerScreen* top = layer.top();
            if (top == nullptr || is_update_question(top) || top->name() == "settings")
                return;
            layer.close(top);
        }
    };
    const auto wait_shown = [&](std::string_view why) {
        const auto deadline = deadline_from(std::chrono::seconds{30});
        while (shown_update() == nullptr) {
            require(std::chrono::steady_clock::now() < deadline, why);
            clear_blockers();
            idle_tick();
        }
    };
    const auto close_update = [&]() {
        OaLayer& layer = oa_layer();
        if (LayerScreen* screen = layer.find(QuestionScreen::screen_name))
            if (is_update_question(screen))
                layer.close(screen);
        if (LayerScreen* screen = layer.waiting(QuestionScreen::screen_name))
            if (is_update_question(screen))
                layer.close(screen);
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
        const auto deadline = deadline_from(std::chrono::seconds{5});
        std::optional<AutomationControl> control = named(name);
        while (!control) {
            require(
                std::chrono::steady_clock::now() < deadline, std::string(name) + " is not shown"
            );
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
    const auto force_check = [&]() {
        update::write_checked(preference_values_, 1000);
        preferences_dirty_ = true;
        flush_preferences();
        if (update_ != nullptr)
            update_->ran = false;
        start_update_check(true);
    };

    // 1. German notes, then English when the language has none of its own.
    {
        wait_shown("the question did not open in German");
        const kit::Question& german = shown_update()->question();
        require(
            paragraph_has(german, kGermanOne) && paragraph_has(german, kGermanTwo),
            "the German question did not show the German notes"
        );
        require(
            !paragraph_has(german, kEnglishOne) && !paragraph_has(german, kEnglishTwo),
            "the German question showed the English notes"
        );
        require(
            paragraph_has(german, "9.9.9") || german.title.find("9.9.9") != std::string::npos,
            "the question did not name the release"
        );
        require(
            german.buttons.size() == 3 && german.buttons[0].id == "skip" &&
                german.buttons[1].id == "later" && german.buttons[2].id == "update" &&
                german.cancel_button == 1 && german.primary_button == 2,
            "the question's buttons were not Skip, Later and Update"
        );
        close_update();
        set_language_choice("fr");
        if (update_ != nullptr)
            update_->offered_release.reset();
        clear_blockers();
        wait_shown("the question did not open in French");
        const kit::Question& french = shown_update()->question();
        require(
            paragraph_has(french, kEnglishOne) && paragraph_has(french, kEnglishTwo),
            "the French question did not fall back to the English notes"
        );
        require(
            !paragraph_has(french, kGermanOne) && !paragraph_has(french, kGermanTwo),
            "the French question showed the German notes"
        );
        std::cout << "update-offer: 1\n";
        std::cout.flush();
    }

    // 2. A skirmish that is already running when the offer is copied shows nothing.
    {
        close_update();
        clear_blockers();
        force_check();
        require(update_ != nullptr && update_->checker != nullptr, "the check did not start");
        const auto quiet = deadline_from(std::chrono::seconds{30});
        while (update_->checker->snapshot().state != update::CheckState::offered) {
            const update::CheckSnapshot latest = update_->checker->snapshot();
            require(
                std::chrono::steady_clock::now() < quiet,
                "the check did not offer before the skirmish (" + latest.failure + ")"
            );
            std::this_thread::sleep_for(std::chrono::milliseconds{20});
        }
        if (update_ != nullptr)
            update_->offered_release.reset();
        start_benchmark_skirmish();
        require(screen_ == Screen::match, "the skirmish did not start");
        require(
            shown_update() == nullptr &&
                !is_update_question(oa_layer().waiting(QuestionScreen::screen_name)),
            "the question opened during the skirmish"
        );
        for (int frame = 0; frame < 8; ++frame)
            idle_tick();
        require(screen_ == Screen::match, "the skirmish ended");
        require(
            update_snapshot().state == update::CheckState::offered,
            "the skirmish did not keep the offer"
        );
        require(
            shown_update() == nullptr &&
                !is_update_question(oa_layer().waiting(QuestionScreen::screen_name)),
            "the question opened while the skirmish ran"
        );
        leave_match();
        load(Screen::main_menu);
        wait_shown("the question did not open after the skirmish");
        std::cout << "update-offer: 2\n";
        std::cout.flush();
    }

    // 3. A message box keeps the question waiting.
    {
        close_update();
        if (update_ != nullptr)
            update_->offered_release.reset();
        clear_blockers();
        show_frontend_message(
            "Hold the update", 0x140, entry::message_show_ok, entry::message_fit_width
        );
        require(dialogs::dialog_count() != 0, "the message box did not open");
        for (int frame = 0; frame < 4; ++frame)
            idle_tick();
        require(shown_update() == nullptr, "the question opened over the message box");
        require(
            is_update_question(oa_layer().waiting(QuestionScreen::screen_name)),
            "the question did not wait for the message box"
        );
        dialogs::close_dialog();
        wait_shown("the question did not open after the message box");
        std::cout << "update-offer: 3\n";
        std::cout.flush();
    }

    // 4. Settings keeps the question waiting.
    {
        close_update();
        if (update_ != nullptr)
            update_->offered_release.reset();
        clear_blockers();
        open_engine_settings_from_menu();
        require(
            engine_settings_dialog() != nullptr && oa_layer().find("settings") != nullptr,
            "Settings did not open"
        );
        for (int frame = 0; frame < 4; ++frame)
            idle_tick();
        require(shown_update() == nullptr, "the question opened over Settings");
        require(
            is_update_question(oa_layer().waiting(QuestionScreen::screen_name)),
            "the question did not wait for Settings"
        );
        if (LayerScreen* settings = oa_layer().find("settings"))
            oa_layer().close(settings);
        require(engine_settings_dialog() == nullptr, "Settings did not close");
        wait_shown("the question did not open after Settings");
        std::cout << "update-offer: 4\n";
        std::cout.flush();
    }

    // 5. Update opens the signed feed's page, once.
    {
        const std::size_t before = web_links_->requests.size();
        click_named("oa.prompt.update");
        require(web_links_->requests.size() == before + 1, "Update did not open one page");
        require(web_links_->requests.back() == kPage, "Update did not open the feed's page");
        require(shown_update() == nullptr, "Update left the question open");
        std::cout << "update-offer: 5\n";
        std::cout.flush();
    }

    // 6. Skip remembers the release. The same feed stays quiet. A higher one asks.
    {
        if (update_ != nullptr)
            update_->offered_release.reset();
        wait_shown("the question did not return for Skip");
        click_named("oa.prompt.skip");
        const std::optional<int64_t> skipped = update::read_skipped(preference_values_);
        require(
            skipped.has_value() && *skipped == first_release, "Skip did not remember the release"
        );
        require(shown_update() == nullptr, "Skip left the question open");
        if (update_ != nullptr)
            update_->offered_release.reset();
        force_check();
        const auto same = deadline_from(std::chrono::seconds{30});
        while (update_snapshot().state == update::CheckState::checking ||
               (update_ != nullptr && update_->checker != nullptr &&
                update_->checker->snapshot().state == update::CheckState::checking)) {
            require(
                std::chrono::steady_clock::now() < same, "the skipped feed's check did not finish"
            );
            idle_tick();
        }
        for (int frame = 0; frame < 6; ++frame)
            idle_tick();
        require(
            shown_update() == nullptr &&
                !is_update_question(oa_layer().waiting(QuestionScreen::screen_name)),
            "the skipped release asked again"
        );
        if (update_ != nullptr)
            update_->offered_release = first_release;
        const SignedFeed higher = sign_feed(feed_json(higher_release, built.package), kKeyId, 0x61);
        server.serve_bytes("/v1/engine/stable.json", higher.bytes, "application/json");
        server.serve_bytes("/v1/engine/stable.json.sig", higher.signature, "text/plain");
        force_check();
        wait_shown("a higher release did not ask");
        require(
            update_ != nullptr && update_->offered_release.has_value() &&
                *update_->offered_release == higher_release,
            "the higher release was not the one asked"
        );
        std::cout << "update-offer: 6\n";
        std::cout.flush();
    }

    // 7. Later writes nothing, and the next run asks again.
    {
        const std::optional<int64_t> before = update::read_skipped(preference_values_);
        click_named("oa.prompt.later");
        require(update::read_skipped(preference_values_) == before, "Later wrote a preference");
        require(shown_update() == nullptr, "Later left the question open");
        if (update_ != nullptr)
            update_->offered_release.reset();
        wait_shown("a new run did not ask again");
        require(
            update::read_skipped(preference_values_) == before, "asking again wrote a preference"
        );
        std::cout << "update-offer: 7\n";
        std::cout.flush();
    }

    // 8. Escape answers Later.
    {
        const std::optional<int64_t> before = update::read_skipped(preference_values_);
        press_escape();
        require(shown_update() == nullptr, "Escape left the question open");
        require(update::read_skipped(preference_values_) == before, "Escape skipped the release");
        std::cout << "update-offer: 8\n";
        std::cout.flush();
    }
}

} // namespace oa::app

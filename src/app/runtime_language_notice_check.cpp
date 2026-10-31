// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-language-notice: over a settled main menu, a language that is only
// available shows its notice. The chosen case stores English. The system
// case remembers the language so the notice does not return.

#include "oa_layer.hpp"
#include "user_folder_state.hpp"

#include "oa/app/runtime.hpp"
#include "oa/data/languages.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/theme.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace oa::app {

namespace {

namespace languages = oa::data::languages;
namespace settings = oa::ui::engine_settings;
namespace kit = oa::ui::kit;
namespace preferences = oa::platform::preferences;

/// The notice's name on the OA layer.
constexpr std::string_view kScreenName = "language-notice";
/// Use English.
constexpr std::string_view kUseEnglish = "language-notice.use-english";
/// Download.
constexpr std::string_view kDownload = "language-notice.download";

/// Stops the check with a reason.
///
/// @param what what went wrong
[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("language notice check: " + what);
}

/// Returns a preference, trimmed. Empty when the key is absent.
///
/// @param values the preferences
/// @param key the key
/// @return the value
std::string preference_of(const preferences::Values& values, std::string_view key) {
    const auto found = values.find(std::string(key));
    if (found == values.end())
        return {};
    return std::string(settings::language_notice_trim(found->second));
}

/// Requires a fact.
///
/// @param held the fact
/// @param what what went wrong when it does not hold
void require(bool held, const std::string& what) {
    if (!held)
        fail(what);
}

/// Names one part of the notice, and requires the modern fonts to draw it.
///
/// @param runtime the runtime
/// @param part the part
void name_part(Runtime& runtime, std::string_view part) {
    if (part.empty())
        return;
    std::cout << "language notice: " << part << '\n';
    require(runtime.modern_fonts_draw(part), "a part does not draw: " + std::string(part));
}

/// Returns the path of one step's snapshot beside --snapshot.
///
/// @param snapshot the --snapshot path
/// @param step the step's name
/// @return the step's snapshot path
fs::path step_snapshot(const fs::path& snapshot, std::string_view step) {
    fs::path file = snapshot.parent_path() / snapshot.stem();
    file += '-' + std::string(step) + ".ppm";
    return file;
}

} // namespace

void Runtime::check_language_notice() {
    if (!options_.preferences_file)
        fail("needs --preferences-file");
    if (sdl_.window == nullptr || sdl_.renderer == nullptr)
        fail("the window did not open");

    const std::string raw = preference_of(preference_values_, settings::key::language);
    const bool chosen = settings::language_notice_same_tag(raw, "zh-Hans");
    const bool system = settings::language_notice_same_tag(raw, languages::system_choice);
    require(chosen || system, "the language preference is neither zh-Hans nor system");

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
    require(modern_fonts_open(), "the modern fonts did not open");

    user_folder_state().check_shows_notice = true;
    for (int pass = 0; pass < 6; ++pass)
        tell_missing_language();

    LayerScreen* screen = oa_layer().find(kScreenName);
    require(screen != nullptr, "the notice did not show over the main menu");
    require(oa_layer().waiting(kScreenName) == nullptr, "the notice is still waiting");

    const LayerPlacement placed = screen->placement(oa_layer().view());
    require(
        placed.points_width == kit::metrics_of(kit::SizeClass::compact).notice_width,
        "the notice is not laid out at Compact"
    );

    const kit::DisplayList list = screen->display_list();
    for (const kit::Item& item : list.items) {
        if (item.role == kit::Role::header) {
            if (const auto* header = std::get_if<kit::HeaderLook>(&item.look))
                name_part(*this, header->title);
            name_part(*this, item.text);
        }
        if (item.role == kit::Role::text)
            name_part(*this, item.text);
        if (item.role == kit::Role::button) {
            if (const auto* button = std::get_if<kit::ButtonLook>(&item.look))
                name_part(*this, button->caption);
        }
    }
    std::cout.flush();

    const languages::PackNotice* lines = languages::pack_notice("zh-Hans");
    const languages::Language* language = languages::find_by_tag("zh-Hans");
    require(lines != nullptr && language != nullptr, "zh-Hans has no notice lines");
    const std::string english_body = std::string(language->english_name) +
                                     " is now a separate download. Until it is installed, the "
                                     "game shows English.";
    const kit::Control* body = nullptr;
    const kit::Control* use = nullptr;
    const kit::Control* download = nullptr;
    for (const kit::Control& control : list.controls) {
        if (control.name == std::string(kScreenName) + ".body")
            body = &control;
        if (control.name == kUseEnglish)
            use = &control;
        if (control.name == kDownload)
            download = &control;
    }
    require(body != nullptr, "the notice has no body");
    require(use != nullptr, "the notice has no Use English button");
    require(download != nullptr, "the notice has no Download button");
    require(
        body->text.find(language->endonym) != std::string::npos,
        "the title is not the language's own name"
    );
    require(body->text.find(lines->body) != std::string::npos, "the sizeless body is not shown");
    int own_lines = 0;
    for (const kit::Item& item : list.items)
        if (item.role == kit::Role::text && item.text == lines->body)
            ++own_lines;
    require(own_lines == 1, "the Chinese body is not one line");
    require(body->text.find(english_body) != std::string::npos, "the English body is not shown");
    require(
        body->text.find(lines->unavailable) != std::string::npos,
        "the notice does not say the pack cannot be downloaded"
    );
    require(
        download->text == lines->download && !download->enabled,
        "Download is not the disabled download button"
    );
    require(
        download->text.find("It cannot be downloaded now.") == std::string::npos,
        "Download's caption is the status"
    );

    if (!options_.snapshot.empty()) {
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        write_ppm(step_snapshot(options_.snapshot, "notice"), presented);
        require(
            presented.width == static_cast<uint32_t>(kCanvasWidth) &&
                presented.height == static_cast<uint32_t>(kCanvasHeight),
            "the snapshot is not 640x480"
        );
    }

    const int32_t scale = placed.points_width > 0 ? placed.shown.width / placed.points_width : 1;
    const oa::ui::display_layout::Point at{
        placed.shown.x + (use->rect.x + use->rect.width / 2) * scale,
        placed.shown.y + (use->rect.y + use->rect.height / 2) * scale
    };
    send_check_pointer(SDL_EVENT_MOUSE_MOTION, at, 0);
    send_check_pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, at, SDL_BUTTON_LEFT);
    send_check_pointer(SDL_EVENT_MOUSE_BUTTON_UP, at, SDL_BUTTON_LEFT);
    require(oa_layer().find(kScreenName) == nullptr, "Use English did not close the notice");
    require(oa_layer().waiting(kScreenName) == nullptr, "Use English left the notice waiting");

    const preferences::Values stored = preferences::load(preference_path_);
    if (system) {
        require(
            preference_of(stored, settings::key::language) == raw,
            "Use English changed the system language setting"
        );
        const std::string dismissed =
            preference_of(stored, settings::key::language_notice_dismissed);
        require(
            settings::language_notice_dismissed_has(dismissed, "zh-Hans"),
            "Use English did not remember zh-Hans"
        );
        tell_missing_language();
        require(
            oa_layer().find(kScreenName) == nullptr && oa_layer().waiting(kScreenName) == nullptr,
            "the notice returned after Use English"
        );
        return;
    }

    require(
        preference_of(stored, settings::key::language) == "en", "Use English did not store English"
    );
    for (int pass = 0; pass < 300; ++pass)
        tell_missing_language();
    require(
        oa_layer().find(kScreenName) == nullptr && oa_layer().waiting(kScreenName) == nullptr,
        "the notice returned after Use English"
    );
}

} // namespace oa::app

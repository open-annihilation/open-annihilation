// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The question a verified release asks at the main menu. It waits through a
// match, Settings and a message box. Skip remembers the release, Later
// writes nothing, and Update downloads the package where this copy can
// install it, or opens the release's page. A release held after it failed
// to install, and one whose package is already downloaded, do not ask.
#include "oa/app/runtime.hpp"

#include "oa_layer.hpp"
#include "update_state.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/update/build.hpp"
#include "oa/app/update/centre.hpp"
#include "oa/app/update/choices.hpp"
#include "oa/app/update/offer.hpp"
#if defined(OA_SELF_UPDATE) && OA_SELF_UPDATE
#include "oa/app/update/download.hpp"
#endif
#include "oa/app/update/route.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/ui/library/text.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace update = oa::app::update;
namespace library = oa::ui::library;
namespace kit = oa::ui::kit;
namespace feed = oa::data::engine_feed;

/// The button that downloads the release, until the update centre names it.
constexpr std::string_view kDownloadCaption = "Download update";
/// Skip this version's caption, until the update centre names it.
constexpr std::string_view kSkipCaption = "Skip this version";
/// Later's caption, until the update centre names it.
constexpr std::string_view kLaterCaption = "Later";
/// The heading of the release's notes.
constexpr std::string_view kWhatsNew = "What's new";
/// The line that says a match pauses the download.
constexpr std::string_view kMatchPause = "Downloads pause during a match and carry on after it.";
/// The button Skip this version answers: quiet, at the footer's left.
constexpr int32_t kSkipButton = 0;
/// The button Later answers, and the one Escape answers.
constexpr int32_t kLaterButton = 1;
/// The button Update answers, and the one the keys mark.
constexpr int32_t kUpdateButton = 2;

/// Returns one of the interface's words in the language shown.
///
/// @param context unused
/// @param english the word, in English
/// @return its translation, or the English
std::string_view shown_word(void* /*context*/, std::string_view english) {
    return oa::data::languages::interface_text(english);
}

/// The hooks the question looks its words up through.
///
/// @return the hooks
library::TextHooks text_hooks() {
    return library::TextHooks{nullptr, shown_word};
}

/// This program's build. The base path is cached for the process.
update::ThisBuild running_build() {
    // SDL caches this string for the process. The caller does not free it.
    const char* const base = SDL_GetBasePath();
    if (base == nullptr)
        return update::this_build({});
    return update::this_build(path_from_utf8(base));
}

/// The update question, named by its Skip button. Another question uses the
/// same word.
///
/// @param screen a screen of the layer, or null
/// @return true when it is the update question
bool is_update_question(const LayerScreen* screen) {
    if (screen == nullptr || screen->name() != QuestionScreen::screen_name)
        return false;
    const kit::Question& asked = static_cast<const QuestionScreen*>(screen)->question();
    return !asked.buttons.empty() && asked.buttons.front().id == "skip";
}

/// The notice a build that cannot replace itself shows. Its word is its
/// screen name, so it is not the update question and not another prompt.
constexpr std::string_view kPackagerNotice = "update-notice";

/// The packager notice on the layer or waiting to be shown.
///
/// @param layer the layer
/// @return true when the notice is up or waiting
bool packager_notice_queued(OaLayer& layer) {
    return layer.find(kPackagerNotice) != nullptr || layer.waiting(kPackagerNotice) != nullptr;
}

/// The update question or the packager notice, up or waiting.
///
/// @param layer the layer
/// @return true when either is up or waiting
bool update_question_queued(OaLayer& layer) {
    return is_update_question(layer.find(QuestionScreen::screen_name)) ||
           is_update_question(layer.waiting(QuestionScreen::screen_name)) ||
           packager_notice_queued(layer);
}

/// The button OPEN THE PAGE answers, when the notice offers it.
constexpr int32_t kPageButton = 0;

#if defined(OA_SELF_UPDATE) && OA_SELF_UPDATE
/// True when the offered package was already downloaded, so the ready prompt
/// asks instead of this question.
///
/// @param options the run's options, which name the data folder
/// @param offer the offer
/// @return true when its zip or its installer is kept under `<data>/updates`
bool offered_package_downloaded(const Options& options, const feed::Offer& offer) {
    if (offer.release == nullptr || offer.package == nullptr)
        return false;
    try {
        const std::filesystem::path data =
            options.data_dir ? *options.data_dir : oa::platform::preferences::data_directory();
        return update::package_downloaded(data / "updates", *offer.release, *offer.package);
    } catch (const std::exception&) {
        return false;
    }
}
#endif

} // namespace

void Runtime::offer_update() {
    if (update_ == nullptr)
        return;
    if (match_ != nullptr || screen_ == Screen::match)
        return;
    // The main menu, including while a dialog or Settings keeps the question
    // waiting. A match, a package's frame and the way into multiplayer do not ask.
    if (screen_ != Screen::main_menu || frame_owned_by_package() ||
        state_.state != frontend::state_id::main_menu ||
        state_.pending_signal == frontend::signal_id::multiplayer)
        return;
    // An unattended or headless run stays quiet unless this run named a feed.
    if (!options_.update_feed.has_value() && (options_.unattended || options_.headless_check))
        return;
    // A copy macOS is running from a protected place cannot be replaced.
    // The move prompt is the one this run asks.
#if defined(OA_SELF_UPDATE) && OA_SELF_UPDATE
    if (update_->mac_translocated)
        return;
#endif
    const update::CheckSnapshot& held = update_->held;
    if (held.state != update::CheckState::offered)
        return;
    if (!update::offer_wanted(held.offer, update::read_skipped(preference_values_)))
        return;
    // --update-and-quit and Download in the background fetch without asking.
    if (options_.update_and_quit || background_download_covers_offer())
        return;
    const int64_t release_number = held.offer.release->release;
    // A release that did not install waits for the player's Try again, and
    // its receipt says so. A package already downloaded asks with the ready
    // prompt instead, and a download already running does not ask again.
    if (update::release_held(preference_values_, release_number))
        return;
#if defined(OA_SELF_UPDATE) && OA_SELF_UPDATE
    if (update::replaces_itself() && held.offer.way == feed::Way::in_place &&
        offered_package_downloaded(options_, held.offer))
        return;
#endif
    // Asked once this run for this release. A match that closed the question
    // cleared the number, so the next settled menu asks again. A higher
    // release still asks.
    if (update_->offered_release.has_value() && *update_->offered_release >= release_number)
        return;
    OaLayer& layer = oa_layer();
    if (update_question_queued(layer))
        return;
#if defined(OA_SELF_UPDATE) && OA_SELF_UPDATE
    // A copy installed for the whole system cannot replace itself. Say so
    // once this run, instead of asking to update.
    {
        const char* const base = SDL_GetBasePath();
        const std::filesystem::path folder =
            base == nullptr ? std::filesystem::path{} : path_from_utf8(base);
        update::RouteFacts facts = update::this_machine(folder);
        facts.install_folder = options_.update_install_folder;
        facts.test_feed = options_.update_feed.has_value();
        facts.way = held.offer.way;
        const update::Where where = update::find_route(update::this_build(folder), facts);
        if (where.why == update::whole_system_copy) {
            const std::string offered_version = held.offer.release->version_text;
            kit::Question asked;
            asked.word = "system-install";
            asked.title = library::filled(
                text_hooks(), update::version_is_out, {{"version", offered_version}}
            );
            asked.paragraphs.push_back(
                {std::string(oa::data::languages::interface_text(update::whole_system_copy)), false}
            );
            const std::string ok{oa::data::languages::interface_text("OK")};
            asked.buttons.push_back({ok, true, "ok"});
            asked.cancel_button = 0;
            asked.primary_button = 0;
            asked.marked = 0;
            update_->offered_release = release_number;
            Runtime* runtime = this;
            QuestionScreen::Host host;
            host.answer = [](int32_t) { return true; };
            // A match closes the notice. The number stays set, so this run
            // does not show it again.
            host.tick = [runtime]() {
                return runtime->match_ != nullptr || runtime->screen_ == Screen::match;
            };
            auto question = std::make_unique<QuestionScreen>(
                layer, Screen::main_menu, std::move(asked), std::move(host)
            );
            question->show_on_any_menu();
            layer.show_when_free(std::move(question));
            return;
        }
    }
#endif
    // A build that cannot replace itself tells the player who updates it.
    // OK asks again next start and does not remember a skip. With no
    // packager, OPEN THE PAGE opens the release's page.
    if (!update::replaces_itself()) {
        const std::string offered_version = held.offer.release->version_text;
        const update::PackagerNotice notice = update::packager_notice(
            offered_version,
            update::packager_name(),
            update::packager_update_command(),
            shown_word,
            nullptr
        );
        kit::Question asked;
        asked.word = std::string(kPackagerNotice);
        asked.title = notice.title;
        for (const std::string& paragraph : notice.paragraphs)
            asked.paragraphs.push_back({paragraph, false});
        const std::string open_page{oa::data::languages::interface_text("OPEN THE PAGE")};
        const std::string ok{oa::data::languages::interface_text("OK")};
        if (notice.open_the_page) {
            asked.buttons.push_back({open_page, true, "page"});
            asked.buttons.push_back({ok, false, "ok"});
            asked.cancel_button = 1;
            asked.primary_button = kPageButton;
            asked.marked = kPageButton;
        } else {
            asked.buttons.push_back({ok, true, "ok"});
            asked.cancel_button = 0;
            asked.primary_button = 0;
            asked.marked = 0;
        }
        update_->offered_page = held.offer.release->page;
        update_->offered_release = release_number;
        Runtime* runtime = this;
        const bool open_the_page = notice.open_the_page;
        QuestionScreen::Host host;
        host.answer = [runtime, open_the_page](int32_t button) {
            if (open_the_page && button == kPageButton)
                runtime->open_release_page();
            return true;
        };
        // A match closes the notice without an answer, and the next settled
        // menu of this run asks again. OK leaves the number set.
        host.tick = [runtime]() {
            const bool in_match = runtime->match_ != nullptr || runtime->screen_ == Screen::match;
            if (in_match && runtime->update_)
                runtime->update_->offered_release.reset();
            return in_match;
        };
        auto question = std::make_unique<QuestionScreen>(
            layer, Screen::main_menu, std::move(asked), std::move(host)
        );
        question->show_on_any_menu();
        layer.show_when_free(std::move(question));
        return;
    }
    const library::TextHooks hooks = text_hooks();
    const update::OfferText told =
        update::offer_text(held.offer, running_build(), shown_language().tag, hooks);
    if (told.title.empty() || told.paragraphs.size() != 2)
        return;
    // The size and the actions are the update centre's, so the question,
    // Settings › Updates and the menu pill name one size and one state.
    const update::Centre centre = update_centre();
    const update::CentreWords words = update::centre_words(centre, hooks);
    const bool about_offer = centre.target_release == release_number;
    std::string leading{oa::data::languages::interface_text(kDownloadCaption)};
    std::string skip{oa::data::languages::interface_text(kSkipCaption)};
    std::string later{oa::data::languages::interface_text(kLaterCaption)};
    if (about_offer) {
        for (const update::ActWords& act : words.actions) {
            if (act.act == update::Act::download || act.act == update::Act::open_page)
                leading = act.caption;
            else if (act.act == update::Act::skip)
                skip = act.caption;
            else if (act.act == update::Act::later)
                later = act.caption;
        }
    }

    kit::Question asked;
    asked.title = told.title;
    asked.paragraphs.push_back(
        {about_offer && !words.detail.empty() ? words.detail : told.paragraphs[0], false}
    );
    if (!told.notes.empty()) {
        kit::QuestionPart notes;
        notes.kind = kit::PartKind::boxes;
        kit::QuestionBox box;
        box.heading = std::string(oa::data::languages::interface_text(kWhatsNew));
        for (const std::string& line : told.notes)
            box.lines.push_back({kit::LineMark::bullet, line});
        notes.boxes.push_back(std::move(box));
        asked.parts.push_back(std::move(notes));
    }
    {
        kit::QuestionPart stay;
        stay.kind = kit::PartKind::text;
        stay.text = told.paragraphs[1] + " " +
                    std::string(oa::data::languages::interface_text(kMatchPause));
        asked.parts.push_back(std::move(stay));
    }
    kit::QuestionButton skip_button{std::move(skip), false, "skip"};
    skip_button.quiet = true;
    asked.buttons.push_back(std::move(skip_button));
    asked.buttons.push_back({std::move(later), false, "later"});
    asked.buttons.push_back({std::move(leading), true, "update"});
    asked.cancel_button = kLaterButton;
    asked.primary_button = kUpdateButton;
    asked.marked = kUpdateButton;

    // The page this question opens, taken now, so a later check cannot change it.
    update_->offered_page = held.offer.release->page;
    update_->offered_release = release_number;

    Runtime* runtime = this;
    QuestionScreen::Host host;
    host.answer = [runtime](int32_t button) {
        if (button == kSkipButton && runtime->update_ && runtime->update_->offered_release) {
            update::write_skipped(runtime->preference_values_, *runtime->update_->offered_release);
            runtime->preferences_dirty_ = true;
            try {
                runtime->flush_preferences();
            } catch (const std::exception& failure) {
                std::cerr << "open-annihilation: update check: failed: " << failure.what() << '\n';
            }
        } else if (button == kUpdateButton) {
            if (!runtime->start_update_download())
                runtime->open_release_page();
        }
        return true;
    };
    // A match closes the question without an answer, and the next settled
    // menu of this run asks again. Later, Skip and Update leave the number set.
    host.tick = [runtime]() {
        const bool in_match = runtime->match_ != nullptr || runtime->screen_ == Screen::match;
        if (in_match && runtime->update_)
            runtime->update_->offered_release.reset();
        return in_match;
    };
    auto question = std::make_unique<QuestionScreen>(
        layer, Screen::main_menu, std::move(asked), std::move(host)
    );
    question->show_on_any_menu();
    layer.show_when_free(std::move(question));
}

void Runtime::open_release_page() {
    std::string page;
    if (update_ && update_->offered_page.has_value())
        page = *update_->offered_page;
    else if (update_ && update_->held.offer.release != nullptr)
        page = update_->held.offer.release->page;
    if (page.empty())
        page = std::string(update::words::releases_page);
    if (web_address_available()) {
        open_web_link(page);
        return;
    }
    const library::TextHooks hooks = text_hooks();
    kit::Question asked;
    asked.title = std::string(oa::data::languages::interface_text("Open Annihilation"));
    asked.paragraphs.push_back(
        {library::filled(hooks, update::words::game_mode, {{"page", page}}), false}
    );
    asked.buttons.push_back({std::string(oa::data::languages::interface_text("OK")), true, "ok"});
    asked.cancel_button = 0;
    asked.primary_button = 0;
    asked.marked = 0;
    QuestionScreen::Host host;
    host.answer = [](int32_t) { return true; };
    // The player already chose Update. Closing this for a match does not ask again.
    Runtime* runtime = this;
    host.tick = [runtime]() {
        return runtime->match_ != nullptr || runtime->screen_ == Screen::match;
    };
    OaLayer& layer = oa_layer();
    auto question = std::make_unique<QuestionScreen>(
        layer, Screen::main_menu, std::move(asked), std::move(host)
    );
    question->show_on_any_menu();
    layer.push(std::move(question));
}

} // namespace oa::app

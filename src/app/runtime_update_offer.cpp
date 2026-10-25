// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The question a verified release asks at the main menu. It waits through a
// match, Settings and a message box. Skip remembers the release, Later
// writes nothing, and Update opens the release's page.
#include "oa/app/runtime.hpp"

#include "oa_layer.hpp"
#include "update_state.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/update/build.hpp"
#include "oa/app/update/offer.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/ui/library/text.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <exception>
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

/// The button Skip this version answers.
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

/// One bullet of the release's notes.
///
/// @param line the note, as the feed wrote it
/// @return the line with a bullet
std::string bullet(const std::string& line) {
    std::string text = "• ";
    text += line;
    return text;
}

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
    const update::CheckSnapshot& held = update_->held;
    if (held.state != update::CheckState::offered)
        return;
    if (!update::offer_wanted(held.offer, update::read_skipped(preference_values_)))
        return;
    const int64_t release_number = held.offer.release->release;
    // Asked once this run for this release. A match that closed the question
    // cleared the number, so the next settled menu asks again. A higher
    // release still asks.
    if (update_->offered_release.has_value() && *update_->offered_release >= release_number)
        return;
    OaLayer& layer = oa_layer();
    if (update_question_queued(layer))
        return;
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

    kit::Question asked;
    asked.title = told.title;
    asked.paragraphs.push_back({told.paragraphs[0], false});
    if (!told.notes.empty()) {
        std::string notes = library::filled(
            hooks, update::words::new_in, {{"version", held.offer.release->version_text}}
        );
        for (const std::string& line : told.notes) {
            notes += '\n';
            notes += bullet(line);
        }
        asked.paragraphs.push_back({std::move(notes), false});
    }
    asked.paragraphs.push_back({told.paragraphs[1], false});
    asked.buttons.push_back(
        {std::string(oa::data::languages::interface_text(update::words::skip)), false, "skip"}
    );
    asked.buttons.push_back(
        {std::string(oa::data::languages::interface_text(update::words::later)), false, "later"}
    );
    asked.buttons.push_back(
        {std::string(oa::data::languages::interface_text(update::words::update)), true, "update"}
    );
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

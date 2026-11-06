// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Notices for the frontend entries the game data cannot support, such as
// skirmish in the Total Annihilation demo (1997), and the web link the
// notice's website button opens; and the main menu's notice of a new
// renderer record, worded for each kind: a driver that failed, a graphics
// card that could not be used, and one that could not draw the battlefield.
#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa_layer.hpp"
#include "render_host.hpp"
#include "render_run.hpp"
#include "user_folder_state.hpp"
#include "web_link_state.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_state/app_modes.hpp"
#include "oa/ui/kit/components_more.hpp"

#include <SDL3/SDL.h>

#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace dialogs = oa::ui::frontend_dialogs;

// Wrap width of the message box shown when the notice cannot be drawn.
constexpr int32_t kMessageWidth = 0x140;

[[nodiscard]] std::string_view text_or_empty(const char* text) {
    return text == nullptr ? std::string_view{} : std::string_view(text);
}

/// What the main menu says of an accelerated-unusable record.
constexpr std::string_view kCardUnusableNotice =
    "The graphics card could not be used, so the processor draws the game. You can try it "
    "again under Graphics in the OA settings.";
/// What it says of a failed-driver record, whatever tier the next driver runs.
constexpr std::string_view kDriverFailedNotice =
    "A graphics driver failed, so the game now uses another one. You can try it again under "
    "Graphics in the OA settings.";
/// What it says of a full-unusable record: the Basic tier stands.
constexpr std::string_view kFullUnusableNotice =
    "The graphics card could not draw the battlefield, so it only scales it now. You can try "
    "Full again under Graphics in the OA settings.";

/// Returns what the main menu says of a new record.
///
/// @param kind the record's kind
/// @return the notice's text
[[nodiscard]] std::string_view notice_text(renderer_state::NoticeKind kind) noexcept {
    switch (kind) {
    case renderer_state::NoticeKind::failed_driver:
        return kDriverFailedNotice;
    case renderer_state::NoticeKind::accelerated_unusable:
        return kCardUnusableNotice;
    case renderer_state::NoticeKind::full_unusable:
        return kFullUnusableNotice;
    }
    return kCardUnusableNotice;
}

/// Frames in a row the main menu shows before its notice: a start that
/// passes the main menu at its first update shows none.
constexpr uint32_t kNoticeMenuFrames = 2;

/// Tells whether the main menu shows as itself: its own frame, not a screen
/// package's, with no multiplayer signal waiting to leave it, as -n leaves
/// one until the next pass of the frontend, which may be many frames on.
///
/// @param screen the screen shown
/// @param state the frontend's state
/// @param owned a screen package owns the frame
/// @return true when the main menu shows and stays
[[nodiscard]] bool
main_menu_settled(Screen screen, const oa::ui::frontend_state::State& state, bool owned) noexcept {
    return screen == Screen::main_menu && !owned &&
           state.state == oa::ui::frontend_state::state_id::main_menu &&
           state.pending_signal != oa::ui::frontend_state::signal_id::multiplayer;
}

} // namespace

bool answer_multiplayer_entry(Runtime& runtime) {
    // The same two answers the menu's own MULTI gives before it opens.
    if (runtime.eligible_map_names_.empty()) {
        runtime.play_menu_sound(menu::Sound::big_button);
        runtime.show_missing_content(Runtime::MissingContent::multiplayer_maps);
        return true;
    }
    if (runtime.refuse_incomplete_mod_start()) {
        runtime.play_menu_sound(menu::Sound::big_button);
        return true;
    }
    return false;
}

void Runtime::show_missing_content(MissingContent missing) {
    std::string_view text;
    switch (missing) {
    case MissingContent::skirmish_maps:
        text = "This game includes no skirmish maps, so skirmish is not available.";
        break;
    case MissingContent::multiplayer_maps:
        text = "This game includes no multiplayer maps, so multiplayer is not available.";
        break;
    case MissingContent::further_missions:
        text = "Campaign complete. This game includes no further missions.";
        break;
    }
    // The engine's own words, in the player's language when the interface
    // catalogue has them.
    text = oa::data::languages::interface_text(text);
    auto context = screen_context();
    const auto closed = [](void* runtime, dialogs::NoticeChoice choice) {
        auto& owner = *static_cast<Runtime*>(runtime);
        if (choice == dialogs::NoticeChoice::website)
            owner.open_web_link(project_website_address);
        else
            owner.notice_returns_to_main_menu_ = true;
    };
    if (dialogs::open_notice(
            &context, text, web_link_caption(project_website_address), this, closed
        ))
        return;
    show_frontend_message(
        std::string(text), kMessageWidth, entry::message_show_ok, entry::message_fit_width
    );
}

void Runtime::run_pending_notice_return() {
    if (!notice_returns_to_main_menu_)
        return;
    notice_returns_to_main_menu_ = false;
    if (screen_ == Screen::main_menu)
        return;
    frontend::reset_to_main_menu(state_, *this);
    set_app_mode(state_, frontend::mode_id::frontend);
    step(frontend::Step::reload_unit_overrides, state_);
    frontend::dispatch(state_, *this, frontend_states_);
}

void Runtime::tell_renderer_records() {
    if (!render_run_ || render_run_->host == nullptr)
        return;
    auto& run = *render_run_;
    if (!main_menu_settled(screen_, state_, frame_owned_by_package())) {
        run.main_menu_frames = 0;
        return;
    }
    if (++run.main_menu_frames < kNoticeMenuFrames || dialogs::dialog_count() != 0 ||
        engine_settings_dialog() != nullptr || saves_notice_shown())
        return;
    auto& records = run.host->records();
    // Most frames have nothing to tell, and cost no more than this.
    if (!renderer_state::has_untold_record(records.records()))
        return;
    std::vector<std::string> passed_over;
    for (const auto& noted : run.notices_noted)
        passed_over.push_back(noted.driver);
    const auto notice = renderer_state::next_notice(records.records(), passed_over);
    if (!notice)
        return;
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver == nullptr)
        driver = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    const bool unwatched =
        options_.unattended ||
        unattended_environment(text_or_empty(SDL_getenv("CI")), text_or_empty(driver));
    const auto action = renderer_state::notice_action(unwatched, options_.check_renderer_ladder);
    if (action != renderer_state::NoticeAction::show)
        run.notices_noted.push_back(*notice);
    if (action != renderer_state::NoticeAction::note) {
        // Later starts that merely honour the record show nothing.
        std::ignore = renderer_state::mark_told(records.records(), notice->driver);
        std::ignore = records.write_records();
    }
    if (action != renderer_state::NoticeAction::show)
        return;
    ++run.notices_shown;
    const std::string_view text = oa::data::languages::interface_text(notice_text(notice->kind));
    auto context = screen_context();
    const auto closed = [](void* runtime, dialogs::NoticeChoice choice) {
        if (choice == dialogs::NoticeChoice::website)
            static_cast<Runtime*>(runtime)->open_web_link(project_website_address);
    };
    if (dialogs::open_notice(
            &context, text, web_link_caption(project_website_address), this, closed
        ))
        return;
    show_frontend_message(
        std::string(text), kMessageWidth, entry::message_show_ok, entry::message_fit_width
    );
}

void Runtime::destroy_web_link_state(WebLinkState* state) noexcept {
    delete state;
}

void Runtime::open_web_link(std::string_view address) {
    if (!web_links_ || web_links_->hooks.open == nullptr)
        return;
    const auto& hooks = web_links_->hooks;
    const std::string text(address);
    if (!hooks.open(hooks.context, text.c_str()))
        status_ = "cannot open " + text + ": " + SDL_GetError();
}

const ContentCheck& Runtime::content_check() const noexcept {
    return content_check_;
}

void Runtime::tell_content_difference() {
    // Most starts match, and most frames have nothing to tell.
    if (content_notice_closed_ || options_.game_dir_named || !content_notice_due(content_check_))
        return;
    constexpr std::string_view notice_word = "content-notice";
    if (oa_layer_ != nullptr &&
        (oa_layer_->find(notice_word) != nullptr || oa_layer_->waiting(notice_word) != nullptr))
        return;
    if (!main_menu_settled(screen_, state_, frame_owned_by_package())) {
        content_notice_frames_ = 0;
        return;
    }
    if (++content_notice_frames_ < kNoticeMenuFrames || dialogs::dialog_count() != 0 ||
        engine_settings_dialog() != nullptr || engine_settings_fonts() == nullptr)
        return;
    // A run nobody watches leaves it for one someone does, unless a check
    // asked for the main menu's notices.
    if (UserFolderState::unwatched(options_.unattended) && !user_folder_state().check_shows_notice)
        return;
    const ContentNoticeCopy copy = content_difference_notice(content_check_);
    oa::ui::kit::Notice told;
    told.title = copy.title;
    told.paragraphs.reserve(copy.lines.size());
    for (const std::string& line : copy.lines)
        told.paragraphs.push_back({line, false});
    told.open_caption = copy.details;
    told.ok_caption = copy.ok;
    told.word = std::string(notice_word);
    NoticeScreen::Host host;
    // OK and Escape close it for this start only. The next start shows it
    // again while the files differ. Nothing is written to skip it.
    host.ok = [this] {
        content_notice_closed_ = true;
        play_ui_sound("Options", 0);
    };
    // DETAILS lists the files. Settings' Game Content page replaces this
    // list; the notice stays open underneath it.
    host.open = [this] {
        const ContentNoticeCopy files = content_file_notice(content_check_);
        oa::ui::kit::Question asked;
        asked.title = files.title;
        asked.paragraphs.reserve(files.lines.size());
        for (const std::string& line : files.lines)
            asked.paragraphs.push_back({line, false});
        asked.buttons.push_back({files.ok, true, "ok"});
        asked.cancel_button = 0;
        asked.primary_button = 0;
        asked.word = "content-files";
        QuestionScreen::Host answers;
        answers.answer = [](int32_t) { return true; };
        auto& layer = oa_layer();
        layer.push(
            std::make_unique<QuestionScreen>(
                layer, Screen::main_menu, std::move(asked), std::move(answers)
            )
        );
        return std::string();
    };
    // Leaving the main menu closes it without counting as OK, so it returns
    // with the menu.
    host.tick = [this] { return screen_ != Screen::main_menu; };
    auto& layer = oa_layer();
    layer.show_when_free(
        std::make_unique<NoticeScreen>(layer, Screen::main_menu, std::move(told), std::move(host))
    );
}

void Runtime::choose_web_links() {
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver == nullptr)
        driver = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    const bool unwatched =
        options_.unattended ||
        unattended_environment(text_or_empty(SDL_getenv("CI")), text_or_empty(driver));
    if (!web_links_)
        web_links_.reset(new WebLinkState());
    web_links_->hooks = unwatched ? recorded_web_links(web_links_->requests) : browser_web_links();
}

} // namespace oa::app

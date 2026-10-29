// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The first-run setup over the main menu. A player who already has the game
// sees language, online features, then the OA buttons. A start that copied
// the game on the way in continues that walk at online features, then the
// buttons. --check-first-run walks all five steps here, including the game
// and the copy. The screen asks; this file applies the answer and writes
// the preference when the player finishes or skips. Quit does not.

#include "oa_layer.hpp"
#include "render_run.hpp"
#include "engine_settings_menu_host.hpp"
#include "engine_settings_state.hpp"
#include "language_state.hpp"

#include "oa/app/content/downloads.hpp"
#include "oa/app/content/service.hpp"
#include "oa/app/content/settings.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/setup_before_game.hpp"
#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/first_run/first_run.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_state/app_modes.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/ui/pad_controls.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace first_run = oa::ui::first_run;
namespace languages = oa::data::languages;
namespace settings = oa::ui::engine_settings;
namespace frontend_state = oa::ui::frontend_state;
namespace kit = oa::ui::kit;
namespace layout = oa::ui::display_layout;
namespace text_font = oa::platform::text_font;

/// Names the fonts a centred language adds so the welcome can draw words the
/// game font has no glyph for. Empty puts that stack back to the language in use.
void note_first_run_preview_fonts(std::vector<PackFontFile> fonts);

/// The screen's name on the OA layer.
constexpr std::string_view kScreenName = "first-run";

/// Core Prime's registry id.
constexpr std::string_view kCorePrime = "coreprime";

/// The registry `--check-first-run` serves. A played start never reads it.
constexpr std::string_view kCheckRegistry = "ridge";

/// Settled main-menu frames before the setup opens on its own.
constexpr uint32_t kMenuFrames = 2;

/// The progress bar's whole, so a large pack never narrows into a 32-bit byte count.
constexpr int32_t kProgressWhole = 1000;

class FirstRunScreen;

/// Applies what the setup asks and decides when it shows.
class FirstRunHost {
  public:

    /// Logs the gate, opens the setup when it should, and keeps an open one current.
    ///
    /// @param runtime the runtime
    static void tell(Runtime& runtime);

    /// Opens one step for a stage line. A token that is not a step does nothing.
    ///
    /// @param runtime the runtime
    /// @param token language, online or buttons
    /// @return false when the token is not a step
    static bool open_staged(Runtime& runtime, std::string_view token);

    /// Writes the finished preference.
    ///
    /// @param runtime the runtime
    static void finish(Runtime& runtime);

    /// Applies Turn on or Not now.
    ///
    /// @param runtime the runtime
    /// @param online_on Turn on online features is chosen
    static void apply_online(Runtime& runtime, bool online_on);

    /// Switches to a built-in or installed language, or queues a pack.
    ///
    /// @param runtime the runtime
    /// @param session the setup
    /// @param index the card
    static void pick(Runtime& runtime, first_run::Session& session, int32_t index);

    /// Cancels the pack that is downloading.
    ///
    /// @param runtime the runtime
    static void cancel_download(Runtime& runtime);

    /// Tries the selected pack again.
    ///
    /// @param runtime the runtime
    /// @param session the setup
    static void retry_download(Runtime& runtime, const first_run::Session& session);

    /// Logs the step once the screen is on the layer.
    ///
    /// @param runtime the runtime
    /// @param step the step on screen
    static void note_open(Runtime& runtime, first_run::Step step);

    /// Returns the gate's facts.
    static first_run::GateFacts facts_for(const Runtime& runtime, bool sdl_ready);

    /// Tells whether the main menu is the settled screen.
    static bool menu_settled(const Runtime& runtime);

    /// Writes the preferences.
    static void flush(Runtime& runtime);

    /// Tells whether Core Prime's downloads are on.
    static bool downloads_on(Runtime& runtime);

    /// Tells whether Core Prime's catalogue can be read, or the check's while it runs.
    static bool catalogue_readable(Runtime& runtime);

    /// Remembers a language so the startup notice does not ask about it.
    static void dismiss_notice(Runtime& runtime, std::string_view tag);

    /// Switches the language the game shows, and stores that choice.
    static void switch_language(Runtime& runtime, std::string_view tag);

    /// Queues a pack.
    static void queue_pack(Runtime& runtime, std::string_view tag);

    /// Reads the queue's row for a pack.
    static bool queued_pack(Runtime& runtime, std::string_view tag, content::DownloadView& item);
};

/// Returns the gate's facts.
///
/// @param runtime the runtime
/// @param sdl_ready the video driver can be read
/// @return the facts
first_run::GateFacts FirstRunHost::facts_for(const Runtime& runtime, bool sdl_ready) {
    first_run::GateFacts facts;
    facts.force = runtime.options_.first_run;
    const auto found = runtime.preference_values_.find(std::string(first_run::preference_key));
    if (found != runtime.preference_values_.end())
        facts.done = settings::language_notice_trim(found->second) == first_run::preference_done;
    facts.headless_check = runtime.options_.headless_check;
    facts.preferences_file = runtime.options_.preferences_file.has_value();
    facts.benchmark = runtime.options_.benchmark_frames.has_value();
    facts.stage = !runtime.options_.stage_file.empty();
    facts.fark = runtime.options_.remote_controlled;
    if (!sdl_ready) {
        facts.unattended = runtime.options_.unattended;
        return facts;
    }
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver == nullptr)
        driver = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    const char* ci = SDL_getenv("CI");
    facts.unattended = runtime.options_.unattended ||
                       unattended_environment(
                           ci == nullptr ? std::string_view{} : std::string_view(ci),
                           driver == nullptr ? std::string_view{} : std::string_view(driver)
                       );
    return facts;
}

/// Tells whether the main menu is the settled screen.
///
/// @param runtime the runtime
/// @return true when it is
bool FirstRunHost::menu_settled(const Runtime& runtime) {
    return runtime.screen_ == Screen::main_menu && !runtime.frame_owned_by_package() &&
           runtime.state_.state == frontend_state::state_id::main_menu &&
           runtime.state_.pending_signal != frontend_state::signal_id::multiplayer;
}

/// Writes the preferences, and says so when they cannot be written.
///
/// @param runtime the runtime
void FirstRunHost::flush(Runtime& runtime) {
    runtime.preferences_dirty_ = true;
    try {
        runtime.flush_preferences();
    } catch (const std::exception& failure) {
        std::cerr << "open-annihilation: the first-run setup was not recorded: " << failure.what()
                  << '\n';
    }
}

namespace {

/// Returns a catalogue phrase as the player reads it.
///
/// @param english the English phrase
/// @return the phrase in the language shown
std::string looked_up(std::string_view english) {
    return std::string(languages::interface_text(english));
}

/// Looks one of the sample Library's words up. The same catalogue the live
/// Library uses: the English pattern, whole, then the caller fills its places.
///
/// @param english the word, in English as the Library's text.hpp writes it
/// @return its translation, or the English
std::string_view sample_shown(void* /*context*/, std::string_view english) {
    return languages::interface_text(english);
}

/// Tells whether two tags match.
///
/// @param left a tag
/// @param right a tag
/// @return true when they match
bool same_tag(std::string_view left, std::string_view right) noexcept {
    return settings::language_notice_same_tag(left, right);
}

} // namespace

/// Tells whether Core Prime's downloads are on.
///
/// An install ID that is off is the only off. No content service yet, and a
/// registry the snapshot does not list, can download.
///
/// @param runtime the runtime
/// @return false when Core Prime's install ID is off
bool FirstRunHost::downloads_on(Runtime& runtime) {
    if (!runtime.content_)
        return true;
    const std::shared_ptr<const content::Snapshot> snapshot = runtime.content_service().snapshot();
    if (!snapshot)
        return true;
    for (const content::RegistryView& listed : snapshot->registries) {
        if (listed.registry.descriptor.id != kCorePrime)
            continue;
        const bool required =
            listed.registry.descriptor.install_id == oa::data::registry::InstallIdUse::required;
        if (required && content::read_install_id(runtime.preference_values_, kCorePrime).state ==
                            content::InstallIdState::off)
            return false;
        return true;
    }
    return true;
}

/// Tells whether a catalogue the setup reads can be read.
///
/// Core Prime's catalogue counts. While `--check-first-run` is on, the
/// check's own registry counts too: its catalogue cannot be signed as Core
/// Prime.
///
/// @param runtime the runtime
/// @return true when the snapshot lists one with a catalogue and no error
bool FirstRunHost::catalogue_readable(Runtime& runtime) {
    if (!runtime.content_)
        return false;
    const std::shared_ptr<const content::Snapshot> snapshot = runtime.content_service().snapshot();
    if (!snapshot)
        return false;
    bool check_catalogue = false;
    for (const content::RegistryView& listed : snapshot->registries) {
        const bool readable = listed.catalogue != nullptr && listed.last_error.empty();
        if (!readable)
            continue;
        if (listed.registry.descriptor.id == kCorePrime)
            return true;
        if (runtime.options_.check_first_run && listed.registry.descriptor.id == kCheckRegistry)
            check_catalogue = true;
    }
    return check_catalogue;
}

/// Remembers a language so the startup notice does not ask about it.
///
/// @param runtime the runtime
/// @param tag the language's tag
void FirstRunHost::dismiss_notice(Runtime& runtime, std::string_view tag) {
    const std::string key(settings::key::language_notice_dismissed);
    const auto found = runtime.preference_values_.find(key);
    const std::string_view stored = found == runtime.preference_values_.end()
                                        ? std::string_view{}
                                        : std::string_view(found->second);
    runtime.preference_values_[key] = settings::language_notice_dismiss_tag(stored, tag);
    flush(runtime);
}

/// Switches the language the game shows, and stores that choice.
///
/// @param runtime the runtime
/// @param tag the language's tag
void FirstRunHost::switch_language(Runtime& runtime, std::string_view tag) {
    runtime.preference_values_[std::string(settings::key::language)] = std::string(tag);
    if (runtime.engine_settings_)
        runtime.engine_settings_->current.language = std::string(tag);
    runtime.set_language_choice(tag);
    flush(runtime);
}

/// Queues a pack. A refusal is kept on the card until a download replaces it.
///
/// @param runtime the runtime
/// @param tag the pack's tag
void FirstRunHost::queue_pack(Runtime& runtime, std::string_view tag) {
    auto& run = runtime.first_run_run_;
    run.download_tag = std::string(tag);
    run.pending_failure.clear();
    run.pending_failure_tag.clear();
    std::string failure;
    if (runtime.download_language(tag, failure))
        return;
    run.pending_failure_tag = std::string(tag);
    run.pending_failure = looked_up(failure);
}

namespace {

/// Fills one card from a language the registry knows.
///
/// @param language the language
/// @param kind where it comes from
/// @param shown the tag the game shows
/// @return the card
first_run::Card
card_from(const languages::Language& language, first_run::CardKind kind, std::string_view shown) {
    first_run::Card card;
    card.tag = std::string(language.tag);
    card.own_name = std::string(language.endonym);
    card.english_name = std::string(language.english_name);
    card.kind = kind;
    card.current = same_tag(card.tag, shown);
    return card;
}

/// Scales a download into the progress bar.
///
/// @param[in,out] card the card
/// @param done bytes held
/// @param size the pack's size
void set_progress(first_run::Card& card, uint64_t done, uint64_t size) {
    card.progress_done_text = settings::download_size_text(done);
    card.progress_size_text = settings::download_size_text(size);
    if (size == 0) {
        card.progress_done = 0;
        card.progress_whole = 0;
        return;
    }
    const double share = static_cast<double>(done) / static_cast<double>(size);
    const double clamped = share < 0.0 ? 0.0 : (share > 1.0 ? 1.0 : share);
    long rounded = std::lround(clamped * static_cast<double>(kProgressWhole));
    if (rounded < 0)
        rounded = 0;
    if (rounded > kProgressWhole)
        rounded = kProgressWhole;
    card.progress_done = static_cast<int32_t>(rounded);
    card.progress_whole = kProgressWhole;
}

} // namespace

/// Reads the queue's row for a pack, when one is there.
///
/// @param runtime the runtime
/// @param tag the pack's tag
/// @param[out] item the row; unchanged when there is none
/// @return true when the queue holds it
bool FirstRunHost::queued_pack(
    Runtime& runtime, std::string_view tag, content::DownloadView& item
) {
    if (!runtime.content_)
        return false;
    const std::vector<content::DownloadView> items = runtime.content_downloads().view();
    for (const content::DownloadView& queued : items) {
        const bool core_prime = queued.target.registry == kCorePrime;
        const bool check_registry =
            runtime.options_.check_first_run && queued.target.registry == kCheckRegistry;
        if ((!core_prime && !check_registry) || !same_tag(queued.target.key, tag))
            continue;
        item = queued;
        return true;
    }
    return false;
}

namespace {

/// Puts a queued pack's state on its card.
///
/// @param[in,out] card the card
/// @param item the queue's row
void apply_queue(first_run::Card& card, const content::DownloadView& item) {
    switch (item.state) {
    case content::DownloadState::waiting:
    case content::DownloadState::asking:
    case content::DownloadState::challenge:
    case content::DownloadState::downloading:
    case content::DownloadState::paused_for_match:
        card.pack = first_run::PackState::downloading;
        set_progress(card, item.done, item.target.size);
        break;
    case content::DownloadState::verifying:
    case content::DownloadState::handing_over:
    case content::DownloadState::installing:
        card.pack = first_run::PackState::checking;
        break;
    case content::DownloadState::failed:
    case content::DownloadState::cancelled:
        card.pack = first_run::PackState::failed;
        card.failure = looked_up(content::failure_text(item.failure));
        break;
    case content::DownloadState::done:
        card.pack = first_run::PackState::checking;
        break;
    }
}

/// Tells whether two cards show the same thing.
///
/// @param left a card
/// @param right a card
/// @return true when they do
bool same_card(const first_run::Card& left, const first_run::Card& right) {
    return left.tag == right.tag && left.own_name == right.own_name &&
           left.english_name == right.english_name && left.kind == right.kind &&
           left.pack == right.pack && left.current == right.current &&
           left.size_text == right.size_text &&
           left.progress_done_text == right.progress_done_text &&
           left.progress_size_text == right.progress_size_text &&
           left.progress_done == right.progress_done &&
           left.progress_whole == right.progress_whole && left.failure == right.failure;
}

/// Tells whether two rectangles are the same.
///
/// @param left a rectangle
/// @param right a rectangle
/// @return true when they are
bool same_rect(const kit::Rect& left, const kit::Rect& right) noexcept {
    return left.x == right.x && left.y == right.y && left.width == right.width &&
           left.height == right.height;
}

/// Maps a window rectangle into a screen's points.
///
/// @param placed where the screen shows
/// @param window the rectangle, in window pixels
/// @return the rectangle, in the screen's points
kit::Rect points_of(const LayerPlacement& placed, const layout::Rect& window) {
    if (window.width <= 0 || window.height <= 0)
        return {};
    const kit::Point top_left =
        layer_point(placed, static_cast<float>(window.x), static_cast<float>(window.y));
    const kit::Point bottom_right = layer_point(
        placed,
        static_cast<float>(window.x) + static_cast<float>(window.width),
        static_cast<float>(window.y) + static_cast<float>(window.height)
    );
    kit::Rect rect{
        top_left.x, top_left.y, bottom_right.x - top_left.x, bottom_right.y - top_left.y
    };
    if (rect.width < 0)
        rect.width = 0;
    if (rect.height < 0)
        rect.height = 0;
    return rect;
}

} // namespace

/// The setup as a screen of the OA layer.
class FirstRunScreen final : public LayerScreen {
  public:

    /// Makes the setup on a step.
    ///
    /// @param runtime the runtime
    /// @param step the step it opens on
    FirstRunScreen(Runtime& runtime, first_run::Step step) : runtime_(runtime) {
        session_.model.step = step;
        session_.model.gog_offered = setup_gog_offered() && !runtime_.options_.check_first_run;
        // The check walks all five steps on this screen. A staged game or
        // copying step is part of that walk too. Anything else follows the
        // walk this start is on: the welcome, or the setup after a copy.
        if (runtime_.options_.check_first_run || step == first_run::Step::game ||
            step == first_run::Step::copying)
            session_.model.walk = first_run::Walk::setup;
        else
            session_.model.walk = setup_menu_walk();
        // The welcome, including Show again, has a menu to return to, so the
        // left button is Skip. A five-step walk keeps the word the pre-game
        // window chose, or Quit when this is the first start.
        session_.model.leave = session_.model.walk == first_run::Walk::welcome
                                   ? first_run::Leave::skip
                                   : setup_menu_leave();
    }

    /// Takes a previewed language's fonts off both stacks, so a setup
    /// closed while such a language is in the middle leaves only the
    /// language in use. A copy this screen started is stopped, and the
    /// screen is allowed to sleep again.
    ~FirstRunScreen() override {
        if (copy_woke_screen_)
            SDL_EnableScreenSaver();
        setup_stop_join();
        if (preview_fonts_tag_.empty())
            return;
        note_first_run_preview_fonts({});
        if (text_font::FontStack* stack = runtime_.oa_layer().smooth_fonts())
            runtime_.use_language_fonts(*stack);
    }

    FirstRunScreen(const FirstRunScreen&) = delete;
    FirstRunScreen& operator=(const FirstRunScreen&) = delete;

    /// Returns the screen's name.
    ///
    /// @return first-run
    [[nodiscard]] std::string_view name() const override { return kScreenName; }

    /// Returns the step on screen.
    ///
    /// @return the step
    [[nodiscard]] first_run::Step step() const noexcept { return session_.model.step; }

    /// Returns the room the step fills: the front end's, or the match gallery's.
    ///
    /// @param seen what the screen is placed on
    /// @return its place
    [[nodiscard]] LayerPlacement placement(const LayerView& seen) const override {
        const kit::Frame given = frame_for(seen);
        if (given.area.width <= 0 || given.area.height <= 0)
            return {};
        if (!seen.match) {
            // At the setup's own scale, which the layer draws it at and maps
            // input back by.
            LayerView scaled = seen;
            scaled.scale_percent = given.scale_percent;
            return centred_placement(scaled, given.area.width, given.area.height);
        }
        const layout::Rect safe = OaLayer::match_safe_area(*seen.match_layout);
        const kit::Rect drawn = kit::to_canvas(given.area, given.scale_percent);
        return {
            {safe.x + (safe.width - drawn.width) / 2,
             safe.y + (safe.height - drawn.height) / 2,
             drawn.width,
             drawn.height},
            given.area.width,
            given.area.height,
            given.scale_percent
        };
    }

    /// Tells that the setup takes every input.
    ///
    /// @return true
    [[nodiscard]] bool modal() const override { return true; }

    /// The language and online steps darken the menu. The buttons step draws
    /// its own dim, with holes over both OA buttons.
    ///
    /// @return true on the first two steps
    [[nodiscard]] bool backdrop() const override {
        return session_.model.step != first_run::Step::buttons;
    }

    /// The buttons step leaves the menu showing through the holes.
    ///
    /// @return false on the buttons step
    [[nodiscard]] bool opaque() const override {
        return session_.model.step != first_run::Step::buttons;
    }

    /// Lays the step out for the view, and keeps the cards current.
    ///
    /// @param seen what the screen is placed on
    void lay_out(const LayerView& seen) override {
        const kit::Frame given = frame_for(seen);
        if (given.area.width <= 0 || given.area.height <= 0)
            return;
        const bool frame_same = have_frame_ && given.area.width == frame_.area.width &&
                                given.area.height == frame_.area.height &&
                                given.size_class == frame_.size_class &&
                                given.scale_percent == frame_.scale_percent;
        frame_ = given;
        have_frame_ = true;
        const bool moved = refill(seen);
        if (!frame_same || moved || session_.model.step != laid_step_ || !laid_)
            relayout();
        FirstRunHost::note_open(runtime_, session_.model.step);
    }

    /// Puts an installed language's font on the smooth stack while that
    /// language is in the middle and is not the one the game shows, so the
    /// step's words can be read. The language itself is not changed. Turning
    /// away puts the stack back to the language in use.
    void prepare_preview_fonts() const {
        text_font::FontStack* stack = runtime_.oa_layer().smooth_fonts();
        if (stack == nullptr)
            return;
        std::string tag;
        if (session_.model.step == first_run::Step::language && session_.carousel.selected >= 0 &&
            static_cast<std::size_t>(session_.carousel.selected) < session_.model.cards.size()) {
            const first_run::Card& card =
                session_.model.cards[static_cast<std::size_t>(session_.carousel.selected)];
            const languages::Language* centred = languages::find_by_tag(card.tag);
            if (card.kind == first_run::CardKind::installed && centred != nullptr &&
                languages::playable(*centred) &&
                !same_tag(centred->tag, runtime_.shown_language().tag))
                tag = card.tag;
        }
        const uint64_t generation = runtime_.language_fonts_generation();
        if (preview_fonts_ready_ && tag == preview_fonts_tag_ &&
            generation == preview_fonts_generation_)
            return;
        runtime_.use_language_fonts(*stack);
        std::vector<PackFontFile> preview_faces;
        if (!tag.empty() && runtime_.language_ != nullptr) {
            // The later group wins, as the catalogue adds the engine's texts,
            // then the player's, then a mod's.
            const LoadedLanguagePack* pack = nullptr;
            const Runtime::LanguageState& state = *runtime_.language_;
            for (const auto* group : {&state.engine_packs, &state.player_packs, &state.mod_packs}) {
                for (const auto& loaded : *group) {
                    if (loaded != nullptr && same_tag(loaded->pack.manifest().tag, tag))
                        pack = loaded.get();
                }
            }
            if (pack != nullptr) {
                preview_faces = pack_fonts(*pack);
                for (const PackFontFile& font : preview_faces) {
                    if (!stack->add_face(font.file, font.role))
                        std::cerr << "open-annihilation: the language font "
                                  << path_to_utf8(font.file) << " did not open\n";
                }
            }
        }
        note_first_run_preview_fonts(preview_faces);
        preview_fonts_tag_ = std::move(tag);
        preview_fonts_generation_ = generation;
        preview_fonts_ready_ = true;
    }

    /// Draws the step.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override {
        prepare_preview_fonts();
        first_run::draw(canvas, scene_, session_);
    }

    /// Takes a pointer. A single click is one press.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the setup's points
    /// @param finger_reach a finger's reach; 0 for a mouse
    /// @return what the setup did
    LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) override {
        return pointer_with_clicks(kind, button, at, finger_reach, 1);
    }

    /// Takes a pointer, including a double-click.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the setup's points
    /// @param finger_reach a finger's reach; 0 for a mouse
    /// @param clicks how many presses landed as one
    /// @return what the setup did
    LayerAnswer pointer_with_clicks(
        ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach, uint8_t clicks
    ) override {
        first_run::PointerKind phase = first_run::PointerKind::move;
        switch (kind) {
        case ScreenInputKind::pointer_move:
            phase = first_run::PointerKind::move;
            break;
        case ScreenInputKind::pointer_down:
            if (button != SDL_BUTTON_LEFT) {
                if (first_run::stop_copy_continue(session_)) {
                    relayout();
                    return LayerAnswer::redraw;
                }
                return LayerAnswer::none;
            }
            phase = first_run::PointerKind::down;
            break;
        case ScreenInputKind::pointer_up:
            if (button != SDL_BUTTON_LEFT) {
                if (first_run::stop_copy_continue(session_)) {
                    relayout();
                    return LayerAnswer::redraw;
                }
                return LayerAnswer::none;
            }
            phase = first_run::PointerKind::up;
            break;
        default:
            return LayerAnswer::none;
        }
        return apply(first_run::pointer(session_, scene_, phase, at, finger_reach, clicks));
    }

    /// Takes a key. Escape does nothing on the language step. On the buttons
    /// step Escape and Enter finish the walk.
    ///
    /// @param pressed the key
    /// @return what the setup did
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        return apply(first_run::key(session_, scene_, pressed));
    }

    /// On the language step, the next and previous focus actions move the carousel.
    /// On the game step they move between the places the game was found.
    ///
    /// @param action the gamepad action
    /// @return what the setup did; pass lets the action become a key
    LayerAnswer pad_action(oa::ui::pad_controls::Action action) override {
        if (session_.model.step != first_run::Step::language &&
            session_.model.step != first_run::Step::game)
            return LayerAnswer::pass;
        if (action == oa::ui::pad_controls::Action::focus_next)
            return apply(first_run::key(session_, scene_, kit::Key::right));
        if (action == oa::ui::pad_controls::Action::focus_previous)
            return apply(first_run::key(session_, scene_, kit::Key::left));
        return LayerAnswer::pass;
    }

    /// Takes a turn of the wheel. On the language step it moves the carousel.
    /// On the game and copying steps it scrolls a list that does not fit.
    ///
    /// @param at the pointer, in the setup's points
    /// @param notches how far the wheel turned; positive is away from the player
    /// @return what the setup did
    LayerAnswer wheel(kit::Point /*at*/, float notches) override {
        return apply(first_run::wheel(session_, scene_, notches));
    }

    /// Returns the display list.
    ///
    /// @return the list
    [[nodiscard]] kit::DisplayList display_list() const override { return scene_.list; }

    /// Returns the pointer and the focus.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override { return session_.interaction; }

    /// Advances the carousel and the call to action, and follows a download.
    ///
    /// @return redraw when what it draws changed
    LayerAnswer tick() override {
        const uint64_t now = SDL_GetTicks();
        uint32_t dt_ms = 0;
        if (last_ticks_ != 0 && now >= last_ticks_) {
            const uint64_t gap = now - last_ticks_;
            dt_ms = gap > 1000 ? 1000 : static_cast<uint32_t>(gap);
        }
        last_ticks_ = now;
        const uint32_t elapsed_before = session_.carousel.elapsed_ms;
        const int32_t continue_before = session_.copy_continue_ms;
        first_run::tick(session_, dt_ms);
        if (session_.model.step == first_run::Step::copying && session_.copy_continue_ms == 0) {
            session_.copy_continue_ms = -1;
            session_.copy_continue_stopped = true;
            first_run::Outcome advance;
            advance.ask = first_run::Ask::advance;
            return apply(advance);
        }
        const bool slid = session_.carousel.elapsed_ms != elapsed_before;
        const int32_t continue_now = session_.copy_continue_ms;
        const bool continue_moved = continue_now != continue_before;
        const bool moved = runtime_.oa_layer_ != nullptr && refill(runtime_.oa_layer().view());
        const bool pulse = session_.model.step == first_run::Step::buttons && dt_ms > 0;
        bool copy_moved = false;
        if (session_.model.step == first_run::Step::game) {
            const first_run::GogPhase was = session_.model.gog.phase;
            const GogTick gog = setup_gog_tick(runtime_.user_folder(), true, session_.model);
            if (gog == GogTick::ready) {
                first_run::Outcome advance;
                advance.ask = first_run::Ask::advance;
                return apply(advance);
            }
            if (gog == GogTick::changed)
                copy_moved = true;
            if (session_.model.gog.phase != was)
                session_.focus_ready = false;
        }
        runtime_.first_run_run_.gog_open = session_.model.gog.phase != first_run::GogPhase::none;
        if (session_.model.step == first_run::Step::copying) {
            first_run::CopyView view;
            if (setup_pull_copy(view)) {
                session_.model.copy = std::move(view);
                copy_moved = true;
            }
        }
        const bool controller =
            SDL_WasInit(SDL_INIT_GAMEPAD) == SDL_INIT_GAMEPAD && SDL_HasGamepad();
        const bool controller_moved = controller != session_.model.controller;
        session_.model.controller = controller;
        const uint64_t before = revision_;
        if (moved || slid || pulse || copy_moved || controller_moved || continue_moved)
            relayout();
        return revision_ == before ? LayerAnswer::none : LayerAnswer::redraw;
    }

    /// Returns a number that changes when what the setup draws changes.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Leaving without Finish or Skip writes nothing, so the setup returns.
    void close(bool /*by_key*/) override {
        setup_gog_stop();
        runtime_.first_run_run_.gog_open = false;
        runtime_.first_run_run_.opened_logged = false;
        runtime_.first_run_run_.opened_step = -1;
    }

    /// Rebuilds the cards when a download or the catalogue moved.
    void refresh() {
        if (refill(runtime_.oa_layer().view()))
            relayout();
    }

  private:

    /// The frame the step is laid out in.
    ///
    /// @param seen what the screen is placed on
    /// @return the frame; empty when it cannot be placed
    [[nodiscard]] kit::Frame frame_for(const LayerView& seen) const {
        kit::Frame given{};
        if (seen.match) {
            if (seen.match_layout == nullptr)
                return given;
            given = OaLayer::match_gallery(*seen.match_layout, seen.interface_percent);
        } else {
            given = front_frame(seen);
        }
        given.area.x = 0;
        given.area.y = 0;
        return given;
    }

    /// The frame the setup lays out in over the front end: the layer's room,
    /// at the setup's own scale.
    ///
    /// With the Interface size at Auto that scale is first_run::setup_frame's:
    /// the room holding first_run::setup_room, never below the layer's scale,
    /// with fractions while the layer offers them (the modern fonts are on
    /// and open). A size the player chose is kept as it is.
    ///
    /// @param seen what the screen is placed on
    /// @return the frame
    [[nodiscard]] static kit::Frame front_frame(const LayerView& seen) noexcept {
        const int32_t layer = std::max(seen.scale_percent, int32_t{100});
        kit::Frame given{};
        if (seen.interface_percent > 0) {
            kit::Viewport room;
            room.width = std::max(seen.room.width, int32_t{0});
            room.height = std::max(seen.room.height, int32_t{0});
            room.scale_percent = layer;
            given = kit::frame_of(room);
        } else {
            const bool fractional = seen.interface_percent == kit::auto_with_fraction_percent;
            given = first_run::setup_frame(seen.room.width, seen.room.height, layer, fractional);
        }
        given.reduced_motion = seen.frame.reduced_motion;
        return given;
    }

    /// Lays the current step out again.
    void relayout() {
        if (frame_.area.width <= 0 || frame_.area.height <= 0)
            return;
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        scene_ = first_run::lay_out(session_, frame_, fonts);
        first_run::ensure_focus(session_, scene_.list);
        laid_step_ = session_.model.step;
        laid_ = true;
        ++revision_;
    }

    /// Rebuilds the cards, the catalogue note and the button rectangles.
    ///
    /// The step and the online choice stay as the player left them. The
    /// carousel follows the language in use only when that language changes.
    ///
    /// @param seen what the screen is placed on
    /// @return true when what the step shows changed
    bool refill(const LayerView& seen) {
        if (!runtime_.first_run_run_.download_tag.empty()) {
            const languages::Language* got =
                languages::find_by_tag(runtime_.first_run_run_.download_tag);
            if (got != nullptr && languages::playable(*got)) {
                const std::string tag = runtime_.first_run_run_.download_tag;
                runtime_.first_run_run_.download_tag.clear();
                runtime_.first_run_run_.download_item = 0;
                runtime_.first_run_run_.pending_failure.clear();
                runtime_.first_run_run_.pending_failure_tag.clear();
                FirstRunHost::switch_language(runtime_, tag);
            }
        }

        const LayerPlacement placed = placement(seen);
        const OaLayer::MenuButtonWindows windows = runtime_.oa_layer().menu_button_windows();
        const kit::Rect library_button = points_of(placed, windows.library);
        const kit::Rect settings_button = points_of(placed, windows.settings);

        const languages::Language& shown = runtime_.shown_language();
        const std::string shown_tag(shown.tag);
        std::vector<first_run::Card> cards;
        for (const languages::Language* language : languages::known_languages()) {
            if (language == nullptr || language->source != languages::Source::built_in)
                continue;
            cards.push_back(card_from(*language, first_run::CardKind::built_in, shown_tag));
        }
        for (const languages::Language* language : languages::known_languages()) {
            if (language == nullptr || language->source != languages::Source::installed)
                continue;
            if (!languages::playable(*language))
                continue;
            cards.push_back(card_from(*language, first_run::CardKind::installed, shown_tag));
        }

        const bool online = FirstRunHost::downloads_on(runtime_);
        const bool readable = FirstRunHost::catalogue_readable(runtime_);
        first_run::CatalogueNote note = first_run::CatalogueNote::none;
        if (!online)
            note = first_run::CatalogueNote::downloads_off;
        else if (!readable)
            note = first_run::CatalogueNote::unread;

        if (runtime_.language_ != nullptr) {
            for (const Runtime::CatalogueLanguage& pack : runtime_.language_->available) {
                const bool core_prime = pack.registry == kCorePrime;
                const bool check_registry =
                    runtime_.options_.check_first_run && pack.registry == kCheckRegistry;
                if (!core_prime && !check_registry)
                    continue;
                bool listed = false;
                for (const first_run::Card& card : cards)
                    if (same_tag(card.tag, pack.entry.tag))
                        listed = true;
                if (listed)
                    continue;
                const languages::Language* known = languages::find_by_tag(pack.entry.tag);
                if (known != nullptr && languages::playable(*known))
                    continue;
                first_run::Card card;
                card.tag = pack.entry.tag;
                card.own_name =
                    pack.entry.endonym.empty() ? pack.entry.english_name : pack.entry.endonym;
                card.english_name = pack.entry.english_name;
                card.kind = first_run::CardKind::pack;
                card.current = same_tag(card.tag, shown_tag);
                card.size_text = settings::download_size_text(pack.bytes);
                if (note != first_run::CatalogueNote::none) {
                    card.pack = first_run::PackState::offline;
                } else {
                    content::DownloadView item;
                    if (FirstRunHost::queued_pack(runtime_, card.tag, item)) {
                        runtime_.first_run_run_.download_item = item.item;
                        apply_queue(card, item);
                    } else if (same_tag(runtime_.first_run_run_.pending_failure_tag, card.tag)) {
                        card.pack = first_run::PackState::failed;
                        card.failure = runtime_.first_run_run_.pending_failure;
                    }
                }
                cards.push_back(std::move(card));
            }
        }

        int32_t library_count = 0;
        const kit::OaButtonMark mark = Runtime::EngineSettingsMenuHost::library_mark(runtime_);
        if (mark.corner == kit::OaButtonCorner::count) {
            const auto most = static_cast<std::size_t>(std::numeric_limits<int32_t>::max());
            library_count = mark.count > most ? std::numeric_limits<int32_t>::max()
                                              : static_cast<int32_t>(mark.count);
        }

        const std::string sample_own(shown.endonym);
        const std::string sample_english(shown.english_name);
        // The window's own title bar names the game; full screen has none.
        const bool windowed =
            runtime_.sdl_.window != nullptr &&
            (SDL_GetWindowFlags(runtime_.sdl_.window) & SDL_WINDOW_FULLSCREEN) == 0;
        bool changed = windowed != session_.model.windowed || note != session_.model.catalogue ||
                       library_count != session_.model.library_count ||
                       sample_own != session_.model.sample_own ||
                       sample_english != session_.model.sample_english ||
                       !same_rect(library_button, session_.model.library_button) ||
                       !same_rect(settings_button, session_.model.settings_button) ||
                       cards.size() != session_.model.cards.size();
        for (std::size_t at = 0; !changed && at < cards.size(); ++at)
            changed = !same_card(cards[at], session_.model.cards[at]);

        session_.model.cards = std::move(cards);
        session_.model.windowed = windowed;
        session_.model.catalogue = note;
        session_.model.library_button = library_button;
        session_.model.settings_button = settings_button;
        session_.model.library_count = library_count;
        session_.model.sample_own = sample_own;
        session_.model.sample_english = sample_english;
        session_.model.sample_text = oa::ui::library::TextHooks{nullptr, sample_shown};

        std::string current_tag;
        for (const first_run::Card& card : session_.model.cards)
            if (card.current)
                current_tag = card.tag;
        const int32_t selected_before = session_.carousel.selected;
        if (!selection_set_ || current_tag != bound_tag_) {
            int32_t chosen = 0;
            const auto count = static_cast<int32_t>(session_.model.cards.size());
            for (int32_t at = 0; at < count; ++at) {
                if (session_.model.cards[static_cast<std::size_t>(at)].current)
                    chosen = at;
            }
            session_.carousel.selected = chosen;
            bound_tag_ = current_tag;
            selection_set_ = true;
            first_run::bind(session_);
            // The language in use starts in the middle. A later move eases.
            kit::carousel_settle(session_.carousel);
        } else {
            first_run::bind(session_);
        }
        return changed || session_.carousel.selected != selected_before;
    }

    /// Does what a key or a pointer asked, then lays the step out again.
    ///
    /// @param outcome what the setup asked
    /// @return what the layer should do
    LayerAnswer apply(const first_run::Outcome& outcome) {
        if (outcome.pass)
            return LayerAnswer::pass;
        const auto stop_menu_copy = [this] {
            if (session_.model.step != first_run::Step::copying)
                return;
            if (!session_.model.copy.running && !session_.model.copy.paused)
                return;
            setup_request_cancel();
            setup_stop_join();
            if (copy_woke_screen_) {
                SDL_EnableScreenSaver();
                copy_woke_screen_ = false;
            }
            session_.model.copy.running = false;
            session_.model.copy.paused = false;
        };
        bool leave = false;
        switch (outcome.ask) {
        case first_run::Ask::none:
            break;
        case first_run::Ask::skip:
            stop_menu_copy();
            FirstRunHost::finish(runtime_);
            leave = true;
            break;
        case first_run::Ask::back:
            if (session_.model.step == first_run::Step::online) {
                if (session_.model.walk == first_run::Walk::setup) {
                    first_run::CopyView view;
                    static_cast<void>(setup_pull_copy(view));
                    if (!view.bundles.empty() || view.checked)
                        session_.model.copy = std::move(view);
                    session_.model.copy.checked = true;
                    session_.model.copy.running = false;
                    session_.model.copy.paused = false;
                    session_.model.step = first_run::Step::copying;
                    first_run::reset_copy_continue(session_);
                } else {
                    session_.model.step = first_run::Step::language;
                }
            } else if (session_.model.step == first_run::Step::buttons) {
                session_.model.step = first_run::Step::online;
            } else if (session_.model.step == first_run::Step::game) {
                session_.model.step = first_run::Step::language;
            }
            session_.focus_ready = false;
            break;
        case first_run::Ask::advance:
            if (session_.model.step == first_run::Step::language) {
                const int32_t index = session_.carousel.selected;
                if (index >= 0 && static_cast<std::size_t>(index) < session_.model.cards.size()) {
                    const first_run::Card& card =
                        session_.model.cards[static_cast<std::size_t>(index)];
                    const languages::Language* known = languages::find_by_tag(card.tag);
                    if (known != nullptr && languages::playable(*known))
                        FirstRunHost::switch_language(runtime_, card.tag);
                }
                if (session_.model.walk == first_run::Walk::setup) {
                    session_.model.step = first_run::Step::game;
                    if (runtime_.options_.check_first_run)
                        setup_install_check_source(runtime_.user_folder(), session_.model);
                } else {
                    session_.model.step = first_run::Step::online;
                }
                session_.focus_ready = false;
            } else if (session_.model.step == first_run::Step::game) {
                if (setup_begin_copy(runtime_.user_folder(), session_.model)) {
                    session_.model.step = first_run::Step::copying;
                    first_run::reset_copy_continue(session_);
                    session_.copy_continue_armed = true;
                    session_.focus_ready = false;
                    if (!copy_woke_screen_) {
                        SDL_DisableScreenSaver();
                        copy_woke_screen_ = true;
                    }
                }
            } else if (session_.model.step == first_run::Step::copying) {
                if (session_.model.copy.checked) {
                    if (copy_woke_screen_) {
                        SDL_EnableScreenSaver();
                        copy_woke_screen_ = false;
                    }
                    session_.model.step = first_run::Step::online;
                    session_.focus_ready = false;
                }
            } else if (session_.model.step == first_run::Step::online) {
                FirstRunHost::apply_online(runtime_, session_.model.online_on);
                session_.model.step = first_run::Step::buttons;
                session_.focus_ready = false;
            } else {
                FirstRunHost::finish(runtime_);
                leave = true;
            }
            break;
        case first_run::Ask::pick:
            FirstRunHost::pick(runtime_, session_, outcome.index);
            break;
        case first_run::Ask::cancel_download:
            FirstRunHost::cancel_download(runtime_);
            break;
        case first_run::Ask::retry_download:
            FirstRunHost::retry_download(runtime_, session_);
            break;
        case first_run::Ask::choose_on:
        case first_run::Ask::choose_off:
            break;
        case first_run::Ask::gog_open:
        case first_run::Ask::gog_paste:
        case first_run::Ask::gog_cancel:
            setup_gog_ask(outcome.ask, runtime_.sdl_.window, runtime_.user_folder(), session_.model);
            runtime_.first_run_run_.gog_open =
                session_.model.gog.phase != first_run::GogPhase::none;
            session_.focus_ready = false;
            break;
        case first_run::Ask::locate:
            if (setup_locate_source(runtime_.sdl_.window, runtime_.user_folder(), session_.model))
                session_.focus_ready = false;
            break;
        case first_run::Ask::pause_copy:
            setup_set_paused(!session_.model.copy.paused);
            break;
        case first_run::Ask::cancel_copy:
            setup_request_cancel();
            setup_stop_join();
            if (copy_woke_screen_) {
                SDL_EnableScreenSaver();
                copy_woke_screen_ = false;
            }
            session_.model.copy.running = false;
            session_.model.copy.paused = false;
            session_.model.step = first_run::Step::game;
            first_run::reset_copy_continue(session_);
            session_.focus_ready = false;
            break;
        case first_run::Ask::quit:
            stop_menu_copy();
            leave = true;
            break;
        }
        if (leave)
            return LayerAnswer::close;
        if (session_.model.step == first_run::Step::copying ||
            outcome.ask == first_run::Ask::pause_copy) {
            first_run::CopyView view;
            if (setup_pull_copy(view))
                session_.model.copy = std::move(view);
        }
        if (outcome.redraw || outcome.ask != first_run::Ask::none) {
            refill(runtime_.oa_layer().view());
            relayout();
            return LayerAnswer::redraw;
        }
        return LayerAnswer::none;
    }

    Runtime& runtime_;                                     ///< the runtime
    first_run::Session session_{};                         ///< the step, the cards and the focus
    first_run::Scene scene_{};                             ///< the laid-out step
    kit::Frame frame_{};                                   ///< the frame it was last laid out on
    bool have_frame_{};                                    ///< lay_out has seen a frame
    bool laid_{};                                          ///< relayout has run
    first_run::Step laid_step_{first_run::Step::language}; ///< the step last laid out
    uint64_t revision_{};                                  ///< changes when the drawing changes
    uint64_t last_ticks_{};                                ///< SDL_GetTicks at the previous tick
    bool selection_set_{};    ///< the carousel has been placed on a language
    std::string bound_tag_{}; ///< the language the carousel last followed
    /// The installed language whose font was added for a preview; empty when
    /// the stack holds only the language in use.
    mutable std::string preview_fonts_tag_{};
    mutable uint64_t preview_fonts_generation_{}; ///< the font generation that addition followed
    mutable bool preview_fonts_ready_{};          ///< prepare_preview_fonts has run
    bool copy_woke_screen_{}; ///< this screen asked the display to stay awake for a copy
};

/// Returns the setup, showing or waiting.
///
/// @param layer the OA layer
/// @return the screen; null when there is none
FirstRunScreen* setup_screen(OaLayer& layer) {
    if (LayerScreen* shown = layer.find(kScreenName))
        return dynamic_cast<FirstRunScreen*>(shown);
    if (LayerScreen* waiting = layer.waiting(kScreenName))
        return dynamic_cast<FirstRunScreen*>(waiting);
    return nullptr;
}

void FirstRunHost::note_open(Runtime& runtime, first_run::Step step) {
    if (runtime.oa_layer_ == nullptr || runtime.oa_layer().find(kScreenName) == nullptr)
        return;
    const int coded = static_cast<int>(step);
    if (runtime.first_run_run_.opened_logged && runtime.first_run_run_.opened_step == coded)
        return;
    runtime.first_run_run_.opened_logged = true;
    runtime.first_run_run_.opened_step = coded;
    std::cout << "first-run: showing step " << first_run::step_token(step) << '\n';
    std::cout.flush();
}

void FirstRunHost::finish(Runtime& runtime) {
    runtime.preference_values_[std::string(first_run::preference_key)] =
        std::string(first_run::preference_done);
    flush(runtime);
    // A --first-run start's menu hum waits for this (Runtime::menu_hum_waits).
    runtime.setup_finished_ = true;
    // Skip setup and Start playing both end the walk. --first-run shows it
    // again, so the movies stay until that return will not happen.
    runtime.play_intro_after_setup();
}

void FirstRunHost::apply_online(Runtime& runtime, bool online_on) {
    if (online_on) {
        content::write_check_for_updates(
            runtime.preference_values_, content::CheckForUpdates::automatically
        );
        if (runtime.engine_settings_)
            runtime.engine_settings_->current.content_check = settings::ContentCheck::automatically;
        const content::InstallId held =
            content::read_install_id(runtime.preference_values_, kCorePrime);
        if (held.state == content::InstallIdState::off)
            content::turn_install_id_on(runtime.preference_values_, kCorePrime);
        const content::InstallId after =
            content::read_install_id(runtime.preference_values_, kCorePrime);
        if (after.state == content::InstallIdState::on)
            runtime.content_install_ids_changed();
        runtime.content_service().set_check_for_updates(content::CheckForUpdates::automatically);
    } else {
        content::write_check_for_updates(
            runtime.preference_values_, content::CheckForUpdates::never
        );
        if (runtime.engine_settings_)
            runtime.engine_settings_->current.content_check = settings::ContentCheck::never;
        content::turn_install_id_off(runtime.preference_values_, kCorePrime);
        runtime.content_install_ids_changed();
        runtime.content_service().set_check_for_updates(content::CheckForUpdates::never);
    }
    flush(runtime);
}

void FirstRunHost::pick(Runtime& runtime, first_run::Session& session, int32_t index) {
    if (index < 0 || static_cast<std::size_t>(index) >= session.model.cards.size())
        return;
    const first_run::Card card = session.model.cards[static_cast<std::size_t>(index)];
    const languages::Language* known = languages::find_by_tag(card.tag);
    if (known != nullptr && languages::playable(*known)) {
        switch_language(runtime, card.tag);
        return;
    }
    dismiss_notice(runtime, card.tag);
    queue_pack(runtime, card.tag);
}

void FirstRunHost::cancel_download(Runtime& runtime) {
    if (!runtime.content_ || runtime.first_run_run_.download_item == 0)
        return;
    runtime.content_downloads().cancel(runtime.first_run_run_.download_item);
}

void FirstRunHost::retry_download(Runtime& runtime, const first_run::Session& session) {
    const int32_t index = session.carousel.selected;
    if (index < 0 || static_cast<std::size_t>(index) >= session.model.cards.size())
        return;
    const std::string tag = session.model.cards[static_cast<std::size_t>(index)].tag;
    runtime.first_run_run_.pending_failure.clear();
    runtime.first_run_run_.pending_failure_tag.clear();
    runtime.first_run_run_.download_tag = tag;
    if (runtime.content_ && runtime.first_run_run_.download_item != 0) {
        content::DownloadView item;
        if (queued_pack(runtime, tag, item) && (item.state == content::DownloadState::failed ||
                                                item.state == content::DownloadState::cancelled)) {
            runtime.content_downloads().retry(item.item);
            runtime.first_run_run_.download_item = item.item;
            return;
        }
    }
    queue_pack(runtime, tag);
}

void FirstRunHost::tell(Runtime& runtime) {
    runtime.play_intro_after_setup();
    runtime.log_first_run_gate(true);
    if (runtime.oa_layer_ == nullptr)
        return;
    OaLayer& layer = runtime.oa_layer();
    const first_run::Hold hold = first_run::decide(facts_for(runtime, true));
    if (FirstRunScreen* screen = setup_screen(layer)) {
        if (!runtime.first_run_run_.staged && hold != first_run::Hold::show) {
            layer.close(screen);
            return;
        }
        screen->refresh();
        FirstRunHost::note_open(runtime, screen->step());
        return;
    }
    if (runtime.first_run_run_.staged)
        return;
    if (hold != first_run::Hold::show) {
        runtime.first_run_run_.main_menu_frames = 0;
        return;
    }
    if (!menu_settled(runtime)) {
        runtime.first_run_run_.main_menu_frames = 0;
        return;
    }
    if (++runtime.first_run_run_.main_menu_frames < kMenuFrames ||
        oa::ui::frontend_dialogs::dialog_count() != 0 ||
        runtime.engine_settings_dialog() != nullptr || runtime.engine_settings_fonts() == nullptr)
        return;
    const first_run::Step opening =
        runtime.options_.check_first_run ? first_run::Step::language : setup_menu_step();
    layer.show_when_free(std::make_unique<FirstRunScreen>(runtime, opening));
    if (FirstRunScreen* opened = dynamic_cast<FirstRunScreen*>(layer.find(kScreenName)))
        FirstRunHost::note_open(runtime, opened->step());
}

bool FirstRunHost::open_staged(Runtime& runtime, std::string_view token) {
    const std::optional<first_run::Step> step = first_run::step_named(token);
    if (!step)
        return false;
    runtime.first_run_run_.staged = true;
    if (runtime.oa_layer_ == nullptr)
        return true;
    OaLayer& layer = runtime.oa_layer();
    if (FirstRunScreen* screen = setup_screen(layer))
        layer.close(screen);
    layer.push(std::make_unique<FirstRunScreen>(runtime, *step));
    FirstRunHost::note_open(runtime, *step);
    return true;
}

void Runtime::tell_first_run() {
    FirstRunHost::tell(*this);
}

void Runtime::play_intro_after_setup() {
    if (!options_.intro_after_setup)
        return;
    // 3.1c plays the logo and the intro before the menu is used, while
    // PlayMovie is set. The setup is that arrival, so the movies wait until
    // the walk will not show again. A switch that skips them plays nothing
    // on this start; that is the same skip a start without the setup uses.
    if (first_run::decide(FirstRunHost::facts_for(*this, true)) == first_run::Hold::show)
        return;
    options_.intro_after_setup = false;
    std::cout << "first-run: done\n" << std::flush;
    RendererHost* host = render_run_ ? render_run_->host : nullptr;
    play_intro_movies(options_, sdl_.window, sdl_.renderer, host, &full_screen_switch_);
}

void Runtime::log_first_run_gate(bool sdl_ready) {
    const first_run::Hold hold = first_run::decide(FirstRunHost::facts_for(*this, sdl_ready));
    const int coded = static_cast<int>(hold);
    if (first_run_run_.gate_logged && first_run_run_.gate_hold == coded)
        return;
    first_run_run_.gate_logged = true;
    first_run_run_.gate_hold = coded;
    if (hold == first_run::Hold::show)
        return;
    std::cout << "first-run: not showing: " << first_run::hold_reason(hold) << '\n';
    std::cout.flush();
}

bool Runtime::first_run_blocks_language_notice() {
    if (oa_layer_ != nullptr &&
        (oa_layer().find(kScreenName) != nullptr || oa_layer().waiting(kScreenName) != nullptr))
        return true;
    if (first_run_run_.staged)
        return true;
    return first_run::decide(FirstRunHost::facts_for(*this, true)) == first_run::Hold::show;
}

void Runtime::forget_first_run() {
    preference_values_.erase(std::string(first_run::preference_key));
    preferences_dirty_ = true;
    // SHOW AGAIN is the short welcome: language, online features, the buttons.
    setup_welcome_again();
}

bool Runtime::open_staged_first_run(std::string_view token) {
    return FirstRunHost::open_staged(*this, token);
}

} // namespace oa::app

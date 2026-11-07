// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The notice that a language is only available as a download, over the
// settled main menu. Download queues that pack. Use English records the
// answer. Escape closes the notice for this run and writes nothing.

#include "oa_layer.hpp"
#include "user_folder_state.hpp"

#include "oa/app/command_line.hpp"
#include "oa/app/content/downloads.hpp"
#include "oa/app/content/service.hpp"
#include "oa/app/content/settings.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "language_state.hpp"
#include "oa/data/languages.hpp"
#include "oa/data/registry/descriptor.hpp"
#include "oa/platform/locale.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/engine_settings/language_notice.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_state/app_modes.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/input.hpp"

#include <SDL3/SDL.h>

#include <exception>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

/// The language the notice is about, and which of the two cases it is.
struct NoticeCase {
    std::string tag;    ///< the language's tag
    bool system_case{}; ///< the operating system asked, not the setting
};

/// The catalogue row for a tag, copied so the row can change afterwards.
struct Offer {
    bool present{};       ///< a catalogue lists the pack
    std::string registry; ///< the registry id
    uint64_t bytes{};     ///< the pack's size
};

/// One queued pack for the notice's tag.
struct QueuedPack {
    bool active{};          ///< queued and not failed
    bool failed{};          ///< failed or cancelled
    float fraction{-1.F};   ///< how far it has come, from 0 to 1
    std::string progress{}; ///< "<done> of <size>"
};

/// The notice's hold on the runtime. Runtime names it as a friend.
class LanguageNoticeHost {
  public:

    /// Builds what the notice shows now.
    ///
    /// @param runtime the runtime
    /// @return the notice
    [[nodiscard]] static oa::ui::engine_settings::LanguageNotice model(Runtime& runtime);

    /// Decides, shows, refreshes or closes the notice for one frame.
    ///
    /// @param runtime the runtime
    static void tell(Runtime& runtime);

    /// Tells whether the language can be shown, so the notice is done.
    ///
    /// @param tag the language's tag
    /// @return true when a pack is installed and this build draws it
    [[nodiscard]] static bool tag_playable(std::string_view tag);

    /// Queues the pack the notice is about.
    ///
    /// @param runtime the runtime
    static void download(Runtime& runtime);

    /// Records Use English and writes the preferences.
    ///
    /// @param runtime the runtime
    static void use_english(Runtime& runtime);

    /// Closes the notice for this run without writing the preferences.
    ///
    /// @param runtime the runtime
    static void close_for_run(Runtime& runtime) noexcept;

    /// Returns the fonts the notice is drawn in.
    ///
    /// @param runtime the runtime
    /// @return the fonts; null before they load
    [[nodiscard]] static const oa::ui::engine_settings::DialogFonts* fonts(Runtime& runtime) {
        return runtime.engine_settings_fonts();
    }

  private:

    /// Returns one preference, trimmed. Empty when the key is absent.
    [[nodiscard]] static std::string preference(const Runtime& runtime, std::string_view key);

    /// Tells whether this run is one nobody watches.
    [[nodiscard]] static bool unwatched_run(const Runtime& runtime);

    /// Tells whether the main menu is the settled screen.
    [[nodiscard]] static bool menu_settled(const Runtime& runtime);

    /// Returns the case the notice applies to, when one does.
    [[nodiscard]] static std::optional<NoticeCase> notice_case(Runtime& runtime);

    /// Returns the catalogue row for a tag.
    [[nodiscard]] static Offer offer_of(Runtime& runtime, std::string_view tag);

    /// Tells whether a registry can download.
    [[nodiscard]] static bool downloads_on(Runtime& runtime, std::string_view registry);

    /// Returns the queued pack for a tag, when the queue is already started.
    [[nodiscard]] static QueuedPack
    queued_pack(Runtime& runtime, std::string_view tag, std::string_view registry);
};

namespace {

namespace languages = oa::data::languages;
namespace settings = oa::ui::engine_settings;
namespace content = oa::app::content;
namespace frontend_state = oa::ui::frontend_state;
namespace kit = oa::ui::kit;

/// The screen's name, which its controls' names start with.
constexpr std::string_view kScreenName = "language-notice";
/// Frames in a row the main menu shows before the notice.
constexpr uint32_t kNoticeMenuFrames = 2;
/// The English body when the pack's size is known, around the name and the size.
constexpr std::string_view kSizedEnglish = " is now a separate download (";
/// The rest of that body.
constexpr std::string_view kSizedEnglishTail = "). Until it is installed, the game shows English.";
/// The English body when the size is not known, after the name.
constexpr std::string_view kSizelessEnglish =
    " is now a separate download. Until it is installed, the game shows English.";
/// The English line when a download failed.
constexpr std::string_view kFailedEnglish = "The download failed.";
/// The English line when the pack cannot be downloaded.
constexpr std::string_view kCannotEnglish = "It cannot be downloaded now.";

class LanguageNoticeScreen;

/// Tells whether two tags match, ignoring ASCII case and reading '_' as '-'.
///
/// @param left a tag
/// @param right a tag
/// @return true when they match
bool same_tag(std::string_view left, std::string_view right) noexcept {
    return settings::language_notice_same_tag(left, right);
}

/// Returns a pattern with its first "%s" replaced by a size.
///
/// @param pattern the pattern
/// @param size the size
/// @return the text
std::string with_size(std::string_view pattern, std::string_view size) {
    std::string text(pattern);
    const auto at = text.find("%s");
    if (at == std::string::npos)
        return text;
    text.replace(at, 2, size);
    return text;
}

/// The notice on the OA layer, laid out as a question.
class LanguageNoticeScreen final : public LayerScreen {
  public:

    /// Makes the notice.
    ///
    /// @param runtime the runtime
    /// @param notice what it shows
    /// @param tag the language it is about
    LanguageNoticeScreen(Runtime& runtime, settings::LanguageNotice notice, std::string tag)
        : runtime_(runtime), model_(std::move(notice)), tag_(std::move(tag)) {
        interaction_.focused = 1;
        interaction_.focus_shown = true;
        rebuild();
    }

    /// Returns the screen's name.
    ///
    /// @return language-notice
    [[nodiscard]] std::string_view name() const override { return kScreenName; }

    /// Returns the notice centred on the main menu. Empty in a match, over
    /// any other screen, or before it has been laid out.
    ///
    /// @param seen what the screen is placed on
    /// @return its place
    [[nodiscard]] LayerPlacement placement(const LayerView& seen) const override {
        if (seen.match || seen.game_screen != Screen::main_menu)
            return {};
        int32_t width = 0;
        int32_t height = 0;
        for (const kit::Item& item : list_.items) {
            if (item.role != kit::Role::fill)
                continue;
            width = item.rect.width;
            height = item.rect.height;
            break;
        }
        if (width <= 0 || height <= 0)
            return {};
        return centred_placement(seen, width, height);
    }

    /// Tells that the notice takes every input.
    ///
    /// @return true
    [[nodiscard]] bool modal() const override { return true; }

    /// Tells that the main menu darkens under the notice.
    ///
    /// @return true
    [[nodiscard]] bool backdrop() const override { return true; }

    /// Lays the notice out at the view's size class.
    ///
    /// @param seen what the screen is placed on
    void lay_out(const LayerView& seen) override {
        if (seen.match || seen.frame.area.width <= 0 || seen.frame.area.height <= 0)
            return;
        if (have_frame_ && seen.frame.area.width == frame_.area.width &&
            seen.frame.area.height == frame_.area.height &&
            seen.frame.size_class == frame_.size_class)
            return;
        frame_ = seen.frame;
        have_frame_ = true;
        rebuild();
    }

    /// Draws the notice.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override { kit::paint(canvas, shown_list()); }

    /// Takes a pointer. A release over an enabled button presses it.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the notice's points
    /// @param finger_reach a finger's reach; 0 for a mouse
    /// @return what the notice did
    LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) override {
        kit::PointerOutcome outcome{};
        switch (kind) {
        case ScreenInputKind::pointer_move:
            outcome = kit::pointer_move(interaction_, list_, at);
            break;
        case ScreenInputKind::pointer_down:
            if (button != SDL_BUTTON_LEFT)
                return LayerAnswer::none;
            outcome = finger_reach > 0 ? kit::finger_down(interaction_, list_, at, finger_reach)
                                       : kit::pointer_down(interaction_, list_, at);
            break;
        case ScreenInputKind::pointer_up:
            if (button != SDL_BUTTON_LEFT)
                return LayerAnswer::none;
            outcome = kit::pointer_up(interaction_, list_, at);
            break;
        default:
            return LayerAnswer::none;
        }
        if (outcome.result == kit::PointerResult::activated)
            return activate(outcome.control);
        if (outcome.result == kit::PointerResult::redraw) {
            ++revision_;
            return LayerAnswer::redraw;
        }
        return LayerAnswer::none;
    }

    /// Takes a key. Escape and N close the notice for this run. Enter and
    /// Space press the focused button when it is enabled. Y presses Download
    /// when it is enabled.
    ///
    /// @param pressed the key
    /// @return what the notice did
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        if (pressed == kit::Key::no) {
            LanguageNoticeHost::close_for_run(runtime_);
            return LayerAnswer::close;
        }
        if (pressed == kit::Key::yes) {
            const kit::Control* download = kit::control_of(list_, 1);
            if (download == nullptr || !download->enabled)
                return LayerAnswer::none;
            return activate(download->id);
        }
        const kit::KeyOutcome outcome = kit::key(interaction_, list_, pressed);
        if (outcome.result == kit::KeyResult::cancel) {
            LanguageNoticeHost::close_for_run(runtime_);
            return LayerAnswer::close;
        }
        if (outcome.result == kit::KeyResult::redraw) {
            ++revision_;
            return LayerAnswer::redraw;
        }
        if (outcome.result == kit::KeyResult::to_control)
            return activate(outcome.control);
        return LayerAnswer::none;
    }

    /// Takes a turn of the wheel, which the notice swallows.
    ///
    /// @return none
    LayerAnswer wheel(kit::Point /*at*/, float /*notches*/) override { return LayerAnswer::none; }

    /// Returns the display list, with the focus ring on the focused button.
    ///
    /// @return the list
    [[nodiscard]] kit::DisplayList display_list() const override { return shown_list(); }

    /// Returns the pointer and the focus.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override { return interaction_; }

    /// Closes once the language can be shown, and refreshes the size and
    /// the download while it stays.
    ///
    /// @return close when the language can be shown
    LayerAnswer tick() override {
        if (LanguageNoticeHost::tag_playable(tag_))
            return LayerAnswer::close;
        const uint64_t before = revision_;
        refresh(LanguageNoticeHost::model(runtime_));
        return revision_ == before ? LayerAnswer::none : LayerAnswer::redraw;
    }

    /// Returns a number that changes when what the notice draws changes.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// The notice writes nothing as it leaves. Use English wrote already.
    void close(bool /*by_key*/) override {}

    /// Replaces what the notice shows, when it differs.
    ///
    /// @param notice what it shows now
    void refresh(settings::LanguageNotice notice) {
        if (notice == model_)
            return;
        model_ = std::move(notice);
        rebuild();
    }

  private:

    /// Lays the notice out again at the frame it last took.
    void rebuild() {
        kit::Frame frame = frame_;
        if (!have_frame_) {
            frame.size_class = kit::SizeClass::compact;
            frame.area = {0, 0, 640, 480};
        }
        list_ =
            settings::layout_language_notice(model_, frame, LanguageNoticeHost::fonts(runtime_));
        ++revision_;
    }

    /// Returns the list with the focus ring moved onto the focused button.
    ///
    /// @return the list
    [[nodiscard]] kit::DisplayList shown_list() const {
        kit::DisplayList list = list_;
        std::vector<kit::Item> items;
        items.reserve(list.items.size());
        for (kit::Item& item : list.items)
            if (item.role != kit::Role::focus_ring)
                items.push_back(std::move(item));
        list.items = std::move(items);
        kit::mark_states(list, interaction_);
        if (interaction_.focus_shown)
            if (const kit::Control* control = kit::control_of(list, interaction_.focused))
                kit::add_focus_ring(list, control->rect, true);
        return list;
    }

    /// Presses a button. A disabled button does nothing. Use English records
    /// the answer. Download queues the pack.
    ///
    /// @param id the button
    /// @return what the notice did
    LayerAnswer activate(kit::ControlId id) {
        const kit::Control* control = kit::control_of(list_, id);
        if (control == nullptr || !control->enabled)
            return LayerAnswer::none;
        if (control->name == std::string(kScreenName) + ".use-english") {
            LanguageNoticeHost::use_english(runtime_);
            return LayerAnswer::close;
        }
        if (control->name == std::string(kScreenName) + ".download") {
            if (!model_.download_enabled)
                return LayerAnswer::none;
            LanguageNoticeHost::download(runtime_);
            refresh(LanguageNoticeHost::model(runtime_));
            return LayerAnswer::redraw;
        }
        return LayerAnswer::none;
    }

    Runtime& runtime_;               ///< the runtime
    settings::LanguageNotice model_; ///< what the notice shows
    std::string tag_;                ///< the language it is about
    kit::DisplayList list_{};        ///< the question, focus ring on Download
    kit::Interaction interaction_{}; ///< the pointer and the focus
    kit::Frame frame_{};             ///< the frame it was last laid out on
    bool have_frame_{};              ///< lay_out has seen a frame
    uint64_t revision_{};            ///< changes when the drawing changes
};

/// Returns the notice's screen, showing or waiting.
///
/// @param layer the OA layer; null when the runtime has none yet
/// @return the screen; null when there is none
LanguageNoticeScreen* notice_screen(OaLayer* layer) {
    if (layer == nullptr)
        return nullptr;
    if (LayerScreen* shown = layer->find(kScreenName))
        return dynamic_cast<LanguageNoticeScreen*>(shown);
    if (LayerScreen* waiting = layer->waiting(kScreenName))
        return dynamic_cast<LanguageNoticeScreen*>(waiting);
    return nullptr;
}

/// Closes the notice when it is up. A command line that names the language
/// does this, and so does the language becoming one the game can show.
///
/// @param layer the OA layer
void close_notice(OaLayer& layer) {
    if (LanguageNoticeScreen* screen = notice_screen(&layer))
        layer.close(screen);
}

} // namespace

/// Returns one preference, trimmed. Empty when the key is absent.
///
/// @param runtime the runtime
/// @param key the key
/// @return the value
std::string LanguageNoticeHost::preference(const Runtime& runtime, std::string_view key) {
    const auto found = runtime.preference_values_.find(std::string(key));
    if (found == runtime.preference_values_.end())
        return {};
    return std::string(settings::language_notice_trim(found->second));
}

/// Tells whether this run is one nobody watches.
///
/// @param runtime the runtime
/// @return true when nobody watches
bool LanguageNoticeHost::unwatched_run(const Runtime& runtime) {
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver == nullptr)
        driver = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    const char* ci = SDL_getenv("CI");
    return runtime.options_.unattended ||
           unattended_environment(
               ci == nullptr ? std::string_view{} : std::string_view(ci),
               driver == nullptr ? std::string_view{} : std::string_view(driver)
           );
}

/// Tells whether the main menu is the settled screen.
///
/// @param runtime the runtime
/// @return true when it is
bool LanguageNoticeHost::menu_settled(const Runtime& runtime) {
    return runtime.screen_ == Screen::main_menu && !runtime.frame_owned_by_package() &&
           runtime.state_.state == frontend_state::state_id::main_menu &&
           runtime.state_.pending_signal != frontend_state::signal_id::multiplayer;
}

/// Returns the case the notice applies to, when one does.
///
/// The chosen case reads the language preference itself. An available tag
/// there is the case. System default is the case when the operating system's
/// language is available and has not been dismissed.
///
/// @param runtime the runtime
/// @return the case; nothing when the notice does not apply
std::optional<NoticeCase> LanguageNoticeHost::notice_case(Runtime& runtime) {
    const std::string raw = preference(runtime, settings::key::language);
    if (!raw.empty() && !same_tag(raw, languages::system_choice)) {
        const languages::Language* language = languages::find_by_tag(raw);
        if (language != nullptr && language->source == languages::Source::available &&
            languages::meets(language->needs) && !languages::playable(*language))
            return NoticeCase{std::string(language->tag), false};
        return std::nullopt;
    }
    if (runtime.language_state().choice != languages::system_choice)
        return std::nullopt;
    const languages::Language* wanted =
        languages::wanted_language(oa::platform::locale::preferred_locales());
    if (wanted == nullptr || !languages::meets(wanted->needs) ||
        wanted->source != languages::Source::available || languages::playable(*wanted))
        return std::nullopt;
    const auto dismissed =
        runtime.preference_values_.find(std::string(settings::key::language_notice_dismissed));
    const std::string_view stored = dismissed == runtime.preference_values_.end()
                                        ? std::string_view{}
                                        : std::string_view(dismissed->second);
    if (settings::language_notice_dismissed_has(stored, wanted->tag))
        return std::nullopt;
    return NoticeCase{std::string(wanted->tag), true};
}

/// Returns the catalogue row for a tag.
///
/// @param runtime the runtime
/// @param tag the language's tag
/// @return the row; present is false when none lists it
Offer LanguageNoticeHost::offer_of(Runtime& runtime, std::string_view tag) {
    Offer offer;
    for (const Runtime::CatalogueLanguage& pack : runtime.language_state().available) {
        if (!same_tag(pack.entry.tag, tag))
            continue;
        offer.present = true;
        offer.registry = pack.registry;
        offer.bytes = pack.bytes;
        break;
    }
    return offer;
}

/// Tells whether a registry can download.
///
/// A registry the snapshot does not list can. No snapshot yet can too.
/// Downloads are off when the registry requires an install id and that id is off.
///
/// @param runtime the runtime
/// @param registry the registry id
/// @return false when its downloads are off
bool LanguageNoticeHost::downloads_on(Runtime& runtime, std::string_view registry) {
    if (!runtime.content_)
        return true;
    const std::shared_ptr<const content::Snapshot> snapshot = runtime.content_service().snapshot();
    if (!snapshot)
        return true;
    for (const content::RegistryView& listed : snapshot->registries) {
        if (listed.registry.descriptor.id != registry)
            continue;
        const bool required =
            listed.registry.descriptor.install_id == oa::data::registry::InstallIdUse::required;
        if (required && content::read_install_id(runtime.preference_values_, registry).state ==
                            content::InstallIdState::off)
            return false;
        return true;
    }
    return true;
}

/// Returns the queued pack for a tag, when the queue is already started.
///
/// @param runtime the runtime
/// @param tag the language's tag
/// @param registry the catalogue's registry; empty to take any
/// @return the pack
QueuedPack
LanguageNoticeHost::queued_pack(Runtime& runtime, std::string_view tag, std::string_view registry) {
    QueuedPack queued;
    if (!runtime.content_)
        return queued;
    const std::vector<content::DownloadView> items = runtime.content_downloads().view();
    for (const content::DownloadView& item : items) {
        if (!same_tag(item.target.key, tag))
            continue;
        if (!registry.empty() && item.target.registry != registry)
            continue;
        if (item.state == content::DownloadState::failed ||
            item.state == content::DownloadState::cancelled) {
            queued.failed = true;
            return queued;
        }
        queued.active = true;
        const uint64_t size = item.target.size;
        if (size == 0 || item.done >= size)
            queued.fraction = item.done >= size && size != 0 ? 1.F : 0.F;
        else
            queued.fraction =
                static_cast<float>(static_cast<double>(item.done) / static_cast<double>(size));
        queued.progress =
            settings::download_size_text(item.done) + " of " + settings::download_size_text(size);
        return queued;
    }
    return queued;
}

settings::LanguageNotice LanguageNoticeHost::model(Runtime& runtime) {
    const std::string tag = runtime.language_notice_run_.tag;
    const languages::Language* known = languages::find_by_tag(tag);
    const languages::PackNotice* lines = languages::pack_notice(tag);
    std::string english_name(tag);
    if (known != nullptr && !known->english_name.empty())
        english_name = std::string(known->english_name);
    std::string title = english_name;
    if (known != nullptr && !known->endonym.empty() && runtime.modern_fonts_draw(known->endonym))
        title = std::string(known->endonym);
    const Offer offer = offer_of(runtime, tag);
    const bool can_download = offer.present && downloads_on(runtime, offer.registry);
    const QueuedPack queued = queued_pack(runtime, tag, offer.registry);
    const bool failed =
        !queued.active && (queued.failed || runtime.language_notice_run_.download_failed);

    settings::LanguageNotice notice;
    notice.tag = tag;
    notice.title = std::move(title);
    if (lines != nullptr) {
        if (offer.present)
            notice.own_body =
                with_size(lines->body_sized, settings::download_size_text(offer.bytes));
        else
            notice.own_body = std::string(lines->body);
    }
    if (offer.present) {
        notice.english_body = english_name + std::string(kSizedEnglish) +
                              settings::download_size_text(offer.bytes) +
                              std::string(kSizedEnglishTail);
    } else {
        notice.english_body = english_name + std::string(kSizelessEnglish);
    }
    notice.download_enabled = can_download;
    if (failed) {
        notice.failed = true;
        notice.download_label = lines != nullptr ? std::string(lines->retry) : "Try again";
        notice.status = lines != nullptr ? std::string(lines->failed) + "\n" : std::string();
        notice.status += kFailedEnglish;
        return notice;
    }
    if (queued.active) {
        notice.download_label = lines != nullptr ? std::string(lines->download) : "Download";
        notice.progress = queued.fraction;
        notice.progress_text = queued.progress;
        return notice;
    }
    notice.download_label = lines != nullptr ? std::string(lines->download) : "Download";
    if (!can_download) {
        if (lines != nullptr)
            notice.status = std::string(lines->unavailable) + "\n";
        notice.status += kCannotEnglish;
    }
    return notice;
}

bool LanguageNoticeHost::tag_playable(std::string_view tag) {
    const languages::Language* language = languages::find_by_tag(tag);
    return language != nullptr && languages::playable(*language);
}

void LanguageNoticeHost::download(Runtime& runtime) {
    std::string failure;
    const bool queued = runtime.download_language(runtime.language_notice_run_.tag, failure);
    const QueuedPack pack = queued_pack(runtime, runtime.language_notice_run_.tag, {});
    if (pack.active)
        runtime.language_notice_run_.download_failed = false;
    else if (!queued || pack.failed)
        runtime.language_notice_run_.download_failed = true;
    else
        runtime.language_notice_run_.download_failed = false;
    static_cast<void>(failure);
}

void LanguageNoticeHost::use_english(Runtime& runtime) {
    auto& run = runtime.language_notice_run_;
    if (run.system_case) {
        const auto found =
            runtime.preference_values_.find(std::string(settings::key::language_notice_dismissed));
        const std::string_view stored = found == runtime.preference_values_.end()
                                            ? std::string_view{}
                                            : std::string_view(found->second);
        runtime.preference_values_[std::string(settings::key::language_notice_dismissed)] =
            settings::language_notice_dismiss_tag(stored, run.tag);
    } else {
        runtime.preference_values_[std::string(settings::key::language)] = "en";
        runtime.set_language_choice("en");
    }
    runtime.preferences_dirty_ = true;
    try {
        runtime.flush_preferences();
    } catch (const std::exception& failure) {
        std::cerr << "open-annihilation: the language notice was not recorded: " << failure.what()
                  << '\n';
    }
}

void LanguageNoticeHost::close_for_run(Runtime& runtime) noexcept {
    runtime.language_notice_run_.closed_for_run = true;
}

void LanguageNoticeHost::tell(Runtime& runtime) {
    if (runtime.first_run_blocks_language_notice()) {
        if (runtime.oa_layer_)
            close_notice(runtime.oa_layer());
        return;
    }
    if (oa::app::command_line::launch_language(runtime.options_.launch) != nullptr) {
        if (runtime.oa_layer_)
            close_notice(runtime.oa_layer());
        return;
    }
    if (LanguageNoticeScreen* screen =
            notice_screen(runtime.oa_layer_ ? &runtime.oa_layer() : nullptr)) {
        if (tag_playable(runtime.language_notice_run_.tag)) {
            runtime.oa_layer().close(screen);
            return;
        }
        screen->refresh(model(runtime));
        return;
    }
    auto& run = runtime.language_notice_run_;
    if (run.closed_for_run)
        return;
    const std::optional<NoticeCase> decided = notice_case(runtime);
    if (!decided) {
        run.main_menu_frames = 0;
        return;
    }
    if (!menu_settled(runtime)) {
        run.main_menu_frames = 0;
        return;
    }
    if (++run.main_menu_frames < kNoticeMenuFrames ||
        oa::ui::frontend_dialogs::dialog_count() != 0 ||
        runtime.engine_settings_dialog() != nullptr || runtime.engine_settings_fonts() == nullptr)
        return;
    if (unwatched_run(runtime) && !runtime.user_folder_state().check_shows_notice)
        return;
    run.tag = decided->tag;
    run.system_case = decided->system_case;
    run.download_failed = false;
    runtime.oa_layer().show_when_free(
        std::make_unique<LanguageNoticeScreen>(runtime, model(runtime), run.tag)
    );
}

void Runtime::tell_missing_language() {
    LanguageNoticeHost::tell(*this);
}

} // namespace oa::app

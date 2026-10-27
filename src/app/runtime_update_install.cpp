// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The download, the ready prompt, the Updated notice, the receipt the
// helper left, and the hand-off to oa-update. Download and install are
// separate decisions: a ready package installs only when the player
// scheduled it for the quit or chose to install now. A check records the
// helper instead of starting it. With self-update left out, none of the
// download or the hand-off is compiled in.
#include "oa/app/runtime.hpp"

#include "engine_settings_state.hpp"
#include "oa_layer.hpp"
#include "runtime_update_start.hpp"
#include "update_state.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/app/update/build.hpp"
#include "oa/app/update/centre.hpp"
#include "oa/app/update/choices.hpp"
#include "oa/app/update/offer.hpp"
#include "oa/app/update/plan.hpp"
#include "oa/app/update/receipt.hpp"
#include "oa/app/update/route.hpp"
#include "oa/platform/machine.hpp"
#include "oa/data/engine_feed/feed.hpp"
#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/platform/files.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/platform/system.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/library/text.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#ifndef OA_PROCESS_SPAWNING
#error "OA_PROCESS_SPAWNING (0 or 1) says whether this build starts other programs"
#endif

#if defined(OA_SELF_UPDATE) && OA_SELF_UPDATE
#include "oa/app/update/download.hpp"
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace oa::app {

namespace feed = oa::data::engine_feed;
namespace library = oa::ui::library;
namespace kit = oa::ui::kit;
namespace settings = oa::ui::engine_settings;
namespace languages = oa::data::languages;

namespace {

namespace fs = std::filesystem;
namespace update = oa::app::update;

/// The ready prompt's word, so it is not the update question.
constexpr std::string_view kReadyWord = "ready-update";
/// Keep it ready, the button Escape answers: nothing is scheduled.
[[maybe_unused]] constexpr int32_t kKeepReady = 0;
/// The install-now button, named for the route.
[[maybe_unused]] constexpr int32_t kInstallNow = 1;
/// Install when I quit, the primary button: the release installs the next
/// time the game quits.
[[maybe_unused]] constexpr int32_t kWhenIQuit = 2;
/// The receipt notice's word on the layer.
constexpr std::string_view kReceiptWord = "update-receipt";
/// The heading of the box that lists the receipt's steps.
constexpr std::string_view kWhatHappened = "What happened";
/// The heading of the box that lists the steps that put an unfinished recovery right.
constexpr std::string_view kPutRight = "To put the game right";
/// The recovery screen's callout when recovery did not finish: the steps that
/// put the game right are listed under it.
constexpr std::string_view kLeftHalfMoved = "Some files were left half-moved.";
/// The line the recovery screen's callout says where the old version was put back.
constexpr std::string_view kKeptAsTheyWere =
    "Your saved games, settings and mods stay as they are.";
/// Show the log on the recovery screen: quiet, at the footer's left.
constexpr int32_t kReceiptLog = 0;
/// The card's first line on Settings › Updates; {version} is this build.
constexpr std::string_view kInstalledVersion = "Installed {version}";
/// The card's heading while the dialog shows Check for updates at Never.
constexpr std::string_view kNeverChecks = "OA never checks";
/// The badge beside Updates in Settings' section list while a release is ready.
constexpr std::string_view kReadyBadge = "ready";
/// The ready prompt's heading over its three answers.
[[maybe_unused]] constexpr std::string_view kWhenShouldItGoIn = "When should it go in?";
/// A receipt left running this long was cut short, in seconds. A younger one
/// may belong to a helper that is still at work.
constexpr int64_t kReceiptCutShortSeconds = 600;
/// The Updated notice's name on the layer.
constexpr std::string_view kUpdatedNotice = "updated-notice";

/// The title of the ready prompt.
[[maybe_unused]] constexpr std::string_view kReadyTitle = "Open Annihilation {version} is ready";
/// The helper's window title. {version} is the release being installed.
[[maybe_unused]] constexpr std::string_view kInstalling = "Installing {version}";
/// The helper's window line. {version} is the version that is running.
[[maybe_unused]] constexpr std::string_view kKept =
    "{version} is kept, in case you want it back. The game starts again by itself.";
/// The helper's window line when Installer will put the app in Applications.
/// {version} is the release being installed.
[[maybe_unused]] constexpr std::string_view kInstallerWill =
    "Installer will put {version} in Applications. Start it from there when it finishes.";
/// The Updated notice's title. {version} is this build.
constexpr std::string_view kUpdatedTo = "Updated to {version}";
/// The Updated notice's line while a version is kept. {version} is that one.
constexpr std::string_view kKeptSentence =
    "{version} is kept. To go back to it, open Settings › Updates and press ROLL BACK.";
/// The prompt's title. {version} is this build.
constexpr std::string_view kBackOn = "Back on {version}";
/// Failed starts: {version} is the one that closed, {old} is this build.
constexpr std::string_view kClosedTwice =
    "{version} closed twice before it reached the main menu, so OA went back to {old}, with the "
    "settings you had.";
/// Failed starts: {version} is the one that will not be offered.
constexpr std::string_view kWontBeOffered =
    "{version} won't be offered again. The next release will be, or choose {version} in "
    "Settings › Updates to try it again.";
/// The player's roll back. {old} is this build.
constexpr std::string_view kYouWentBack = "You went back to {old}, with the settings you had.";
/// The player's roll back. {new} is the version that was left.
constexpr std::string_view kNewWont =
    "{new} won't be offered again until a newer release, or choose it in Settings › Updates.";
/// The helper's title for a roll back. {version} is the one being restored.
[[maybe_unused]] constexpr std::string_view kGoingBack = "Going back to {version}";
/// SHOW THE LOG, and the button that leaves the question up.
constexpr int32_t kShowTheLog = 0;
/// OK, the button Escape answers.
constexpr int32_t kBackOk = 1;

/// Returns one of the interface's words in the language shown.
///
/// @param context unused
/// @param english the word, in English
/// @return its translation, or the English
std::string_view shown_word(void* /*context*/, std::string_view english) {
    return languages::interface_text(english);
}

/// Returns the English word. The helper's window uses this when the player's
/// language is not one the bundled fonts can draw.
///
/// @param context unused
/// @param english the word
/// @return the same word
std::string_view english_word(void* /*context*/, std::string_view english) {
    return english;
}

/// True when this machine is a Steam Deck or this run is in Steam Game Mode.
///
/// @return true when the update installs at the next start
[[maybe_unused]] bool installs_at_next_start() {
    return oa::platform::running_steam_deck_model() != oa::platform::SteamDeckModel::none ||
           oa::platform::running_in_steam_game_mode();
}

/// The hooks a line of the player's own is looked up through.
///
/// @return the hooks
library::TextHooks shown_hooks() {
    return library::TextHooks{nullptr, shown_word};
}

/// The hooks the helper's title and line are looked up through.
///
/// English, and a language compiled into the game, are translated. Any other
/// language stays English: the helper draws with the fonts the game ships.
///
/// @return the hooks
[[maybe_unused]] library::TextHooks plan_hooks() {
    const languages::Language& shown = languages::interface_language();
    if (shown.source == languages::Source::built_in || shown.tag == "en")
        return shown_hooks();
    return library::TextHooks{nullptr, english_word};
}

/// This program's build. The base path is cached for the process.
update::ThisBuild running_build() {
    // SDL caches this string for the process. The caller does not free it.
    const char* const base = SDL_GetBasePath();
    if (base == nullptr)
        return update::this_build({});
    return update::this_build(path_from_utf8(base));
}

/// The folder that holds this program. Empty when it cannot be named.
[[maybe_unused]] fs::path program_folder() {
    const char* const base = SDL_GetBasePath();
    if (base == nullptr)
        return {};
    return path_from_utf8(base);
}

/// `<data>/updates`, or nothing when the data folder cannot be named.
std::optional<fs::path> updates_folder(const Options& options, std::string& failure) {
    try {
        const fs::path data =
            options.data_dir ? *options.data_dir : oa::platform::preferences::data_directory();
        return data / "updates";
    } catch (const std::exception& thrown) {
        failure = thrown.what();
        return std::nullopt;
    }
}

/// An absolute path as UTF-8. A path the system cannot make absolute is
/// returned as it was given.
[[maybe_unused]] std::string absolute_utf8(const fs::path& path) {
    std::error_code error;
    const fs::path full = fs::absolute(path, error);
    if (error || full.empty())
        return path_to_utf8(path);
    return path_to_utf8(full);
}

/// This process's id, for the plan's wait.
[[maybe_unused]] int64_t own_process() noexcept {
#ifdef _WIN32
    return static_cast<int64_t>(GetCurrentProcessId());
#else
    return static_cast<int64_t>(getpid());
#endif
}

/// The ready prompt, named by its word.
bool is_ready_question(const LayerScreen* screen) {
    if (screen == nullptr || screen->name() != QuestionScreen::screen_name)
        return false;
    const kit::Question& asked = static_cast<const QuestionScreen*>(screen)->question();
    return asked.word == kReadyWord;
}

/// Seconds since 1970.
int64_t wall_seconds() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(now).count();
}

/// Returns a part of a question that holds one box.
///
/// @param heading the box's heading, looked up
/// @param lines the box's lines, each with its mark
/// @return the part
kit::QuestionPart box_part(std::string_view heading, std::vector<kit::BoxLine> lines) {
    kit::QuestionPart part;
    part.kind = kit::PartKind::boxes;
    kit::QuestionBox box;
    box.heading = std::string(languages::interface_text(heading));
    box.lines = std::move(lines);
    part.boxes.push_back(std::move(box));
    return part;
}

/// The recovery screen of a receipt that failed: its outcome's words as the
/// title; a callout that says what stayed as it was, or, when recovery did
/// not finish, what was left; What happened, each step marked done or
/// failed; the steps that put the game right when recovery did not finish;
/// the release that waits; and Show the log, Try again where the release
/// can be tried again, and Continue.
///
/// @param receipt the receipt
/// @param centre the update centre's state for it
/// @param running this build's version, for the steps that put the game right
/// @param retry Try again is offered
/// @return the question; its word is update-receipt and Continue's id is ok
kit::Question receipt_question(
    const update::Receipt& receipt,
    const update::Centre& centre,
    std::string_view running,
    bool retry
) {
    const library::TextHooks hooks = shown_hooks();
    const update::CentreWords words = update::centre_words(centre, hooks);
    kit::Question asked;
    asked.word = std::string(kReceiptWord);
    asked.title = words.status;
    kit::QuestionPart callout;
    callout.kind = kit::PartKind::callout;
    callout.tone = kit::CalloutTone::ok;
    if (centre.stage == update::Stage::recovery_incomplete) {
        callout.tone = kit::CalloutTone::danger;
        callout.text = std::string(languages::interface_text(kLeftHalfMoved));
    } else if (centre.stage == update::Stage::failed_restored) {
        callout.text = std::string(languages::interface_text(kKeptAsTheyWere));
    } else {
        callout.text = words.detail;
    }
    if (!callout.text.empty())
        asked.parts.push_back(std::move(callout));
    std::vector<kit::BoxLine> happened;
    for (const update::ReceiptStep& step : receipt.steps)
        happened.push_back(
            {step.result == update::StepResult::done ? kit::LineMark::done : kit::LineMark::failed,
             update::step_words(step, receipt.from_version, receipt.to_version, hooks)}
        );
    if (!happened.empty())
        asked.parts.push_back(box_part(kWhatHappened, std::move(happened)));
    if (centre.stage == update::Stage::recovery_incomplete) {
        const std::string version =
            receipt.from_version.empty() ? std::string(running) : receipt.from_version;
        std::vector<kit::BoxLine> right;
        for (std::string& line : update::recovery_steps(version, hooks))
            right.push_back({kit::LineMark::bullet, std::move(line)});
        asked.parts.push_back(box_part(kPutRight, std::move(right)));
    }
    if (centre.stage == update::Stage::failed_restored && !words.detail.empty()) {
        kit::QuestionPart waits;
        waits.kind = kit::PartKind::text;
        waits.text = words.detail;
        asked.parts.push_back(std::move(waits));
    }
    std::string show_log;
    std::string try_again;
    for (const update::ActWords& act :
         update::centre_words(
             [&]() {
                 update::Centre both = centre;
                 both.actions = {update::Act::show_log, update::Act::retry};
                 return both;
             }(),
             hooks
         )
             .actions) {
        if (act.act == update::Act::show_log)
            show_log = act.caption;
        else if (act.act == update::Act::retry)
            try_again = act.caption;
    }
    kit::QuestionButton log_button{show_log, false, "show-the-log"};
    log_button.quiet = true;
    asked.buttons.push_back(std::move(log_button));
    if (retry)
        asked.buttons.push_back({try_again, false, "try-again"});
    asked.buttons.push_back({std::string(languages::interface_text("Continue")), true, "ok"});
    const auto continue_button = static_cast<int32_t>(asked.buttons.size() - 1);
    asked.cancel_button = continue_button;
    asked.primary_button = continue_button;
    asked.marked = continue_button;
    return asked;
}

/// The word of the update centre's question, which the update pill opens.
constexpr std::string_view kCentreWord = "update-centre";
/// The update pill's button that opens the ready prompt again.
constexpr std::string_view kPillInstall = "Install\u2026";
/// The update pill's button that asks what a scheduled release does.
constexpr std::string_view kPillChange = "Change";
/// The rows between the update pill and the main menu's buttons above it, in points.
constexpr int32_t kPillGap = 8;
/// The update pill's margin from the main menu's top and right edges, in points.
constexpr int32_t kPillMargin = 12;

/// Returns the word an action's button is named by to automation.
///
/// @param act the action
/// @return a word of a-z and hyphens
std::string_view act_id(update::Act act) noexcept {
    switch (act) {
    case update::Act::check_now:
        return "check-now";
    case update::Act::download:
        return "download";
    case update::Act::pause:
        return "pause";
    case update::Act::resume:
        return "resume";
    case update::Act::cancel_download:
        return "cancel-download";
    case update::Act::install_on_quit:
        return "when-i-quit";
    case update::Act::install_now:
        return "restart-now";
    case update::Act::keep_ready:
        return "keep-ready";
    case update::Act::unschedule:
        return "unschedule";
    case update::Act::skip:
        return "skip";
    case update::Act::later:
        return "later";
    case update::Act::try_version:
        return "try-version";
    case update::Act::retry:
        return "try-again";
    case update::Act::details:
        return "details";
    case update::Act::show_log:
        return "show-the-log";
    case update::Act::open_page:
        return "page";
    case update::Act::roll_back:
        return "roll-back";
    case update::Act::whats_new:
        return "whats-new";
    }
    return "action";
}

/// Returns the caption the update centre gives one of a state's actions.
///
/// @param words the state's words
/// @param act the action
/// @return the caption; empty when the state does not offer it
std::string act_caption(const update::CentreWords& words, update::Act act) {
    for (const update::ActWords& offered : words.actions)
        if (offered.act == act)
            return offered.caption;
    return {};
}

/// Returns the update pill for the update centre's state: a download under
/// way, paused, being checked, ready, scheduled or stopped, with the one
/// action that fits in it. Every other state shows no pill.
///
/// @param centre the update centre's state
/// @return the pill; nothing when the state shows none
std::optional<kit::UpdatePill> pill_of(const update::Centre& centre) {
    const update::CentreWords words = update::centre_words(centre, shown_hooks());
    kit::UpdatePill pill;
    pill.text = words.pill;
    const int32_t percent = std::max(centre.percent, int32_t{0});
    switch (centre.stage) {
    case update::Stage::downloading:
        pill.icon = kit::UpdatePillIcon::download;
        pill.percent = percent;
        pill.caption = act_caption(words, update::Act::pause);
        pill.button_id = "pause";
        break;
    case update::Stage::paused_by_player:
        pill.icon = kit::UpdatePillIcon::paused;
        pill.percent = percent;
        pill.paused = true;
        pill.caption = act_caption(words, update::Act::resume);
        pill.button_id = "resume";
        break;
    case update::Stage::paused_by_match:
        pill.icon = kit::UpdatePillIcon::paused;
        pill.percent = percent;
        pill.paused = true;
        break;
    case update::Stage::verifying:
        pill.icon = kit::UpdatePillIcon::download;
        pill.percent = 100;
        break;
    case update::Stage::ready:
        pill.icon = kit::UpdatePillIcon::ready;
        pill.caption = std::string(languages::interface_text(kPillInstall));
        pill.button_id = "install";
        break;
    case update::Stage::scheduled:
        pill.icon = kit::UpdatePillIcon::scheduled;
        pill.caption = std::string(languages::interface_text(kPillChange));
        pill.button_id = "change";
        break;
    case update::Stage::could_not_connect:
    case update::Stage::not_verified:
        // Only a download's failure: a check's own stays in Settings.
        if (centre.reason.empty())
            return std::nullopt;
        [[fallthrough]];
    case update::Stage::download_stopped:
        pill.icon = kit::UpdatePillIcon::problem;
        pill.caption = act_caption(words, update::Act::retry);
        pill.button_id = "retry";
        break;
    default:
        return std::nullopt;
    }
    return pill;
}

/// The update centre as a question, which the update pill opens: the
/// state's heading as the title, its detail, the bar while a download is
/// under way, its note, and its actions as buttons, the first in the accent,
/// then Close where there is room.
///
/// @param centre the update centre's state
/// @param[out] acts the action of each button, from the left; Close has none
/// @return the question; its word is update-centre
kit::Question centre_question(const update::Centre& centre, std::vector<update::Act>& acts) {
    const update::CentreWords words = update::centre_words(centre, shown_hooks());
    kit::Question asked;
    asked.word = std::string(kCentreWord);
    asked.title = words.status;
    if (!words.detail.empty())
        asked.paragraphs.push_back({words.detail, false});
    if (centre.percent >= 0 && (centre.stage == update::Stage::downloading ||
                                centre.stage == update::Stage::paused_by_player ||
                                centre.stage == update::Stage::paused_by_match)) {
        asked.progress = centre.percent * kit::question_progress_whole / 100;
        asked.progress_paused = centre.stage != update::Stage::downloading;
    }
    if (!words.note.empty()) {
        kit::QuestionPart note;
        note.kind = kit::PartKind::text;
        note.text = words.note;
        asked.parts.push_back(std::move(note));
    }
    acts.clear();
    for (const update::ActWords& act : words.actions) {
        if (act.act == update::Act::roll_back || asked.buttons.size() == kit::most_question_buttons)
            continue;
        asked.buttons.push_back({act.caption, asked.buttons.empty(), std::string(act_id(act.act))});
        acts.push_back(act.act);
    }
    if (asked.buttons.size() < kit::most_question_buttons)
        asked.buttons.push_back({std::string(languages::interface_text("Close")), false, "close"});
    const auto last = static_cast<int32_t>(asked.buttons.size()) - 1;
    asked.cancel_button = last;
    asked.primary_button = 0;
    asked.marked = 0;
    return asked;
}

/// The update pill on the main menu: the update centre's state in short,
/// at the main menu's top right, with one small button. A press on the pill
/// opens the update centre's question. It takes no key, and what it does not
/// take goes on to the main menu.
class UpdatePillScreen final : public LayerScreen {
  public:

    /// What the pill's host does.
    struct Host {
        /// Returns the pill as the update centre's state shows it now;
        /// nothing closes the pill.
        std::function<std::optional<kit::UpdatePill>()> look{};
        /// Opens the update centre's question, where the state is told in full.
        std::function<void()> opened{};
        /// Takes the pill's button: its id.
        std::function<void(const std::string& button)> pressed{};
    };

    /// Makes the pill.
    ///
    /// @param layer the layer it shows on, whose fonts it is drawn in
    /// @param pill how it looks first
    /// @param host what it does
    UpdatePillScreen(OaLayer& layer, kit::UpdatePill pill, Host host)
        : layer_(layer), pill_(std::move(pill)), host_(std::move(host)) {}

    /// Returns "update-pill".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return kit::update_pill_word; }

    /// Places the pill at the main menu's top right, clear of the game's own
    /// buttons; under the main menu's buttons while an extension's overlay
    /// has put them at the top. It shows on the main menu alone, and not
    /// while a modal screen shows.
    ///
    /// @param view what it is placed on
    /// @return its place; empty off the main menu
    [[nodiscard]] LayerPlacement placement(const LayerView& view) const override {
        // A modal screen, such as Settings or a prompt, has the player's
        // attention: the pill steps aside until it closes.
        if (view.match || view.game_screen != Screen::main_menu ||
            layer_.screen_fonts() == nullptr || layer_.modal_shown(false))
            return {};
        const kit::PlacedUpdatePill placed = kit::place_update_pill(pill_, layer_.screen_fonts());
        const int32_t percent = std::max(view.scale_percent, int32_t{100});
        const auto points = [percent](int32_t pixels) {
            return static_cast<int32_t>(int64_t{pixels} * 100 / percent);
        };
        const int32_t right = points(view.picture.x + view.picture.width) - kPillMargin;
        int32_t top = points(view.picture.y) + kPillMargin;
        const OaLayer::MenuButtonWindows buttons = layer_.menu_button_windows();
        const int32_t middle = points(view.picture.y + view.picture.height / 2);
        if (buttons.settings.height > 0 && points(buttons.settings.y) < middle)
            top = points(buttons.settings.y + buttons.settings.height) + kPillGap;
        const kit::Rect at{right - placed.width, top, placed.width, placed.height};
        const kit::Rect shown = kit::to_canvas(at, percent);
        return {
            {shown.x, shown.y, shown.width, shown.height}, placed.width, placed.height, percent
        };
    }

    /// Tells that what the pill does not take goes on.
    ///
    /// @return false
    [[nodiscard]] bool modal() const override { return false; }

    /// Tells that the pill darkens nothing.
    ///
    /// @return false
    [[nodiscard]] bool backdrop() const override { return false; }

    /// Tells that the pill's corners show the main menu.
    ///
    /// @return false
    [[nodiscard]] bool opaque() const override { return false; }

    /// Draws the pill.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override { kit::draw_update_pill(canvas, pill_); }

    /// Takes the pointer over the pill: it lights, a press on it holds, and a
    /// release over what the press held opens the update centre's question
    /// or presses the button. Anything off the pill goes on.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the pill's points
    /// @return what the pill did
    LayerAnswer pointer(
        ScreenInputKind kind, uint8_t button, kit::Point at, int32_t /*finger_reach*/
    ) override {
        const kit::Fonts* fonts = layer_.screen_fonts();
        const kit::PlacedUpdatePill placed = kit::place_update_pill(pill_, fonts);
        const bool inside = at.x >= 0 && at.y >= 0 && at.x < placed.width && at.y < placed.height;
        kit::PillPointer what = kit::PillPointer::move;
        switch (kind) {
        case ScreenInputKind::pointer_move:
            break;
        case ScreenInputKind::pointer_down:
            if (button != SDL_BUTTON_LEFT || !inside)
                return LayerAnswer::pass;
            what = kit::PillPointer::press;
            break;
        case ScreenInputKind::pointer_up:
            if (button != SDL_BUTTON_LEFT || pill_.pressed == kit::no_control)
                return LayerAnswer::pass;
            what = kit::PillPointer::release;
            break;
        default:
            return LayerAnswer::pass;
        }
        const kit::UpdatePillAction acted = kit::update_pill_pointer(pill_, what, at, fonts);
        if (acted != kit::UpdatePillAction::none)
            ++revision_;
        if (acted == kit::UpdatePillAction::opened && host_.opened)
            host_.opened();
        else if (acted == kit::UpdatePillAction::pressed && host_.pressed)
            host_.pressed(pill_.button_id);
        if (what == kit::PillPointer::move && !inside)
            return LayerAnswer::pass;
        return acted == kit::UpdatePillAction::none ? LayerAnswer::none : LayerAnswer::redraw;
    }

    /// Lets every key go on.
    ///
    /// @return pass
    LayerAnswer key(kit::Key /*pressed*/, uint32_t /*sdl_key*/) override {
        return LayerAnswer::pass;
    }

    /// Lets the wheel go on.
    ///
    /// @return pass
    LayerAnswer wheel(kit::Point /*at*/, float /*notches*/) override { return LayerAnswer::pass; }

    /// Returns the pill's display list.
    ///
    /// @return the list
    [[nodiscard]] kit::DisplayList display_list() const override {
        return kit::update_pill_list(pill_, layer_.screen_fonts());
    }

    /// Returns what the pointer lights and holds.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override {
        kit::Interaction shown{};
        shown.hovered = pill_.hovered;
        shown.pressed = pill_.pressed;
        return shown;
    }

    /// Follows the update centre: new words, a new share or a new button
    /// redraw it, and a state that shows no pill closes it.
    ///
    /// @return close, redraw or none
    LayerAnswer tick() override {
        std::optional<kit::UpdatePill> now = host_.look ? host_.look() : std::nullopt;
        if (!now)
            return LayerAnswer::close;
        if (now->icon == pill_.icon && now->text == pill_.text && now->percent == pill_.percent &&
            now->paused == pill_.paused && now->caption == pill_.caption &&
            now->button_id == pill_.button_id)
            return LayerAnswer::none;
        now->hovered = pill_.hovered;
        now->pressed = pill_.pressed;
        pill_ = std::move(*now);
        ++revision_;
        return LayerAnswer::redraw;
    }

    /// Returns the pill's revision.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Tells that the pill draws only what its revision names.
    ///
    /// @return true
    [[nodiscard]] bool revision_covers_drawing() const override { return true; }

    /// Lets the pill go; it keeps nothing open.
    void close(bool /*by_key*/) override {}

  private:

    OaLayer& layer_;       ///< the layer it shows on
    kit::UpdatePill pill_; ///< how it looks
    Host host_;            ///< what it does
    uint64_t revision_{1}; ///< moves on with each change of look
};

/// The ready prompt, up or waiting.
[[maybe_unused]] bool ready_question_queued(OaLayer& layer) {
    return is_ready_question(layer.find(QuestionScreen::screen_name)) ||
           is_ready_question(layer.waiting(QuestionScreen::screen_name));
}

#if defined(OA_SELF_UPDATE) && OA_SELF_UPDATE

/// The route this run may install through.
///
/// The offer check asks to be told there is none, so Update opens the
/// release page. The download check names an install folder and keeps its
/// route.
update::Where route_for(const Options& options, feed::Way way) {
    update::RouteFacts facts = update::this_machine(program_folder());
    facts.install_folder = options.update_install_folder;
    facts.test_feed = options.update_feed.has_value();
    facts.way = way;
    facts.no_install_route = options.check_update_offer;
    return update::find_route(running_build(), facts);
}

/// True when the stored switch asks for a download in the background.
bool background_switch_on(const platform::preferences::Values& values) {
    const auto found = values.find(std::string(settings::key::update_download));
    return found != values.end() && found->second == "1";
}

/// The helper beside this program. Windows adds the suffix. On macOS the
/// helper sits in Contents/MacOS, the executable's own folder: the base
/// path names Contents/Resources instead.
fs::path helper_path(const fs::path& folder) {
#ifdef _WIN32
    return folder / "oa-update.exe";
#elif defined(__APPLE__)
    const update::RouteFacts facts = update::this_machine(folder);
    if (!facts.executable.empty())
        return facts.executable.parent_path() / "oa-update";
    return folder / "oa-update";
#else
    return folder / "oa-update";
#endif
}

#if defined(__APPLE__) && defined(OA_SELF_UPDATE) && OA_SELF_UPDATE
/// Reveals the bundle in Finder. A build that starts no program only says so.
void reveal_in_finder(const std::string& bundle) {
#if !defined(__APPLE__) || OA_PROCESS_SPAWNING == 0
    (void)bundle;
    std::cerr << "open-annihilation: this build cannot show the app in Finder\n";
#else
    std::vector<std::string> stored;
    stored.emplace_back("/usr/bin/open");
    stored.emplace_back("-R");
    stored.push_back(bundle);
    std::vector<char*> argv;
    argv.reserve(stored.size() + 1);
    for (std::string& word : stored)
        argv.push_back(word.data());
    argv.push_back(nullptr);
    const SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0) {
        std::cerr << "open-annihilation: " << SDL_GetError() << '\n';
        return;
    }
    void* const argument_list = argv.data();
    std::ignore =
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argument_list);
    std::ignore =
        SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process* process = SDL_CreateProcessWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (process == nullptr) {
        std::cerr << "open-annihilation: cannot show the app in Finder: " << SDL_GetError() << '\n';
        return;
    }
    SDL_DestroyProcess(process);
#endif
}
#endif

/// A recorded helper start. The caller has already stored the path and the
/// arguments, and this build starts nothing.
bool record_helper(
    void* /*context*/,
    const fs::path& /*helper*/,
    const std::vector<std::string>& /*arguments*/,
    std::string& /*why*/
) {
    return true;
}

/// Starts the helper and does not wait for it.
bool spawn_helper(
    void* /*context*/,
    const fs::path& helper,
    const std::vector<std::string>& arguments,
    std::string& why
) {
#if OA_PROCESS_SPAWNING == 0
    why = "this build starts no other program";
    (void)helper;
    (void)arguments;
    return false;
#else
    std::vector<std::string> stored;
    stored.reserve(arguments.size() + 1);
    stored.push_back(path_to_utf8(helper));
    stored.insert(stored.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(stored.size() + 1);
    for (std::string& word : stored)
        argv.push_back(word.data());
    argv.push_back(nullptr);
    const SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0) {
        why = SDL_GetError();
        return false;
    }
    // SDL reads the arguments as a mutable array but never writes them.
    void* const argument_list = argv.data();
    std::ignore =
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argument_list);
    std::ignore =
        SDL_SetBooleanProperty(properties, SDL_PROP_PROCESS_CREATE_BACKGROUND_BOOLEAN, true);
    SDL_Process* process = SDL_CreateProcessWithProperties(properties);
    SDL_DestroyProperties(properties);
    if (process == nullptr) {
        why = std::string("cannot start ") + stored.front() + ": " + SDL_GetError();
        return false;
    }
    SDL_DestroyProcess(process);
    return true;
#endif
}

/// Tells whether nobody is watching this run.
bool nobody_watching(const Options& options) {
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver == nullptr)
        driver = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    const char* const ci = SDL_getenv("CI");
    const std::string_view ci_text = ci == nullptr ? std::string_view{} : std::string_view(ci);
    const std::string_view driver_text =
        driver == nullptr ? std::string_view{} : std::string_view(driver);
    return options.unattended || unattended_environment(ci_text, driver_text);
}

/// Writes the plan. The name is the one a build without self-update must not contain.
bool write_update_plan(const fs::path& path, const update::Plan& plan, std::string& why) {
    const std::string text = update::plan_text(plan);
    const std::vector<uint8_t> bytes(text.begin(), text.end());
    if (!oa::platform::replace_file(path, bytes, &why))
        return false;
    return true;
}

/// One allowed argument of the plan, and whether its value is a path.
bool known_plan_flag(std::string_view flag, bool& path_value) {
    path_value = flag == "--game-dir" || flag == "--data-dir" || flag == "--user-folder" ||
                 flag == "--preferences-file" || flag == "--log-dir";
    return path_value || flag == "--update-feed" || flag == "--update-key";
}

#endif

/// The preferences file this run reads and writes.
fs::path live_preferences(const Options& options, const fs::path& preference_path) {
    if (options.preferences_file && !options.preferences_file->empty())
        return *options.preferences_file;
    if (!preference_path.empty())
        return preference_path;
    return oa::platform::preferences::default_file();
}

/// The install folder's `update-starts`, removed once this build is confirmed.
/// Anything that is not that file is left alone.
void remove_update_starts() {
    const fs::path counter = update_starts_file();
    std::error_code error;
    if (!counter.empty() && fs::is_regular_file(counter, error))
        fs::remove(counter, error);
}

/// Reads a whole file. Empty when it cannot be read.
std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

/// Reads a state file. Nothing when it is missing or refused.
std::optional<update::State> read_installed_state(const fs::path& updates) {
    const std::vector<uint8_t> bytes = read_bytes(updates / "state.json");
    if (bytes.empty())
        return std::nullopt;
    std::string error;
    return update::read_state(bytes, &error);
}

} // namespace

void Runtime::offer_move_to_applications() {
#if !defined(__APPLE__) || !defined(OA_SELF_UPDATE) || !OA_SELF_UPDATE
    return;
#else
    if (update_ == nullptr || update_->mac_move_offered)
        return;
    if (match_ != nullptr || screen_ == Screen::match)
        return;
    if (!options_.update_feed.has_value() && (options_.unattended || options_.headless_check))
        return;
    if (!update_->mac_facts_known) {
        const update::RouteFacts facts = update::this_machine(program_folder());
        update_->mac_facts_known = true;
        update_->mac_translocated = facts.translocated;
        update_->mac_bundle = path_to_utf8(facts.bundle);
    }
    if (!update_->mac_translocated || update_->mac_bundle.empty())
        return;
    if (!update_menu_settled())
        return;
    OaLayer& layer = oa_layer();
    const std::string later{languages::interface_text("LATER")};
    const std::string finder{languages::interface_text("SHOW IN FINDER")};
    const std::string title{languages::interface_text("Move OA to Applications")};
    const std::string body{languages::interface_text(
        "macOS runs this copy from a protected place, so it can't update itself. Drag Open "
        "Annihilation to Applications and start it from there."
    )};
    kit::Question asked;
    asked.word = "move-to-applications";
    asked.title = title;
    asked.paragraphs.push_back({body, false});
    asked.buttons.push_back({later, false, "later"});
    asked.buttons.push_back({finder, true, "finder"});
    asked.cancel_button = 0;
    asked.primary_button = 1;
    asked.marked = 1;
    const std::string bundle = update_->mac_bundle;
    update_->mac_move_offered = true;
    QuestionScreen::Host host;
    host.answer = [bundle](int32_t button) {
        if (button == 1)
            reveal_in_finder(bundle);
        return true;
    };
    Runtime* runtime = this;
    host.tick = [runtime]() {
        return runtime->match_ != nullptr || runtime->screen_ == Screen::match;
    };
    auto question = std::make_unique<QuestionScreen>(
        layer, Screen::main_menu, std::move(asked), std::move(host)
    );
    question->show_on_any_menu();
    layer.show_when_free(std::move(question));
#endif
}

bool Runtime::update_menu_settled() {
    return screen_ == Screen::main_menu && !frame_owned_by_package() &&
           state_.state == frontend::state_id::main_menu &&
           state_.pending_signal != frontend::signal_id::multiplayer &&
           oa::ui::frontend_dialogs::dialog_count() == 0 && engine_settings_dialog() == nullptr;
}

bool Runtime::hand_off_requested() const {
#if !defined(OA_SELF_UPDATE) || !OA_SELF_UPDATE
    return false;
#else
    if (update_ == nullptr || update_->download == nullptr)
        return false;
    if (update_->download->snapshot().state != update::DownloadState::ready)
        return false;
    // Quitting installs only what the player scheduled, or chose to install
    // now, which schedules it too. A package kept ready stays ready, and a
    // quit leaves it.
    return options_.update_and_quit ||
           update::installs_at_quit(preference_values_, update_->download_release);
#endif
}

void Runtime::hand_off_update() {
#if !defined(OA_SELF_UPDATE) || !OA_SELF_UPDATE
    return;
#else
    if (update_ == nullptr || update_->handed || !hand_off_requested())
        return;
    const update::DownloadSnapshot shot = update_->download->snapshot();
    if (shot.state != update::DownloadState::ready || shot.path.empty())
        return;
    std::string failure;
    const std::optional<fs::path> folder = updates_folder(options_, failure);
    if (!folder) {
        std::cerr << "open-annihilation: update: " << failure << '\n';
        return;
    }
    const feed::Way way = update_->held.offer.way == feed::Way::page_only ? feed::Way::page_only
                                                                          : feed::Way::in_place;
    const update::Where where = route_for(options_, way);
    if (!where.route.has_value() || where.program.empty() || where.install.empty()) {
        const std::string why =
            where.why.empty() ? std::string("no route for this platform yet") : where.why;
        std::cerr << "open-annihilation: update: " << why << '\n';
        return;
    }
    const std::string kept =
        update_->from_version.empty() ? running_build().version_text : update_->from_version;
    const library::TextHooks hooks = plan_hooks();
    update::Plan plan;
    plan.plan = update::plan_format;
    plan.action = update::Action::install;
    plan.route = *where.route;
    plan.wait_for = own_process();
    plan.install = absolute_utf8(where.install);
    plan.program = absolute_utf8(where.program);
    if (options_.game_dir_named && !options_.game_dir.empty()) {
        plan.arguments.push_back("--game-dir");
        plan.arguments.push_back(absolute_utf8(options_.game_dir));
    }
    if (options_.data_dir && !options_.data_dir->empty()) {
        plan.arguments.push_back("--data-dir");
        plan.arguments.push_back(absolute_utf8(*options_.data_dir));
    }
    if (options_.user_folder && !options_.user_folder->empty()) {
        plan.arguments.push_back("--user-folder");
        plan.arguments.push_back(absolute_utf8(*options_.user_folder));
    }
    if (options_.preferences_file && !options_.preferences_file->empty()) {
        plan.arguments.push_back("--preferences-file");
        plan.arguments.push_back(absolute_utf8(*options_.preferences_file));
    } else if (!preference_path_.empty()) {
        plan.arguments.push_back("--preferences-file");
        plan.arguments.push_back(absolute_utf8(preference_path_));
    }
    if (options_.log_dir && !options_.log_dir->empty()) {
        plan.arguments.push_back("--log-dir");
        plan.arguments.push_back(absolute_utf8(*options_.log_dir));
    }
    if (options_.skip_intro)
        plan.arguments.push_back("--skip-intro");
    if (options_.mute)
        plan.arguments.push_back("--mute");
    if (options_.update_feed.has_value() && !options_.update_feed->empty()) {
        plan.arguments.push_back("--update-feed");
        plan.arguments.push_back(*options_.update_feed);
    }
    for (const std::string& key : options_.update_keys) {
        plan.arguments.push_back("--update-key");
        plan.arguments.push_back(key);
    }
    plan.from_version = kept;
    plan.feed = absolute_utf8(*folder / "feed.json");
    plan.signature = absolute_utf8(*folder / "feed.json.sig");
    plan.release = update_->download_release;
    plan.package = update_->download_package;
    plan.file = absolute_utf8(shot.path);
    plan.installer = where.installer;
    plan.preferences = preference_path_.empty() ? plan.install : absolute_utf8(preference_path_);
    if (options_.preferences_file && !options_.preferences_file->empty())
        plan.preferences = absolute_utf8(*options_.preferences_file);
    plan.updates = absolute_utf8(*folder);
    plan.title = library::filled(hooks, kInstalling, {{"version", update_->download_version}});
    if (where.installer) {
        plan.line =
            library::filled(hooks, kInstallerWill, {{"version", update_->download_version}});
    } else if (installs_at_next_start()) {
        plan.line = library::filled(
            hooks, update::installs_next_start, {{"version", update_->download_version}}
        );
    } else {
        plan.line = library::filled(hooks, kKept, {{"version", kept}});
    }

    const fs::path plan_file = *folder / "plan.json";
    std::string why;
    if (!write_update_plan(plan_file, plan, why)) {
        std::cerr << "open-annihilation: update: " << why << '\n';
        return;
    }

    const fs::path helper = helper_path(program_folder());
    std::vector<std::string> helper_arguments;
    helper_arguments.push_back("--plan");
    helper_arguments.push_back(absolute_utf8(plan_file));
    for (const std::string& key : options_.update_keys) {
        helper_arguments.push_back("--feed-key");
        helper_arguments.push_back(key);
    }
    update_->helper = path_to_utf8(helper);
    update_->helper_arguments = helper_arguments;
    update_->handed = true;
    if (!update_->helper_chosen) {
        update_->helper_chosen = true;
        const bool record = options_.fixed_clock ||
                            (nobody_watching(options_) && !options_.update_feed.has_value());
        if (record) {
            update_->start_helper = record_helper;
            update_->helper_context = update_.get();
        } else {
            update_->start_helper = spawn_helper;
            update_->helper_context = nullptr;
        }
    }
    if (update_->start_helper == nullptr) {
        std::cerr << "open-annihilation: update: the helper was not started\n";
        return;
    }
    std::string start_why;
    if (!update_->start_helper(update_->helper_context, helper, helper_arguments, start_why)) {
        std::cerr << "open-annihilation: update: " << start_why << '\n';
        return;
    }
    // The helper has the release now, and its receipt tells how it went.
    // The schedule is spent; a release that does not install is held instead.
    if (update::read_scheduled(preference_values_).has_value()) {
        update::clear_scheduled(preference_values_);
        preferences_dirty_ = true;
        try {
            flush_preferences();
        } catch (const std::exception& failure) {
            std::cerr << "open-annihilation: update: " << failure.what() << '\n';
        }
    }
#endif
}

bool Runtime::report_update_handoff() {
    // A run that ends here ended through the game's own exit, not a crash.
    // A start the player quit before the main menu takes back its count, so
    // only a crash or an error that stops the game counts towards a roll back.
#if defined(OA_SELF_UPDATE) && OA_SELF_UPDATE
    const bool helper_started = update_ != nullptr && update_->handed;
#else
    const bool helper_started = false;
#endif
    if (exit_status_ == 0 && !helper_started)
        release_quit_start(options_);
#if !defined(OA_SELF_UPDATE) || !OA_SELF_UPDATE
    return true;
#else
    if (!options_.check_update_download || update_ == nullptr || !update_->handed)
        return true;
    std::string failure;
    const std::optional<fs::path> folder = updates_folder(options_, failure);
    if (!folder) {
        std::cerr << "open-annihilation: update: " << failure << '\n';
        return false;
    }
    const std::vector<uint8_t> bytes = read_bytes(*folder / "plan.json");
    std::string why;
    const std::optional<update::Plan> read = update::read_plan(bytes, &why);
    if (!read) {
        std::cerr << "open-annihilation: update: " << why << '\n';
        return false;
    }
    std::size_t index = 0;
    while (index < read->arguments.size()) {
        const std::string& flag = read->arguments[index];
        if (flag == "--skip-intro" || flag == "--mute") {
            ++index;
            continue;
        }
        bool path_value = false;
        if (!known_plan_flag(flag, path_value) || index + 1 >= read->arguments.size()) {
            std::cerr << "open-annihilation: update: an argument is not allowed\n";
            return false;
        }
        const std::string& value = read->arguments[index + 1];
        if (value.empty() || (path_value && !path_from_utf8(value).is_absolute())) {
            std::cerr << "open-annihilation: update: " << flag << " is not an absolute path\n";
            return false;
        }
        index += 2;
    }
    const fs::path file_path = path_from_utf8(read->file);
    const bool file_ok =
        path_to_utf8(file_path.parent_path().filename()) == std::to_string(read->release);
    const bool feed_ok = read->feed.size() >= 9 && read->feed.ends_with("feed.json");
    const bool signature_ok =
        read->signature.size() >= 13 && read->signature.ends_with("feed.json.sig");
    const bool words_ok = read->title.find(update_->download_version) != std::string::npos &&
                          read->line.find(read->from_version) != std::string::npos;
    if (read->route != update::Route::check_folder || read->installer ||
        read->wait_for != own_process() || !file_ok || !feed_ok || !signature_ok ||
        read->package.empty() || read->release == 0 || !words_ok || read->preferences.empty() ||
        read->updates.empty() || read->program.empty() || read->install.empty()) {
        std::cerr << "open-annihilation: update: the plan does not read back\n";
        return false;
    }
    std::cout << "update-download: helper " << update_->helper;
    for (const std::string& argument : update_->helper_arguments)
        std::cout << ' ' << argument;
    std::cout << '\n';
    std::cout << "update-download: plan read back\n";
    std::cout.flush();
    return true;
#endif
}

void Runtime::hand_off_roll_back() {
#if !defined(OA_SELF_UPDATE) || !OA_SELF_UPDATE
    return;
#else
    if (update_ == nullptr || update_->handed || !roll_back_requested_)
        return;
    std::string failure;
    const std::optional<fs::path> folder = updates_folder(options_, failure);
    if (!folder) {
        std::cerr << "open-annihilation: update: " << failure << '\n';
        return;
    }
    const update::Where where = route_for(options_, feed::Way::in_place);
    const update::ThisBuild built = running_build();
    const std::string kept = update_->kept_version;
    const library::TextHooks hooks = plan_hooks();
    update::Plan plan;
    plan.plan = update::plan_format;
    plan.action = update::Action::roll_back;
    plan.roll_back_reason = update::RollBackReason::player;
    plan.wait_for = own_process();
    if (where.route.has_value() && !where.program.empty() && !where.install.empty()) {
        plan.route = *where.route;
        plan.install = absolute_utf8(where.install);
        plan.program = absolute_utf8(where.program);
        plan.installer = where.installer;
    } else {
        plan.route = update::Route::check_folder;
        fs::path install =
            options_.update_install_folder ? *options_.update_install_folder : program_folder();
        if (install.empty())
            install = *folder;
        plan.install = absolute_utf8(install);
        const update::RouteFacts facts = update::this_machine(program_folder());
        const fs::path program = facts.executable.empty() ? install : facts.executable;
        plan.program = absolute_utf8(program);
        plan.installer = false;
    }
    if (options_.game_dir_named && !options_.game_dir.empty()) {
        plan.arguments.push_back("--game-dir");
        plan.arguments.push_back(absolute_utf8(options_.game_dir));
    }
    if (options_.data_dir && !options_.data_dir->empty()) {
        plan.arguments.push_back("--data-dir");
        plan.arguments.push_back(absolute_utf8(*options_.data_dir));
    }
    if (options_.user_folder && !options_.user_folder->empty()) {
        plan.arguments.push_back("--user-folder");
        plan.arguments.push_back(absolute_utf8(*options_.user_folder));
    }
    if (options_.preferences_file && !options_.preferences_file->empty()) {
        plan.arguments.push_back("--preferences-file");
        plan.arguments.push_back(absolute_utf8(*options_.preferences_file));
    } else if (!preference_path_.empty()) {
        plan.arguments.push_back("--preferences-file");
        plan.arguments.push_back(absolute_utf8(preference_path_));
    }
    if (options_.log_dir && !options_.log_dir->empty()) {
        plan.arguments.push_back("--log-dir");
        plan.arguments.push_back(absolute_utf8(*options_.log_dir));
    }
    if (options_.skip_intro)
        plan.arguments.push_back("--skip-intro");
    if (options_.mute)
        plan.arguments.push_back("--mute");
    plan.from_version = built.version_text;
    plan.feed = absolute_utf8(*folder / "feed.json");
    plan.signature = absolute_utf8(*folder / "feed.json.sig");
    plan.release = update_->release > 0 ? update_->release : built.release;
    plan.package = "kept";
    plan.file = absolute_utf8(*folder / "kept.pkg");
    plan.preferences = preference_path_.empty() ? plan.install : absolute_utf8(preference_path_);
    if (options_.preferences_file && !options_.preferences_file->empty())
        plan.preferences = absolute_utf8(*options_.preferences_file);
    plan.updates = absolute_utf8(*folder);
    plan.title = library::filled(hooks, kGoingBack, {{"version", kept}});
    if (plan.title.size() > update::max_window_line_bytes)
        plan.title.resize(update::max_window_line_bytes);
    plan.line.clear();

    const fs::path plan_file = *folder / "roll-back.json";
    std::string why;
    if (!write_update_plan(plan_file, plan, why)) {
        std::cerr << "open-annihilation: update: " << why << '\n';
        return;
    }
    const fs::path helper = helper_path(program_folder());
    std::vector<std::string> helper_arguments;
    helper_arguments.push_back("--plan");
    helper_arguments.push_back(absolute_utf8(plan_file));
    update_->helper = path_to_utf8(helper);
    update_->helper_arguments = std::move(helper_arguments);
    update_->handed = true;
    if (!update_->helper_chosen) {
        update_->helper_chosen = true;
        const bool record = options_.fixed_clock ||
                            (nobody_watching(options_) && !options_.update_feed.has_value());
        update_->start_helper = record ? record_helper : spawn_helper;
        update_->helper_context = record ? update_.get() : nullptr;
    }
    if (update_->start_helper == nullptr) {
        std::cerr << "open-annihilation: update: the helper was not started\n";
        return;
    }
    std::string start_why;
    if (!update_->start_helper(
            update_->helper_context, helper, update_->helper_arguments, start_why
        ))
        std::cerr << "open-annihilation: update: " << start_why << '\n';
#endif
}

bool Runtime::report_roll_back_handoff() {
    if (!options_.check_update_roll_back || options_.roll_back_case != "settings")
        return true;
#if !defined(OA_SELF_UPDATE) || !OA_SELF_UPDATE
    return false;
#else
    if (update_ == nullptr || !update_->handed)
        return false;
    std::string failure;
    const std::optional<fs::path> folder = updates_folder(options_, failure);
    if (!folder) {
        std::cerr << "open-annihilation: update: " << failure << '\n';
        return false;
    }
    const std::vector<uint8_t> bytes = read_bytes(*folder / "roll-back.json");
    std::string why;
    const std::optional<update::Plan> read = update::read_plan(bytes, &why);
    if (!read || read->action != update::Action::roll_back ||
        read->roll_back_reason != update::RollBackReason::player) {
        std::cerr << "open-annihilation: update: the plan does not roll back\n";
        return false;
    }
    std::cout << "update-roll-back: plan roll-back\n";
    std::cout.flush();
    return true;
#endif
}

bool Runtime::start_update_download() {
#if !defined(OA_SELF_UPDATE) || !OA_SELF_UPDATE
    return false;
#else
    if (!update::replaces_itself())
        return false;
    if (match_ != nullptr || screen_ == Screen::match)
        return false;
    if (update_ == nullptr)
        return false;
    const update::CheckSnapshot& held = update_->held;
    if (held.state != update::CheckState::offered || held.offer.release == nullptr ||
        held.offer.package == nullptr)
        return false;
    if (held.offer.way != feed::Way::in_place)
        return false;
    // Copied before anything else replaces the snapshot the pointers address.
    const feed::Release release = *held.offer.release;
    const feed::Package package = *held.offer.package;
    const update::Where where = route_for(options_, held.offer.way);
    update_->route = where;
    update_->route_known = true;
    if (!where.route.has_value()) {
        if (!update_->logged_route) {
            update_->logged_route = true;
            std::cerr << "open-annihilation: update download: " << where.why << '\n';
        }
        return false;
    }
    if (update_->download != nullptr && update_->download_release == release.release) {
        const update::DownloadState state = update_->download->snapshot().state;
        if (state == update::DownloadState::downloading ||
            state == update::DownloadState::paused_for_match ||
            state == update::DownloadState::verifying || state == update::DownloadState::ready)
            return true;
    }
    std::string failure;
    const std::optional<fs::path> folder = updates_folder(options_, failure);
    if (!folder) {
        std::cerr << "open-annihilation: update download: " << failure << '\n';
        return false;
    }
    // Asking for a held release is the player's Try again.
    if (update::release_held(preference_values_, release.release)) {
        update::clear_held(preference_values_);
        preferences_dirty_ = true;
    }
    if (update_->download == nullptr)
        update_->download = std::make_unique<update::PackageDownload>();
    update_->download_release = release.release;
    update_->download_version = release.version_text;
    update_->download_package = package.id;
    update_->download_installer = where.installer;
    update_->from_version = running_build().version_text;
    update_->download_attempted = true;
    update_->download->set_match_running(match_ != nullptr || screen_ == Screen::match);
    update_->download->start(release, package, where.installer, *folder);
    return true;
#endif
}

void Runtime::retry_update_download() {
#if defined(OA_SELF_UPDATE) && OA_SELF_UPDATE
    if (update_ != nullptr)
        update_->download_attempted = false;
    // RETRY is the player's Try again, for a held release too.
    static_cast<void>(start_update_download());
#endif
}

bool Runtime::background_download_covers_offer() const {
#if !defined(OA_SELF_UPDATE) || !OA_SELF_UPDATE
    return false;
#else
    if (update_ == nullptr)
        return false;
    const update::CheckSnapshot& held = update_->held;
    if (held.state != update::CheckState::offered || held.offer.release == nullptr ||
        held.offer.way != feed::Way::in_place)
        return false;
    if (!update::offer_wanted(held.offer, update::read_skipped(preference_values_)))
        return false;
    if (!background_switch_on(preference_values_))
        return false;
    return route_for(options_, held.offer.way).route.has_value();
#endif
}

void Runtime::offer_ready() {
#if !defined(OA_SELF_UPDATE) || !OA_SELF_UPDATE
    return;
#else
    if (options_.update_and_quit || update_ == nullptr)
        return;
    if (match_ != nullptr || screen_ == Screen::match) {
        update_->ready_prompt_release.reset();
        return;
    }
    if (screen_ != Screen::main_menu || frame_owned_by_package() ||
        state_.state != frontend::state_id::main_menu ||
        state_.pending_signal == frontend::signal_id::multiplayer)
        return;
    if (!options_.update_feed.has_value() && (options_.unattended || options_.headless_check))
        return;
    // The words are the update centre's own for a ready release, so the
    // prompt, Settings › Updates and the menu pill name one state and one size.
    const update::Centre centre = update_centre();
    if (centre.stage != update::Stage::ready && centre.stage != update::Stage::scheduled)
        return;
    const int64_t release_number = centre.target_release;
    // A release already scheduled for the quit was chosen; it does not ask.
    // Install updates set to install when I quit schedules it here.
    if (update::installs_at_quit(preference_values_, release_number))
        return;
    if (update::read_install_policy(preference_values_) == update::InstallPolicy::when_i_quit) {
        update::write_scheduled(preference_values_, release_number);
        preferences_dirty_ = true;
        try {
            flush_preferences();
        } catch (const std::exception& failure) {
            std::cerr << "open-annihilation: update: " << failure.what() << '\n';
        }
        return;
    }
    if (update_->ready_prompt_release.has_value() &&
        *update_->ready_prompt_release >= release_number)
        return;
    OaLayer& layer = oa_layer();
    if (ready_question_queued(layer))
        return;
    const std::string version = centre.target_version;
    const update::CentreWords words = update::centre_words(centre, shown_hooks());
    std::string keep_ready;
    std::string when_i_quit;
    std::string install_now;
    std::vector<kit::BoxLine> answers;
    for (const update::ActWords& act : words.actions) {
        if (act.act == update::Act::keep_ready)
            keep_ready = act.caption;
        else if (act.act == update::Act::install_on_quit)
            when_i_quit = act.caption;
        else if (act.act == update::Act::install_now)
            install_now = act.caption;
        else
            continue;
        answers.push_back(
            {kit::LineMark::bullet,
             act.note.empty() ? act.caption : act.caption + " \u00b7 " + act.note}
        );
    }
    kit::Question asked;
    asked.word = std::string(kReadyWord);
    asked.title = library::filled(shown_hooks(), kReadyTitle, {{"version", version}});
    if (!words.detail.empty()) {
        kit::QuestionPart checked;
        checked.kind = kit::PartKind::callout;
        checked.tone = kit::CalloutTone::ok;
        checked.text = words.detail;
        asked.parts.push_back(std::move(checked));
    }
    asked.parts.push_back(box_part(kWhenShouldItGoIn, std::move(answers)));
    asked.buttons.push_back({keep_ready, false, "keep-ready"});
    asked.buttons.push_back({install_now, false, "restart-now"});
    asked.buttons.push_back({when_i_quit, true, "when-i-quit"});
    asked.cancel_button = kKeepReady;
    asked.primary_button = kWhenIQuit;
    asked.marked = kWhenIQuit;
    update_->ready_prompt_release = release_number;
    Runtime* runtime = this;
    QuestionScreen::Host host;
    // Keep it ready schedules nothing. When I quit is stored, so it holds
    // across starts until it is taken back or the release installs. Installing
    // now schedules the release and quits at once.
    host.answer = [runtime](int32_t button) {
        if (button == kWhenIQuit)
            runtime->take_update_action(update::Act::install_on_quit);
        else if (button == kInstallNow)
            runtime->take_update_action(update::Act::install_now);
        return true;
    };
    host.tick = [runtime]() {
        const bool in_match = runtime->match_ != nullptr || runtime->screen_ == Screen::match;
        if (in_match && runtime->update_)
            runtime->update_->ready_prompt_release.reset();
        return in_match;
    };
    auto question = std::make_unique<QuestionScreen>(
        layer, Screen::main_menu, std::move(asked), std::move(host)
    );
    question->show_on_any_menu();
    layer.show_when_free(std::move(question));
#endif
}

void Runtime::offer_updated_notice() {
    if (options_.update_and_quit)
        return;
    if (update_ == nullptr)
        update_.reset(new UpdateState());
    if (update_->updated_notice_shown)
        return;
    if (match_ != nullptr || screen_ == Screen::match || !update_menu_settled())
        return;
    // The helper's receipt is read once a start, at the settled menu, and
    // shown once. A failure holds its release until the player tries again,
    // and takes back the schedule, so a quit does not try it again by itself.
    update_->updated_notice_shown = true;
    {
        std::string folder_failure;
        const std::optional<fs::path> updates = updates_folder(options_, folder_failure);
        const fs::path receipt_path =
            updates ? *updates / std::string(update::receipt_file_name) : fs::path{};
        std::optional<update::Receipt> receipt =
            updates ? update::load_receipt(receipt_path) : std::nullopt;
        // A receipt still running may belong to a helper at work; one left
        // running long enough was cut short.
        const bool finished = receipt.has_value() &&
                              (receipt->outcome != update::Outcome::running ||
                               wall_seconds() - receipt->started_at >= kReceiptCutShortSeconds);
        if (finished && !receipt->shown) {
            receipt->shown = true;
            std::string why;
            if (!update::write_receipt(receipt_path, *receipt, &why))
                std::cerr << "open-annihilation: update: " << why << '\n';
            if (update::failed_outcome(receipt->outcome)) {
                if (update::holds_release(receipt->outcome) && receipt->release > 0) {
                    update::write_held(preference_values_, receipt->release);
                    update::clear_scheduled(preference_values_);
                    preferences_dirty_ = true;
                    try {
                        flush_preferences();
                    } catch (const std::exception& thrown) {
                        std::cerr << "open-annihilation: update: " << thrown.what() << '\n';
                    }
                }
                // The recovery screen is the receipt's Show details.
                update_->receipt = *receipt;
                update_->receipt_loaded = true;
                take_update_action(update::Act::details);
                return;
            }
        }
        if (receipt.has_value()) {
            update_->receipt = std::move(receipt);
            update_->receipt_loaded = true;
        }
    }
    if (!options_.updated_from.has_value())
        return;
    const std::optional<formats::oamod::EngineVersion> previous =
        formats::oamod::parse_engine_version(*options_.updated_from);
    const update::ThisBuild built = running_build();
    if (!previous.has_value() || !(built.version > *previous))
        return;
    kit::Notice told;
    told.word = std::string(kUpdatedNotice);
    told.title = library::filled(shown_hooks(), kUpdatedTo, {{"version", built.version_text}});
    told.ok_caption = std::string(languages::interface_text("OK"));
    std::string failure;
    const std::optional<fs::path> folder = updates_folder(options_, failure);
    if (folder) {
        const std::vector<uint8_t> bytes = read_bytes(*folder / "feed.json");
        std::string error;
        const std::optional<feed::Feed> stored = feed::read_feed(bytes, &error);
        const feed::Release* notes_release = nullptr;
        if (stored) {
            for (const feed::Release& release : stored->releases) {
                if (release.version_text == built.version_text) {
                    notes_release = &release;
                    break;
                }
            }
            if (notes_release == nullptr && !stored->releases.empty())
                notes_release = &stored->releases.front();
        }
        if (notes_release != nullptr) {
            const feed::Notes* notes = feed::notes_for(*notes_release, shown_language().tag);
            if (notes != nullptr) {
                const std::string listed = update::notes_paragraph(
                    notes_release->version_text, notes->lines, shown_hooks()
                );
                if (!listed.empty())
                    told.paragraphs.push_back({listed, false});
            }
        }
        const std::optional<update::State> state = read_installed_state(*folder);
        if (state && !state->kept_version.empty()) {
            told.paragraphs.push_back(
                {library::filled(shown_hooks(), kKeptSentence, {{"version", state->kept_version}}),
                 false}
            );
        }
    }
    NoticeScreen::Host host;
    host.ok = []() {};
    Runtime* runtime = this;
    host.tick = [runtime]() {
        return runtime->match_ != nullptr || runtime->screen_ == Screen::match;
    };
    OaLayer& layer = oa_layer();
    auto notice =
        std::make_unique<NoticeScreen>(layer, Screen::main_menu, std::move(told), std::move(host));
    notice->show_on_any_menu();
    layer.show_when_free(std::move(notice));
}

std::string Runtime::update_header_note() const {
    // The update centre's short form, the one the menu pill shows, while a
    // package is downloading, paused, being checked, ready or scheduled.
    const update::Centre centre = update_centre();
    switch (centre.stage) {
    case update::Stage::downloading:
    case update::Stage::paused_by_player:
    case update::Stage::paused_by_match:
    case update::Stage::verifying:
    case update::Stage::ready:
    case update::Stage::scheduled:
        return update::centre_words(centre, shown_hooks()).pill;
    default:
        return {};
    }
}

update::Centre Runtime::update_centre() const {
    if (update_ != nullptr && update_->pictured.has_value())
        return update::centre_state(*update_->pictured);
    update::CentreInputs inputs;
    const update::ThisBuild built = update::this_build({});
    inputs.current_version = built.version_text;
    inputs.mode = update::read_check_mode(preference_values_);
    inputs.skipped = update::read_skipped(preference_values_);
    inputs.held = update::read_held(preference_values_);
    inputs.scheduled = update::read_scheduled(preference_values_);
    inputs.manager = std::string(update::packager_name());
    // This start replaced an older version: the same test the Updated notice makes.
    if (options_.updated_from.has_value()) {
        const std::optional<formats::oamod::EngineVersion> previous =
            formats::oamod::parse_engine_version(*options_.updated_from);
        if (previous.has_value() && built.version > *previous)
            inputs.updated_from = *options_.updated_from;
    }
    if (options_.rolled_back_from.has_value())
        inputs.rolled_back_from = *options_.rolled_back_from;
    if (update_ == nullptr) {
        inputs.way = update::install_way(
            update::replaces_itself(), update::packager_name(), std::nullopt, false, false, false
        );
        return update::centre_state(inputs);
    }
    // What state.json and the receipt say is read once a run; the update
    // state's own pointer may be written through from here.
    if (!update_->kept_loaded) {
        update_->kept_loaded = true;
        std::string folder_failure;
        if (const std::optional<fs::path> folder = updates_folder(options_, folder_failure)) {
            if (const std::optional<update::State> state = read_installed_state(*folder)) {
                update_->kept_version = state->kept_version;
                update_->installed_unconfirmed =
                    !state->confirmed && state->installed_version == built.version_text;
            }
        }
    }
    if (!update_->receipt_loaded) {
        update_->receipt_loaded = true;
        std::string folder_failure;
        if (const std::optional<fs::path> folder = updates_folder(options_, folder_failure))
            update_->receipt =
                update::load_receipt(*folder / std::string(update::receipt_file_name));
    }
    inputs.kept_version = update_->kept_version;
    inputs.unconfirmed = update_->installed_unconfirmed && !update_->version_confirmed;
    inputs.receipt = update_->receipt;
    const update::CheckSnapshot& held = update_->held;
    inputs.check = held.state;
    inputs.check_failure = held.failure_kind;
    if (held.offer.release != nullptr) {
        inputs.offer_version = held.offer.release->version_text;
        inputs.offer_release = held.offer.release->release;
    }
    // The way is found once for each kind of offer: it reads the machine.
    const bool page_only = held.offer.way == feed::Way::page_only;
    if (!update_->centre_way_known || update_->centre_way_page_only != page_only) {
        update_->centre_way_known = true;
        update_->centre_way_page_only = page_only;
#if defined(OA_SELF_UPDATE) && OA_SELF_UPDATE
        const update::Where where =
            route_for(options_, page_only ? feed::Way::page_only : feed::Way::in_place);
        update_->centre_installer = where.installer;
        update_->centre_way = update::install_way(
            update::replaces_itself(),
            update::packager_name(),
            where.route,
            where.installer,
            page_only,
            installs_at_next_start()
        );
#else
        update_->centre_installer = false;
        update_->centre_way = update::install_way(
            update::replaces_itself(),
            update::packager_name(),
            std::nullopt,
            false,
            page_only,
            false
        );
#endif
    }
    inputs.way = update_->centre_way;
    // The offer names the size of the file a download fetches: the
    // installer where the route uses it, the zip otherwise.
    if (held.offer.package != nullptr)
        inputs.offer_size = update_->centre_installer && held.offer.package->installer.has_value()
                                ? held.offer.package->installer->size
                                : held.offer.package->zip.size;
#if defined(OA_SELF_UPDATE) && OA_SELF_UPDATE
    if (update_->download != nullptr) {
        const update::DownloadSnapshot shot = update_->download->snapshot();
        inputs.download = shot.state;
        inputs.download_failure = shot.failure_kind;
        inputs.download_reason = shot.failure;
        inputs.download_release = update_->download_release;
        inputs.download_version = update_->download_version;
        inputs.received = shot.received;
        inputs.total = shot.total;
        inputs.bytes_per_second = shot.bytes_per_second;
    }
    inputs.handing_over = update_->handed && !roll_back_requested_;
#endif
    return update::centre_state(inputs);
}

void Runtime::take_update_action(update::Act action) {
    const update::Centre centre = update_centre();
    const auto store = [this]() {
        preferences_dirty_ = true;
        try {
            flush_preferences();
        } catch (const std::exception& failure) {
            std::cerr << "open-annihilation: update: " << failure.what() << '\n';
        }
    };
    const auto updates_log = [this]() {
        std::string failure;
        const std::optional<fs::path> folder = updates_folder(options_, failure);
        if (folder)
            std::ignore = open_player_folder(*folder);
        else
            std::cerr << "open-annihilation: update: " << failure << '\n';
    };
    switch (action) {
    case update::Act::check_now:
        start_update_check(true);
        return;
    case update::Act::download:
        if (!start_update_download())
            open_release_page();
        return;
    case update::Act::pause:
#if defined(OA_SELF_UPDATE) && OA_SELF_UPDATE
        if (update_ != nullptr && update_->download != nullptr)
            update_->download->pause();
#endif
        return;
    case update::Act::resume:
#if defined(OA_SELF_UPDATE) && OA_SELF_UPDATE
        if (update_ != nullptr && update_->download != nullptr)
            update_->download->resume();
#endif
        return;
    case update::Act::cancel_download:
#if defined(OA_SELF_UPDATE) && OA_SELF_UPDATE
        if (update_ != nullptr && update_->download != nullptr) {
            // Nothing half-downloaded is kept. Download in the background does
            // not start it again this run; Download update does.
            update_->download->discard();
            update_->download_attempted = true;
            if (update::installs_at_quit(preference_values_, update_->download_release)) {
                update::clear_scheduled(preference_values_);
                store();
            }
        }
#endif
        return;
    case update::Act::install_on_quit:
        if (centre.target_release > 0 &&
            (centre.stage == update::Stage::ready || centre.stage == update::Stage::scheduled)) {
            update::write_scheduled(preference_values_, centre.target_release);
            store();
        }
        return;
    case update::Act::install_now:
        // Installing now: the ready release is scheduled, and the quit hands it over.
        if (centre.target_release > 0 &&
            (centre.stage == update::Stage::ready || centre.stage == update::Stage::scheduled)) {
            update::write_scheduled(preference_values_, centre.target_release);
            store();
            quit_requested_ = true;
            exit_status_ = 0;
        }
        return;
    case update::Act::unschedule:
        update::clear_scheduled(preference_values_);
        store();
        return;
    case update::Act::skip:
        if (centre.target_release > 0) {
            update::write_skipped(preference_values_, centre.target_release);
            store();
        }
        return;
    case update::Act::try_version:
        update::clear_skipped(preference_values_);
        store();
        return;
    case update::Act::retry: {
        // A held release is the player's to try again. A check that failed is
        // asked again; a download that stopped is started again.
        if (update::read_held(preference_values_).has_value()) {
            update::clear_held(preference_values_);
            store();
        }
        bool download_failed = false;
#if defined(OA_SELF_UPDATE) && OA_SELF_UPDATE
        download_failed = update_ != nullptr && update_->download != nullptr &&
                          update_->download->snapshot().state == update::DownloadState::failed;
#endif
        const bool check_failed =
            centre.stage == update::Stage::could_not_check ||
            (centre.stage == update::Stage::could_not_connect && !download_failed);
        if (check_failed)
            start_update_check(true);
        else
            retry_update_download();
        return;
    }
    case update::Act::details: {
        OaLayer& layer = oa_layer();
        if (layer.find(kReceiptWord) != nullptr || layer.waiting(kReceiptWord) != nullptr)
            return;
        kit::Question asked;
        const bool receipt_failed = update_ != nullptr && update_->receipt.has_value() &&
                                    update::failed_outcome(update_->receipt->outcome);
        if (receipt_failed) {
            const update::Receipt& receipt = *update_->receipt;
            update::CentreInputs inputs;
            inputs.current_version = centre.current_version;
            inputs.held = receipt.release;
            inputs.receipt = receipt;
            const update::Centre failed = update::centre_state(inputs);
            const bool retry = update::holds_release(receipt.outcome) &&
                               failed.stage != update::Stage::recovery_incomplete;
            asked = receipt_question(receipt, failed, centre.current_version, retry);
        } else {
            // A refused check or a download that stopped: its words and the
            // line the check or the download wrote, in English.
            const update::CentreWords words = update::centre_words(centre, shown_hooks());
            asked.word = std::string(kReceiptWord);
            asked.title = words.status;
            if (!words.detail.empty())
                asked.paragraphs.push_back({words.detail, false});
            std::string line = centre.reason;
            if (line.empty() && update_ != nullptr)
                line = update_->held.failure;
            if (!line.empty()) {
                kit::QuestionPart told;
                told.kind = kit::PartKind::text;
                told.text = line;
                asked.parts.push_back(std::move(told));
            }
            asked.buttons.push_back(
                {std::string(languages::interface_text("Continue")), true, "ok"}
            );
        }
        const int32_t retry_button = asked.buttons.size() == 3 ? 1 : -1;
        Runtime* runtime = this;
        QuestionScreen::Host host;
        host.answer = [runtime, receipt_failed, retry_button](int32_t button) {
            if (receipt_failed && button == kReceiptLog) {
                runtime->take_update_action(update::Act::show_log);
                return false;
            }
            if (button == retry_button)
                runtime->take_update_action(update::Act::retry);
            return true;
        };
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
    case update::Act::show_log:
        updates_log();
        return;
    case update::Act::open_page:
    case update::Act::whats_new:
        open_release_page();
        return;
    case update::Act::roll_back: {
        // The question asks once. Roll back and restart quits, and main()
        // starts the helper. Cancel leaves the screen under it as it was.
        if (centre.kept_version.empty())
            return;
        kit::Question asked =
            settings::updates::roll_back_question(centre.kept_version, centre.current_version);
        Runtime* runtime = this;
        QuestionScreen::Host host;
        host.answer = [runtime](int32_t button) {
            if (button == 1) {
                runtime->roll_back_requested_ = true;
                runtime->quit_requested_ = true;
                runtime->exit_status_ = 0;
            }
            return true;
        };
        host.tick = [runtime]() {
            return runtime->match_ != nullptr || runtime->screen_ == Screen::match;
        };
        OaLayer& layer = oa_layer();
        auto question = std::make_unique<QuestionScreen>(
            layer, Screen::main_menu, std::move(asked), std::move(host)
        );
        question->show_on_any_menu();
        layer.push(std::move(question));
        return;
    }
    case update::Act::keep_ready:
    case update::Act::later:
        return;
    }
}

bool Runtime::apply_update_download(settings::Dialog& dialog) {
    // Settings › Updates shows the update centre's one state: the menu pill,
    // the prompts and the header note read the same. The check's own line,
    // with when it last reached the server, stands while the centre's state
    // is the check's; every other state is the centre's line, and a ready
    // or a stopped download keeps the page's own line and button for it.
    const bool show = update::replaces_itself() && !options_.check_engine_settings;
    if (update_ == nullptr)
        update_.reset(new UpdateState());
    const update::Centre centre = update_centre();
    const update::CentreWords words = update::centre_words(centre, shown_hooks());
    settings::UpdatesOffer offer = dialog.updates;
    offer.show_download = show;
    offer.download = settings::DownloadShown::none;
    offer.download_version.clear();
    offer.download_done.clear();
    offer.download_size.clear();
    offer.download_reason.clear();
    offer.kept_version = centre.kept_version;
    {
        // The check's lines, as the page shows them while the check decides.
        const update::CheckSnapshot& snapshot = update_snapshot();
        settings::UpdateReading reading;
        const std::string version = centre.current_version;
        reading.version = version;
        switch (snapshot.state) {
        case update::CheckState::idle:
            reading.stand = settings::UpdateStand::idle;
            break;
        case update::CheckState::checking:
            reading.stand = settings::UpdateStand::checking;
            break;
        case update::CheckState::up_to_date:
            reading.stand = settings::UpdateStand::up_to_date;
            break;
        case update::CheckState::offered:
            reading.stand = settings::UpdateStand::offered;
            break;
        case update::CheckState::failed:
            reading.stand = settings::UpdateStand::failed;
            break;
        case update::CheckState::cancelled:
            reading.stand = settings::UpdateStand::cancelled;
            break;
        }
        reading.never = dialog.chosen.update_check == settings::UpdateCheck::never;
        reading.checked_at = snapshot.checked_at;
        reading.now = wall_seconds();
        const std::string offered_version = snapshot.offer.release != nullptr
                                                ? snapshot.offer.release->version_text
                                                : std::string{};
        reading.offer_version = offered_version;
        reading.replaces_itself = update::replaces_itself();
        if (!reading.replaces_itself)
            reading.packager = update::packager_name();
        const settings::UpdateLines lines = settings::update_lines(reading);
        offer.status = lines.status;
        offer.can_check = lines.can_check;
        offer.offered = lines.offered;
        offer.updated_by = lines.updated_by;
    }
    // The heading and the detail on one line: a full stop ends a sentence,
    // and a dot joins a heading that is not one.
    const auto centre_line = [&words]() {
        if (words.detail.empty())
            return words.status;
        const bool sentence = !words.status.empty() && words.status.back() == '.';
        return words.status + (sentence ? " " : " \u00b7 ") + words.detail;
    };
    const auto centre_owns = [&]() {
        offer.status = centre_line();
        offer.can_check = false;
        offer.offered = false;
        offer.updated_by.clear();
    };
    switch (centre.stage) {
    case update::Stage::not_checked:
    case update::Stage::never:
    case update::Stage::checking:
    case update::Stage::up_to_date:
    case update::Stage::offered:
    case update::Stage::page_only:
    case update::Stage::managed:
    case update::Stage::could_not_check:
        // A made-up state for the pictures has no check behind it: its line
        // is the centre's, with the button the check would offer.
        if (update_->pictured.has_value()) {
            offer.status = centre_line();
            offer.offered =
                centre.stage == update::Stage::offered || centre.stage == update::Stage::page_only;
            offer.can_check =
                centre.stage != update::Stage::checking && centre.stage != update::Stage::never;
            offer.updated_by.clear();
        }
        break;
    case update::Stage::could_not_connect:
    case update::Stage::not_verified:
        if (show && centre.target_release > 0 && !centre.reason.empty()) {
            offer.download = settings::DownloadShown::stopped;
            offer.download_reason = centre_line();
        }
        break;
    case update::Stage::download_stopped:
        if (show) {
            offer.download = settings::DownloadShown::stopped;
            offer.download_reason = centre.reason.empty() ? words.status : centre.reason;
        }
        break;
    case update::Stage::ready:
        if (show) {
            offer.download = settings::DownloadShown::ready;
            offer.download_version = centre.target_version;
            offer.download_size = settings::update_size_text(centre.total);
            break;
        }
        [[fallthrough]];
    default:
        centre_owns();
        break;
    }
    // The card: the update centre's one state, its heading, detail, bar,
    // note and actions. Roll back… stays Previous version's, and Later and
    // Keep it ready, which ask nothing of a page that stays open, are left
    // out. While the dialog shows Never, it offers no check.
    offer.card_status.clear();
    offer.card_installed.clear();
    offer.card_detail.clear();
    offer.card_note.clear();
    offer.percent = -1;
    offer.paused = false;
    offer.actions.clear();
    std::string badge;
    if (show) {
        const bool never = dialog.chosen.update_check == settings::UpdateCheck::never;
        offer.card_status = words.status;
        offer.card_detail = words.detail;
        offer.card_note = words.note;
        if (never && (centre.stage == update::Stage::not_checked ||
                      centre.stage == update::Stage::up_to_date)) {
            offer.card_status = std::string(languages::interface_text(kNeverChecks));
            offer.card_detail.clear();
        }
        offer.card_installed = library::filled(
            shown_hooks(), kInstalledVersion, {{"version", centre.current_version}}
        );
        const bool moving = centre.stage == update::Stage::downloading ||
                            centre.stage == update::Stage::paused_by_player ||
                            centre.stage == update::Stage::paused_by_match;
        if (moving && centre.percent >= 0) {
            offer.percent = centre.percent;
            offer.paused = centre.stage != update::Stage::downloading;
            badge = std::to_string(centre.percent) + "%";
        }
        if (centre.stage == update::Stage::ready || centre.stage == update::Stage::scheduled)
            badge = std::string(languages::interface_text(kReadyBadge));
        for (const update::ActWords& act : words.actions) {
            if (act.act == update::Act::roll_back || act.act == update::Act::later ||
                act.act == update::Act::keep_ready || (never && act.act == update::Act::check_now))
                continue;
            if (offer.actions.size() == settings::most_update_card_actions)
                break;
            offer.actions.push_back(
                {static_cast<int32_t>(act.act), std::string(act_id(act.act)), act.caption}
            );
        }
    }
    const auto same_actions = [](const std::vector<settings::UpdateCardAction>& left,
                                 const std::vector<settings::UpdateCardAction>& right) {
        if (left.size() != right.size())
            return false;
        for (std::size_t index = 0; index < left.size(); ++index)
            if (left[index].act != right[index].act || left[index].id != right[index].id ||
                left[index].caption != right[index].caption)
                return false;
        return true;
    };
    const std::string note = update_header_note();
    const bool changed =
        offer.show_download != dialog.updates.show_download ||
        offer.download != dialog.updates.download ||
        offer.download_version != dialog.updates.download_version ||
        offer.download_size != dialog.updates.download_size ||
        offer.download_reason != dialog.updates.download_reason ||
        offer.kept_version != dialog.updates.kept_version ||
        offer.status != dialog.updates.status || offer.can_check != dialog.updates.can_check ||
        offer.offered != dialog.updates.offered || offer.updated_by != dialog.updates.updated_by ||
        offer.card_status != dialog.updates.card_status ||
        offer.card_installed != dialog.updates.card_installed ||
        offer.card_detail != dialog.updates.card_detail ||
        offer.card_note != dialog.updates.card_note || offer.percent != dialog.updates.percent ||
        offer.paused != dialog.updates.paused ||
        !same_actions(offer.actions, dialog.updates.actions) || dialog.header_note != note ||
        dialog.updates_badge != badge;
    dialog.updates = std::move(offer);
    dialog.header_note = note;
    dialog.updates_badge = std::move(badge);
    return changed;
}

void Runtime::consider_background_download() {
#if !defined(OA_SELF_UPDATE) || !OA_SELF_UPDATE
    return;
#else
    if (options_.update_and_quit || update_ == nullptr)
        return;
    if (match_ != nullptr || screen_ == Screen::match)
        return;
    const update::CheckSnapshot& held = update_->held;
    if (held.state != update::CheckState::offered || held.offer.way != feed::Way::in_place)
        return;
    if (!update::offer_wanted(held.offer, update::read_skipped(preference_values_)))
        return;
    if (update_->download_attempted || held.offer.release == nullptr ||
        held.offer.package == nullptr)
        return;
    // A release that did not install waits for Try again.
    if (update::release_held(preference_values_, held.offer.release->release))
        return;
    // A package an earlier start downloaded is found again, checked and kept
    // ready; nothing else downloads without Download in the background.
    std::string failure;
    const std::optional<fs::path> folder = updates_folder(options_, failure);
    const bool downloaded =
        folder && update::package_downloaded(*folder, *held.offer.release, *held.offer.package);
    if (!downloaded && !background_switch_on(preference_values_))
        return;
    static_cast<void>(start_update_download());
#endif
}

void Runtime::consider_update_and_quit() {
#if !defined(OA_SELF_UPDATE) || !OA_SELF_UPDATE
    return;
#else
    if (!options_.update_and_quit || update_ == nullptr || quit_requested_)
        return;
    const update::CheckSnapshot& held = update_->held;
    if (held.state == update::CheckState::idle || held.state == update::CheckState::checking) {
        if (!update_->ran) {
            const update::CheckMode mode = update::read_check_mode(preference_values_);
            if (mode == update::CheckMode::never)
                update::write_check_mode(preference_values_, update::CheckMode::automatically);
            start_update_check(true);
            if (mode == update::CheckMode::never)
                update::write_check_mode(preference_values_, update::CheckMode::never);
            update_->ran = true;
        } else if (update_->checker == nullptr) {
            std::cout << "update-download: no offer\n";
            quit_requested_ = true;
            exit_status_ = 1;
        }
        return;
    }
    if (held.state == update::CheckState::offered && held.offer.way == feed::Way::in_place) {
        if (!update_->download_attempted) {
            if (!start_update_download()) {
                const std::string why =
                    update_->route.why.empty() ? std::string("no offer") : update_->route.why;
                std::cout << "update-download: " << why << '\n';
                quit_requested_ = true;
                exit_status_ = 1;
            }
            return;
        }
        if (update_->download == nullptr)
            return;
        const update::DownloadSnapshot shot = update_->download->snapshot();
        if (shot.state == update::DownloadState::failed) {
            std::cout << "update-download: " << shot.failure << '\n';
            quit_requested_ = true;
            exit_status_ = 1;
            return;
        }
        if (shot.state == update::DownloadState::ready) {
            quit_requested_ = true;
            exit_status_ = 0;
        }
        return;
    }
    std::cout << "update-download: no offer\n";
    if (held.state == update::CheckState::failed && !held.failure.empty())
        std::cout << "update-download: " << held.failure << '\n';
    quit_requested_ = true;
    exit_status_ = 1;
#endif
}

void Runtime::settle_roll_back() {
    if (!options_.rolled_back_from.has_value())
        return;
    if (update_ == nullptr)
        update_.reset(new UpdateState());
    if (update_->roll_back_bookkept)
        return;
    update_->roll_back_bookkept = true;
    std::string failure;
    const std::optional<fs::path> folder = updates_folder(options_, failure);
    if (!folder)
        return;
    const std::optional<update::State> loaded = read_installed_state(*folder);
    if (!loaded)
        return;
    // The helper already rewrote a restored install: this build is confirmed
    // and nothing is kept. The skip is already in the preferences that loaded.
    if (loaded->confirmed || loaded->installed_version != *options_.rolled_back_from)
        return;
    const int64_t bad_release = loaded->installed_release;
    oa::platform::preferences::Values values = preference_values_;
    if (!loaded->preferences_copy.empty()) {
        try {
            values = oa::platform::preferences::load(path_from_utf8(loaded->preferences_copy));
        } catch (const std::exception& thrown) {
            std::cerr << "open-annihilation: update: " << thrown.what() << '\n';
            values = preference_values_;
        }
    }
    update::write_skipped(values, bad_release);
    try {
        const fs::path live = live_preferences(options_, preference_path_);
        oa::platform::preferences::save(live, values);
        preference_values_ = std::move(values);
        preferences_dirty_ = false;
    } catch (const std::exception& thrown) {
        std::cerr << "open-annihilation: update: " << thrown.what() << '\n';
        update::write_skipped(preference_values_, bad_release);
        preferences_dirty_ = true;
    }
    const update::ThisBuild built = running_build();
    update::State state = *loaded;
    state.installed_version = built.version_text;
    state.installed_release = built.release;
    update::confirm_state(state);
    state.kept_version.clear();
    state.kept_path.clear();
    state.preferences_copy.clear();
    std::string why;
    if (!update::write_state(*folder / "state.json", state, &why))
        std::cerr << "open-annihilation: update: " << why << '\n';
    remove_update_starts();
    update_->version_confirmed = true;
}

void Runtime::confirm_installed_version() {
    if (update_ == nullptr)
        update_.reset(new UpdateState());
    if (update_->version_confirmed)
        return;
    // Reaching the main menu is enough: a welcome, a message or Settings over
    // it does not keep the start unconfirmed, however long it stays open.
    if (screen_ != Screen::main_menu || frame_owned_by_package() ||
        state_.state != frontend::state_id::main_menu)
        return;
    update_->version_confirmed = true;
    std::string failure;
    const std::optional<fs::path> folder = updates_folder(options_, failure);
    if (!folder)
        return;
    const std::optional<update::State> loaded = read_installed_state(*folder);
    if (!loaded || loaded->confirmed)
        return;
    const update::ThisBuild built = running_build();
    if (loaded->installed_version != built.version_text)
        return;
    update::State state = *loaded;
    update::confirm_state(state);
    std::string why;
    if (!update::write_state(*folder / "state.json", state, &why)) {
        update_->version_confirmed = false;
        std::cerr << "open-annihilation: update: " << why << '\n';
        return;
    }
    remove_update_starts();
}

void Runtime::offer_roll_back_prompt() {
    if (!options_.rolled_back_from.has_value() || options_.roll_back_reason.empty())
        return;
    if (update_ == nullptr)
        update_.reset(new UpdateState());
    if (update_->roll_back_prompted)
        return;
    if (match_ != nullptr || screen_ == Screen::match || !update_menu_settled())
        return;
    update_->roll_back_prompted = true;
    const update::ThisBuild built = running_build();
    const std::string bad = *options_.rolled_back_from;
    const bool player = options_.roll_back_reason == "player";
    kit::Question asked;
    asked.word = "roll-back";
    asked.title = library::filled(shown_hooks(), kBackOn, {{"version", built.version_text}});
    if (player) {
        asked.paragraphs.push_back(
            {library::filled(shown_hooks(), kYouWentBack, {{"old", built.version_text}}), false}
        );
        asked.paragraphs.push_back(
            {library::filled(shown_hooks(), kNewWont, {{"new", bad}}), false}
        );
    } else {
        asked.paragraphs.push_back(
            {library::filled(
                 shown_hooks(), kClosedTwice, {{"version", bad}, {"old", built.version_text}}
             ),
             false}
        );
        asked.paragraphs.push_back(
            {library::filled(shown_hooks(), kWontBeOffered, {{"version", bad}}), false}
        );
    }
    asked.buttons.push_back(
        {std::string(languages::interface_text("SHOW THE LOG")), false, "show-the-log"}
    );
    asked.buttons.push_back({std::string(languages::interface_text("OK")), true, "ok"});
    asked.cancel_button = kBackOk;
    asked.primary_button = kBackOk;
    asked.marked = kBackOk;
    Runtime* runtime = this;
    QuestionScreen::Host host;
    host.answer = [runtime](int32_t button) {
        if (button != kShowTheLog)
            return true;
        fs::path logs;
        try {
            const fs::path data = runtime->options_.data_dir
                                      ? *runtime->options_.data_dir
                                      : oa::platform::preferences::data_directory();
            logs = data / "logs";
        } catch (const std::exception& thrown) {
            std::cerr << "open-annihilation: update: " << thrown.what() << '\n';
            return false;
        }
        std::ignore = runtime->open_player_folder(logs);
        return false;
    };
    host.tick = [runtime]() {
        return runtime->match_ != nullptr || runtime->screen_ == Screen::match;
    };
    OaLayer& layer = oa_layer();
    auto question = std::make_unique<QuestionScreen>(
        layer, Screen::main_menu, std::move(asked), std::move(host)
    );
    question->show_on_any_menu();
    layer.show_when_free(std::move(question));
}

void Runtime::tick_update_install() {
#if defined(OA_SELF_UPDATE) && OA_SELF_UPDATE
    if (update_ != nullptr && update_->download != nullptr)
        update_->download->set_match_running(match_ != nullptr || screen_ == Screen::match);
#endif
    // The skip is stored before a prompt can offer the bad release, and
    // before the menu has settled. Reaching the main menu confirms.
    settle_roll_back();
    confirm_installed_version();
    offer_ready();
    // The update pill shows the centre's state in short on the main menu
    // while a download is under way, ready, scheduled or stopped. A press on
    // it opens the update centre's question.
    if (screen_ == Screen::main_menu && !options_.update_and_quit &&
        (options_.update_feed.has_value() || !(options_.unattended || options_.headless_check))) {
        OaLayer& layer = oa_layer();
        const bool up = layer.find(kit::update_pill_word) != nullptr ||
                        layer.waiting(kit::update_pill_word) != nullptr;
        std::optional<kit::UpdatePill> pill = up ? std::nullopt : pill_of(update_centre());
        if (pill.has_value() && layer.screen_fonts() != nullptr) {
            Runtime* runtime = this;
            // The pill, and its Change, open the update centre as a question
            // that follows the state while it shows: Pause, Resume, Cancel
            // download and the other actions of the state, as buttons.
            const auto open_centre = [runtime]() {
                OaLayer& shown_on = runtime->oa_layer();
                if (shown_on.find(kCentreWord) != nullptr)
                    return;
                auto acts = std::make_shared<std::vector<update::Act>>();
                auto screen_slot = std::make_shared<QuestionScreen*>(nullptr);
                kit::Question asked = centre_question(runtime->update_centre(), *acts);
                QuestionScreen::Host asked_host;
                asked_host.answer = [runtime, acts](int32_t answered) {
                    if (answered < 0 || static_cast<std::size_t>(answered) >= acts->size())
                        return true;
                    const update::Act act = (*acts)[static_cast<std::size_t>(answered)];
                    runtime->take_update_action(act);
                    // Pause, Resume, the schedule and Try again change the
                    // state the question shows; the others are done with it.
                    return act != update::Act::pause && act != update::Act::resume &&
                           act != update::Act::install_on_quit && act != update::Act::unschedule &&
                           act != update::Act::retry;
                };
                asked_host.tick = [runtime, acts, screen_slot]() {
                    if (runtime->match_ != nullptr || runtime->screen_ == Screen::match)
                        return true;
                    QuestionScreen* shown = *screen_slot;
                    if (shown == nullptr)
                        return false;
                    // A state with no pill is not this question's to show.
                    const update::Centre now = runtime->update_centre();
                    if (!pill_of(now).has_value())
                        return true;
                    std::vector<update::Act> now_acts;
                    kit::Question next = centre_question(now, now_acts);
                    kit::Question& held = shown->question();
                    if (next.title == held.title &&
                        next.paragraphs.size() == held.paragraphs.size() &&
                        (next.paragraphs.empty() ||
                         next.paragraphs.front().text == held.paragraphs.front().text) &&
                        next.progress == held.progress &&
                        next.progress_paused == held.progress_paused && now_acts == *acts)
                        return false;
                    next.hovered = held.hovered;
                    next.pressed = held.pressed;
                    if (now_acts == *acts)
                        next.marked = held.marked;
                    held = std::move(next);
                    *acts = std::move(now_acts);
                    shown->changed();
                    return false;
                };
                auto question = std::make_unique<QuestionScreen>(
                    shown_on, Screen::main_menu, std::move(asked), std::move(asked_host)
                );
                *screen_slot = question.get();
                shown_on.push(std::move(question));
            };
            UpdatePillScreen::Host host;
            host.look = [runtime]() { return pill_of(runtime->update_centre()); };
            host.opened = open_centre;
            host.pressed = [runtime, open_centre](const std::string& button) {
                if (button == "pause") {
                    runtime->take_update_action(update::Act::pause);
                } else if (button == "resume") {
                    runtime->take_update_action(update::Act::resume);
                } else if (button == "retry") {
                    runtime->take_update_action(update::Act::retry);
                } else if (button == "install") {
                    if (runtime->update_)
                        runtime->update_->ready_prompt_release.reset();
                    runtime->offer_ready();
                } else if (button == "change") {
                    open_centre();
                }
            };
            layer.show_when_free(
                std::make_unique<UpdatePillScreen>(layer, std::move(*pill), std::move(host))
            );
        }
    }
    if (!update_menu_settled())
        return;
    offer_roll_back_prompt();
    offer_updated_notice();
    consider_background_download();
    consider_update_and_quit();
}

} // namespace oa::app

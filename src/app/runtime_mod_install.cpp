// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The packages opened in the game, installed over the main menu: each taken
// from the inbox once the menu has settled, read, its question asked in a
// prompt in the settings dialog's look, a question screen of the OA layer,
// its files unpacked a budget a frame with the progress shown, put in place,
// and what came of it told; a change
// to the mod played waits for the run to end. A catalogue map pack, a
// catalogue language pack and a catalogue expansion ask nothing and show
// nothing, including while
// Settings or a notice show and once the main menu has settled; in a run
// nobody watches a language pack still installs, and a map pack while the
// map browser is the top screen or GET MAP shows over the battle room. A
// package whose origin is a catalogue never shows the notice that it was
// installed or updated: the Library's row already says so. A file the
// player opened still does, with OPEN FOLDER and OK. A failure is still
// told. A mod the player chose in the Library also starts while the Library
// shows: a built-in registry's asks nothing, an added registry's asks only
// before it replaces another registry's copy or goes beside a folder of the
// player's own, and while the Library shows its questions show over it and
// the rest in its queue; its update to the mod being played waits until the
// player leaves that mod, and is not told in a notice once it is made.
// PLAY NOW switches to the mod installed, SWITCH OFF in the Library to no
// mod, and ROLL BACK in the Library swaps a mod folder with the version
// its .backup keeps.

#include "engine_settings_state.hpp"
#include "library_host.hpp"
#include "mod_install_state.hpp"
#include "oa_layer.hpp"
#include "user_folder_state.hpp"

#include "oa/app/game_directory.hpp"
#include "package_paths.hpp"

#include "oa/app/package_install.hpp"
#include "oa/app/package_install/handoff.hpp"
#include "oa/app/package_install/inbox.hpp"
#include "oa/app/package_install/oamod.hpp"
#include "oa/app/package_install/oaexp.hpp"
#include "oa/app/package_install/prompts.hpp"
#include "oa/app/platform_hooks.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/base/threads.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/engine_settings/prompt.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_state/app_modes.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/map_browser/screen.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {

namespace settings = oa::ui::engine_settings;
namespace install = oa::app::package_install;

namespace {

/// Frames in a row the main menu shows before a prompt.
constexpr uint32_t kPromptMenuFrames = 2;
/// The package bytes an unpacking step reads.
constexpr uint64_t kUnpackBudget = uint64_t{256} << 10;
/// The most time a frame spends unpacking.
constexpr std::chrono::milliseconds kUnpackFrameTime{15};
/// How often the hand-off folder is looked in, in milliseconds.
constexpr uint64_t kHandoffPollMs = 1000;
/// The slices a wait between renames pumps events in, in milliseconds.
constexpr uint32_t kWaitSliceMs = 50;
/// The sound a prompt's button plays as it closes the prompt.
constexpr std::string_view kCloseSound = "Options";

/// Writes a line to standard error, which the log keeps.
///
/// @param line the line
void log_line(std::string_view line) {
    std::cerr << "open-annihilation: package install: " << line << '\n';
}

/// Writes a refusal's text for a log line, with the problem's detail in
/// brackets only when the text does not already hold it.
///
/// @param text the refusal's text
/// @param detail the problem's detail; may be empty
/// @return the text, naming the detail once
std::string with_detail(std::string text, std::string_view detail) {
    if (!detail.empty() && text.find(detail) == std::string::npos)
        text += " (" + std::string(detail) + ")";
    return text;
}

/// Waits between tries of a rename, keeping the window answering: SDL's
/// events are pumped every slice.
///
/// @param milliseconds how long
void wait_pumping(void*, uint32_t milliseconds) {
    while (milliseconds > 0) {
        const uint32_t slice = std::min(milliseconds, kWaitSliceMs);
        SDL_PumpEvents();
        oa::base::threads::sleep_ms(slice);
        milliseconds -= slice;
    }
}

/// Returns the word a prompt's button is named by to automation: what it
/// answers, its enumerator's name with '_' as '-'.
///
/// @param answer what the button answers
/// @return the word
std::string_view answer_word(install::Answer answer) noexcept {
    switch (answer) {
    case install::Answer::cancel:
        return "cancel";
    case install::Answer::ok:
        return "ok";
    case install::Answer::replace:
        return "replace";
    case install::Answer::alongside:
        return "alongside";
    case install::Answer::reinstall:
        return "reinstall";
    case install::Answer::open_folder:
        return "open-folder";
    case install::Answer::play_now:
        return "play-now";
    }
    return {};
}

/// REPLACE MOD's title: an added registry's mod over the copy another
/// registry installed.
constexpr std::string_view kReplaceModTitle = "REPLACE MOD";
/// What REPLACE MOD asks, with the mod's name and the registries' names.
constexpr std::string_view kReplaceModText =
    "{name} from {installed} is installed. Replace it with {name} from {new}? The one from "
    "{installed} is kept, and Roll back brings it back.";
/// Why a built-in registry's mod is not installed where its folder name
/// holds a folder of the player's own that is not the mod.
constexpr std::string_view kOwnFolderText =
    "{folder} is a folder of your own, not {name}. Move or rename it, then Retry.";
/// The title of the prompt that says a mod was not installed, the same
/// English words as the installer's own, so one catalogue entry serves both.
constexpr std::string_view kNotInstalledTitle = "MOD NOT INSTALLED";
/// Its first sentence, with the package's name, as the installer's.
constexpr std::string_view kNotInstalledText = "{file} cannot be installed.";
/// The buttons' captions, as the settings' own dialogs show them.
constexpr std::string_view kCancelCaption = "Cancel";
constexpr std::string_view kReplaceCaption = "Replace";
constexpr std::string_view kOkCaption = "OK";

/// Returns a template looked up in the language shown, with its places
/// ({name}) filled; the values are not looked up.
///
/// @param english the template, in English
/// @param places each place's name and value
/// @return the text
std::string filled(
    std::string_view english,
    std::initializer_list<std::pair<std::string_view, std::string_view>> places
) {
    const std::string_view pattern = oa::data::languages::interface_text(english);
    std::string text;
    std::size_t at = 0;
    while (at < pattern.size()) {
        const std::size_t opening = pattern.find('{', at);
        const std::size_t closing =
            opening == std::string_view::npos ? opening : pattern.find('}', opening + 1);
        if (closing == std::string_view::npos) {
            text.append(pattern.substr(at));
            break;
        }
        text.append(pattern.substr(at, opening - at));
        const std::string_view place = pattern.substr(opening + 1, closing - opening - 1);
        const auto value = std::find_if(places.begin(), places.end(), [&](const auto& named) {
            return named.first == place;
        });
        text.append(
            value != places.end() ? value->second : pattern.substr(opening, closing - opening + 1)
        );
        at = closing + 1;
    }
    return text;
}

/// What a mod the player chose in the Library does with a plan that asks,
/// by whether its registry is built in and by the origin record of the
/// folder the plan acts on.
enum class ChosenModStep : uint8_t {
    ask,           ///< asks the plan's own question, as a file does
    replace,       ///< replaces the folder's copy with no question; the copy goes to .backup
    reinstall,     ///< installs the same registry's copy again with no question
    ask_registry,  ///< asks REPLACE MOD: the folder holds the mod from another registry
    refuse_folder, ///< refused with no question: the folder is the player's own, not the mod
};

/// Decides what a mod the player chose in the Library does with its plan.
/// A built-in registry's never asks: it replaces whatever copy the folder
/// holds, and never goes beside a folder of the player's own. An added
/// registry's replaces its own copy, and a copy with no record, with no
/// question; asks REPLACE MOD over another registry's copy; and otherwise
/// asks what a file asks.
///
/// @param plan the plan
/// @param origin the package's catalogue origin
/// @param built_in its registry is built in
/// @return what it does
ChosenModStep
chosen_mod_step(const install::InstallPlan& plan, const install::Origin& origin, bool built_in) {
    switch (plan.kind) {
    case install::PlanKind::ask_update:
    case install::PlanKind::ask_reinstall:
    case install::PlanKind::ask_version: {
        const std::optional<install::Origin>& record = plan.installed.origin;
        const bool same = record && record->kind == install::OriginKind::catalogue &&
                          record->registry == origin.registry &&
                          record->catalogue_id == origin.catalogue_id;
        // The same registry's copy of this revision is installed again; any
        // other copy is kept as the .backup, which ROLL BACK brings back.
        const ChosenModStep silent = plan.kind == install::PlanKind::ask_reinstall && same
                                         ? ChosenModStep::reinstall
                                         : ChosenModStep::replace;
        if (built_in || !record || same)
            return silent;
        if (record->kind == install::OriginKind::catalogue && record->registry != origin.registry)
            return ChosenModStep::ask_registry;
        return ChosenModStep::ask;
    }
    case install::PlanKind::ask_alongside:
        return built_in ? ChosenModStep::refuse_folder : ChosenModStep::ask;
    default:
        return ChosenModStep::ask;
    }
}

/// Returns REPLACE MOD: CANCEL, and REPLACE, which replaces the folder's
/// copy and keeps it as the .backup; CANCEL is marked first.
///
/// @param name the mod's name
/// @param installed the name of the registry whose copy is installed
/// @param incoming the name of the registry the package comes from
/// @return the prompt
install::PackagePrompt replace_registry_prompt(
    std::string_view name, std::string_view installed, std::string_view incoming
) {
    install::PackagePrompt made{};
    made.prompt.title = std::string(oa::data::languages::interface_text(kReplaceModTitle));
    made.prompt.paragraphs.push_back(
        {filled(kReplaceModText, {{"name", name}, {"installed", installed}, {"new", incoming}}),
         false}
    );
    made.prompt.buttons.push_back(
        {std::string(oa::data::languages::interface_text(kCancelCaption)), false, {}}
    );
    made.prompt.buttons.push_back(
        {std::string(oa::data::languages::interface_text(kReplaceCaption)), true, {}}
    );
    made.answers = {install::Answer::cancel, install::Answer::replace};
    made.prompt.cancel_button = 0;
    made.prompt.primary_button = 1;
    made.prompt.marked = 0;
    return made;
}

/// Returns the prompt that says a mod was not installed because its folder
/// name holds a folder of the player's own: MOD NOT INSTALLED, with OK.
///
/// @param file_name the package's name
/// @param reason why, as the Library's queue says it
/// @return the prompt
install::PackagePrompt own_folder_prompt(std::string_view file_name, std::string reason) {
    install::PackagePrompt made{};
    made.prompt.title = std::string(oa::data::languages::interface_text(kNotInstalledTitle));
    made.prompt.paragraphs.push_back({filled(kNotInstalledText, {{"file", file_name}}), false});
    made.prompt.paragraphs.push_back({std::move(reason), false});
    made.prompt.buttons.push_back(
        {std::string(oa::data::languages::interface_text(kOkCaption)), true, {}}
    );
    made.answers = {install::Answer::ok};
    return made;
}

/// Returns a prompt's words as one line: its paragraphs, a space between.
///
/// @param prompt the prompt
/// @return the words
std::string prompt_words(const settings::Prompt& prompt) {
    std::string words;
    for (const auto& paragraph : prompt.paragraphs) {
        if (!words.empty())
            words += ' ';
        words += paragraph.text;
    }
    return words;
}

} // namespace

void Runtime::destroy_mod_install_state(ModInstallState* state) noexcept {
    if (state != nullptr && state->unpacking && state->stage == ModInstallState::Stage::unpacking)
        state->unpacking->cancel();
    delete state;
}

Runtime::ModInstallState& Runtime::mod_install_state() {
    if (!mod_install_state_) {
        mod_install_state_.reset(new ModInstallState{});
        // The folders the start's recovery, or a change that waited, left
        // to delete.
        const auto discards = install::take_discards();
        mod_install_state_->discarder.add(discards);
    }
    return *mod_install_state_;
}

bool Runtime::mod_install_prompt_shown() const noexcept {
    return oa_layer_ && mod_install_state_ && mod_install_state_->prompt != nullptr &&
           oa_layer_->holds(mod_install_state_->prompt);
}

void Runtime::ModInstallState::show(Runtime& runtime, install::PackagePrompt made, bool at_once) {
    ++prompts_shown;
    // Each button is named to automation by what it answers.
    for (std::size_t place = 0; place < made.prompt.buttons.size() && place < made.answers.size();
         ++place)
        made.prompt.buttons[place].id = std::string(answer_word(made.answers[place]));
    answers = std::move(made.answers);
    if (prompt != nullptr) {
        prompt->question() = std::move(made.prompt);
        prompt->changed();
        return;
    }
    QuestionScreen::Host host;
    // The answer goes to the install, which takes the prompt off itself
    // once it is done with it (hide).
    host.answer = [&runtime](int32_t button) {
        runtime.answer_mod_install_prompt(button);
        return false;
    };
    // A question is set aside, and a telling ends, once another screen
    // replaces the main menu; an unpacking runs on under its prompt, which
    // shows again over the main menu.
    host.tick = [&runtime] {
        return runtime.screen_ != Screen::main_menu &&
               runtime.mod_install_state().stage != Stage::unpacking;
    };
    host.closed = [&runtime](const QuestionScreen& leaving) {
        auto& state = runtime.mod_install_state();
        if (state.prompt != &leaving)
            return;
        state.prompt = nullptr;
        state.answers.clear();
        state.set_aside();
    };
    auto& layer = runtime.oa_layer();
    auto screen = std::make_unique<QuestionScreen>(
        layer, Screen::main_menu, std::move(made.prompt), std::move(host)
    );
    prompt = screen.get();
    if (at_once)
        layer.push(std::move(screen));
    else
        layer.show_when_free(std::move(screen));
}

void Runtime::ModInstallState::update(Runtime& runtime, install::PackagePrompt made) {
    const uint32_t counted = prompts_shown;
    show(runtime, std::move(made));
    prompts_shown = counted;
}

void Runtime::ModInstallState::hide(Runtime& runtime) {
    answers.clear();
    if (prompt == nullptr)
        return;
    // Let go first, so that its leaving sets nothing aside.
    const QuestionScreen* leaving = prompt;
    prompt = nullptr;
    runtime.oa_layer().close(leaving);
}

void Runtime::ModInstallState::set_aside() {
    if (stage == Stage::asking) {
        install::return_package_file({file, origin});
        stage = Stage::idle;
        package.reset();
    } else if (stage == Stage::telling || stage == Stage::idle) {
        stage = Stage::idle;
    }
}

void Runtime::ModInstallState::release_opened_copy() {
    if (opened_copy.empty())
        return;
    const PlatformHooks& hooks = platform_hooks();
    if (hooks.release_opened_file != nullptr)
        hooks.release_opened_file(hooks.context, path_to_utf8(opened_copy).c_str());
    opened_copy.clear();
}

void Runtime::ModInstallState::finish_package() {
    release_opened_copy();
    package.reset();
    kind = nullptr;
    chosen = false;
    from_built_in = false;
    install::finish_package_file();
}

bool Runtime::ModInstallState::told_in_library(const Runtime& runtime) const {
    return chosen && runtime.oa_layer_ && runtime.oa_layer_->find(library_screen_name) != nullptr;
}

void Runtime::ModInstallState::report(
    Runtime& runtime,
    install::OutcomeResult result,
    std::string reason,
    const std::vector<std::string>& lines
) {
    if (chosen) {
        LibraryNote note{origin.registry, origin.catalogue_id, {}, {}};
        if (result == install::OutcomeResult::refused || result == install::OutcomeResult::failed) {
            note.problem = reason;
            if (!note.problem.empty() && note.problem.back() == ':' && !lines.empty())
                note.problem += ' ' + lines.front();
        }
        note_for_library(runtime, std::move(note));
    }
    install::report_package_outcome({.file = file, .result = result, .reason = std::move(reason)});
}

void Runtime::ModInstallState::note_for_library(Runtime& runtime, LibraryNote note) {
    std::erase_if(library_notes, [&note](const LibraryNote& kept) {
        return kept.registry == note.registry && kept.key == note.key;
    });
    if (!note.problem.empty() || !note.held_target.empty() || note.cancelled)
        library_notes.push_back(std::move(note));
    // The Library builds its model again at its next tick.
    if (runtime.library_)
        runtime.library_->refreshed = false;
}

void Runtime::ModInstallState::tell_roll_back_refusal(Runtime& runtime) {
    if (!roll_back_refusal)
        return;
    install::PackagePrompt made = std::move(*roll_back_refusal);
    roll_back_refusal.reset();
    if (prompt != nullptr || stage != Stage::idle)
        return;
    told_folder.clear();
    play_folder.clear();
    stage = Stage::telling;
    show(runtime, std::move(made), true);
}

void Runtime::take_handed_mod_files() {
    const auto folder = install::handoff_folder();
    if (!folder)
        return;
    const auto files = install::take_handed_files(*folder);
    for (const auto& file : files) {
        log_line("handed over by another start: " + path_to_utf8(file));
        install::post_opened_file(file);
    }
    if (!files.empty() && sdl_.window != nullptr) {
        if ((SDL_GetWindowFlags(sdl_.window) & SDL_WINDOW_MINIMIZED) != 0)
            std::ignore = SDL_RestoreWindow(sdl_.window);
        std::ignore = SDL_RaiseWindow(sdl_.window);
    }
}

void Runtime::tell_mod_installs() {
    auto& state = mod_install_state();
    // A run nobody watches, unless its check shows the prompts.
    const bool nobody_watches =
        UserFolderState::unwatched(options_.unattended) && !state.check_shows_prompts;
    // An unpacking runs on under its prompt, a budget a frame, whatever
    // shows. A mod's prompt is modal on the main menu. A catalogue map pack
    // unpacks with no prompt on any screen but a match.
    if (state.stage == ModInstallState::Stage::unpacking && state.unpacking &&
        state.kind != nullptr) {
        const install::PackageKind& kind = *state.kind;
        const fs::path root = user_folder_ / std::string(kind.root_folder);
        const bool watched = !nobody_watches;
        if (state.placing) {
            state.placing = false;
            install::ChangeOptions options{};
            options.expected = state.expected;
            options.hooks.wait = wait_pumping;
            const install::ChangeResult result =
                install::commit_change(kind, root, state.target, state.change, options);
            state.discarder.add(result.discards);
            state.discarder.add(state.unpacking->discards());
            state.unpacking.reset();
            const fs::path folder = root / path_from_utf8(state.target);
            if (!result.changed) {
                install::Problem problem{};
                problem.refusal = result.refusal;
                problem.detail = result.detail;
                log_line(state.package->file_name + ": not put in place: " + result.detail);
                state.report(
                    *this, install::OutcomeResult::failed, install::refusal_text(kind, problem)
                );
                if (!state.finish_quietly(*this)) {
                    state.stage = ModInstallState::Stage::telling;
                    state.show(
                        *this,
                        install::refused_prompt(kind, state.package->file_name, problem, true)
                    );
                }
            } else {
                log_line(state.package->file_name + ": installed in " + path_to_utf8(folder));
                state.report(*this, install::OutcomeResult::installed);
                state.told_folder = folder;
                state.play_folder = folder;
                package_changed(kind, folder);
                // A catalogue install is not told: the Library's row shows
                // it. A file the player opened still is.
                if (!state.finish_quietly(*this)) {
                    if (state.origin.kind == install::OriginKind::catalogue) {
                        state.hide(*this);
                        state.stage = ModInstallState::Stage::idle;
                    } else {
                        state.stage = ModInstallState::Stage::telling;
                        const bool play_now = package_offers_play(kind);
                        state.show(
                            *this,
                            state.change == install::Change::install
                                ? install::installed_prompt(kind, state.incoming, folder, play_now)
                                : install::updated_prompt(
                                      kind,
                                      state.change,
                                      state.incoming,
                                      state.expected,
                                      folder,
                                      result,
                                      play_now
                                  )
                        );
                    }
                }
            }
            state.finish_package();
            return;
        }
        const auto started = std::chrono::steady_clock::now();
        install::Problem problem{};
        auto step = oa::formats::zip::StreamStep::more;
        while (step == oa::formats::zip::StreamStep::more) {
            step = state.unpacking->step(kUnpackBudget, problem);
            if (watched && std::chrono::steady_clock::now() - started >= kUnpackFrameTime)
                break;
        }
        if (step == oa::formats::zip::StreamStep::failed) {
            state.discarder.add(state.unpacking->discards());
            state.unpacking.reset();
            log_line(state.package->file_name + ": " + install::refusal_text(kind, problem));
            state.report(
                *this, install::OutcomeResult::refused, install::refusal_text(kind, problem)
            );
            if (!state.finish_quietly(*this)) {
                state.stage = ModInstallState::Stage::telling;
                state.show(
                    *this, install::refused_prompt(kind, state.package->file_name, problem, false)
                );
            }
            state.finish_package();
            return;
        }
        if (step == oa::formats::zip::StreamStep::more) {
            // While the Library shows, its queue tells a chosen mod's progress.
            if (state.quiet || state.told_in_library(*this)) {
                state.hide(*this);
                return;
            }
            const bool checking = state.unpacking->checking();
            auto made = install::installing_prompt(
                kind,
                state.incoming,
                checking ? state.unpacking->checked_bytes() : state.unpacking->done_bytes(),
                checking ? state.unpacking->archive_bytes() : state.unpacking->total_bytes(),
                checking ? install::InstallingPhase::checking : install::InstallingPhase::unpacking,
                state.package->file_name
            );
            made.prompt.hovered =
                state.prompt != nullptr ? state.prompt->question().hovered : settings::no_control;
            made.prompt.pressed =
                state.prompt != nullptr ? state.prompt->question().pressed : settings::no_control;
            state.update(*this, std::move(made));
            return;
        }
        // Unpacked whole. A kind that checks the staged files does so before
        // they are put in place or left for the run's end. The folder the
        // game plays, when the kind checks nothing of its own, is checked as
        // PLAY NOW checks a folder, then changed once the run ends and
        // its archives are closed. Any other is put in place on the next
        // frame, once the prompt says so.
        if (kind.check_staged != nullptr) {
            install::Problem staged{};
            if (!kind.check_staged(
                    state.unpacking->staging(), *state.package, package_options(kind), staged
                )) {
                for (const auto& line : staged.lines)
                    log_line(state.package->file_name + ": " + line);
                log_line(
                    state.package->file_name + ": " + install::refusal_text(kind, staged) +
                    (staged.detail.empty() ? "" : " (" + staged.detail + ")")
                );
                state.unpacking->cancel();
                state.discarder.add(state.unpacking->discards());
                state.unpacking.reset();
                state.report(
                    *this,
                    install::OutcomeResult::refused,
                    install::refusal_text(kind, staged),
                    staged.lines
                );
                if (!state.finish_quietly(*this)) {
                    state.stage = ModInstallState::Stage::telling;
                    state.show(
                        *this,
                        install::refused_prompt(kind, state.package->file_name, staged, false)
                    );
                }
                state.finish_package();
                return;
            }
        } else if (state.target_played) {
            const auto& game_folder =
                options_.game_folders.empty() ? options_.game_dir : options_.game_folders.back();
            const auto check = check_picked_mod_folder(
                state.unpacking->staging(),
                game_folder,
                ModChoice{{}, {}, options_.accept_unimplemented_hacks, &preference_values_}
            );
            if (!check.refusal.empty()) {
                for (const auto& line : check.errors)
                    log_line(state.package->file_name + ": " + line);
                state.unpacking->cancel();
                state.discarder.add(state.unpacking->discards());
                state.unpacking.reset();
                install::Problem refused{};
                refused.refusal = install::Refusal::manifest_errors;
                refused.lines = check.errors;
                state.report(
                    *this,
                    install::OutcomeResult::refused,
                    install::refusal_text(kind, refused),
                    refused.lines
                );
                if (!state.finish_quietly(*this)) {
                    state.stage = ModInstallState::Stage::telling;
                    state.show(
                        *this,
                        install::refused_prompt(kind, state.package->file_name, refused, false)
                    );
                }
                state.finish_package();
                return;
            }
        }
        if (!state.target_played) {
            state.placing = true;
            if (state.quiet || state.told_in_library(*this)) {
                state.hide(*this);
                return;
            }
            state.update(
                *this,
                install::installing_prompt(
                    kind,
                    state.incoming,
                    state.unpacking->total_bytes(),
                    state.unpacking->total_bytes(),
                    install::InstallingPhase::placing,
                    state.package->file_name
                )
            );
            return;
        }
        install::PendingChange pending{};
        pending.kind = &kind;
        pending.root = root;
        pending.target = state.target;
        pending.change = state.change;
        pending.incoming = state.incoming;
        pending.replaced = state.expected;
        pending.file_name = state.package->file_name;
        pending.origin = state.origin;
        pending.file = state.file;
        pending.hold = state.unpacking->take_hold();
        install::set_pending_change(std::move(pending));
        log_line(state.package->file_name + ": put in place as the run ends");
        state.unpacking.reset();
        state.hide(*this);
        state.stage = ModInstallState::Stage::idle;
        if (state.chosen) {
            // A mod the player chose in the Library is held until they leave
            // the mod: switching mod or quitting makes the change, and the
            // Library's queue says it waits. The download queue counts it
            // installed, so that it is not fetched again at the next start.
            log_line(state.package->file_name + ": held until the player leaves the mod");
            state.report(*this, install::OutcomeResult::installed);
            state.note_for_library(
                *this, {state.origin.registry, state.origin.catalogue_id, {}, state.target}
            );
            state.finish_package();
            return;
        }
        state.finish_package();
        request_soft_restart();
        return;
    }
    namespace frontend_state = oa::ui::frontend_state;
    const bool settled = screen_ == Screen::main_menu && !frame_owned_by_package() &&
                         state_.state == frontend_state::state_id::main_menu &&
                         state_.pending_signal != frontend_state::signal_id::multiplayer &&
                         !saves_notice_shown() && oa::ui::frontend_dialogs::dialog_count() == 0 &&
                         engine_settings_dialog() == nullptr;
    // A registry's name as the Library shows it; its id when the Library
    // does not list it.
    const auto library_registry_name = [this](std::string_view registry) {
        for (const oa::ui::library::Registry& listed : library_host().model.inputs.registries)
            if (listed.id == registry && !listed.name.empty())
                return listed.name;
        return std::string(registry);
    };
    // Opens the package taken and plans it. A quiet install shows nothing:
    // a plan that asks goes back to the inbox, and a refusal is reported.
    const auto begin_package = [&](const install::OpenedPackage& opened_file) {
        state.file = opened_file.file;
        state.origin = opened_file.origin;
        state.told_folder.clear();
        state.play_folder.clear();
        state.kind = install::kind_for_file(state.file);
        log_line("opening " + path_to_utf8(state.file));
        const std::string name = path_to_utf8(state.file.filename());
        // A mod the player chose in the Library: its origin says so, and its
        // registry, asked once, whether it asks before it replaces. An
        // earlier note of the same registry and key is done with.
        state.chosen = state.kind != nullptr && state.kind->name == "oamod" &&
                       state.origin.kind == install::OriginKind::catalogue;
        state.from_built_in = state.chosen && library_registry_built_in(state.origin.registry);
        if (state.chosen)
            state.note_for_library(
                *this, {state.origin.registry, state.origin.catalogue_id, {}, {}}
            );
        if (state.kind == nullptr) {
            log_line(name + ": it is not a kind of package this game installs");
            state.report(
                *this,
                install::OutcomeResult::refused,
                "It is not a kind of package this game installs."
            );
            if (!state.finish_quietly(*this)) {
                state.stage = ModInstallState::Stage::telling;
                state.show(*this, install::unknown_kind_prompt(name));
            }
            install::finish_package_file();
            return;
        }
        // The platform may bring a file the system opened into the game's own
        // storage first. A catalogue package was not opened that way.
        fs::path readable = state.file;
        if (state.origin.kind != install::OriginKind::catalogue) {
            if (const PlatformHooks& hooks = platform_hooks(); hooks.take_opened_file != nullptr) {
                std::string copy;
                std::string why;
                if (!hooks.take_opened_file(
                        hooks.context, path_to_utf8(state.file).c_str(), &copy, &why
                    )) {
                    install::Problem problem{};
                    problem.refusal = install::Refusal::unreadable;
                    problem.detail = why;
                    log_line(path_to_utf8(state.file) + ": " + why);
                    state.report(
                        *this,
                        install::OutcomeResult::refused,
                        install::refusal_text(*state.kind, problem)
                    );
                    if (!state.finish_quietly(*this)) {
                        state.stage = ModInstallState::Stage::telling;
                        state.show(
                            *this, install::refused_prompt(*state.kind, name, problem, false)
                        );
                    }
                    state.kind = nullptr;
                    install::finish_package_file();
                    return;
                }
                if (!copy.empty() && copy != path_to_utf8(state.file)) {
                    readable = path_from_utf8(copy);
                    state.opened_copy = readable;
                }
            }
        }
        auto opened = install::open_package(readable, package_options(*state.kind));
        if (!opened.package) {
            log_line(
                name + ": " +
                with_detail(
                    opened.problem.refusal == install::Refusal::unknown_kind
                        ? std::string("it is not a kind of package this game installs")
                        : install::refusal_text(*state.kind, opened.problem),
                    opened.problem.detail
                )
            );
            for (const auto& line : opened.problem.lines)
                log_line(name + ": " + line);
            state.report(
                *this,
                install::OutcomeResult::refused,
                opened.problem.refusal == install::Refusal::unknown_kind
                    ? std::string("It is not a kind of package this game installs.")
                    : install::refusal_text(*state.kind, opened.problem),
                opened.problem.lines
            );
            if (!state.finish_quietly(*this)) {
                state.stage = ModInstallState::Stage::telling;
                state.show(
                    *this,
                    opened.problem.refusal == install::Refusal::unknown_kind
                        ? install::unknown_kind_prompt(name)
                        : install::refused_prompt(*state.kind, name, opened.problem, false)
                );
            }
            state.finish_package();
            return;
        }
        state.package = std::move(opened.package);
        state.kind = state.package->kind;
        // The texts name the file the player opened, not the platform's copy.
        state.package->file_name = name;
        for (const auto& warning : state.package->warnings)
            log_line(state.package->file_name + ": " + warning);
        if (state.package->backup_left_out)
            log_line(state.package->file_name + ": its own .backup folder is left out");
        if (state.package->origin_left_out)
            log_line(state.package->file_name + ": its own .oa-origin.yaml is left out");
        if (state.kind == nullptr || state.kind->plan == nullptr) {
            log_line(name + ": it is not a kind of package this game installs");
            state.report(
                *this,
                install::OutcomeResult::refused,
                "It is not a kind of package this game installs."
            );
            if (!state.finish_quietly(*this)) {
                state.stage = ModInstallState::Stage::telling;
                state.show(*this, install::unknown_kind_prompt(name));
            }
            state.finish_package();
            return;
        }
        const install::PackageKind& kind = *state.kind;
        const fs::path root = user_folder_ / std::string(kind.root_folder);
        state.incoming = state.package->incoming;
        state.incoming.origin = state.origin;
        // An expansion is not unpacked by the generic installer. Its files
        // are the built-in table's, copied with the .oa-content record. A
        // catalogue install shows nothing. A file the player opened is told
        // once it is in place, and is not asked first.
        if (kind.name == "expansion") {
            install::Problem problem{};
            fs::path installed;
            const std::string_view registry_id = state.origin.kind == install::OriginKind::catalogue
                                                     ? std::string_view(state.origin.registry)
                                                     : std::string_view{};
            const bool placed = install::oaexp::install_oaexp(
                readable, user_folder_, registry_id, &installed, problem
            );
            if (!placed) {
                log_line(
                    name + ": " + with_detail(install::refusal_text(kind, problem), problem.detail)
                );
                for (const auto& line : problem.lines)
                    log_line(name + ": " + line);
                state.report(
                    *this,
                    install::OutcomeResult::refused,
                    install::refusal_text(kind, problem),
                    problem.lines
                );
                if (!state.finish_quietly(*this)) {
                    state.stage = ModInstallState::Stage::telling;
                    state.show(
                        *this,
                        install::refused_prompt(kind, state.package->file_name, problem, false)
                    );
                }
                state.finish_package();
                return;
            }
            log_line(name + ": installed in " + path_to_utf8(installed));
            state.report(*this, install::OutcomeResult::installed);
            state.told_folder = installed;
            if (!state.finish_quietly(*this)) {
                if (state.origin.kind == install::OriginKind::catalogue) {
                    state.hide(*this);
                    state.stage = ModInstallState::Stage::idle;
                } else {
                    state.stage = ModInstallState::Stage::telling;
                    state.show(
                        *this, install::installed_prompt(kind, state.incoming, installed, false)
                    );
                }
            }
            state.finish_package();
            return;
        }
        state.plan = kind.plan(state.incoming, install::folder_hooks(kind, root));
        switch (state.plan.kind) {
        case install::PlanKind::install:
            state.target = state.plan.target;
            state.change = state.plan.reinstalling ? install::Change::reinstall
                           : state.plan.replacing  ? install::Change::replace
                                                   : install::Change::install;
            state.expected = state.plan.reinstalling || state.plan.replacing
                                 ? state.plan.installed
                                 : install::InstalledPackage{};
            state.target_played = false;
            answer_mod_install_prompt(-1);
            return;
        case install::PlanKind::refuse: {
            install::Problem problem{};
            problem.refusal = state.plan.refusal;
            problem.subject = state.incoming.id;
            log_line(name + ": " + install::refusal_text(kind, problem));
            state.report(
                *this, install::OutcomeResult::refused, install::refusal_text(kind, problem)
            );
            if (!state.finish_quietly(*this)) {
                state.stage = ModInstallState::Stage::telling;
                state.show(
                    *this, install::refused_prompt(kind, state.package->file_name, problem, false)
                );
            }
            state.finish_package();
            return;
        }
        default:
            break;
        }
        if (state.quiet) {
            install::return_package_file({state.file, state.origin});
            state.catalogue_question = state.file;
            state.release_opened_copy();
            state.package.reset();
            state.kind = nullptr;
            state.hide(*this);
            state.quiet = false;
            state.stage = ModInstallState::Stage::idle;
            return;
        }
        // A mod the player chose in the Library asks only what its registry
        // must: the player asked for it there. While the Library shows, its
        // question shows over the Library at once.
        const ChosenModStep step =
            state.chosen ? chosen_mod_step(state.plan, state.origin, state.from_built_in)
                         : ChosenModStep::ask;
        switch (step) {
        case ChosenModStep::replace:
        case ChosenModStep::reinstall:
            state.target = state.plan.target;
            state.change = step == ChosenModStep::reinstall ? install::Change::reinstall
                                                            : install::Change::replace;
            state.expected = state.plan.installed;
            state.target_played = false;
            answer_mod_install_prompt(-1);
            return;
        case ChosenModStep::refuse_folder: {
            // The folder is left as it is, and nothing goes beside it.
            const std::string folder = std::string(kind.root_folder) + "/" + state.incoming.id;
            std::string reason =
                filled(kOwnFolderText, {{"folder", folder}, {"name", state.incoming.name}});
            log_line(state.package->file_name + ": " + reason);
            state.report(*this, install::OutcomeResult::refused, reason);
            if (!state.finish_quietly(*this)) {
                state.stage = ModInstallState::Stage::telling;
                state.show(*this, own_folder_prompt(state.package->file_name, std::move(reason)));
            }
            state.finish_package();
            return;
        }
        case ChosenModStep::ask_registry: {
            const install::Origin& record = *state.plan.installed.origin;
            state.stage = ModInstallState::Stage::asking;
            state.show(
                *this,
                replace_registry_prompt(
                    state.incoming.name,
                    library_registry_name(record.registry),
                    library_registry_name(state.origin.registry)
                ),
                state.told_in_library(*this)
            );
            return;
        }
        case ChosenModStep::ask:
            break;
        }
        // A chosen mod's change to the mod being played waits until the
        // player leaves it, so its question says nothing of a reload.
        state.stage = ModInstallState::Stage::asking;
        state.show(
            *this,
            install::question_prompt(
                kind,
                state.plan,
                state.incoming,
                state.package->file_name,
                root,
                !state.chosen && !state.plan.target.empty() &&
                    package_target_in_use(kind, root / path_from_utf8(state.plan.target))
            ),
            state.told_in_library(*this)
        );
    };
    // A catalogue language pack asks nothing. It is installed with no prompt
    // while Settings or a notice show, and in a run nobody watches. A match
    // still waits. Any other package is left in the inbox.
    const auto take_catalogue_language = [&]() -> bool {
        if (state.stage != ModInstallState::Stage::idle || state.prompt != nullptr)
            return false;
        const auto next = install::next_package_file();
        if (!next)
            return false;
        const install::PackageKind* kind = install::kind_for_file(next->file);
        if (kind == nullptr || kind->name != "oalang" ||
            next->origin.kind != install::OriginKind::catalogue)
            return false;
        const auto opened_file = install::take_package_file();
        if (!opened_file)
            return false;
        state.quiet = true;
        begin_package(*opened_file);
        return true;
    };
    // A catalogue expansion asks nothing. It is copied with no prompt while
    // Settings or a notice show, and in a run nobody watches. A match still
    // waits.
    const auto take_catalogue_expansion = [&]() -> bool {
        if (state.stage != ModInstallState::Stage::idle || state.prompt != nullptr)
            return false;
        const auto next = install::next_package_file();
        if (!next)
            return false;
        const install::PackageKind* waiting_kind = install::kind_for_file(next->file);
        if (waiting_kind == nullptr || waiting_kind->name != "expansion" ||
            next->origin.kind != install::OriginKind::catalogue)
            return false;
        const auto opened_file = install::take_package_file();
        if (!opened_file)
            return false;
        state.quiet = true;
        begin_package(*opened_file);
        return true;
    };
    // A mod the player chose in the Library starts as catalogue_mod_may_start
    // says: on the main menu and also while the Library is the top screen
    // over it, Settings under it included. Any other package waits for the
    // settled main menu, as a file always has.
    const auto chosen_mod = [](const install::OpenedPackage& waiting) {
        const install::PackageKind* kind = install::kind_for_file(waiting.file);
        return kind != nullptr && kind->name == "oamod" &&
               waiting.origin.kind == install::OriginKind::catalogue;
    };
    const auto take_chosen_mod = [&]() -> bool {
        if (state.stage != ModInstallState::Stage::idle || state.prompt != nullptr)
            return false;
        const auto next = install::next_package_file();
        if (!next || !chosen_mod(*next) || !catalogue_mod_may_start())
            return false;
        const auto opened_file = install::take_package_file();
        if (!opened_file)
            return false;
        state.quiet = false;
        begin_package(*opened_file);
        return true;
    };
    // In a run nobody watches, a chosen mod starts only while the Library is
    // the top screen: whoever drives the run asked for it there, and the
    // Library's queue tells what a prompt would. With the Library closed it
    // waits in the inbox, so a run nobody drives leaves a package an earlier
    // run's download queue hands over for a run someone watches or drives.
    const auto library_on_top = [this] {
        const LayerScreen* top = oa_layer_ ? oa_layer_->top() : nullptr;
        return top != nullptr && top->name() == library_screen_name;
    };
    // A map pack, a mutator, a brain or a key layout from a catalogue: an
    // oamap, oamut, oabrain or oakeys package whose origin is one. It asks
    // nothing, and installs with no prompt off the settled main menu.
    const auto catalogue_map_pack = [](const install::OpenedPackage& waiting) {
        const install::PackageKind* kind = install::kind_for_file(waiting.file);
        return kind != nullptr &&
               (kind->name == "oamap" || kind->name == "oamut" || kind->name == "oabrain" ||
                kind->name == "oakeys") &&
               waiting.origin.kind == install::OriginKind::catalogue;
    };
    // Takes a catalogue map pack, mutator, brain or key layout first in the
    // inbox and installs it with no prompt. One whose plan asked waits, as
    // it does off the main menu, for the settled main menu of a run someone
    // watches.
    const auto take_catalogue_map = [&]() -> bool {
        if (state.stage != ModInstallState::Stage::idle || state.prompt != nullptr ||
            !state.catalogue_question.empty())
            return false;
        const auto next = install::next_package_file();
        if (!next || !catalogue_map_pack(*next))
            return false;
        const auto opened_file = install::take_package_file();
        if (!opened_file)
            return false;
        state.quiet = true;
        begin_package(*opened_file);
        return true;
    };
    // In a run nobody watches, a catalogue map pack, mutator, brain or key
    // layout installs
    // only while the player is getting one: the map browser is the top screen,
    // or GET MAP or GET MUTATORS shows over the battle room. Otherwise it
    // waits in the inbox, as a chosen mod does with the Library closed, so a
    // run nobody drives leaves a package an earlier run's download queue hands
    // over for a later run.
    const auto getting_map_pack = [this] {
        const LayerScreen* top = oa_layer_ ? oa_layer_->top() : nullptr;
        return (top != nullptr && top->name() == oa::ui::map_browser::screen_name) ||
               ModInstallState::get_map_shows(*this) || ModInstallState::get_mutators_shows(*this);
    };
    if (!settled) {
        state.settled_frames = 0;
        // Nothing starts while a match loads or runs. A catalogue map pack,
        // mutator, brain or key layout, a language pack or an expansion otherwise installs
        // here, with no prompt; every other package waits.
        if (match_ != nullptr || screen_ == Screen::loading || screen_ == Screen::match)
            return;
        if (state.stage != ModInstallState::Stage::idle || state.prompt != nullptr)
            return;
        if (take_catalogue_language())
            return;
        if (take_catalogue_expansion())
            return;
        if (nobody_watches) {
            if (library_on_top())
                std::ignore = take_chosen_mod();
            if (getting_map_pack())
                std::ignore = take_catalogue_map();
            return;
        }
        if (take_chosen_mod())
            return;
        if (!state.catalogue_question.empty())
            return;
        const auto opened_file = install::take_package_file();
        if (!opened_file)
            return;
        if (!catalogue_map_pack(*opened_file)) {
            install::return_package_file(*opened_file);
            return;
        }
        state.quiet = true;
        begin_package(*opened_file);
        return;
    }
    if (++state.settled_frames < kPromptMenuFrames || engine_settings_fonts() == nullptr)
        return;
    // A run nobody watches leaves the packages waiting for one someone does,
    // except a catalogue language pack and a catalogue expansion, which ask
    // nothing; a catalogue map pack, mutator, brain or key layout while the player is
    // getting a map pack, which installs with no prompt;
    // and a mod chosen in the Library while the Library is the top screen,
    // which asks and installs as it does for a player watching, the folders
    // its changes drop going a step a frame there too.
    if (nobody_watches) {
        if (take_catalogue_language())
            return;
        if (take_catalogue_expansion())
            return;
        if (getting_map_pack() && take_catalogue_map())
            return;
        if (!library_on_top())
            return;
        if (state.discarder.busy())
            std::ignore = state.discarder.step();
        std::ignore = take_chosen_mod();
        return;
    }
    if (state.discarder.busy())
        std::ignore = state.discarder.step();
    if (state.stage != ModInstallState::Stage::idle || state.prompt != nullptr)
        return;
    // What a change that waited for the last run did.
    if (auto outcome = install::take_change_outcome()) {
        const install::PackageKind* kind = outcome->change.kind;
        const fs::path folder = outcome->change.root / path_from_utf8(outcome->change.target);
        state.told_folder = folder;
        state.stage = ModInstallState::Stage::telling;
        const std::string_view told =
            outcome->change.file_name.empty() ? outcome->change.target : outcome->change.file_name;
        if (kind == nullptr) {
            log_line(std::string(told) + ": a change waited with no kind");
            state.show(*this, install::unknown_kind_prompt(told));
            return;
        }
        if (!outcome->result.changed) {
            install::Problem problem{};
            problem.refusal = outcome->result.refusal;
            problem.detail = outcome->result.detail;
            state.show(*this, install::refused_prompt(*kind, told, problem, true));
        } else if (outcome->change.origin.kind == install::OriginKind::catalogue) {
            // The Library's row shows a catalogue install that waited. A
            // roll back carries no catalogue origin, so it is still told.
            state.stage = ModInstallState::Stage::idle;
        } else {
            state.show(
                *this,
                install::updated_prompt(
                    *kind,
                    outcome->change.change,
                    outcome->change.incoming,
                    outcome->change.replaced,
                    folder,
                    outcome->result,
                    false
                )
            );
        }
        return;
    }
    const uint64_t now = SDL_GetTicks();
    if (now >= state.next_handoff_ms) {
        state.next_handoff_ms = now + kHandoffPollMs;
        take_handed_mod_files();
    }
    // A catalogue language pack, a catalogue expansion, and a catalogue map
    // pack, mutator, brain or key layout that asks nothing, install here with
    // nothing shown, as they do off this menu. One whose plan asks is put
    // back for the take below, which shows the question.
    if (take_catalogue_language())
        return;
    if (take_catalogue_expansion())
        return;
    if (take_catalogue_map())
        return;
    // A catalogue map pack, mutator, brain or key layout that needed a question waited for this menu.
    state.catalogue_question.clear();
    state.quiet = false;
    if (take_chosen_mod())
        return;
    // A chosen mod first in the inbox waits while another OA screen shows.
    if (const auto next = install::next_package_file(); !next || chosen_mod(*next))
        return;
    const auto opened_file = install::take_package_file();
    if (!opened_file)
        return;
    begin_package(*opened_file);
}

bool Runtime::catalogue_mod_may_start() {
    namespace frontend_state = oa::ui::frontend_state;
    // Never in a match or on a loading screen, nor while a package owns the
    // frame, the menu is leaving for another or a message box shows.
    if (match_ != nullptr || screen_ != Screen::main_menu || frame_owned_by_package() ||
        state_.state != frontend_state::state_id::main_menu ||
        state_.pending_signal == frontend_state::signal_id::multiplayer ||
        oa::ui::frontend_dialogs::dialog_count() != 0 || engine_settings_fonts() == nullptr)
        return false;
    // Nothing of Open Annihilation's own over the main menu, or the Library
    // on top; never Settings, a notice or a question.
    const LayerScreen* top = oa_layer_ ? oa_layer_->top() : nullptr;
    return top == nullptr || top->name() == library_screen_name;
}

void Runtime::answer_mod_install_prompt(int32_t button) {
    auto& state = mod_install_state();
    install::Answer answer = install::Answer::ok;
    if (button >= 0) {
        if (state.prompt == nullptr || static_cast<std::size_t>(button) >= state.answers.size())
            return;
        answer = state.answers[static_cast<std::size_t>(button)];
    }
    const auto start_unpacking = [&] {
        if (state.kind == nullptr || !state.package)
            return;
        const install::PackageKind& kind = *state.kind;
        const fs::path root = user_folder_ / std::string(kind.root_folder);
        const fs::path folder = root / path_from_utf8(state.target);
        state.target_played = package_target_in_use(kind, folder);
        state.unpacking = std::make_unique<install::Unpacking>();
        state.origin.installed = options_.fixed_clock
                                     ? "2000-01-01"
                                     : install::utc_date_text(std::chrono::system_clock::now());
        install::Problem problem{};
        if (!state.unpacking->start(
                *state.package, root, state.target, state.expected, state.origin, problem
            )) {
            state.discarder.add(state.unpacking->discards());
            state.unpacking.reset();
            log_line(
                state.package->file_name + ": " +
                with_detail(install::refusal_text(kind, problem), problem.detail)
            );
            state.report(
                *this, install::OutcomeResult::refused, install::refusal_text(kind, problem)
            );
            if (!state.finish_quietly(*this)) {
                state.stage = ModInstallState::Stage::telling;
                state.show(
                    *this, install::refused_prompt(kind, state.package->file_name, problem, false)
                );
            }
            state.finish_package();
            return;
        }
        log_line(
            state.package->file_name + ": unpacking into " +
            path_to_utf8(state.unpacking->staging())
        );
        state.stage = ModInstallState::Stage::unpacking;
        state.placing = false;
        // While the Library shows, its queue tells a chosen mod's progress,
        // and the question that asked for it closes.
        if (state.quiet || state.told_in_library(*this)) {
            state.hide(*this);
            return;
        }
        state.show(
            *this,
            install::installing_prompt(
                kind,
                state.incoming,
                0,
                state.unpacking->archive_bytes(),
                install::InstallingPhase::checking,
                state.package->file_name
            )
        );
    };
    switch (state.stage) {
    case ModInstallState::Stage::idle:
        // A plan that asks nothing.
        if (button < 0 && state.package)
            start_unpacking();
        return;
    case ModInstallState::Stage::asking:
        switch (answer) {
        case install::Answer::replace:
            state.target = state.plan.target;
            state.change = install::Change::replace;
            state.expected = state.plan.installed;
            start_unpacking();
            return;
        case install::Answer::reinstall:
            state.target = state.plan.target;
            state.change = install::Change::reinstall;
            state.expected = state.plan.installed;
            start_unpacking();
            return;
        case install::Answer::alongside:
            state.target = state.plan.alongside;
            state.change = install::Change::install;
            state.expected = {};
            start_unpacking();
            return;
        default:
            log_line(state.package ? state.package->file_name + ": cancelled" : "cancelled");
            state.report(*this, install::OutcomeResult::set_aside);
            play_ui_sound(kCloseSound, 0);
            state.hide(*this);
            state.stage = ModInstallState::Stage::idle;
            state.finish_package();
            return;
        }
    case ModInstallState::Stage::unpacking:
        // CANCEL stops the unpacking; nothing is changed.
        if (answer != install::Answer::cancel || state.placing || !state.unpacking)
            return;
        state.unpacking->cancel();
        state.discarder.add(state.unpacking->discards());
        state.unpacking.reset();
        log_line(state.package ? state.package->file_name + ": unpacking cancelled" : "cancelled");
        state.report(*this, install::OutcomeResult::set_aside);
        play_ui_sound(kCloseSound, 0);
        state.hide(*this);
        state.stage = ModInstallState::Stage::idle;
        state.finish_package();
        return;
    case ModInstallState::Stage::telling:
        switch (answer) {
        case install::Answer::open_folder: {
            const FolderOpening opening = open_player_folder(state.told_folder);
            if (state.prompt != nullptr) {
                state.prompt->question().failure = opening.opened ? std::string() : opening.reason;
                state.prompt->changed();
            }
            return;
        }
        case install::Answer::play_now: {
            std::string refusal;
            if (!switch_to_mod_folder(state.play_folder, refusal)) {
                if (state.prompt != nullptr) {
                    state.prompt->question().failure =
                        std::string(oa::data::languages::interface_text(refusal));
                    state.prompt->changed();
                }
                return;
            }
            state.hide(*this);
            state.stage = ModInstallState::Stage::idle;
            return;
        }
        default:
            play_ui_sound(kCloseSound, 0);
            state.hide(*this);
            state.stage = ModInstallState::Stage::idle;
            return;
        }
    }
}

bool Runtime::switch_to_mod_folder(const fs::path& folder, std::string& refusal) {
    auto& state = engine_settings_state();
    settings::EngineSettings chosen = state.current;
    // No mod needs no check: the game folder plays as it is, and the folder
    // an earlier version picked is forgotten with the mod it was.
    chosen.mod_folder = folder.empty() ? std::string() : kept_path(folder);
    if (folder.empty())
        chosen.picked_mod_folder.clear();
    const auto& game_folder =
        options_.game_folders.empty() ? options_.game_dir : options_.game_folders.back();
    if (!folder.empty()) {
        auto check = check_picked_mod_folder(
            folder,
            game_folder,
            ModChoice{{}, {}, options_.accept_unimplemented_hacks, &preference_values_}
        );
        // Without a profile, the start ends on a SIDEDATA it cannot play,
        // such as one a side's section is missing from, in the language
        // chosen.
        if (check.refusal.empty() && check.without_profile)
            if (auto problem =
                    side_data_problem_over(folder, game_folder, data_word_for(chosen.language));
                !problem.empty()) {
                check.refusal = "That folder cannot be played; the log says why.";
                check.errors.push_back(std::move(problem));
            }
        for (const auto& line : check.errors)
            std::cerr << "open-annihilation: the mod chosen: " << line << '\n';
        if (!check.refusal.empty()) {
            refusal = check.refusal;
            return false;
        }
    }
    const settings::EngineSettings opened = state.current;
    apply_engine_settings(chosen);
    if (const auto failure = save_engine_settings(opened, chosen, false)) {
        EngineSettingsState::report_failed_save(*this, *failure);
        refusal = *failure;
        return false;
    }
    request_soft_restart();
    return true;
}

bool Runtime::roll_back_mod_folder(const fs::path& folder, std::string& refusal) {
    const fs::path mods = folder.parent_path();
    const std::string target = path_to_utf8(folder.filename());
    const install::PackageKind& kind = install::mod_kind();
    const install::InstalledPackage installed = install::oamod::read_installed_mod(folder);
    const auto backup = install::oamod::read_backup(folder);
    const std::string title = installed.name.empty() ? target : installed.name;
    auto& install_state = mod_install_state();
    install_state.roll_back_refusal.reset();
    // A refusal changes nothing. Its words go to the caller, and its prompt
    // is kept for the Library to show over itself.
    const auto refuse = [&](install::PackagePrompt made, std::string_view why) {
        log_line(path_to_utf8(folder) + ": not rolled back: " + std::string(why));
        refusal = prompt_words(made.prompt);
        install_state.roll_back_refusal = std::move(made);
        return false;
    };
    if (!backup || backup->kind != install::FolderKind::package)
        return refuse(
            install::oamod::roll_back_failed_prompt(title), "it keeps no version that can be read"
        );
    // The kept version must still be playable: an engine since may refuse it.
    const auto& game_folder =
        options_.game_folders.empty() ? options_.game_dir : options_.game_folders.back();
    const fs::path kept = entry_without_case(folder, install::backup_folder_name)
                              .value_or(folder / std::string(install::backup_folder_name));
    const auto check = check_picked_mod_folder(
        kept,
        game_folder,
        ModChoice{{}, {}, options_.accept_unimplemented_hacks, &preference_values_}
    );
    for (const auto& line : check.errors)
        log_line(path_to_utf8(kept) + ": " + line);
    if (!check.refusal.empty())
        return refuse(
            install::oamod::roll_back_refused_prompt(
                title,
                install::oamod::version_label(
                    backup->version, backup->revision, backup->version == installed.version
                ),
                oa::data::languages::interface_text(check.refusal)
            ),
            "the version it keeps cannot be played"
        );
    auto& state = engine_settings_state();
    const bool played = package_target_in_use(kind, folder);
    install::PendingChange pending{};
    pending.kind = &kind;
    pending.root = mods;
    pending.target = target;
    pending.change = install::Change::roll_back;
    pending.incoming = {backup->id, backup->name, backup->version, backup->revision};
    pending.replaced = installed;
    if (played) {
        // As OK does: Settings, where it is open, keeps its choices, saves
        // them and closes, and the run ends for the folder to change.
        if (settings::Dialog* dialog = engine_settings_dialog()) {
            take_renderer_retry(*dialog);
            keep_renderer_records();
            apply_engine_settings(dialog->chosen);
            const auto failure =
                save_engine_settings(dialog->opened, dialog->chosen, dialog->restored);
            state.last_page = dialog->page;
            state.last_developer_list = dialog->developer;
            state.dialog.reset();
            if (failure) {
                EngineSettingsState::report_failed_save(*this, *failure);
                refusal = *failure;
                return false;
            }
        }
        install::set_pending_change(std::move(pending));
        log_line(path_to_utf8(folder) + ": rolled back as the run ends");
        request_soft_restart();
        return true;
    }
    install::ChangeOptions options{};
    options.expected = installed;
    options.hooks.wait = wait_pumping;
    const install::ChangeResult result =
        install::commit_change(kind, mods, target, install::Change::roll_back, options);
    install_state.discarder.add(result.discards);
    if (!result.changed)
        return refuse(install::oamod::roll_back_failed_prompt(title), result.detail);
    log_line(path_to_utf8(folder) + ": rolled back to " + backup->version);
    package_changed(kind, folder);
    return true;
}

} // namespace oa::app

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the Library's entries ask of the game: ROLL BACK, which asks the Roll
// Back Mod question over the Library and rolls the folder back, a refusal
// told in a prompt; PLAY NOW, which switches the game to the mod, a refusal
// told in a note; SWITCH OFF, which asks the Switch Mod question and plays
// the game with no mod; UPDATE and UPDATE ALL, which start updates through
// request_update, the one place an update reaches the download queue;
// UNINSTALL, which asks first and removes an installed package's folder,
// never the mod being played or the language pack in use; and the footer's
// the header's refresh icon, which asks the registries for their lists again. An update that
// may stop saved games loading, of a mod from a registry the player added,
// asks the update question first (oa::ui::library::asks_before_update);
// every other starts at once.

#include "library_host.hpp"
#include "mod_install_state.hpp"
#include "oa_layer.hpp"
#include "package_paths.hpp"

#include "oa/app/content/downloads.hpp"
#include "oa/app/content/service.hpp"
#include "oa/app/package_install.hpp"
#include "oa/app/package_install/oamod.hpp"
#include "oa/app/runtime.hpp"
#include "oa/base/threads.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/mod_questions.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/library/uninstall.hpp"
#include "oa/ui/library/update_question.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {

namespace library = oa::ui::library;
namespace install = oa::app::package_install;

namespace {

/// The longest a wait between an uninstall's tries sleeps before the
/// window's events are pumped again, in milliseconds.
constexpr uint32_t kUninstallWaitSliceMs = 50;

/// Writes a line of the Library's to standard error, which the log keeps.
///
/// @param line the line
void library_log(std::string_view line) {
    std::cerr << "open-annihilation: library: " << line << '\n';
}

/// Returns the place of a question's button with an id.
///
/// @param question the question
/// @param id the button's name to automation
/// @return its place, from 0; -1 when the question has none such
int32_t button_named(const oa::ui::kit::Question& question, std::string_view id) {
    for (std::size_t place = 0; place < question.buttons.size(); ++place)
        if (question.buttons[place].id == id)
            return static_cast<int32_t>(place);
    return -1;
}

/// Returns the installer's kind of a Library entry's kind.
///
/// @param kind the entry's kind
/// @return the kind; null when the installer has none of that name
const install::PackageKind* package_kind_of(library::Kind kind) {
    switch (kind) {
    case library::Kind::mod:
        return install::find_kind("oamod");
    case library::Kind::map_pack:
        return install::find_kind("oamap");
    case library::Kind::language:
        return install::find_kind("oalang");
    case library::Kind::mutator:
        return install::find_kind("oamut");
    case library::Kind::brain:
        return install::find_kind("oabrain");
    }
    return nullptr;
}

/// Says why an uninstall removed nothing, as its notice says it.
///
/// @param refusal the installer's reason
/// @return the notice's reason
library::UninstallProblem uninstall_problem_of(install::Refusal refusal) {
    switch (refusal) {
    case install::Refusal::busy:
        return library::UninstallProblem::busy;
    case install::Refusal::changed:
        return library::UninstallProblem::changed;
    case install::Refusal::link:
        return library::UninstallProblem::link;
    case install::Refusal::unsafe_name:
        return library::UninstallProblem::outside;
    default:
        return library::UninstallProblem::not_moved;
    }
}

/// Waits between an uninstall's tries while another program holds the
/// folder's files, pumping the window's events so that it stays responsive.
///
/// @param milliseconds how long
void wait_pumping_events(void*, uint32_t milliseconds) {
    while (milliseconds > 0) {
        const uint32_t slice = std::min(milliseconds, kUninstallWaitSliceMs);
        SDL_PumpEvents();
        oa::base::threads::sleep_ms(slice);
        milliseconds -= slice;
    }
}

} // namespace

void Runtime::library_entry_action(library::Action action, const library::EntryId& entry) {
    LibraryHost& host = library_host();
    if (action == library::Action::check_now) {
        // The header's refresh icon: the registries are asked for their lists
        // again, and the icon turns until they answer.
        content_service().refresh(content::RefreshReason::check_now);
        return;
    }
    if (action == library::Action::update) {
        std::ignore = request_update(entry);
        return;
    }
    if (action == library::Action::update_all) {
        // Every UPDATE that can act and is not queued, in the order the list
        // shows them: the Updates tab's, or the model's off it.
        std::vector<library::EntryId> due;
        const auto take_due = [&due](const library::Entry& each) {
            const bool queued = each.queued != nullptr &&
                                each.queued->phase != library::QueueItem::Phase::failed &&
                                each.queued->phase != library::QueueItem::Phase::installed;
            if (each.state == library::State::update && each.can_act && !queued)
                due.push_back(each.id);
        };
        if (host.model.tab == library::Tab::updates)
            for (const std::size_t index : host.model.visible)
                take_due(host.model.entries[index]);
        else
            for (const library::Entry& each : host.model.entries)
                take_due(each);
        // The updates that ask first are asked about once, together; every
        // other starts at once, in list order, every built-in registry's
        // among them, whatever the question's answer.
        std::vector<library::Entry> asking;
        std::vector<library::EntryId> asked_for;
        std::vector<library::EntryId> at_once;
        for (const library::EntryId& each : due) {
            const library::Entry* found = library::find_entry(host.model, each);
            if (found != nullptr && found->update != nullptr &&
                library::asks_before_update(*found, *found->update)) {
                asking.push_back(*found);
                asked_for.push_back(each);
            } else {
                at_once.push_back(each);
            }
        }
        const std::optional<library::UpdateQuestion> question =
            library::update_all_question(asking, host.model.text);
        for (const library::EntryId& each : at_once)
            std::ignore = request_update(each);
        if (question)
            ask_before_updates(library::update_prompt(*question, host.model.text), asked_for);
        return;
    }
    if (action == library::Action::uninstall) {
        library_uninstall(entry);
        return;
    }
    const library::Entry* found = library::find_entry(host.model, entry);
    // PLAY NOW on the base game entry is SWITCH OFF: the same question and
    // the same stored "no mod". It is not an installed package.
    const bool base_game = library::is_base_game(entry);
    if (found == nullptr || (!base_game && found->installed == nullptr))
        return;
    const fs::path folder = base_game ? fs::path{} : found->installed->folder;
    OaLayer& layer = oa_layer();
    // The mod played changes only from the main menu, and never while the
    // command line chose it: PLAY NOW and SWITCH OFF are off then.
    const bool switching =
        action == library::Action::play_now || action == library::Action::switch_off;
    if (switching && (screen_ == Screen::match || match_ != nullptr || !options_.mod_dir.empty() ||
                      options_.base_game))
        return;
    if (action == library::Action::play_now && !base_game) {
        if (!library::package_can_run(host.model, *found))
            return;
        std::string refusal;
        if (!switch_to_mod_folder(folder, refusal)) {
            status_ = refusal;
            library_log("PLAY NOW: " + refusal);
            // The refusal is told over the Library, under the Switch Mod
            // question's title, until OK.
            QuestionScreen::Host told;
            told.tick = [this] { return screen_ != Screen::main_menu; };
            layer.push(
                std::make_unique<QuestionScreen>(
                    layer,
                    Screen::main_menu,
                    library::switch_refused_note(
                        oa::data::languages::interface_text(refusal), host.model.text
                    ),
                    told
                )
            );
        }
        return;
    }
    if (action == library::Action::switch_off ||
        (action == library::Action::play_now && base_game)) {
        // The Switch Mod question of no mod: CANCEL leaves the mod played,
        // SWITCH plays none. PLAY NOW on the base game entry asks it too.
        oa::ui::kit::Question asked = library::switch_off_question(host.model.text);
        const int32_t switch_button = button_named(asked, library::switch_button_id);
        QuestionScreen::Host answers;
        answers.answer = [this, switch_button](int32_t button) {
            if (button != switch_button)
                return true;
            std::string refusal;
            if (!switch_to_mod_folder({}, refusal)) {
                status_ = refusal;
                library_log("SWITCH OFF: " + refusal);
            }
            return true;
        };
        answers.tick = [this] { return screen_ != Screen::main_menu; };
        layer.push(
            std::make_unique<QuestionScreen>(layer, Screen::main_menu, std::move(asked), answers)
        );
        return;
    }
    if (action != library::Action::roll_back)
        return;
    // A refusal is told in a prompt over the Library (MOD NOT ROLLED BACK)
    // and in the status line, and the Library's feed looks again: the
    // version put back may have an update.
    const auto roll_back = [this, folder] {
        std::string refusal;
        if (!roll_back_mod_folder(folder, refusal)) {
            status_ = refusal;
            library_log("ROLL BACK: " + refusal);
            mod_install_state().tell_roll_back_refusal(*this);
        }
        if (library_) {
            library_->look_again(*this);
            library_->refreshed = false;
        }
    };
    // With no version kept that can be read there is nothing to ask:
    // roll_back_mod_folder refuses it, changing nothing.
    const install::InstalledPackage installed = install::oamod::read_installed_mod(folder);
    const auto kept = install::oamod::read_backup(folder);
    if (!kept || kept->kind != install::FolderKind::package) {
        roll_back();
        return;
    }
    // The Roll Back Mod question over the Library: CANCEL closes it, ROLL
    // BACK rolls the folder back.
    const bool same_version = installed.version == kept->version;
    const std::string title =
        installed.name.empty() ? std::string(library::entry_name(*found)) : installed.name;
    oa::ui::kit::Question asked = library::roll_back_question(
        title,
        install::oamod::version_label(installed.version, installed.revision, same_version),
        install::oamod::version_label(kept->version, kept->revision, same_version),
        package_target_in_use(install::mod_kind(), folder),
        host.model.text
    );
    const int32_t roll_back_button = button_named(asked, library::roll_back_button_id);
    QuestionScreen::Host answers;
    answers.answer = [roll_back, roll_back_button](int32_t button) {
        if (button == roll_back_button)
            roll_back();
        return true;
    };
    // It goes with the main menu, as the Library does.
    answers.tick = [this] { return screen_ != Screen::main_menu; };
    layer.push(
        std::make_unique<QuestionScreen>(layer, Screen::main_menu, std::move(asked), answers)
    );
}

void Runtime::library_uninstall(const library::EntryId& entry) {
    LibraryHost& host = library_host();
    const library::Entry* found = library::find_entry(host.model, entry);
    if (found == nullptr || found->installed == nullptr)
        return;
    const install::PackageKind* kind = package_kind_of(entry.kind);
    if (kind == nullptr || kind->read_installed == nullptr)
        return;
    // What UNINSTALL leads to now: the model's step, and a notice besides for
    // a package this run says is in use, though the model's inputs predate it.
    const auto step_now = [this, kind](const library::Entry& shown) {
        const library::UninstallStep step = library::uninstall_step(library_host().model, shown);
        if (step != library::UninstallStep::ask)
            return step;
        const fs::path& at = shown.installed->folder;
        if (shown.id.kind == library::Kind::mod && package_target_in_use(*kind, at))
            return library::UninstallStep::switch_off_first;
        if (shown.id.kind == library::Kind::language && language_pack_in_use(at))
            return library::UninstallStep::language_first;
        return step;
    };
    // A notice over the Library: it goes with the main menu, as the Library does.
    const auto show_notice = [this](oa::ui::kit::Question told) {
        QuestionScreen::Host answers;
        answers.tick = [this] { return screen_ != Screen::main_menu; };
        OaLayer& layer = oa_layer();
        layer.push(
            std::make_unique<QuestionScreen>(layer, Screen::main_menu, std::move(told), answers)
        );
    };
    const library::UninstallStep step = step_now(*found);
    if (step == library::UninstallStep::off)
        return;
    const std::string name(library::entry_name(*found));
    if (step != library::UninstallStep::ask) {
        library_log("UNINSTALL: " + name + " is in use; nothing is removed");
        show_notice(library::uninstall_notice(*found, step, host.model.text));
        return;
    }
    // The question names the folder as the player knows it, under their own
    // folder, and the answer removes only what the folder holds now.
    const fs::path folder = found->installed->folder;
    const std::string shown_folder =
        std::string(kind->root_folder) + "/" + path_to_utf8(folder.filename());
    const install::InstalledPackage expected = kind->read_installed(folder);
    oa::ui::kit::Question asked =
        library::uninstall_question(host.model, *found, shown_folder, host.model.text);
    int32_t uninstall_button = -1;
    for (std::size_t place = 0; place < asked.buttons.size(); ++place)
        if (asked.buttons[place].id == library::uninstall_button_id)
            uninstall_button = static_cast<int32_t>(place);
    // UNINSTALL looks again: the entry, still installed, its button still
    // on and still not in use, then its folder, which must lie in the
    // player's own folder for its kind and hold what the question named.
    const auto uninstall_now = [this, kind, entry, expected, step_now, show_notice] {
        const library::Library& model = library_host().model;
        const library::Entry* still = library::find_entry(model, entry);
        if (still == nullptr || still->installed == nullptr)
            return;
        const library::UninstallStep again = step_now(*still);
        if (again == library::UninstallStep::off)
            return;
        if (again != library::UninstallStep::ask) {
            show_notice(library::uninstall_notice(*still, again, model.text));
            return;
        }
        const std::string title(library::entry_name(*still));
        const fs::path at = still->installed->folder;
        const fs::path root = user_folder_ / std::string(kind->root_folder);
        std::optional<library::UninstallProblem> problem;
        if (user_folder_.empty() || !same_folder(kept_path(at.parent_path()), kept_path(root))) {
            library_log("UNINSTALL: " + path_to_utf8(at) + " is not in " + path_to_utf8(root));
            problem = library::UninstallProblem::outside;
        } else {
            install::ChangeOptions options{};
            options.expected = expected;
            options.hooks.wait = wait_pumping_events;
            const install::UninstallResult result =
                install::uninstall_package(*kind, root, path_to_utf8(at.filename()), options);
            mod_install_state().discarder.add(result.discards);
            if (!result.removed) {
                library_log("UNINSTALL: " + title + " not removed: " + result.detail);
                problem = uninstall_problem_of(result.refusal);
            }
        }
        if (problem) {
            show_notice(library::uninstall_failed_notice(title, *problem, model.text));
            return;
        }
        library_log("UNINSTALL: removed " + title + " from " + path_to_utf8(at));
        // Uninstalling the default computer player clears the preference, so
        // it does not keep naming a brain that is gone. The built-in player
        // is the default again.
        if (entry.kind == library::Kind::brain) {
            const auto stored =
                preference_values_.find(std::string(oa::ui::engine_settings::key::brain_default));
            if (stored != preference_values_.end() && stored->second == entry.key)
                set_default_brain({});
        }
        // What reads the kind's folder reads it again, and the Library's
        // feed lists what is installed again.
        package_changed(*kind, at);
        if (library_) {
            library_->look_again(*this);
            library_->refreshed = false;
        }
    };
    QuestionScreen::Host answers;
    answers.answer = [uninstall_now, uninstall_button](int32_t button) {
        if (button == uninstall_button)
            uninstall_now();
        return true;
    };
    // It goes with the main menu, as the Library does.
    answers.tick = [this] { return screen_ != Screen::main_menu; };
    OaLayer& layer = oa_layer();
    layer.push(
        std::make_unique<QuestionScreen>(layer, Screen::main_menu, std::move(asked), answers)
    );
}

bool Runtime::language_pack_in_use(const fs::path& folder) const {
    const std::string kept = kept_path(folder);
    for (const fs::path& used : language_pack_folders())
        if (same_folder(kept_path(used), kept))
            return true;
    return false;
}

void Runtime::ask_before_updates(
    const oa::ui::kit::Question& question, const std::vector<library::EntryId>& entries
) {
    int32_t update_button = -1;
    for (std::size_t place = 0; place < question.buttons.size(); ++place)
        if (question.buttons[place].id == library::update_button_id)
            update_button = static_cast<int32_t>(place);
    QuestionScreen::Host answers;
    // UPDATE leaves the details page at once. At the compact size that page
    // has no tabs, and it would stay until a later refresh dropped the row
    // from Updates. The player has confirmed, so the list and its tabs are
    // there on the next tick, while this question closes and the download is
    // queued. NOT NOW leaves the page where it is, and UPDATE asked from the
    // list, or at a size with no details page, leaves the Library as it is.
    answers.answer = [this, entries, update_button](int32_t button) {
        if (button != update_button)
            return true;
        if (library_ && library_->state.details_page) {
            library::close_details(library_->model);
            library_->state.details_page = false;
            library_->state.menu = library::MenuKind::none;
            library_->state.details_scroll = 0;
            library_->state.interaction.focus_shown = false;
            ++library_->model_revision;
        }
        for (const library::EntryId& each : entries)
            std::ignore = request_update(each, true);
        return true;
    };
    // It goes with the main menu, as the Library and the settings it asks
    // over do.
    answers.tick = [this] { return screen_ != Screen::main_menu; };
    OaLayer& layer = oa_layer();
    layer.push(std::make_unique<QuestionScreen>(layer, Screen::main_menu, question, answers));
}

bool Runtime::request_update(const library::EntryId& entry, bool answered) {
    LibraryHost& host = library_host();
    const library::Entry* found = library::find_entry(host.model, entry);
    if (found == nullptr || host.feed.enqueue == nullptr || entry.registry.empty())
        return false;
    // The update question, where asks_before_update says the update asks:
    // pushed at once over the screen that asked. Its UPDATE comes back here
    // answered; NOT NOW leaves the update.
    if (!answered && found->update != nullptr) {
        const bool playing = found->installed != nullptr && found->installed->playing;
        if (const std::optional<library::UpdateQuestion> asked =
                library::update_question(*found, *found->update, playing, host.model.text)) {
            ask_before_updates(library::update_prompt(*asked, host.model.text), {entry});
            return true;
        }
    }
    // What the installer noted of an earlier try is done with.
    if (mod_install_state_)
        mod_install_state_->note_for_library(*this, {entry.registry, entry.key, {}, {}});
    std::string why;
    const std::optional<uint64_t> item = host.feed.enqueue(
        host.feed.context, entry.registry, entry.key, content::DownloadReason::update, &why
    );
    if (!item) {
        if (!why.empty()) {
            status_ = why;
            library_log(why);
        }
        return false;
    }
    // The queue line keeps saying what the update changes once its update
    // note is gone.
    if (found->update != nullptr && found->installed != nullptr) {
        const library::ChangeTexts change =
            library::change_texts(*found->installed, *found->update, host.model.text);
        host.kept_updates.push_back(
            {entry.registry, entry.kind, entry.key, found->update->rules, change.from, change.to}
        );
    }
    return true;
}

} // namespace oa::app

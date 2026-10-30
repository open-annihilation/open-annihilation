// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mod packages a run installs (Runtime::ModInstallState): the package
// being installed, what its plan asks, its unpacking, the prompt over the
// main menu that asks and tells, a question screen of the OA layer
// (QuestionScreen, oa_layer.hpp), what the Library's queue says of the mods
// the player chose there, and the folders left to delete, which
// runtime_mod_install.cpp drives.
#pragma once

#include "oa/app/package_install.hpp"
#include "oa/app/package_install/inbox.hpp"
#include "oa/app/package_install/prompts.hpp"
#include "oa/app/runtime.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace oa::app {

class QuestionScreen;

struct Runtime::ModInstallState {
    /// Where the install of the package taken is.
    enum class Stage : uint8_t {
        idle,      ///< no package is being installed; the prompt, if any, tells
        asking,    ///< its question shows
        unpacking, ///< its files are unpacked, the progress shown
        telling,   ///< what came of it shows
    };
    Stage stage{Stage::idle};
    /// The prompt over the main menu: its screen on the OA layer, or waiting
    /// for it; null while none shows.
    QuestionScreen* prompt{};
    /// What the prompt's buttons answer, left to right.
    std::vector<package_install::Answer> answers;
    /// The package taken from the inbox, as it was opened.
    std::filesystem::path file;
    /// Where that package came from. installed is set when unpacking starts.
    package_install::Origin origin{};
    /// The copy the platform made of it, which it releases once the install
    /// is done with it; empty when the game reads the file where it is.
    std::filesystem::path opened_copy;
    /// The package, read and checked.
    std::optional<package_install::Package> package;
    /// The kind of the package taken, from package->kind.
    const package_install::PackageKind* kind{};
    package_install::Incoming incoming{}; ///< what the package installs
    package_install::InstallPlan plan{};  ///< where it goes and what it asks
    /// The unpacking while it runs, and until its change is put in place.
    std::unique_ptr<package_install::Unpacking> unpacking;
    std::string target;               ///< the folder in the kind's root the change acts on
    package_install::Change change{}; ///< the change the answer picked
    package_install::InstalledPackage expected{}; ///< what the target held when it was planned
    bool target_played{};                         ///< the target is the mod the game plays
    /// The unpacking is done and the prompt says the files are being put in
    /// place: the change is made on the next frame, after that is drawn.
    bool placing{};
    std::filesystem::path told_folder; ///< the folder OPEN FOLDER shows
    std::filesystem::path play_folder; ///< the folder PLAY NOW plays
    /// The folders to delete, a step each main menu frame.
    package_install::Discarder discarder;
    /// Frames in a row the main menu has shown as itself.
    uint32_t settled_frames{};
    /// --check-mod-install shows the prompts although nobody watches the run.
    /// Without it, a run nobody watches takes only a catalogue language pack,
    /// a mod chosen in the Library while the Library is the top screen, and a
    /// catalogue map pack while the map browser is the top screen or GET MAP
    /// shows over the battle room (get_map_shows), and a catalogue mutator
    /// while GET MUTATORS shows there (get_mutators_shows).
    bool check_shows_prompts{};
    /// Prompts this run has shown.
    uint32_t prompts_shown{};
    /// A catalogue map pack or language pack installing with nothing shown,
    /// including off the main menu. A finished install returns to idle
    /// instead of telling.
    bool quiet{};
    /// A catalogue map pack whose plan asks, put back until the main menu
    /// settles, so it is not opened again on every frame off the menu.
    std::filesystem::path catalogue_question{};
    /// The package is a mod the player chose in the Library: an oamod
    /// package whose origin is a catalogue. Its registry decides what it
    /// asks; while the Library shows, its questions show over the Library at
    /// once, and its progress, outcome and refusals show in the Library's
    /// queue instead of a prompt.
    bool chosen{};
    /// The chosen mod's registry is built in (Runtime::library_registry_built_in,
    /// asked once for the package): it asks nothing.
    bool from_built_in{};

    /// What the Library's queue says of one mod the player chose there,
    /// beyond what the download queue knows: why the installer refused it,
    /// or that its change waits for the player to leave the mod being played.
    struct LibraryNote {
        std::string registry{}; ///< its registry's id
        std::string key{};      ///< its key in that registry
        /// Why it was not installed, in the player's words; empty when it
        /// was installed.
        std::string problem{};
        /// The folder of Mods its change waits to act on until the player
        /// leaves the mod being played (package_install::pending_change_target);
        /// empty when nothing waits.
        std::string held_target{};
        /// The player cancelled the held update: the queue leaves that line
        /// out, and the catalogue's update is offered again.
        bool cancelled{};
    };

    /// The notes, one for each registry and key at most.
    std::vector<LibraryNote> library_notes{};
    /// Why the last ROLL BACK was refused, as the prompt the Library shows
    /// over itself; nothing once it was taken, or when it was not refused.
    std::optional<package_install::PackagePrompt> roll_back_refusal{};

    /// Tells whether the package being installed is told in the Library's
    /// queue rather than in a prompt: a chosen mod while the Library shows.
    ///
    /// @param runtime the runtime, whose OA layer may hold the Library
    /// @return true while it is
    [[nodiscard]] bool told_in_library(const Runtime& runtime) const;

    /// Tells whether GET MAP shows over the battle room: its line, with its
    /// button or the download's bar, on a joiner whose host plays a pack map
    /// this machine lacks at the host's release (runtime_get_map.cpp). A run
    /// nobody watches installs a catalogue map pack while it shows.
    ///
    /// @param runtime the runtime, whose OA layer holds GET MAP's screen
    ///     while the multiplayer screens show
    /// @return true while it shows
    [[nodiscard]] static bool get_map_shows(const Runtime& runtime);

    /// Tells whether GET MUTATORS shows over the battle room: the guest's
    /// block, whose line or button stands for a mutator of the host's list
    /// this machine lacks (runtime_get_mutators.cpp). A run nobody watches
    /// installs a catalogue mutator while it shows, as it does a map pack
    /// while GET MAP shows.
    ///
    /// @param runtime the runtime, whose OA layer holds the guest's block
    ///     while the multiplayer screens show
    /// @return true while it shows
    [[nodiscard]] static bool get_mutators_shows(const Runtime& runtime);

    /// Ends a quiet install, or the install of a chosen mod while the
    /// Library shows: no prompt, and idle for the next package.
    ///
    /// @param runtime the runtime, whose OA layer holds any prompt
    /// @return true when the install shows nothing more
    bool finish_quietly(Runtime& runtime) {
        if (!quiet && !told_in_library(runtime))
            return false;
        hide(runtime);
        quiet = false;
        stage = Stage::idle;
        return true;
    }

    /// Tells the download queue what became of the package being installed,
    /// as package_install::report_package_outcome does, and for a chosen
    /// mod notes it for the Library's queue: why it was not installed, or
    /// nothing once it was. The Library builds its model again.
    ///
    /// @param runtime the runtime, whose Library shows the note
    /// @param result what became of it
    /// @param reason why it was refused or failed; empty otherwise
    /// @param lines the diagnostics under the reason, the first of which
    ///     follows a reason that ends in a colon, for the queue and the
    ///     Library's note alike
    void report(
        Runtime& runtime,
        package_install::OutcomeResult result,
        std::string reason = {},
        const std::vector<std::string>& lines = {}
    );

    /// Notes a chosen mod for the Library's queue, replacing the note of its
    /// registry and key, and has the Library build its model again.
    ///
    /// @param runtime the runtime, whose Library shows the note
    /// @param note the note; one with no problem, nothing held and not
    ///     cancelled is dropped
    void note_for_library(Runtime& runtime, LibraryNote note);

    /// Shows why the last ROLL BACK was refused (roll_back_refusal) in a
    /// prompt over the screen that asked, at once, while no other prompt
    /// shows and no package is being installed; the screen stays open under
    /// it. Nothing when it was not refused.
    ///
    /// @param runtime the runtime
    void tell_roll_back_refusal(Runtime& runtime);

    /// When the hand-off folder is looked in next, in SDL ticks.
    uint64_t next_handoff_ms{};

    /// Shows a prompt over the main menu, counted among those shown: in the
    /// prompt that shows, or in a new one the OA layer shows once it is free
    /// (OaLayer::show_when_free), or at once over the screen that asked.
    ///
    /// @param runtime the runtime
    /// @param made the prompt and what its buttons answer
    /// @param at_once a screen asked it of its own: it shows over that
    ///     screen at once (OaLayer::push)
    void show(Runtime& runtime, package_install::PackagePrompt made, bool at_once = false);

    /// Shows the prompt again with what it says now, such as an unpacking's
    /// progress, as show does but not counted.
    ///
    /// @param runtime the runtime
    /// @param made the prompt and what its buttons answer
    void update(Runtime& runtime, package_install::PackagePrompt made);

    /// Takes the prompt off the OA layer, or out of its queue.
    ///
    /// @param runtime the runtime
    void hide(Runtime& runtime);

    /// Sets a question aside once its prompt goes for any reason but the
    /// install's own: its package waits again in the inbox, and a telling
    /// ends. An unpacking runs on, and its next progress shows a new prompt.
    void set_aside();

    /// Gives the copy the platform made of the package back to it to remove.
    void release_opened_copy();

    /// Ends the install of the package taken: its copy released and the
    /// inbox told; what comes next is no chosen mod until it says so.
    void finish_package();
};

} // namespace oa::app

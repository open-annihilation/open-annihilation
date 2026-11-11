// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One entry into an expansion, as an extension asks for it. The sentences
// are already in the language the player is reading. The header names no
// other header of the engine: an extension that includes it links
// oa::app::headers and needs no more of the engine's.
//
// An entry runs in this order: the online features (the update check, the
// catalogue check, and a needed install ID of the registry the entry
// picked) are on, the latest Open Annihilation is running, the mod is
// switched off, and the latest package of the expansion is installed and
// in use. Only then is it ready. A quiet run (nobody to answer) asks
// nothing in the first two steps, changes no setting and logs what a
// player would have been asked.
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::languages {
struct Language;
}

namespace oa::app {

class Runtime;

/// The mod switched off for an entry, kept without a mod's own prefix.
inline constexpr std::string_view kExpansionSwitchedFolder =
    "open-annihilation.expansion.switched-folder";

/// The name that question showed, kept beside the folder.
inline constexpr std::string_view kExpansionSwitchedName =
    "open-annihilation.expansion.switched-name";

/// One request to enter on the next run. Cleared the moment it is taken.
inline constexpr std::string_view kExpansionEnterOnce = "open-annihilation.expansion.enter-once";

/// Settings › Updates' Check for updates, as the preferences store it.
inline constexpr std::string_view kUpdateCheckKey = "open-annihilation.update-check";

/// The Library's check for content updates, as the preferences store it.
inline constexpr std::string_view kCatalogueCheckKey = "open-annihilation.content-updates";

/// The word either key holds when its check is off.
inline constexpr std::string_view kCheckOffWord = "never";

/// The word Turn on stores in a key whose check was off.
inline constexpr std::string_view kCheckOnWord = "automatically";

/// Where one entry into an expansion stands.
enum class ExpansionEntryStep : uint8_t {
    ready,           ///< the expansion is on, and the caller may open it
    waiting,         ///< a check or a download is still running
    asking,          ///< the update question is waiting for an answer
    mod_asking,      ///< a question about switching a mod off is waiting
    stayed,          ///< the player stayed on the menu
    failed,          ///< the entry stopped, and the notice says why
    services_asking, ///< the question about the online features is waiting
    retry_asking,    ///< the update check failed, and Try again is waiting
    updating,        ///< the newer Open Annihilation is downloading
    restarting,      ///< the run ends, and the next one enters on its own
};

/// What the caller asks when an entry begins.
///
/// The sentences are already in the language the player is reading. An empty
/// registry uses the first catalogue that lists the package.
struct ExpansionEntryAsk {
    std::string registry{}; ///< registry id; empty searches every catalogue
    std::string key{};      ///< the package id
    /// The update question; "{version}" is replaced by the offered release.
    std::string question{};
    std::string update_label{}; ///< the button that takes the update
    std::string cancel_label{}; ///< the button that stays on the menu
    std::string ok_label{};     ///< the button that closes a failure
    std::string progress{};     ///< the progress line; "{percent}" is replaced
    /// The question that asks to turn the online features on.
    std::string services_question{};
    /// The same question when it also names one registry's install ID.
    /// "{registry}" is that registry's name.
    std::string services_and_id_question{};
    /// The question that names only that registry's install ID.
    /// "{registry}" is that registry's name.
    std::string install_id_question{};
    std::string turn_on_label{}; ///< the button that turns them on and goes on
    /// The sentence a failed update check shows.
    std::string retry_question{};
    std::string retry_label{}; ///< the button that checks again
    /// When true, nobody can answer: "-y", a headless or unattended run, a
    /// check. The online features and the update are not asked about and no
    /// setting changes: the log says what a player would have been asked,
    /// and the entry goes on as such a run always has. The package is still
    /// brought up to date, and no question ever holds the entry.
    bool quiet{false};
    /// When true, this entry is the one a restart carried. A package it
    /// still has to install then stops it instead of restarting again.
    bool carried{false};
    /// When true, the run is one of the game's own checks. It asks only the
    /// registries the player added for their lists, never takes the package
    /// from a built-in registry (registry_reached), and starts no update
    /// check of its own unless the run named a feed.
    bool own_check{false};
    /// When true, a check that has not answered does not hold the entry. A
    /// result already in hand is still taken, and a missing package is still
    /// fetched. An offer already in hand is declined so the entry can go on.
    /// A player still reads the services setting and the game files. When
    /// false, the entry waits for the menu to settle and for the check to
    /// answer, and that wait ends on its own if the check does not.
    bool skip_unanswered_check{false};
    /// When true, a fetch or a failure is reported on the caller's screen
    /// instead of a question on the menu. When the caller cannot show that
    /// screen, it turns this off and the question is used.
    bool report_on_screen{false};
    /// The question that asks to switch a mod off before the update check.
    /// Empty when no mod is playing. The sentence is already translated.
    std::string mod_question{};
    std::string mod_switch_label{}; ///< the button that switches the mod off
    std::string mod_cancel_label{}; ///< the button that keeps the mod
    /// When true, the mod is switched off with no question. A run that
    /// cannot ask sets this.
    bool mod_switch_quietly{false};
    /// When true, the command line named the mod, so the entry does not
    /// switch it off. A player reads `mod_question` on one button. A run
    /// that cannot ask stays on the menu and does not restart.
    bool mod_command_line{false};
    /// The mod folder, as a path. Empty when no mod is playing, and when
    /// the command line named the mod.
    std::string mod_folder{};
    /// The name the question shows.
    std::string mod_name{};
};

/// What an entry is doing now.
///
/// The flags say which checks ran. A flag left clear means that check was
/// not performed, and a caller must not describe it as done.
struct ExpansionEntryStatus {
    ExpansionEntryStep step{ExpansionEntryStep::waiting}; ///< where it stands
    std::string notice{};       ///< the question, the progress line or the failure
    bool services_known{false}; ///< the update and catalogue settings were read
    bool services_on{false};    ///< update and catalogue checks are not both off
    bool files_known{false};    ///< the start's game-files check was read
    /// The first file of another bundle that differs. Empty when none does.
    std::string mismatched_file{};
    /// Files the check named as matching. Empty when the check only reports
    /// that nothing differed.
    std::vector<std::string> matched_files{};
    bool feed_known{false};        ///< a check of the update feed produced a result
    bool feed_failed{false};       ///< that check failed
    bool update_offered{false};    ///< the feed offers a newer release
    std::string offered_version{}; ///< the offered release's version text
    /// A quiet run went on without the update. A player never does.
    bool update_declined{false};
    std::string running_version{}; ///< this build's version text
    bool extension_known{false};   ///< the catalogue's package was considered
    /// The fetch's percent, 0 to 100. -1 when no fetch is showing.
    int32_t extension_percent{-1};
    bool extension_installed{false}; ///< this entry downloaded and installed it
    bool extension_current{false};   ///< the installed package matches the catalogue
    std::string extension_version{}; ///< the catalogue package's version
    int64_t extension_release{0};    ///< the catalogue package's release; 0 when unknown
    /// Set when this entry installed the package, which must be in use
    /// before the expansion opens. The caller restarts the run once
    /// (restart_expansion_entry), and the next run reads it from its start.
    bool extension_new{false};
    /// The entry stopped before any later check. Nothing was contacted after.
    bool stopped{false};
};

/// Starts an entry: the update check, then the package when it is not
/// already in the game folder.
///
/// A call while a check or a download from this entry is running does
/// nothing. The sentences in `ask` are shown as given.
///
/// @param runtime the running game
/// @param ask the package and the sentences
void begin_expansion_entry(Runtime& runtime, const ExpansionEntryAsk& ask);

/// Moves an entry on by one step.
///
/// @param runtime the running game
/// @return where the entry stands
[[nodiscard]] ExpansionEntryStatus advance_expansion_entry(Runtime& runtime);

/// Answers the update question.
///
/// Update downloads the offered release where this copy installs it in
/// place, saves the one-shot entry, and installs it and starts the game
/// again once it is ready, so the next run enters. Where the release is
/// only offered on its page, Update opens the page and the entry stops.
/// Cancel stays on the menu. No answer enters on the older version. A call
/// while that question is not open does nothing.
///
/// @param runtime the running game
/// @param take_update true for Update, false for Cancel
void answer_expansion_entry(Runtime& runtime, bool take_update);

/// Ends the run so the next one enters, once the entry found the newly
/// installed package not in use.
///
/// The one-shot entry is saved first. An entry a restart already carried
/// stops with the notice instead, so a run never restarts twice.
///
/// @param runtime the running game
/// @param notice the sentence shown when the run will not restart
void restart_expansion_entry(Runtime& runtime, const std::string& notice);

/// Reports whether an entry is in the middle of a check or a download.
///
/// @param runtime the running game
/// @return true while a new entry would be ignored
[[nodiscard]] bool expansion_entry_busy(const Runtime& runtime);

/// Tells a running entry whether to draw its fetch on the caller's screen.
///
/// @param runtime the running game
/// @param on_screen true to leave the menu question closed
void expansion_entry_show_on_screen(Runtime& runtime, bool on_screen);

/// Returns the language the game shows its text in.
///
/// @param runtime the running game
/// @return the language; English before the language has started
[[nodiscard]] const oa::data::languages::Language& expansion_entry_language(const Runtime& runtime);

/// Asks whether to switch a remembered mod back on.
///
/// A call while an entry is busy does nothing. The sentences are already
/// translated. Switch on is the first button. Not now is the second.
///
/// @param runtime the running game
/// @param question the sentence
/// @param on_label the button that switches the mod on
/// @param later_label the button that leaves it off
void ask_remembered_mod(
    Runtime& runtime,
    const std::string& question,
    const std::string& on_label,
    const std::string& later_label
);

/// Reports whether the remembered-mod question is open.
///
/// @param runtime the running game
/// @return true while that question is waiting
[[nodiscard]] bool remembered_mod_question_open(const Runtime& runtime);

/// Answers the remembered-mod question.
///
/// Switch on is true. Not now is false. Either answer forgets the remembered
/// mod. A call while that question is not open does nothing.
///
/// @param runtime the running game
/// @param switch_on true to switch the remembered mod on
void answer_remembered_mod(Runtime& runtime, bool switch_on);

/// Reports whether the question about switching a mod off is open.
///
/// @param runtime the running game
/// @return true while that question is waiting
[[nodiscard]] bool expansion_mod_question_open(const Runtime& runtime);

/// Answers the question about switching a mod off.
///
/// The first button switches it off. Cancel keeps it. A call while that
/// question is not open does nothing.
///
/// @param runtime the running game
/// @param switch_off true to switch the mod off and enter
void answer_expansion_mod_question(Runtime& runtime, bool switch_off);

/// The mod a run is playing, for an entry that may switch it off.
struct ActiveMod {
    bool active{false};         ///< a mod profile or a mod folder is on
    bool has_profile{false};    ///< that mod has a profile
    std::string profile_id{};   ///< the profile's id; empty with no profile
    std::string profile_name{}; ///< the profile's name; empty with no profile
    /// The mod folder, as UTF-8. Empty when no mod is playing.
    std::string folder{};
};

/// Reads the mod a run is playing.
///
/// @param runtime the running game
/// @return the mod; inactive when none is playing
[[nodiscard]] ActiveMod active_mod(const Runtime& runtime);

/// Stores one preference under its own key, with no mod prefix.
///
/// An empty value removes the key.
///
/// @param runtime the running game
/// @param key the key
/// @param value the value; empty removes the key
void keep_expansion_preference(Runtime& runtime, std::string_view key, std::string_view value);

/// Reads one preference stored under its own key, with no mod prefix.
///
/// @param runtime the running game
/// @param key the key
/// @return the value; nothing when the key is missing or empty
[[nodiscard]] std::optional<std::string>
expansion_preference(const Runtime& runtime, std::string_view key);

/// Switches to a mod folder, or to no mod when `folder` is empty.
///
/// @param runtime the running game
/// @param folder the mod folder, as UTF-8; empty for no mod
/// @param[out] refusal why it cannot be played, in a few words
/// @return true when the run ends to play it
bool switch_active_mod(Runtime& runtime, std::string_view folder, std::string& refusal);

/// Reports whether the run will start again.
///
/// @param runtime the running game
/// @return true after a switch asked the run to end
[[nodiscard]] bool expansion_will_restart(const Runtime& runtime) noexcept;

/// Saves the preferences.
///
/// @param runtime the running game
void save_expansion_preferences(Runtime& runtime);

/// Ends the run.
///
/// @param runtime the running game
void end_expansion_run(Runtime& runtime);

/// How many mutators a launch would carry.
///
/// The room's list and the running match's applied loadout, saturating at
/// the largest 32-bit count. The run's own pick is not counted.
///
/// @param runtime the running game
/// @return the count
[[nodiscard]] uint32_t active_mutator_count(const Runtime& runtime);

/// A mod was chosen for this run.
///
/// A simulation change with no chosen profile and no chosen folder is not a
/// mod.
///
/// @param profile_set the run was given a mod profile
/// @param layered_folder a mod folder is layered over the game
/// @return true when one of those is set
[[nodiscard]] inline bool chosen_mod(bool profile_set, bool layered_folder) noexcept {
    return profile_set || layered_folder;
}

/// How many mutators a match carries.
///
/// The room's list plus the running match's applied loadout. The run's own
/// pick is not an argument, so a pick left over from a skirmish or from the
/// command line does not count.
///
/// @param room how many mutators the room's list names
/// @param applied how many the running match has applied
/// @return the count, saturating at the largest 32-bit value
[[nodiscard]] inline uint32_t
carried_mutator_count(std::size_t room, std::size_t applied) noexcept {
    constexpr std::size_t most = static_cast<std::size_t>(0xffffffffu);
    if (room >= most || applied >= most - room)
        return 0xffffffffu;
    return static_cast<uint32_t>(room + applied);
}

/// What is playing when an entry is requested.
enum class PlayedMod {
    none,    ///< no mod was chosen
    own,     ///< the expansion's own profile
    foreign, ///< another profile, or a mod folder with no profile
};

/// Who is asking to enter.
enum class EntryWay {
    player, ///< the player can answer a question
    quiet,  ///< a run with nobody to ask, or one that enters on its own
};

/// What the entry does about a mod.
enum class EntryDecision {
    proceed,        ///< no question; the entry goes on
    ask,            ///< the question is shown
    switch_quietly, ///< the mod is switched off with no question
    command_notice, ///< the command line named the mod; the player is told
    command_stay,   ///< the command line named the mod; a quiet run stays
};

/// What one answer to the mod question does.
enum class ModAnswer {
    switch_off, ///< Switch off and enter
    cancel,     ///< Cancel
};

/// The effect of that answer, before the restart.
enum class ModEffect {
    switch_off, ///< switch the mod off and remember it
    stay,       ///< keep the mod and stay on the menu
};

/// The one-shot entry and the remembered mod, as the preferences keep them.
struct EntryCarry {
    bool pending{false};    ///< the next run enters once
    bool remembered{false}; ///< a mod was switched off and is still remembered
};

/// Whether leaving asks to switch the remembered mod back on.
enum class LeaveDecision {
    ask,  ///< the question is shown
    skip, ///< nothing is asked
};

/// What one answer to the leave question is.
enum class LeaveAnswer {
    switch_on, ///< Switch on
    not_now,   ///< Not now
};

/// The effect of that answer.
struct LeaveEffect {
    bool switch_on{false}; ///< switch the remembered mod back on
    bool forget{false};    ///< forget the remembered mod
};

/// Decides what an entry does about the mod that is playing.
///
/// No chosen mod, and the expansion's own profile, go on. Another mod asks
/// when the player can answer, and is switched off with no question when
/// they cannot. A mod the command line named is never switched off: the
/// player is told, and a quiet run stays without a question.
///
/// @param played what is playing
/// @param way who is asking
/// @param command_line the command line named the mod
/// @return the decision
[[nodiscard]] inline EntryDecision
decide_mod_entry(PlayedMod played, EntryWay way, bool command_line = false) noexcept {
    if (played == PlayedMod::foreign && command_line) {
        if (way == EntryWay::quiet)
            return EntryDecision::command_stay;
        return EntryDecision::command_notice;
    }
    if (played != PlayedMod::foreign)
        return EntryDecision::proceed;
    if (way == EntryWay::quiet)
        return EntryDecision::switch_quietly;
    return EntryDecision::ask;
}

/// Applies Switch off and enter, or Cancel.
///
/// Switch off remembers the mod. It saves the one-shot entry only when
/// `one_shot` is set. A quiet switch leaves that clear, because that run
/// enters again on its own. Cancel changes neither.
///
/// @param answer the button
/// @param[in,out] carry the one-shot and the remembered mod
/// @param one_shot true to save the one-shot entry
/// @return what the entry does
inline ModEffect
apply_mod_entry_answer(ModAnswer answer, EntryCarry& carry, bool one_shot = true) noexcept {
    if (answer != ModAnswer::switch_off)
        return ModEffect::stay;
    if (one_shot)
        carry.pending = true;
    carry.remembered = true;
    return ModEffect::switch_off;
}

/// Takes the one-shot entry, once.
///
/// @param[in,out] carry the one-shot and the remembered mod
/// @return true the one time a request was waiting
inline bool take_entry_request(EntryCarry& carry) noexcept {
    if (!carry.pending)
        return false;
    carry.pending = false;
    return true;
}

/// Decides whether leaving asks about the remembered mod.
///
/// It asks once, and only when a mod was switched off and is still remembered.
///
/// @param remembered a mod is still remembered
/// @param already_asked this run already asked
/// @return the decision
[[nodiscard]] inline LeaveDecision decide_mod_leave(bool remembered, bool already_asked) noexcept {
    if (!remembered || already_asked)
        return LeaveDecision::skip;
    return LeaveDecision::ask;
}

/// Applies Switch on or Not now. Either answer forgets the remembered mod.
///
/// @param answer the button
/// @return what the answer does
[[nodiscard]] inline LeaveEffect apply_mod_leave(LeaveAnswer answer) noexcept {
    LeaveEffect effect;
    effect.switch_on = answer == LeaveAnswer::switch_on;
    effect.forget = true;
    return effect;
}

/// Whether the two checks the online features are made of are on.
struct OnlineChecks {
    bool updates{true};   ///< Check for updates is not Never
    bool catalogue{true}; ///< the check for content updates is not Never
};

/// Reads both checks from stored preferences.
///
/// A key that is absent, or holds anything but kCheckOffWord, is on.
///
/// @param values the preferences, a map of key to value
/// @return both checks
template <typename Values>
[[nodiscard]] OnlineChecks read_online_checks(const Values& values) {
    const auto off = [&values](std::string_view key) {
        const auto found = values.find(std::string(key));
        return found != values.end() && found->second == kCheckOffWord;
    };
    OnlineChecks checks;
    checks.updates = !off(kUpdateCheckKey);
    checks.catalogue = !off(kCatalogueCheckKey);
    return checks;
}

/// Turns both checks on in stored preferences.
///
/// A check that is off becomes kCheckOnWord. A check already on keeps the
/// word the player chose.
///
/// @param[in,out] values the preferences, a map of key to value
template <typename Values>
void turn_on_online_checks(Values& values) {
    for (const std::string_view key : {kUpdateCheckKey, kCatalogueCheckKey}) {
        auto found = values.find(std::string(key));
        if (found != values.end() && found->second == kCheckOffWord)
            found->second = std::string(kCheckOnWord);
    }
}

/// What the entry does about the online features.
enum class ServicesDecision {
    proceed, ///< the checks are on, and no needed install ID is off
    ask,     ///< a player is asked to turn them on
    note,    ///< a quiet run logs the question and goes on as it always has
};

/// The install ID of the registry the entry picked, when that registry is known.
///
/// A registry that is not known yet, or whose descriptor does not require an
/// install ID, is not part of the question. An off key for any other
/// registry is not read.
struct InstallIdFeature {
    bool needed{false}; ///< the picked registry requires an install ID
    bool off{false};    ///< that registry's install ID is off
};

/// Which sentence asks about the online features.
enum class ServicesWording {
    checks,        ///< the update check and the catalogue check
    checks_and_id, ///< those checks and the registry's install ID
    install_id,    ///< the registry's install ID alone
};

/// The question the entry would ask, and which features that sentence names.
struct NamedServices {
    ServicesDecision decision{ServicesDecision::proceed}; ///< ask, note or go on
    ServicesWording wording{ServicesWording::checks};     ///< which sentence
    bool names_updates{false};                            ///< the sentence names the update check
    bool names_catalogue{false};  ///< the sentence names the catalogue check
    bool names_install_id{false}; ///< the sentence names the install ID
};

/// Decides what the entry does about the online features, and which it names.
///
/// Either check being off names both checks, which is the sentence the
/// checks have always used. A needed install ID that is off is named with
/// them when a check is also off, and on its own when the checks are on.
/// An install ID that is not required is never named, even when its key is
/// off.
///
/// @param checks the two checks
/// @param feature the picked registry's install ID; empty when it is not known
/// @param way who is asking
/// @return the decision and the sentence
[[nodiscard]] inline NamedServices
named_services(OnlineChecks checks, InstallIdFeature feature, EntryWay way) noexcept {
    NamedServices named;
    const bool checks_off = !checks.updates || !checks.catalogue;
    const bool id_off = feature.needed && feature.off;
    if (!checks_off && !id_off)
        return named;
    named.decision = way == EntryWay::quiet ? ServicesDecision::note : ServicesDecision::ask;
    named.names_updates = checks_off;
    named.names_catalogue = checks_off;
    named.names_install_id = id_off;
    if (checks_off && id_off)
        named.wording = ServicesWording::checks_and_id;
    else if (id_off)
        named.wording = ServicesWording::install_id;
    else
        named.wording = ServicesWording::checks;
    return named;
}

/// Decides what the entry does about the online features.
///
/// @param checks the two checks
/// @param way who is asking
/// @param feature the picked registry's install ID; empty when it is not known
/// @return proceed when nothing is off; otherwise ask a player, and note a quiet run
[[nodiscard]] inline ServicesDecision
decide_services(OnlineChecks checks, EntryWay way, InstallIdFeature feature = {}) noexcept {
    return named_services(checks, feature, way).decision;
}

/// The answers to the question about the online features.
enum class ServicesAnswer {
    turn_on, ///< Turn on
    cancel,  ///< Cancel
};

/// The effect of that answer.
enum class ServicesEffect {
    save_and_go_on, ///< both checks are on now; save them and go on
    stay,           ///< nothing changes, and the player stays on the menu
};

/// Applies Turn on or Cancel.
///
/// @param answer the button
/// @param[in,out] checks the two checks; Turn on sets both on
/// @return what the entry does
inline ServicesEffect apply_services_answer(ServicesAnswer answer, OnlineChecks& checks) noexcept {
    if (answer != ServicesAnswer::turn_on)
        return ServicesEffect::stay;
    checks.updates = true;
    checks.catalogue = true;
    return ServicesEffect::save_and_go_on;
}

/// What the update check said for the player's channel.
enum class FeedAnswer {
    up_to_date, ///< nothing newer is out
    newer,      ///< a newer release is out
    failed,     ///< the check failed: offline, or the feed could not be reached or read
    unanswered, ///< the check has not answered yet
};

/// What the entry does with that answer.
enum class UpdateDecision {
    proceed,    ///< go on to the mod and the package
    ask_update, ///< ask Update or Cancel
    ask_retry,  ///< say the check failed, with Try again or Cancel
    wait,       ///< wait for the answer
    note,       ///< a quiet run logs the question and goes on without the update
};

/// Decides what the entry does with the update check's answer.
///
/// A player never goes on without an answer, and never on an older version.
/// A quiet run is never held: it goes on as such a run always has.
///
/// @param answer the check's answer
/// @param way who is asking
/// @return the decision
[[nodiscard]] inline UpdateDecision decide_update(FeedAnswer answer, EntryWay way) noexcept {
    if (answer == FeedAnswer::up_to_date)
        return UpdateDecision::proceed;
    if (way == EntryWay::quiet)
        return answer == FeedAnswer::newer ? UpdateDecision::note : UpdateDecision::proceed;
    if (answer == FeedAnswer::newer)
        return UpdateDecision::ask_update;
    if (answer == FeedAnswer::failed)
        return UpdateDecision::ask_retry;
    return UpdateDecision::wait;
}

/// The answers to the update question. There is no Not now.
enum class UpdateAnswer {
    update, ///< Update
    cancel, ///< Cancel
};

/// The effect of that answer.
enum class UpdateEffect {
    download_and_restart, ///< the release downloads, installs and starts the game again
    open_page,            ///< the release's page opens, and the entry stops
    stay,                 ///< the player stays on the menu
};

/// Applies Update or Cancel.
///
/// Update in place saves the one-shot entry, so the run the update starts
/// enters on its own. A release offered only on its page opens that page:
/// the player installs it by hand, and nothing is carried.
///
/// @param answer the button
/// @param in_place this copy can download and install the release itself
/// @param[in,out] carry the one-shot entry
/// @return what the entry does
inline UpdateEffect
apply_update_answer(UpdateAnswer answer, bool in_place, EntryCarry& carry) noexcept {
    if (answer != UpdateAnswer::update)
        return UpdateEffect::stay;
    if (!in_place)
        return UpdateEffect::open_page;
    carry.pending = true;
    return UpdateEffect::download_and_restart;
}

/// The answers to the failed check's question.
enum class RetryAnswer {
    try_again, ///< Try again
    cancel,    ///< Cancel
};

/// The effect of that answer.
enum class RetryEffect {
    check_again, ///< the update check runs again, and the entry waits for it
    stay,        ///< the player stays on the menu
};

/// Applies Try again or Cancel.
///
/// @param answer the button
/// @return what the entry does
[[nodiscard]] inline RetryEffect apply_retry_answer(RetryAnswer answer) noexcept {
    return answer == RetryAnswer::try_again ? RetryEffect::check_again : RetryEffect::stay;
}

/// Reports whether an entry asks a registry for its current list, and may
/// take the package from it.
///
/// Every entry does, a quiet one too (-y, a headless or unattended run that
/// plays, such as a test machine on a live server), so a newly published
/// package reaches it on its own. One of the game's own checks reaches only
/// the registries the player added, never a built-in one, so a check never
/// contacts a live server.
///
/// @param built_in the registry is one the game ships with
/// @param own_check the run is one of the game's own checks
/// @return true when the registry is asked and its package may be taken
[[nodiscard]] inline bool registry_reached(bool built_in, bool own_check) noexcept {
    return !(built_in && own_check);
}

/// What an expansion's record says about the package the registries list.
enum class CopyState {
    current,    ///< the record names that package
    missing,    ///< no complete copy is installed
    unrecorded, ///< a complete copy whose record names no package
    changed,    ///< the record names another package
};

/// What the entry does about the package.
enum class PackageAction {
    use_installed, ///< the copy installed is used as it is
    fetch,         ///< the package is downloaded and installed
    stop,          ///< nothing can be installed: the catalogue's message is shown
};

/// Decides whether the entry fetches the package.
///
/// `listed` counts only registries the entry reaches (registry_reached). A
/// copy that is not current is replaced from such a registry, whoever is
/// asking. With nothing listed, a copy installed is used, and a missing one
/// stops the entry.
///
/// @param copy what the record says
/// @param listed a registry the entry reaches lists the package
/// @return the action
[[nodiscard]] inline PackageAction decide_package_fetch(CopyState copy, bool listed) noexcept {
    if (copy == CopyState::current)
        return PackageAction::use_installed;
    if (listed)
        return PackageAction::fetch;
    return copy == CopyState::missing ? PackageAction::stop : PackageAction::use_installed;
}

/// The questions the entry asks before the package.
enum class EntryQuestion {
    services, ///< turn the online features on
    update,   ///< a newer Open Annihilation is out
    retry,    ///< the update check failed
};

/// One of those questions: its sentence and its only two buttons.
struct EntryQuestionLook {
    std::string word{};          ///< the question's word on the layer
    std::string paragraph{};     ///< the sentence
    std::string primary_label{}; ///< the first button, which goes on
    std::string primary_id{};    ///< its name
    std::string cancel_label{};  ///< the second button, which stays on the menu
    std::string cancel_id{};     ///< its name
};

/// Replaces the first `token` in a sentence.
///
/// @param pattern the sentence, already translated
/// @param token the token, such as "{version}"
/// @param value what replaces it
/// @return the sentence to show
[[nodiscard]] inline std::string
fill_entry_token(std::string_view pattern, std::string_view token, std::string_view value) {
    std::string text(pattern);
    const std::size_t at = text.find(token);
    if (at != std::string::npos)
        text.replace(at, token.size(), value);
    return text;
}

/// The sentence the online-features question shows.
///
/// The checks' sentence is unchanged. The other two replace "{registry}"
/// with the registry's name.
///
/// @param ask the sentences
/// @param wording which sentence
/// @param registry_name the registry's name
/// @return the sentence to show
[[nodiscard]] inline std::string services_sentence(
    const ExpansionEntryAsk& ask, ServicesWording wording, std::string_view registry_name
) {
    switch (wording) {
    case ServicesWording::checks_and_id:
        return fill_entry_token(ask.services_and_id_question, "{registry}", registry_name);
    case ServicesWording::install_id:
        return fill_entry_token(ask.install_id_question, "{registry}", registry_name);
    case ServicesWording::checks:
        break;
    }
    return ask.services_question;
}

/// Builds one of the questions the entry asks before the package.
///
/// Each has two buttons: the one that goes on (Turn on, Update, Try again)
/// and Cancel.
///
/// @param which the question
/// @param ask the sentences
/// @param version the offered release's version, for the update question
/// @return the question
[[nodiscard]] inline EntryQuestionLook
entry_question(EntryQuestion which, const ExpansionEntryAsk& ask, std::string_view version) {
    EntryQuestionLook look;
    look.cancel_label = ask.cancel_label;
    look.cancel_id = "cancel";
    switch (which) {
    case EntryQuestion::services:
        look.word = "expansion-services";
        look.paragraph = ask.services_question;
        look.primary_label = ask.turn_on_label;
        look.primary_id = "turn-on";
        break;
    case EntryQuestion::update:
        look.word = "expansion-update";
        look.paragraph = fill_entry_token(ask.question, "{version}", version);
        look.primary_label = ask.update_label;
        look.primary_id = "update";
        break;
    case EntryQuestion::retry:
        look.word = "expansion-retry";
        look.paragraph = ask.retry_question;
        look.primary_label = ask.retry_label;
        look.primary_id = "try-again";
        break;
    }
    return look;
}

} // namespace oa::app

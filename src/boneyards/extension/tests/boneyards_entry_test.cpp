// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Boneyards entry's decisions before the package, over a preferences
// map that stands in for the player's file and a carried request that
// stands in for the one the restart reads: the online features asked about,
// Turn on storing both checks and Cancel changing nothing; a needed install
// ID asked with those checks or on its own, Turn on clearing only that
// registry's off key; a newer Open Annihilation asked with Update and
// Cancel only, Update carrying the entry into the next run or, offered
// only on its page, opening it; a failed check asking Try again; a quiet
// run asked nothing, changing no setting; and the registries each kind of
// run reaches, with the package it then takes. Nothing is fetched.
#include "oa/app/content/settings.hpp"
#include "oa/app/expansion_entry.hpp"
#include "oa/test/check.hpp"

#include <cstdio>
#include <map>
#include <string>

namespace {

using namespace oa::app;

using Preferences = std::map<std::string, std::string>;

/// The sentences the Boneyards asks with, in English.
ExpansionEntryAsk boneyards_ask() {
    ExpansionEntryAsk ask;
    ask.key = "boneyards";
    ask.question = "Open Annihilation {version} is out. The Boneyards needs the latest version.";
    ask.update_label = "Update";
    ask.cancel_label = "Cancel";
    ask.ok_label = "OK";
    ask.services_question =
        "The Boneyards needs Open Annihilation's online features: update checks and the content "
        "catalogue. Turn them on and continue?";
    ask.services_and_id_question =
        "The Boneyards needs Open Annihilation's online features: update checks, the content "
        "catalogue and {registry}'s install ID. Turn them on and continue?";
    ask.install_id_question =
        "The Boneyards needs {registry}'s install ID turned on. Turn it on and continue?";
    ask.turn_on_label = "Turn on";
    ask.retry_question = "Open Annihilation could not check for updates.";
    ask.retry_label = "Try again";
    return ask;
}

void test_online_features_off() {
    // The welcome's Off turns both checks to never; either one off is asked.
    Preferences both_off{
        {std::string(kUpdateCheckKey), "never"},
        {std::string(kCatalogueCheckKey), "never"},
    };
    const OnlineChecks read = read_online_checks(both_off);
    OA_CHECK(!read.updates && !read.catalogue);
    OA_CHECK(decide_services(read, EntryWay::player) == ServicesDecision::ask);
    Preferences one_off{{std::string(kCatalogueCheckKey), "never"}};
    OA_CHECK(
        decide_services(read_online_checks(one_off), EntryWay::player) == ServicesDecision::ask
    );
    Preferences when_asked{{std::string(kUpdateCheckKey), "when-asked"}};
    OA_CHECK(
        decide_services(read_online_checks(when_asked), EntryWay::player) ==
        ServicesDecision::proceed
    );
    OA_CHECK(
        decide_services(read_online_checks(Preferences{}), EntryWay::player) ==
        ServicesDecision::proceed
    );

    const EntryQuestionLook look = entry_question(EntryQuestion::services, boneyards_ask(), {});
    OA_CHECK(look.paragraph == boneyards_ask().services_question);
    OA_CHECK(look.primary_label == "Turn on" && look.primary_id == "turn-on");
    OA_CHECK(look.cancel_label == "Cancel" && look.cancel_id == "cancel");

    // Cancel stays on the menu and changes nothing.
    OnlineChecks kept = read;
    OA_CHECK(apply_services_answer(ServicesAnswer::cancel, kept) == ServicesEffect::stay);
    OA_CHECK(!kept.updates && !kept.catalogue);

    // Turn on stores both keys on and goes on.
    OnlineChecks turned = read;
    OA_CHECK(
        apply_services_answer(ServicesAnswer::turn_on, turned) == ServicesEffect::save_and_go_on
    );
    OA_CHECK(turned.updates && turned.catalogue);
    Preferences saved = both_off;
    turn_on_online_checks(saved);
    OA_CHECK(saved[std::string(kUpdateCheckKey)] == "automatically");
    OA_CHECK(saved[std::string(kCatalogueCheckKey)] == "automatically");
    const OnlineChecks after = read_online_checks(saved);
    OA_CHECK(after.updates && after.catalogue);
    OA_CHECK(decide_services(after, EntryWay::player) == ServicesDecision::proceed);
    // A check already on keeps the player's word.
    Preferences mixed{
        {std::string(kUpdateCheckKey), "when-asked"},
        {std::string(kCatalogueCheckKey), "never"},
    };
    turn_on_online_checks(mixed);
    OA_CHECK(mixed[std::string(kUpdateCheckKey)] == "when-asked");
    OA_CHECK(mixed[std::string(kCatalogueCheckKey)] == "automatically");
}

/// One combination of the two checks and the picked registry's install ID.
///
/// @param updates_off the update check is Never
/// @param catalogue_off the catalogue check is Never
/// @param needed the picked registry requires an install ID
/// @param id_off that registry's install ID is off
/// @param quiet a quiet run, which logs the question and changes nothing
void expect_services(bool updates_off, bool catalogue_off, bool needed, bool id_off, bool quiet) {
    OnlineChecks checks;
    checks.updates = !updates_off;
    checks.catalogue = !catalogue_off;
    const InstallIdFeature feature{needed, id_off};
    const EntryWay way = quiet ? EntryWay::quiet : EntryWay::player;
    const NamedServices named = named_services(checks, feature, way);
    const bool checks_off = updates_off || catalogue_off;
    const bool id_in = needed && id_off;
    const ServicesDecision want = !checks_off && !id_in ? ServicesDecision::proceed
                                  : quiet               ? ServicesDecision::note
                                                        : ServicesDecision::ask;
    std::fprintf(
        stderr,
        "services updates_off=%d catalogue_off=%d needed=%d id_off=%d quiet=%d\n",
        updates_off ? 1 : 0,
        catalogue_off ? 1 : 0,
        needed ? 1 : 0,
        id_off ? 1 : 0,
        quiet ? 1 : 0
    );
    OA_CHECK(named.decision == want);
    OA_CHECK(decide_services(checks, way, feature) == want);
    if (want == ServicesDecision::proceed) {
        OA_CHECK(!named.names_updates && !named.names_catalogue && !named.names_install_id);
        return;
    }
    // Either check being off names both. The install ID is named only when
    // the picked registry requires one and that key is off.
    OA_CHECK(named.names_updates == checks_off);
    OA_CHECK(named.names_catalogue == checks_off);
    OA_CHECK(named.names_install_id == id_in);
    const ExpansionEntryAsk ask = boneyards_ask();
    const std::string sentence = services_sentence(ask, named.wording, "Core Prime");
    if (checks_off && id_in) {
        OA_CHECK(named.wording == ServicesWording::checks_and_id);
        OA_CHECK(sentence.find("update checks") != std::string::npos);
        OA_CHECK(sentence.find("content catalogue") != std::string::npos);
        OA_CHECK(sentence.find("Core Prime's install ID") != std::string::npos);
    } else if (id_in) {
        OA_CHECK(named.wording == ServicesWording::install_id);
        OA_CHECK(sentence.find("content catalogue") == std::string::npos);
        OA_CHECK(sentence.find("Core Prime's install ID") != std::string::npos);
    } else {
        OA_CHECK(named.wording == ServicesWording::checks);
        OA_CHECK(sentence == ask.services_question);
    }
}

void test_install_id_question() {
    for (const bool updates_off : {false, true}) {
        for (const bool catalogue_off : {false, true}) {
            for (const bool needed : {false, true}) {
                for (const bool id_off : {false, true}) {
                    expect_services(updates_off, catalogue_off, needed, id_off, false);
                    expect_services(updates_off, catalogue_off, needed, id_off, true);
                }
            }
        }
    }

    // Turn on clears only the registry the entry picked. Another registry's
    // off key stays. No ID is made here: the download makes one.
    const std::string picked = content::install_id_key("e01-local");
    const std::string other = content::install_id_key("coreprime");
    Preferences values{{picked, "off"}, {other, "off"}};
    const Preferences before = values;
    content::turn_install_id_on(values, "e01-local");
    OA_CHECK(values.find(picked) == values.end());
    OA_CHECK(values[other] == "off");

    // Cancel changes nothing, including an install ID that is off.
    Preferences cancelled = before;
    OnlineChecks kept;
    kept.updates = false;
    kept.catalogue = false;
    OA_CHECK(apply_services_answer(ServicesAnswer::cancel, kept) == ServicesEffect::stay);
    OA_CHECK(!kept.updates && !kept.catalogue);
    OA_CHECK(cancelled == before);
}

void test_newer_release() {
    OA_CHECK(decide_update(FeedAnswer::newer, EntryWay::player) == UpdateDecision::ask_update);
    OA_CHECK(decide_update(FeedAnswer::up_to_date, EntryWay::player) == UpdateDecision::proceed);
    // A player never goes on unanswered.
    OA_CHECK(decide_update(FeedAnswer::unanswered, EntryWay::player) == UpdateDecision::wait);

    // Update and Cancel, and no Not now: the ask's buttons are these two.
    const EntryQuestionLook look = entry_question(EntryQuestion::update, boneyards_ask(), "0.9.1");
    OA_CHECK(
        look.paragraph == "Open Annihilation 0.9.1 is out. The Boneyards needs the latest version."
    );
    OA_CHECK(look.primary_label == "Update" && look.primary_id == "update");
    OA_CHECK(look.cancel_label == "Cancel" && look.cancel_id == "cancel");

    // Update in place carries the entry into the run the update starts, once.
    EntryCarry carry;
    OA_CHECK(
        apply_update_answer(UpdateAnswer::update, true, carry) == UpdateEffect::download_and_restart
    );
    OA_CHECK(carry.pending);
    OA_CHECK(take_entry_request(carry));
    OA_CHECK(!take_entry_request(carry));

    // A release offered only on its page opens it, and nothing is carried.
    EntryCarry by_hand;
    OA_CHECK(apply_update_answer(UpdateAnswer::update, false, by_hand) == UpdateEffect::open_page);
    OA_CHECK(!by_hand.pending);

    // Cancel stays, in place or not, and carries nothing.
    EntryCarry stayed;
    OA_CHECK(apply_update_answer(UpdateAnswer::cancel, true, stayed) == UpdateEffect::stay);
    OA_CHECK(apply_update_answer(UpdateAnswer::cancel, false, stayed) == UpdateEffect::stay);
    OA_CHECK(!stayed.pending);
}

void test_feed_failure() {
    OA_CHECK(decide_update(FeedAnswer::failed, EntryWay::player) == UpdateDecision::ask_retry);
    const EntryQuestionLook look = entry_question(EntryQuestion::retry, boneyards_ask(), {});
    OA_CHECK(look.paragraph == "Open Annihilation could not check for updates.");
    OA_CHECK(look.primary_label == "Try again" && look.primary_id == "try-again");
    OA_CHECK(look.cancel_label == "Cancel" && look.cancel_id == "cancel");
    OA_CHECK(apply_retry_answer(RetryAnswer::try_again) == RetryEffect::check_again);
    OA_CHECK(apply_retry_answer(RetryAnswer::cancel) == RetryEffect::stay);
}

void test_quiet_runs() {
    // A quiet run is never asked: it notes the online features and the
    // update, goes on as it always has, and stores nothing.
    Preferences both_off{
        {std::string(kUpdateCheckKey), "never"},
        {std::string(kCatalogueCheckKey), "never"},
    };
    const Preferences before = both_off;
    OA_CHECK(
        decide_services(read_online_checks(both_off), EntryWay::quiet) == ServicesDecision::note
    );
    OA_CHECK(both_off == before);
    OA_CHECK(
        decide_services(read_online_checks(Preferences{}), EntryWay::quiet) ==
        ServicesDecision::proceed
    );
    OA_CHECK(decide_update(FeedAnswer::newer, EntryWay::quiet) == UpdateDecision::note);
    OA_CHECK(decide_update(FeedAnswer::failed, EntryWay::quiet) == UpdateDecision::proceed);
    OA_CHECK(decide_update(FeedAnswer::unanswered, EntryWay::quiet) == UpdateDecision::proceed);
    OA_CHECK(decide_update(FeedAnswer::up_to_date, EntryWay::quiet) == UpdateDecision::proceed);
    for (const FeedAnswer answer :
         {FeedAnswer::up_to_date, FeedAnswer::newer, FeedAnswer::failed, FeedAnswer::unanswered}) {
        const UpdateDecision decision = decide_update(answer, EntryWay::quiet);
        OA_CHECK(decision != UpdateDecision::ask_update && decision != UpdateDecision::ask_retry);
        OA_CHECK(decision != UpdateDecision::wait);
    }
    OA_CHECK(
        fill_entry_token("Open Annihilation {version} is out.", "{version}", "99.0.0") ==
        "Open Annihilation 99.0.0 is out."
    );
}

/// One kind of run, as the entry is asked by it.
struct RunKind {
    const char* name{};
    bool own_check{}; ///< one of the game's own checks
    EntryWay way{EntryWay::player};
};

/// What the entry does when the only registry that lists the package is
/// built in, or added, as consider_package and package_listed decide it.
PackageAction action_for(const RunKind& run, bool built_in, CopyState copy) {
    return decide_package_fetch(copy, registry_reached(built_in, run.own_check));
}

void test_registry_reach() {
    const RunKind runs[] = {
        {"a check that faces a player (the shortcut and mod-entry checks)", true, EntryWay::player},
        {"a headless check", true, EntryWay::quiet},
        {"-y", false, EntryWay::quiet},
        {"a player's entry", false, EntryWay::player},
        {"a headless or unattended run that plays", false, EntryWay::quiet},
    };
    for (const RunKind& run : runs) {
        std::fprintf(stderr, "registries for %s\n", run.name);
        // A registry the player added is asked by every run, and its package
        // replaces a copy that is not current.
        OA_CHECK(registry_reached(false, run.own_check));
        OA_CHECK(action_for(run, false, CopyState::current) == PackageAction::use_installed);
        OA_CHECK(action_for(run, false, CopyState::missing) == PackageAction::fetch);
        OA_CHECK(action_for(run, false, CopyState::unrecorded) == PackageAction::fetch);
        OA_CHECK(action_for(run, false, CopyState::changed) == PackageAction::fetch);
        if (run.own_check) {
            // The game's own checks never ask a built-in registry and never
            // take its package, whatever the copy's state: a missing copy
            // stops with the catalogue's message, and a copy installed is used.
            OA_CHECK(!registry_reached(true, run.own_check));
            OA_CHECK(action_for(run, true, CopyState::missing) == PackageAction::stop);
            OA_CHECK(action_for(run, true, CopyState::unrecorded) == PackageAction::use_installed);
            OA_CHECK(action_for(run, true, CopyState::changed) == PackageAction::use_installed);
            OA_CHECK(action_for(run, true, CopyState::current) == PackageAction::use_installed);
        } else {
            // Every other run, a quiet one too, takes a newly published
            // package from the built-in registry on its own.
            OA_CHECK(registry_reached(true, run.own_check));
            OA_CHECK(action_for(run, true, CopyState::missing) == PackageAction::fetch);
            OA_CHECK(action_for(run, true, CopyState::unrecorded) == PackageAction::fetch);
            OA_CHECK(action_for(run, true, CopyState::changed) == PackageAction::fetch);
            OA_CHECK(action_for(run, true, CopyState::current) == PackageAction::use_installed);
        }
    }
    // With nothing listed, a copy installed is used and a missing one stops.
    OA_CHECK(decide_package_fetch(CopyState::missing, false) == PackageAction::stop);
    OA_CHECK(decide_package_fetch(CopyState::changed, false) == PackageAction::use_installed);
}

} // namespace

int main() {
    test_online_features_off();
    test_install_id_question();
    test_newer_release();
    test_feed_failure();
    test_quiet_runs();
    test_registry_reach();
    return oa::test::check_exit_status();
}

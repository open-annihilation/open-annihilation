// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// ENTER, the B key and "-y" ask the runtime's entry, then open the Boneyards
// when it is ready. The two entry checks start that once the main menu's
// strip is up. ENTER waits until the menu has settled and a check has
// answered, including a start that lands before the first frame. "-y" does
// not wait for a check that has not answered: a result already in hand is
// still taken, and a missing expansion is still fetched. The strip's pump
// asks again when the one pass ran before the strip could take it. A menu
// picture is written once, while the strip is showing and the Boneyards are
// not open.
#include "boneyards_launch.hpp"
#include "boneyards_options.hpp"

#include "oa/app/app.hpp"
#include "oa/app/check_host.hpp"
#include "oa/app/expansion_entry.hpp"
#include "oa/app/extension.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/shown_text.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/platform/files.hpp"
#include "oa/platform/system.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>

namespace oa::app {
namespace {

namespace console = oa::boneyards::console;

/// The update question, in English. The catalogue's section has this name.
constexpr std::string_view k_question =
    "A newer Open Annihilation is out. Update before entering the Boneyards?";

/// The fetch line, in English. "{percent}" is filled in as the download moves.
constexpr std::string_view k_progress = "Fetching the Boneyards… {percent}%";

/// The package the entry asks for.
constexpr std::string_view k_package = "boneyards";

/// The line the player reads when both checks are off. The update screen
/// shows it, and so does the menu when that screen cannot open.
constexpr const char* k_services_off =
    "Online services are off. Turn them on from the Settings page.";

/// The runtime the entry was installed on. Startup sets it before any click.
Runtime* g_runtime = nullptr;

/// One picture has been written for this run.
bool g_snapshot_written = false;

/// Pumps since the run started, counting the one now running.
int g_pump = 0;

/// The pump that first printed the update question. The update check declines
/// on a later pump.
int g_ask_pump = 0;

/// The entry check has started its one request.
bool g_started = false;

/// This entry drives the real update screen. A check or a headless run does not.
bool g_real = false;

/// "-y" or a click has begun an entry. The pump asks once until this is set,
/// so a pass that ran before the strip could take it is tried again.
bool g_entry_begun = false;

/// The update check has declined.
bool g_declined = false;

/// Each line is printed once for a request.
bool g_printed_ask = false;
bool g_printed_open = false;
bool g_printed_stayed = false;
bool g_printed_failed = false;

/// The Boneyards catalogue beside the program, or an empty one.
///
/// A missing file leaves every line in English. The path is not logged.
///
/// @return the catalogue; it lives for the process
const oa::data::languages::InterfaceText& entry_words() {
    static const oa::data::languages::InterfaceText catalogue = [] {
        oa::data::languages::InterfaceText loaded;
        const std::filesystem::path file =
            oa::platform::path_from_utf8(oa::platform::program_directory()) / "languages" /
            "boneyards.tdf";
        std::ifstream input(file, std::ios::binary);
        if (!input)
            return loaded;
        const std::string bytes(
            (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()
        );
        loaded.add(bytes, nullptr);
        return loaded;
    }();
    return catalogue;
}

/// The sentences and button captions for one request.
///
/// @return the ask; the package id is the Boneyards expansion
ExpansionEntryAsk make_ask() {
    // Each shown word is copied before the next call: the pointer is reused.
    const char* update_word = oa::boneyards::shown_text("Update");
    const std::string update_label(update_word != nullptr ? update_word : "");
    const char* later_word = oa::boneyards::shown_text("Not now");
    const std::string later_label(later_word != nullptr ? later_word : "");
    const char* ok_word = oa::boneyards::shown_text("OK");
    const std::string ok_label(ok_word != nullptr ? ok_word : "");
    const oa::data::languages::InterfaceText& words = entry_words();
    const oa::data::languages::Language& language = expansion_entry_language(*g_runtime);
    ExpansionEntryAsk ask;
    ask.key = std::string(k_package);
    ask.question = std::string(words.text(k_question, language));
    ask.progress = std::string(words.text(k_progress, language));
    ask.update_label = update_label;
    ask.later_label = later_label;
    ask.ok_label = ok_label;
    // ENTER leaves this clear and still waits, and that wait ends on its own.
    // "-y" opens once a result already in hand has been taken, without a
    // check that has not answered. The services and game-files readings still run.
    ask.skip_unanswered_check = boneyards_options().setup_requested;
    return ask;
}

/// A headless run, an unattended run, or a Boneyards check keeps today's entry.
///
/// "-y" is a player's entry: it runs the checks and is not one of these.
///
/// @return true when the real update screen stays off
bool quiet_run() {
    const Options& options = runtime_options(*g_runtime);
    if (options.headless_check || options.unattended)
        return true;
    const BoneyardsOptions& taken = boneyards_options();
    return taken.boneyards_entry != BoneyardsEntryCheck::none || taken.check_boneyards_login ||
           taken.enter_boneyards_ticks || taken.boneyards_launch || taken.boneyards_features ||
           taken.check_boneyards_unit_limit || taken.boneyards_overlay ||
           taken.check_boneyards_shortcut;
}

/// The screen's facts, copied from the checks the entry says it ran.
///
/// @param status where the entry stands
/// @return the facts
console::UpdateCheckFacts facts_of(const ExpansionEntryStatus& status) {
    console::UpdateCheckFacts facts;
    facts.services_checked = status.services_known;
    facts.services_on = status.services_on;
    facts.files_checked = status.files_known;
    facts.mismatched_file = status.mismatched_file;
    facts.matched_files = status.matched_files;
    facts.feed_checked = status.feed_known;
    facts.feed_failed = status.feed_failed;
    facts.update_offered = status.update_offered;
    facts.offered_version = status.offered_version;
    facts.update_declined = status.update_declined;
    facts.extension_checked = status.extension_known;
    facts.extension_percent = status.extension_percent;
    facts.extension_installed_now = status.extension_installed;
    facts.extension_current = status.extension_current;
    facts.extension_version = status.extension_version;
    facts.entry_failed = status.step == ExpansionEntryStep::failed && !status.stopped;
    if (facts.entry_failed)
        facts.failure = status.notice;
    facts.entry_ready = status.step == ExpansionEntryStep::ready;
    return facts;
}

/// Copies the picture onto the screen, and opens it when a failure or a fetch must show.
///
/// The update question stays on the menu, so an ask does not enter. When the
/// window cannot open, the fetch goes back to the menu's question.
///
/// @param runtime the running game
/// @param widget the main-menu strip; null does nothing
/// @param status where the entry stands
void show_real(
    Runtime& runtime, console::StatusWidget* widget, const ExpansionEntryStatus& status
) {
    if (!g_real || widget == nullptr)
        return;
    const console::UpdateCheckPicture picture = console::boneyards_update_picture(facts_of(status));
    console::boneyards_status_widget_set_real_update(widget, picture);
    const bool inside = console::boneyards_status_widget_in_boneyards(widget);
    const bool show_failure = status.step == ExpansionEntryStep::failed;
    const bool show_fetch =
        status.step == ExpansionEntryStep::waiting && status.extension_percent >= 0;
    if (inside || (!show_failure && !show_fetch))
        return;
    console::boneyards_status_widget_enter(widget);
    if (console::boneyards_status_widget_in_boneyards(widget))
        return;
    expansion_entry_show_on_screen(runtime, false);
    if (show_failure && !status.notice.empty()) {
        const char* text =
            status.stopped && !status.services_on ? k_services_off : status.notice.c_str();
        console::boneyards_status_widget_show_message(widget, text);
    }
}

/// Prints where the entry stands, and opens the Boneyards when it is ready.
///
/// @param runtime the running game
/// @param widget the main-menu strip; null does not open
void drive(Runtime& runtime, console::StatusWidget* widget) {
    const ExpansionEntryStatus status = advance_expansion_entry(runtime);
    show_real(runtime, widget, status);
    if (status.step == ExpansionEntryStep::asking && !g_printed_ask) {
        g_printed_ask = true;
        g_ask_pump = g_pump;
        std::cout << "entry flow: asking to update\n" << status.notice << '\n';
        std::cout.flush();
        return;
    }
    if (status.step == ExpansionEntryStep::stayed && !g_printed_stayed) {
        g_printed_stayed = true;
        std::cout << "entry flow: stayed\n";
        std::cout.flush();
        return;
    }
    if (status.step == ExpansionEntryStep::failed && !g_printed_failed) {
        g_printed_failed = true;
        std::cout << "entry flow: failed: " << status.notice << '\n';
        std::cout.flush();
        return;
    }
    if (status.step == ExpansionEntryStep::ready && !g_printed_open) {
        g_printed_open = true;
        if (widget != nullptr)
            console::boneyards_status_widget_enter(widget);
        std::cout << "entry flow: boneyards open\n";
        std::cout.flush();
    }
}

/// ENTER, the B key and "-y". A click during a check or a download is ignored,
/// and so is one after the Boneyards are already open.
///
/// @param widget the strip that was clicked
void on_request(console::StatusWidget* widget) {
    if (widget == nullptr || g_runtime == nullptr)
        return;
    if (console::boneyards_status_widget_in_boneyards(widget))
        return;
    if (expansion_entry_busy(*g_runtime))
        return;
    g_printed_ask = false;
    g_printed_open = false;
    g_printed_stayed = false;
    g_printed_failed = false;
    g_declined = false;
    g_entry_begun = true;
    g_real = !quiet_run();
    ExpansionEntryAsk ask = make_ask();
    ask.report_on_screen = g_real;
    if (g_real)
        console::boneyards_status_widget_begin_real_update(widget);
    begin_expansion_entry(*g_runtime, ask);
    drive(*g_runtime, widget);
}

/// The entry check's part of one pump: start once, decline the update question
/// on the next pump, and keep a download moving.
///
/// @param runtime the running game
void on_pump(Runtime& runtime) {
    ++g_pump;
    console::StatusWidget* widget = boneyards_console_status_widget();
    const bool visible = widget != nullptr && console::boneyards_status_widget_visible(widget) &&
                         !console::boneyards_status_widget_in_boneyards(widget);
    const BoneyardsEntryCheck mode = boneyards_options().boneyards_entry;
    if (mode != BoneyardsEntryCheck::none && !g_started && visible) {
        g_started = true;
        on_request(widget);
        return;
    }
    // "-y" asks once the strip is up, and again when the one pass ran before
    // the strip could take it. ENTER still waits for a click.
    if (!g_entry_begun && visible && boneyards_options().setup_requested) {
        on_request(widget);
        return;
    }
    // "-y" has already said to go on, so an update question is not a stop.
    if ((mode == BoneyardsEntryCheck::update || boneyards_options().setup_requested) &&
        g_printed_ask && !g_declined && g_pump > g_ask_pump) {
        g_declined = true;
        answer_expansion_entry(runtime, false);
        drive(runtime, widget);
        return;
    }
    if (expansion_entry_busy(runtime))
        drive(runtime, widget);
}

/// Writes the main menu once, while the strip is up and the Boneyards are not.
///
/// Checks that enter, and a headless run, keep the picture they write themselves.
/// A path is not printed.
///
/// @param runtime the running game
void write_menu_snapshot(Runtime& runtime) {
    if (g_snapshot_written)
        return;
    const Options& options = runtime_options(runtime);
    if (options.snapshot.empty() || options.headless_check)
        return;
    const BoneyardsOptions& taken = boneyards_options();
    if (taken.boneyards_entry != BoneyardsEntryCheck::none || taken.check_boneyards_login ||
        taken.enter_boneyards_ticks || taken.boneyards_launch || taken.boneyards_features ||
        taken.check_boneyards_unit_limit || taken.boneyards_overlay || taken.setup_requested ||
        taken.check_boneyards_shortcut)
        return;
    console::StatusWidget* widget = boneyards_console_status_widget();
    if (!console::boneyards_status_widget_visible(widget) ||
        console::boneyards_status_widget_in_boneyards(widget))
        return;
    const CheckHost host = check_host(runtime);
    if (host.surface == nullptr)
        return;
    const auto* surface = host.surface(host.context);
    if (surface == nullptr)
        return;
    try {
        write_ppm(options.snapshot, *surface);
        g_snapshot_written = true;
    } catch (const std::exception&) {
        std::cerr << "open-annihilation: the menu picture could not be written\n";
        g_snapshot_written = true;
    }
}

} // namespace

/// The flow ENTER copies onto a strip created after this (console_screens.cpp).
void boneyards_console_set_entry_flow(void (*flow)(oa::boneyards::console::StatusWidget*));

void boneyards_entry_install(Runtime& runtime) {
    g_runtime = &runtime;
    boneyards_console_set_entry_flow(&on_request);
}

void boneyards_entry_on_frame(Runtime& runtime, FrameStage stage) {
    if (stage == FrameStage::presented)
        write_menu_snapshot(runtime);
    else if (stage == FrameStage::pump)
        on_pump(runtime);
}

} // namespace oa::app

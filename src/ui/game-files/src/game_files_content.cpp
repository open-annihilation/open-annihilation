// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the Game files screen says (game_files_internal.hpp): the parts'
// names and notes, and each problem's, sheet's and banner's text and
// buttons, in the wording of the screen's design, with the platform's words
// or the engine's neutral ones.
#include "game_files_internal.hpp"

namespace oa::ui::game_files::detail {

using oa::ui::kit::Role;

namespace {

/// Returns the source's display location, or a neutral phrase when the app gave none.
///
/// @param model the model
/// @return the location
std::string location_of(const Model& model) {
    if (!model.location.empty())
        return model.location;
    return tr("the folder you chose");
}

/// Returns the file a problem names, or a neutral phrase.
///
/// @param model the model
/// @return the file's name
std::string file_of(const Model& model) {
    if (!model.file.empty())
        return model.file;
    return tr("a file");
}

/// Makes a button that asks the app for a command.
///
/// @param label the upper-case label, as the source writes it
/// @param control the control
/// @param command what it asks
/// @param role main, plain or danger
/// @param glyph the mark beside the label
/// @return the button
Action action(
    std::string_view label,
    Control control,
    Command command,
    Role role = Role::button,
    Glyph glyph = Glyph::none
) {
    Action made{};
    made.label = tr(label);
    made.control = control;
    made.command = command;
    made.role = role;
    made.glyph = glyph;
    return made;
}

/// Makes S6's numbered button.
///
/// @param label the label
/// @param index its place in the problem's list, left to right
/// @param command what it asks
/// @param role main, plain or danger
/// @return the button
Action problem_action(std::string_view label, uint16_t index, Command command, Role role) {
    return action(label, {ControlKind::problem_action, index}, command, role);
}

/// Makes a sheet's button (index = its place, top to bottom).
///
/// @param label the label
/// @param index its place
/// @param command what it asks; none: it only closes the sheet
/// @param role main, plain or danger
/// @param glyph the mark beside the label
/// @return the button
Action sheet_action(
    std::string_view label,
    uint16_t index,
    Command command,
    Role role = Role::button,
    Glyph glyph = Glyph::none
) {
    return action(label, {ControlKind::sheet_option, index}, command, role, glyph);
}

/// Returns the row a sheet is about, or null when the index is out of range.
///
/// @param model the model
/// @return the row
const PartRow* sheet_row(const Model& model) noexcept {
    if (model.sheet_part < model.parts.size())
        return &model.parts[model.sheet_part];
    return nullptr;
}

} // namespace

std::string part_kind_name(PartKind kind) {
    switch (kind) {
    case PartKind::game_archives:
        return tr("Game archives");
    case PartKind::update_31c:
        return tr("3.1c update");
    case PartKind::core_contingency:
        return tr("Core Contingency");
    case PartKind::battle_tactics:
        return tr("Battle Tactics");
    case PartKind::extra:
        return tr("Extra units and maps");
    case PartKind::music:
        return tr("Music");
    case PartKind::movies:
        return tr("Movies");
    case PartKind::mod:
        return tr("Mod");
    case PartKind::left_out:
        return tr("Left out");
    case PartKind::demo:
        return tr("The Total Annihilation demo (1997)");
    case PartKind::demo_data:
        return tr("The demo's unpacked data");
    }
    return {};
}

std::string part_name(const PartRow& row, bool management) {
    if (row.kind == PartKind::mod && !row.name.empty())
        return management ? fill("Mod: {name}", {{"name", row.name}}) : row.name;
    return part_kind_name(row.kind);
}

bool part_missing(const PartRow& row) noexcept {
    return row.mark == Mark::missing;
}

bool part_refused(const PartRow& row) noexcept {
    return row.mark == Mark::refused || (row.kind == PartKind::mod && !row.errors.empty());
}

std::string part_detail(const Model& model, const PartRow& row) {
    const bool choosing = model.step == Step::ready_to_copy;
    if (part_missing(row))
        return row.kind == PartKind::update_31c ? tr("Not found") : tr("not found");
    switch (row.kind) {
    case PartKind::game_archives:
    case PartKind::update_31c:
    case PartKind::core_contingency:
    case PartKind::battle_tactics:
        return row.files;
    case PartKind::extra:
        if (!row.files.empty())
            return row.files;
        if (row.count == 1)
            return tr("1 file");
        if (row.count > 1)
            return fill("{n} files", {{"n", std::to_string(row.count)}});
        return {};
    case PartKind::music: {
        std::string tracks = row.files;
        if (row.count == 1)
            tracks = tr("1 track");
        else if (row.count > 1)
            tracks = fill("{n} tracks", {{"n", std::to_string(row.count)}});
        if (!choosing)
            return tracks;
        if (tracks.empty())
            return tr("optional");
        return fill("{tracks} · optional", {{"tracks", tracks}});
    }
    case PartKind::movies:
        return choosing ? tr("intro and endings · optional") : tr("intro and endings");
    case PartKind::mod:
        if (part_refused(row))
            return tr("its mod profile cannot be used");
        return row.files;
    case PartKind::left_out:
        if (choosing && row.count == 1)
            return fill("{kinds} · 1 file", {{"kinds", left_out_kinds_text()}});
        if (choosing && row.count > 1)
            return fill(
                "{kinds} · {n} files",
                {{"kinds", left_out_kinds_text()}, {"n", std::to_string(row.count)}}
            );
        return left_out_kinds_text();
    case PartKind::demo:
        return tr("installer recognised by its size; checked when it is unpacked");
    case PartKind::demo_data:
        return tr("no longer used now that the full game is here");
    }
    return {};
}

std::string part_hint(const Model& model, const PartRow& row, bool short_form) {
    if (model.step != Step::ready_to_copy)
        return {};
    if (row.kind == PartKind::update_31c && part_missing(row))
        return short_form ? tr("Recommended")
                          : tr("Recommended: shared games with players who have it may not "
                               "match.");
    if ((row.kind == PartKind::core_contingency || row.kind == PartKind::battle_tactics) &&
        !part_missing(row))
        return short_form ? std::string{} : tr("Shared games need the same files on every machine");
    return {};
}

std::string part_size(const PartRow& row) {
    if (part_missing(row))
        return {};
    if (!row.bytes_known)
        return fill("at least {size}", {{"size", size_text(row.bytes)}});
    return size_text(row.bytes);
}

std::string left_out_reason_text(uint8_t reason) {
    switch (reason) {
    case 0:
        return tr("Windows program or shortcut");
    case 1:
        return tr("icon, manual or help file");
    case 2:
        return tr("uninstaller");
    case 3:
        return tr("system file");
    case 4:
        return tr("symbolic link, never followed");
    case 5:
        return tr("a name the game cannot use");
    case 6:
        return tr("differs only in capital letters");
    default:
        return tr("left out");
    }
}

std::string left_out_kinds_text() {
    return tr("Windows programs, shortcuts, icons, manuals and help files");
}

ProblemContent problem_content(const Model& model) {
    ProblemContent content{};
    const std::string device = platform_word(model, word_device);
    const Action back = action("Back", {ControlKind::back}, Command::back);
    const auto choose_folder = [](std::string_view label, Role role) {
        return action(label, {ControlKind::choose_folder}, Command::pick_game_folder, role);
    };
    switch (model.problem) {
    case Problem::not_a_game:
        content.title = tr("This folder does not hold a Total Annihilation installation");
        content.glyph = Glyph::folder;
        content.paragraphs.push_back(
            !model.detail.empty()
                ? model.detail
                : tr("It holds no Total Annihilation archives (.hpi, .ufo, .ccx or rev31.gp3 "
                     "files) and no installer of the Total Annihilation demo (1997).")
        );
        content.paragraphs.push_back(tr("Choose the folder that holds totala1.hpi."));
        content.actions = {choose_folder("Choose another folder", Role::button_main), back};
        break;
    case Problem::cannot_play:
        content.title = tr("These files cannot be played");
        content.glyph = Glyph::cross;
        content.colour = red_colour;
        content.paragraphs.push_back(
            model.detail.empty() ? tr("The folder may be missing totala1.hpi.")
                                 : fill(
                                       "{problem} The folder may be missing totala1.hpi.",
                                       {{"problem", model.detail}}
                                   )
        );
        content.actions = {
            problem_action(
                "Add the missing files", 0, Command::pick_game_folder, Role::button_main
            ),
            choose_folder("Choose another folder", Role::button),
            problem_action("Discard", 2, Command::discard, Role::button_danger),
        };
        break;
    case Problem::not_demo_installer:
        content.title = tr("This is not the demo's installer");
        content.glyph = Glyph::file;
        content.paragraphs.push_back(fill(
            "{file} is not the release of the Total Annihilation demo (1997) that Open "
            "Annihilation recognises. Installers of the full game are Windows programs, which "
            "Open Annihilation never runs: install the game on a computer, then copy its folder.",
            {{"file", model.file.empty() ? tr("This file") : model.file}}
        ));
        content.actions = {
            action(
                "Choose another file",
                {ControlKind::choose_installer},
                Command::pick_installer,
                Role::button_main
            ),
            back,
        };
        break;
    case Problem::short_space:
        content.title = fill("Not enough space on this {device}", {{"device", device}});
        content.glyph = Glyph::warning;
        content.paragraphs.push_back(fill(
            "These files need {need} and this {device} has {free} free. {advice}, or turn off a "
            "part above, then tap Check again.",
            {{"need", size_text(model.need_bytes)},
             {"device", device},
             {"free", size_text(model.free_bytes)},
             {"advice", platform_word(model, word_free_space_advice)}}
        ));
        content.actions = {
            problem_action("Check again", 0, Command::recheck_space, Role::button_main),
            problem_action("Change what is copied", 1, Command::copy_anyway, Role::button),
        };
        break;
    case Problem::disk_full:
        content.title = fill("This {device} ran out of space", {{"device", device}});
        content.glyph = Glyph::warning;
        content.paragraphs.push_back(fill(
            "The copy stopped after {size}. What was copied is kept: free some space, then tap "
            "Continue.",
            {{"size", size_text(model.stopped_bytes)}}
        ));
        content.actions = {
            problem_action("Continue", 0, Command::continue_copy, Role::button_main),
            problem_action("Discard", 1, Command::discard, Role::button_danger),
        };
        break;
    case Problem::source_unreadable:
        content.title = tr("The copy stopped");
        content.glyph = Glyph::disk;
        content.paragraphs.push_back(
            model.detail.empty()
                ? fill(
                      "Open Annihilation could not read {file}. Connect it again and tap "
                      "Continue. What was copied is kept.",
                      {{"file", file_of(model)}}
                  )
                : fill(
                      "Open Annihilation could not read {file}: {why}. Connect it again and tap "
                      "Continue. What was copied is kept.",
                      {{"file", file_of(model)}, {"why", model.detail}}
                  )
        );
        content.actions = {
            problem_action("Continue", 0, Command::continue_copy, Role::button_main),
            problem_action("Discard", 1, Command::discard, Role::button_danger),
        };
        break;
    case Problem::download_failed:
        content.title = tr("The copy stopped");
        content.glyph = Glyph::warning;
        content.paragraphs.push_back(
            model.detail.empty()
                ? fill(
                      "{file} is in {cloud} and could not be downloaded. Connect to the internet "
                      "and tap Continue.",
                      {{"file", file_of(model)}, {"cloud", platform_word(model, word_cloud_name)}}
                  )
                : fill(
                      "{file} is in {cloud} and could not be downloaded: {why}. Connect to the "
                      "internet and tap Continue.",
                      {{"file", file_of(model)},
                       {"cloud", platform_word(model, word_cloud_name)},
                       {"why", model.detail}}
                  )
        );
        content.actions = {
            problem_action("Continue", 0, Command::continue_copy, Role::button_main),
            problem_action("Discard", 1, Command::discard, Role::button_danger),
        };
        break;
    case Problem::access_withdrawn:
        content.title = tr("The folder can no longer be read");
        content.glyph = Glyph::folder;
        content.paragraphs.push_back(fill(
            "Open Annihilation may no longer read {location}. Choose the folder again to go on; "
            "what was copied is kept.",
            {{"location", location_of(model)}}
        ));
        content.actions = {
            choose_folder("Choose the folder again", Role::button_main),
            problem_action("Discard", 1, Command::discard, Role::button_danger),
        };
        break;
    case Problem::source_changed:
        content.title = tr("The folder has changed");
        content.glyph = Glyph::refresh;
        content.paragraphs.push_back(fill(
            "Some files in {location} changed since the copy began. Files that match are kept; "
            "the rest are copied again.",
            {{"location", location_of(model)}}
        ));
        content.actions = {
            problem_action("Continue", 0, Command::continue_copy, Role::button_main),
            problem_action("Start again", 1, Command::start_again, Role::button),
        };
        break;
    case Problem::too_large:
        content.title = tr("Is this the right folder?");
        content.glyph = Glyph::warning;
        content.paragraphs.push_back(fill(
            "This folder holds {size}, far more than a Total Annihilation installation (about "
            "1.1 GB with everything).",
            {{"size", size_text(model.copy_bytes)}}
        ));
        content.actions = {
            problem_action("Copy anyway", 0, Command::copy_anyway, Role::button),
            choose_folder("Choose another folder", Role::button_main),
        };
        break;
    case Problem::no_game_folder_yet:
        content.title = tr("No game folder yet");
        content.glyph = Glyph::folder;
        content.paragraphs.push_back(fill(
            "There is no Total Annihilation folder on this {device} yet. {steps} Name it Total "
            "Annihilation. Wait until the copy has finished, then tap Check again.",
            {{"device", device}, {"steps", platform_word(model, word_copy_yourself_steps)}}
        ));
        content.actions = {
            problem_action("Check again", 0, Command::check_copied, Role::button_main),
            back,
        };
        break;
    case Problem::found_misnamed:
        content.title = tr("Found your files");
        content.glyph = Glyph::folder;
        content.colour = green_colour;
        content.paragraphs.push_back(fill(
            "Found your files in {folder}. Use that folder?",
            {{"folder", model.file.empty() ? location_of(model) : model.file}}
        ));
        content.actions = {
            problem_action("Use it", 0, Command::adopt, Role::button_main),
            back,
        };
        break;
    case Problem::found_loose:
        content.title = tr("Found game archives");
        content.glyph = Glyph::file;
        content.colour = green_colour;
        content.paragraphs.push_back(
            tr("Archives are loose in Open Annihilation's folder. Gather them into Total "
               "Annihilation?")
        );
        content.actions = {
            problem_action("Gather them", 0, Command::adopt, Role::button_main),
            back,
        };
        break;
    case Problem::picker_failed:
        content.title = tr("The file picker did not open");
        content.glyph = Glyph::warning;
        content.paragraphs.push_back(
            model.detail.empty() ? tr("The system's file picker could not be opened.")
                                 : fill(
                                       "The system's file picker could not be opened: {error}.",
                                       {{"error", model.detail}}
                                   )
        );
        content.actions = {
            problem_action("Try again", 0, Command::retry, Role::button_main),
            back,
        };
        break;
    case Problem::staging_unwritable:
        content.title = tr("The copy could not start");
        content.glyph = Glyph::warning;
        content.colour = red_colour;
        content.paragraphs.push_back(
            model.detail.empty() ? tr("Open Annihilation could not write its own folder.")
                                 : fill(
                                       "Open Annihilation could not write its own folder: {error}.",
                                       {{"error", model.detail}}
                                   )
        );
        content.actions = {
            problem_action("Try again", 0, Command::retry, Role::button_main),
            back,
        };
        break;
    }
    return content;
}

SheetContent sheet_content(const Model& model) {
    SheetContent content{};
    const std::string device = platform_word(model, word_device);
    switch (model.sheet) {
    case Sheet::none:
        break;
    case Sheet::stop:
        content.title = tr("Stop copying?");
        content.paragraphs.push_back(fill(
            "{done} of {total} is copied. Keep it, and choosing the same folder later continues "
            "from here.",
            {{"done", size_text(model.progress.done_bytes, true)},
             {"total", size_text(model.progress.total_bytes, true)}}
        ));
        content.actions = {
            sheet_action("Keep what was copied", 0, Command::stop_keep, Role::button_main),
            sheet_action("Discard what was copied", 1, Command::stop_discard, Role::button_danger),
            sheet_action("Keep copying", 2, Command::none),
        };
        break;
    case Sheet::replace_confirm:
        content.title = tr("Replace the game files?");
        content.paragraphs.push_back(fill(
            "This replaces the game files on this {device} ({size}) with the folder you chose. "
            "The current folder is kept as Total Annihilation (old) until you remove it. Saved "
            "games and settings stay.",
            {{"device", device}, {"size", size_text(model.replace_bytes)}}
        ));
        content.actions = {
            sheet_action("Replace game files", 0, Command::start_copy, Role::button_main),
            sheet_action("Cancel", 1, Command::none),
        };
        break;
    case Sheet::left_out_list:
        content.title = tr("Left out");
        content.paragraphs.push_back(fill(
            "{kinds} ({size})",
            {{"kinds", left_out_kinds_text()}, {"size", size_text(model.left_out_bytes)}}
        ));
        for (const LeftOutRow& file : model.left_out)
            content.rows.push_back(
                {file.path, left_out_reason_text(file.reason), size_text(file.bytes)}
            );
        if (model.left_out.empty())
            content.paragraphs.push_back(tr("Nothing in this folder is left out."));
        content.notes = model.warnings;
        content.actions = {sheet_action("OK", 0, Command::none, Role::button_main)};
        break;
    case Sheet::mod_errors: {
        content.title = tr("This mod cannot be used");
        if (const PartRow* row = sheet_row(model); row != nullptr) {
            if (!row->name.empty())
                content.paragraphs.push_back(row->name);
            content.paragraphs.push_back(tr("Its mod profile cannot be used:"));
            content.lines = row->errors;
        } else {
            content.paragraphs.push_back(tr("Its mod profile cannot be used:"));
        }
        content.after.push_back(
            tr("It is still copied, so it can be fixed in your file manager; the Library's Mods "
               "tab plays only mods that work.")
        );
        content.actions = {sheet_action("OK", 0, Command::none, Role::button_main)};
        break;
    }
    case Sheet::remove_old_confirm:
        content.title = tr("Remove the old folder?");
        content.paragraphs.push_back(fill(
            "Total Annihilation (old) is the folder that was there before ({size}). The game does "
            "not use it; removing it frees the space.",
            {{"size", size_text(model.old_folder_bytes)}}
        ));
        content.actions = {
            sheet_action("Remove", 0, Command::remove_old, Role::button_danger, Glyph::trash),
            sheet_action("Keep", 1, Command::none),
        };
        break;
    case Sheet::remove_part_confirm: {
        const PartRow* row = sheet_row(model);
        if (row != nullptr && row->kind == PartKind::demo_data) {
            content.title = tr("Remove the demo's data?");
            content.paragraphs.push_back(fill(
                "The demo's unpacked data ({size}) is no longer used.",
                {{"size", size_text(row->bytes)}}
            ));
            content.actions = {
                sheet_action(
                    "Remove", 0, Command::remove_demo_data, Role::button_danger, Glyph::trash
                ),
                sheet_action("Keep", 1, Command::none),
            };
        } else {
            const std::string name = row != nullptr ? part_name(*row, false) : tr("this part");
            content.title = fill(
                "Remove {name} ({size})?",
                {{"name", name}, {"size", row != nullptr ? part_size(*row) : std::string{}}}
            );
            const bool plays_without =
                row != nullptr && (row->kind == PartKind::music || row->kind == PartKind::movies);
            content.paragraphs.push_back(
                plays_without ? tr("The game plays without it.")
                              : tr("Saved games that use its units cannot be loaded without "
                                   "it, and shared games need the same files on every machine.")
            );
            content.paragraphs.push_back(
                tr("It is removed at the next start of Open Annihilation.")
            );
            content.actions = {
                sheet_action(
                    "Remove", 0, Command::manage_remove, Role::button_danger, Glyph::trash
                ),
                sheet_action("Keep", 1, Command::none),
            };
        }
        break;
    }
    case Sheet::remove_all_confirm:
        content.title = tr("Remove all game files?");
        content.paragraphs.push_back(fill(
            "Remove every game file from this {device} ({size})? Saved games and settings stay. "
            "The game cannot start until you add the files again.",
            {{"device", device}, {"size", size_text(model.installed_bytes)}}
        ));
        content.actions = {
            sheet_action(
                "Remove all", 0, Command::manage_remove_all, Role::button_danger, Glyph::trash
            ),
            sheet_action("Keep", 1, Command::none),
        };
        break;
    case Sheet::add_files: {
        content.title = tr("Add files");
        content.paragraphs.push_back(
            tr("Expansions, extra maps and mods are added beside what is there; nothing already "
               "there is replaced.")
        );
        uint16_t index = 0;
        if (model.offers_pick_folder)
            content.actions.push_back(sheet_action(
                "Choose a folder",
                index++,
                Command::pick_additions_folder,
                Role::button_main,
                Glyph::folder
            ));
        if (model.offers_pick_files)
            content.actions.push_back(sheet_action(
                "Choose archives",
                index++,
                Command::pick_archives,
                content.actions.empty() ? Role::button_main : Role::button,
                Glyph::file
            ));
        if (model.offers_pick_folder || model.offers_pick_files)
            content.actions.push_back(sheet_action(
                "Choose the demo's installer",
                index++,
                Command::pick_installer,
                Role::button,
                Glyph::disk
            ));
        content.actions.push_back(sheet_action("Cancel", index, Command::none));
        break;
    }
    case Sheet::scheduled_note:
        content.title = tr("From the next start");
        content.paragraphs.push_back(
            tr("The changes are used from the next start of Open Annihilation.")
        );
        content.actions = {sheet_action("OK", 0, Command::done, Role::button_main)};
        break;
    }
    return content;
}

BannerContent banner_content(const Model& model) {
    BannerContent content{};
    const std::string device = platform_word(model, word_device);
    if (model.step == Step::manage && model.pending_replacement) {
        content.shown = true;
        content.glyph = Glyph::clock;
        content.colour = amber_colour;
        content.text =
            tr("The new game files are used from the next start of Open Annihilation. Adding or "
               "removing files waits until then.");
        content.actions = {action(
            "Cancel the replacement",
            {ControlKind::manage_cancel_pending},
            Command::manage_cancel_pending
        )};
        return content;
    }
    switch (model.banner) {
    case Banner::none:
        return content;
    case Banner::continue_copy: {
        content.glyph = Glyph::clock;
        content.colour = amber_colour;
        const std::string done = size_text(model.stopped_bytes);
        content.text =
            model.stopped_total > 0
                ? fill(
                      "Copying from {location} stopped at {done} of {total}. Choose the same "
                      "folder to continue; what was copied is kept.",
                      {{"location", location_of(model)},
                       {"done", done},
                       {"total", size_text(model.stopped_total)}}
                  )
                : fill(
                      "Copying from {location} stopped at {done}. Choose the same folder to "
                      "continue; what was copied is kept.",
                      {{"location", location_of(model)}, {"done", done}}
                  );
        content.actions = {
            action("Choose folder", {ControlKind::choose_folder, 1}, Command::pick_game_folder),
            action("Discard", {ControlKind::banner_discard}, Command::discard, Role::button_danger),
        };
        break;
    }
    case Banner::folder_refused:
        content.glyph = Glyph::warning;
        content.colour = amber_colour;
        content.text =
            model.detail.empty()
                ? fill(
                      "The Total Annihilation folder on this {device} cannot be played.",
                      {{"device", device}}
                  )
                : fill(
                      "The Total Annihilation folder on this {device} cannot be played: {problem}",
                      {{"device", device}, {"problem", model.detail}}
                  );
        content.actions = {
            action("Check again", {ControlKind::banner_action}, Command::check_copied),
        };
        break;
    case Banner::nothing_added:
        content.glyph = Glyph::info;
        content.colour = green_colour;
        content.text = tr("Copying stopped. Nothing was added.");
        break;
    case Banner::copy_went_on:
        content.glyph = Glyph::info;
        content.colour = green_colour;
        content.text = tr("Copying went on while Open Annihilation was away");
        break;
    case Banner::copy_resumed:
        content.glyph = Glyph::info;
        content.colour = green_colour;
        content.text = tr("Copying paused while Open Annihilation was away and has started again");
        break;
    case Banner::added: {
        content.glyph = Glyph::info;
        content.colour = green_colour;
        const std::string count = std::to_string(model.added_files);
        std::string text;
        if (model.added_from.empty())
            text = model.added_files == 1 ? tr("1 file was added")
                                          : fill("{n} files were added", {{"n", count}});
        else
            text = model.added_files == 1
                       ? fill("1 file was added from {source}", {{"source", model.added_from}})
                       : fill(
                             "{n} files were added from {source}",
                             {{"n", count}, {"source", model.added_from}}
                         );
        if (model.added_kept.size() == 1) {
            text = fill(
                "{added}; {name} is there already and was kept.",
                {{"added", text}, {"name", model.added_kept.front()}}
            );
        } else if (!model.added_kept.empty()) {
            constexpr std::size_t named = 3;
            std::vector<std::string> names;
            for (std::size_t index = 0; index < model.added_kept.size() && index < named; ++index)
                names.push_back(model.added_kept[index]);
            if (model.added_kept.size() > named)
                names.push_back(
                    fill("{n} more", {{"n", std::to_string(model.added_kept.size() - named)}})
                );
            text = fill(
                "{added}; {names} are there already and were kept.",
                {{"added", text}, {"names", join_and(names)}}
            );
        } else {
            text = fill("{added}.", {{"added", text}});
        }
        content.text = fill(
            "{added} The new files are used from the next start of Open Annihilation.",
            {{"added", text}}
        );
        break;
    }
    case Banner::next_start:
        content.glyph = Glyph::clock;
        content.colour = amber_colour;
        content.text = tr("The changes are used from the next start of Open Annihilation.");
        if (!model.pending_removals.empty())
            content.text = fill(
                "{changes} Waiting to be removed: {names}.",
                {{"changes", content.text}, {"names", join_and(model.pending_removals)}}
            );
        break;
    }
    content.shown = true;
    return content;
}

bool change_waits(const Model& model) noexcept {
    return model.pending_replacement || !model.pending_removals.empty() ||
           model.banner == Banner::added || model.banner == Banner::next_start;
}

} // namespace oa::ui::game_files::detail
